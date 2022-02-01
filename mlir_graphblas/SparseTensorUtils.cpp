//===- SparseTensorUtils.cpp - Sparse Tensor Utils for MLIR execution -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements a light-weight runtime support library that is useful
// for sparse tensor manipulations. The functionality provided in this library
// is meant to simplify benchmarking, testing, and debugging MLIR code that
// operates on sparse tensors. The provided functionality is **not** part
// of core MLIR, however.
//
//===----------------------------------------------------------------------===//

#include "mlir/ExecutionEngine/SparseTensorUtils.h"
#include "mlir/ExecutionEngine/CRunnerUtils.h"

#ifdef MLIR_CRUNNERUTILS_DEFINE_FUNCTIONS

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <vector>

//// -> MODIFIED
#include <iostream>
//// <- MODIFIED

//===----------------------------------------------------------------------===//
//
// Internal support for storing and reading sparse tensors.
//
// The following memory-resident sparse storage schemes are supported:
//
// (a) A coordinate scheme for temporarily storing and lexicographically
//     sorting a sparse tensor by index (SparseTensorCOO).
//
// (b) A "one-size-fits-all" sparse tensor storage scheme defined by
//     per-dimension sparse/dense annnotations together with a dimension
//     ordering used by MLIR compiler-generated code (SparseTensorStorage).
//
// The following external formats are supported:
//
// (1) Matrix Market Exchange (MME): *.mtx
//     https://math.nist.gov/MatrixMarket/formats.html
//
// (2) Formidable Repository of Open Sparse Tensors and Tools (FROSTT): *.tns
//     http://frostt.io/tensors/file-formats.html
//
// Two public APIs are supported:
//
// (I) Methods operating on MLIR buffers (memrefs) to interact with sparse
//     tensors. These methods should be used exclusively by MLIR
//     compiler-generated code.
//
// (II) Methods that accept C-style data structures to interact with sparse
//      tensors. These methods can be used by any external runtime that wants
//      to interact with MLIR compiler-generated code.
//
// In both cases (I) and (II), the SparseTensorStorage format is externally
// only visible as an opaque pointer.
//
//===----------------------------------------------------------------------===//

namespace {

static constexpr int kColWidth = 1025;

//// -> MODIFIED
// used by verify
template <typename T>
bool issorted(const std::vector<T> &arr, uint64_t start, uint64_t stop) {
  if (stop <= start + 1) {
    return true;
  }
  T prev = arr[start];
  for(size_t i=start + 1; i < stop; ++i) {
    T cur = arr[i];
    if (cur == prev) {
      continue;
    } else if (cur < prev) {
      return false;
    } else {
      prev = cur;
    }
  }
  return true;
}

template <typename T>
bool isincreasing(const std::vector<T> &arr, uint64_t start, uint64_t stop) {
  if (stop <= start + 1) {
    return true;
  }
  T prev = arr[start];
  for(size_t i=start + 1; i < stop; ++i) {
    T cur = arr[i];
    if (cur <= prev) {
      return false;
    } else {
      prev = cur;
    }
  }
  return true;
}
//// <- MODIFIED

/// A sparse tensor element in coordinate scheme (value and indices).
/// For example, a rank-1 vector element would look like
///   ({i}, a[i])
/// and a rank-5 tensor element like
///   ({i,j,k,l,m}, a[i,j,k,l,m])
template <typename V>
struct Element {
  Element(const std::vector<uint64_t> &ind, V val) : indices(ind), value(val){};
  std::vector<uint64_t> indices;
  V value;
};

/// A memory-resident sparse tensor in coordinate scheme (collection of
/// elements). This data structure is used to read a sparse tensor from
/// any external format into memory and sort the elements lexicographically
/// by indices before passing it back to the client (most packed storage
/// formats require the elements to appear in lexicographic index order).
template <typename V>
struct SparseTensorCOO {
public:
  SparseTensorCOO(const std::vector<uint64_t> &szs, uint64_t capacity)
      : sizes(szs), iteratorLocked(false), iteratorPos(0) {
    if (capacity)
      elements.reserve(capacity);
  }
  /// Adds element as indices and value.
  void add(const std::vector<uint64_t> &ind, V val) {
    assert(!iteratorLocked && "Attempt to add() after startIterator()");
    uint64_t rank = getRank();
    assert(rank == ind.size());
    for (uint64_t r = 0; r < rank; r++)
      assert(ind[r] < sizes[r]); // within bounds
    elements.emplace_back(ind, val);
  }
  /// Sorts elements lexicographically by index.
  void sort() {
    assert(!iteratorLocked && "Attempt to sort() after startIterator()");
    // TODO: we may want to cache an `isSorted` bit, to avoid
    // unnecessary/redundant sorting.
    std::sort(elements.begin(), elements.end(), lexOrder);
  }
  /// Returns rank.
  uint64_t getRank() const { return sizes.size(); }
  /// Getter for sizes array.
  const std::vector<uint64_t> &getSizes() const { return sizes; }
  /// Getter for elements array.
  const std::vector<Element<V>> &getElements() const { return elements; }

  /// Switch into iterator mode.
  void startIterator() {
    iteratorLocked = true;
    iteratorPos = 0;
  }
  /// Get the next element.
  const Element<V> *getNext() {
    assert(iteratorLocked && "Attempt to getNext() before startIterator()");
    if (iteratorPos < elements.size())
      return &(elements[iteratorPos++]);
    iteratorLocked = false;
    return nullptr;
  }

  /// Factory method. Permutes the original dimensions according to
  /// the given ordering and expects subsequent add() calls to honor
  /// that same ordering for the given indices. The result is a
  /// fully permuted coordinate scheme.
  static SparseTensorCOO<V> *newSparseTensorCOO(uint64_t rank,
                                                const uint64_t *sizes,
                                                const uint64_t *perm,
                                                uint64_t capacity = 0) {
    std::vector<uint64_t> permsz(rank);
    for (uint64_t r = 0; r < rank; r++)
      permsz[perm[r]] = sizes[r];
    return new SparseTensorCOO<V>(permsz, capacity);
  }

private:
  /// Returns true if indices of e1 < indices of e2.
  static bool lexOrder(const Element<V> &e1, const Element<V> &e2) {
    uint64_t rank = e1.indices.size();
    assert(rank == e2.indices.size());
    for (uint64_t r = 0; r < rank; r++) {
      if (e1.indices[r] == e2.indices[r])
        continue;
      return e1.indices[r] < e2.indices[r];
    }
    return false;
  }
  const std::vector<uint64_t> sizes; // per-dimension sizes
  std::vector<Element<V>> elements;
  bool iteratorLocked;
  unsigned iteratorPos;
};

/// Abstract base class of sparse tensor storage. Note that we use
/// function overloading to implement "partial" method specialization.
class SparseTensorStorageBase {
public:
  /// Dimension size query.
  virtual uint64_t getDimSize(uint64_t) = 0;

  /// Overhead storage.
  virtual void getPointers(std::vector<uint64_t> **, uint64_t) { fatal("p64"); }
  virtual void getPointers(std::vector<uint32_t> **, uint64_t) { fatal("p32"); }
  virtual void getPointers(std::vector<uint16_t> **, uint64_t) { fatal("p16"); }
  virtual void getPointers(std::vector<uint8_t> **, uint64_t) { fatal("p8"); }
  virtual void getIndices(std::vector<uint64_t> **, uint64_t) { fatal("i64"); }
  virtual void getIndices(std::vector<uint32_t> **, uint64_t) { fatal("i32"); }
  virtual void getIndices(std::vector<uint16_t> **, uint64_t) { fatal("i16"); }
  virtual void getIndices(std::vector<uint8_t> **, uint64_t) { fatal("i8"); }

  /// Primary storage.
  virtual void getValues(std::vector<double> **) { fatal("valf64"); }
  virtual void getValues(std::vector<float> **) { fatal("valf32"); }
  virtual void getValues(std::vector<int64_t> **) { fatal("vali64"); }
  virtual void getValues(std::vector<int32_t> **) { fatal("vali32"); }
  virtual void getValues(std::vector<int16_t> **) { fatal("vali16"); }
  virtual void getValues(std::vector<int8_t> **) { fatal("vali8"); }

  /// Element-wise insertion in lexicographic index order.
  virtual void lexInsert(const uint64_t *, double) { fatal("insf64"); }
  virtual void lexInsert(const uint64_t *, float) { fatal("insf32"); }
  virtual void lexInsert(const uint64_t *, int64_t) { fatal("insi64"); }
  virtual void lexInsert(const uint64_t *, int32_t) { fatal("insi32"); }
  virtual void lexInsert(const uint64_t *, int16_t) { fatal("ins16"); }
  virtual void lexInsert(const uint64_t *, int8_t) { fatal("insi8"); }

  /// Expanded insertion.
  virtual void expInsert(uint64_t *, double *, bool *, uint64_t *, uint64_t) {
    fatal("expf64");
  }
  virtual void expInsert(uint64_t *, float *, bool *, uint64_t *, uint64_t) {
    fatal("expf32");
  }
  virtual void expInsert(uint64_t *, int64_t *, bool *, uint64_t *, uint64_t) {
    fatal("expi64");
  }
  virtual void expInsert(uint64_t *, int32_t *, bool *, uint64_t *, uint64_t) {
    fatal("expi32");
  }
  virtual void expInsert(uint64_t *, int16_t *, bool *, uint64_t *, uint64_t) {
    fatal("expi16");
  }
  virtual void expInsert(uint64_t *, int8_t *, bool *, uint64_t *, uint64_t) {
    fatal("expi8");
  }

  /// Finishes insertion.
  virtual void endInsert() = 0;

  virtual ~SparseTensorStorageBase() = default;

  //// -> MODIFIED
  virtual uint64_t getRank() const { return 0; }
  virtual void *get_rev_ptr() {
    fatal("get_rev_ptr");
    return 0;
  }
  virtual void *get_sizes_ptr() {
    fatal("get_sizes_ptr");
    return 0;
  }
  virtual void *get_pointers_ptr() {
    fatal("get_pointers_ptr");
    return 0;
  }
  virtual void *get_indices_ptr() {
    fatal("get_indices_ptr");
    return 0;
  }
  virtual void *get_values_ptr() {
    fatal("get_values_ptr");
    return 0;
  }

  virtual void swap_rev(void *new_rev) { fatal("swap_rev"); }
  virtual void swap_sizes(void *new_sizes) { fatal("swap_sizes"); }
  virtual void swap_pointers(void *new_pointers) { fatal("swap_pointers"); }
  virtual void swap_indices(void *new_indices) { fatal("swap_indices"); }
  virtual void swap_values(void *new_values) { fatal("swap_values"); }

  virtual void assign_rev(uint64_t d, uint64_t index) { fatal("assign_rev"); }
  virtual void resize_pointers(uint64_t d, uint64_t size) {
    fatal("resize_pointers");
  }
  virtual void resize_index(uint64_t d, uint64_t size) {
    fatal("resize_index");
  }
  virtual void resize_values(uint64_t size) { fatal("resize_values"); }
  virtual void resize_dim(uint64_t d, uint64_t size) { fatal("resize_dim"); }

  virtual void *dup() {
    fatal("dup");
    return NULL;
  }
  //virtual void *empty_like() {
  //  fatal("empty_like");
  //  return NULL;
  //}
  //virtual void *empty(uint64_t ndims) {
  //  fatal("empty");
  //  return NULL;
  //}

  virtual bool verify() {
    fatal("verify");
    return false;
  }

  virtual void print_components(int64_t level) {
    fatal("print_components");
  }
  virtual void print_dense() {
    fatal("print_dense");
  }
  //// <- MODIFIED

private:
  void fatal(const char *tp) {
    fprintf(stderr, "unsupported %s\n", tp);
    exit(1);
  }
};

/// A memory-resident sparse tensor using a storage scheme based on
/// per-dimension sparse/dense annotations. This data structure provides a
/// bufferized form of a sparse tensor type. In contrast to generating setup
/// methods for each differently annotated sparse tensor, this method provides
/// a convenient "one-size-fits-all" solution that simply takes an input tensor
/// and annotations to implement all required setup in a general manner.
template <typename P, typename I, typename V>
class SparseTensorStorage : public SparseTensorStorageBase {
public:
  /// Constructs a sparse tensor storage scheme with the given dimensions,
  /// permutation, and per-dimension dense/sparse annotations, using
  /// the coordinate scheme tensor for the initial contents if provided.
  SparseTensorStorage(const std::vector<uint64_t> &szs, const uint64_t *perm,
                      const DimLevelType *sparsity,
                      SparseTensorCOO<V> *tensor = nullptr)
      : sizes(szs), rev(getRank()), idx(getRank()), pointers(getRank()),
        indices(getRank()) {
    uint64_t rank = getRank();
    // Store "reverse" permutation.
    for (uint64_t r = 0; r < rank; r++)
      rev[perm[r]] = r;
    // Provide hints on capacity of pointers and indices.
    // TODO: needs fine-tuning based on sparsity
    bool allDense = true;
    uint64_t sz = 1;
    for (uint64_t r = 0; r < rank; r++) {
      sz *= sizes[r];
      if (sparsity[r] == DimLevelType::kCompressed) {
        pointers[r].reserve(sz + 1);
        indices[r].reserve(sz);
        sz = 1;
        allDense = false;
      } else {
        assert(sparsity[r] == DimLevelType::kDense &&
               "singleton not yet supported");
      }
    }
    // Prepare sparse pointer structures for all dimensions.
    for (uint64_t r = 0; r < rank; r++)
      if (sparsity[r] == DimLevelType::kCompressed)
        pointers[r].push_back(0);
    // Then assign contents from coordinate scheme tensor if provided.
    if (tensor) {
      // Lexicographically sort the tensor, to ensure precondition of `fromCOO`.
      tensor->sort();
      const std::vector<Element<V>> &elements = tensor->getElements();
      uint64_t nnz = elements.size();
      values.reserve(nnz);
      fromCOO(elements, 0, nnz, 0);
    } else if (allDense) {
      values.resize(sz, 0);
    }
  }

  ~SparseTensorStorage() override = default;

  /// Get the rank of the tensor.
  uint64_t getRank() const override { return sizes.size(); } //// MODIFIED: Added override

  /// Get the size in the given dimension of the tensor.
  uint64_t getDimSize(uint64_t d) override {
    assert(d < getRank());
    return sizes[d];
  }

  /// Partially specialize these getter methods based on template types.
  void getPointers(std::vector<P> **out, uint64_t d) override {
    assert(d < getRank());
    *out = &pointers[d];
  }
  void getIndices(std::vector<I> **out, uint64_t d) override {
    assert(d < getRank());
    *out = &indices[d];
  }
  void getValues(std::vector<V> **out) override { *out = &values; }

  /// Partially specialize lexicographical insertions based on template types.
  void lexInsert(const uint64_t *cursor, V val) override {
    // First, wrap up pending insertion path.
    uint64_t diff = 0;
    uint64_t top = 0;
    if (!values.empty()) {
      diff = lexDiff(cursor);
      endPath(diff + 1);
      top = idx[diff] + 1;
    }
    // Then continue with insertion path.
    insPath(cursor, diff, top, val);
  }

  /// Partially specialize expanded insertions based on template types.
  /// Note that this method resets the values/filled-switch array back
  /// to all-zero/false while only iterating over the nonzero elements.
  void expInsert(uint64_t *cursor, V *values, bool *filled, uint64_t *added,
                 uint64_t count) override {
    if (count == 0)
      return;
    // Sort.
    std::sort(added, added + count);
    // Restore insertion path for first insert.
    uint64_t rank = getRank();
    uint64_t index = added[0];
    cursor[rank - 1] = index;
    lexInsert(cursor, values[index]);
    assert(filled[index]);
    values[index] = 0;
    filled[index] = false;
    // Subsequent insertions are quick.
    for (uint64_t i = 1; i < count; i++) {
      assert(index < added[i] && "non-lexicographic insertion");
      index = added[i];
      cursor[rank - 1] = index;
      insPath(cursor, rank - 1, added[i - 1] + 1, values[index]);
      assert(filled[index]);
      values[index] = 0.0;
      filled[index] = false;
    }
  }

  /// Finalizes lexicographic insertions.
  void endInsert() override {
    if (values.empty())
      endDim(0);
    else
      endPath(0);
  }

  /// Returns this sparse tensor storage scheme as a new memory-resident
  /// sparse tensor in coordinate scheme with the given dimension order.
  SparseTensorCOO<V> *toCOO(const uint64_t *perm) {
    // Restore original order of the dimension sizes and allocate coordinate
    // scheme with desired new ordering specified in perm.
    uint64_t rank = getRank();
    std::vector<uint64_t> orgsz(rank);
    for (uint64_t r = 0; r < rank; r++)
      orgsz[rev[r]] = sizes[r];
    SparseTensorCOO<V> *tensor = SparseTensorCOO<V>::newSparseTensorCOO(
        rank, orgsz.data(), perm, values.size());
    // Populate coordinate scheme restored from old ordering and changed with
    // new ordering. Rather than applying both reorderings during the recursion,
    // we compute the combine permutation in advance.
    std::vector<uint64_t> reord(rank);
    for (uint64_t r = 0; r < rank; r++)
      reord[r] = perm[rev[r]];
    toCOO(*tensor, reord, 0, 0);
    assert(tensor->getElements().size() == values.size());
    return tensor;
  }

  /// Factory method. Constructs a sparse tensor storage scheme with the given
  /// dimensions, permutation, and per-dimension dense/sparse annotations,
  /// using the coordinate scheme tensor for the initial contents if provided.
  /// In the latter case, the coordinate scheme must respect the same
  /// permutation as is desired for the new sparse tensor storage.
  static SparseTensorStorage<P, I, V> *
  newSparseTensor(uint64_t rank, const uint64_t *sizes, const uint64_t *perm,
                  const DimLevelType *sparsity, SparseTensorCOO<V> *tensor) {
    SparseTensorStorage<P, I, V> *n = nullptr;
    if (tensor) {
      assert(tensor->getRank() == rank);
      for (uint64_t r = 0; r < rank; r++)
        assert(sizes[r] == 0 || tensor->getSizes()[perm[r]] == sizes[r]);
      n = new SparseTensorStorage<P, I, V>(tensor->getSizes(), perm, sparsity,
                                           tensor);
      delete tensor;
    } else {
      std::vector<uint64_t> permsz(rank);
      for (uint64_t r = 0; r < rank; r++)
        permsz[perm[r]] = sizes[r];
      n = new SparseTensorStorage<P, I, V>(permsz, perm, sparsity);
    }
    return n;
  }

private:
  /// Initializes sparse tensor storage scheme from a memory-resident sparse
  /// tensor in coordinate scheme. This method prepares the pointers and
  /// indices arrays under the given per-dimension dense/sparse annotations.
  /// Precondition: the `elements` must be lexicographically sorted.
  void fromCOO(const std::vector<Element<V>> &elements, uint64_t lo,
               uint64_t hi, uint64_t d) {
    // Once dimensions are exhausted, insert the numerical values.
    assert(d <= getRank());
    if (d == getRank()) {
      assert(lo < hi && hi <= elements.size());
      values.push_back(elements[lo].value);
      return;
    }
    // Visit all elements in this interval.
    uint64_t full = 0;
    while (lo < hi) {
      assert(lo < elements.size() && hi <= elements.size());
      // Find segment in interval with same index elements in this dimension.
      uint64_t i = elements[lo].indices[d];
      uint64_t seg = lo + 1;
      while (seg < hi && elements[seg].indices[d] == i)
        seg++;
      // Handle segment in interval for sparse or dense dimension.
      if (isCompressedDim(d)) {
        indices[d].push_back(i);
      } else {
        // For dense storage we must fill in all the zero values between
        // the previous element (when last we ran this for-loop) and the
        // current element.
        for (; full < i; full++)
          endDim(d + 1);
        full++;
      }
      fromCOO(elements, lo, seg, d + 1);
      // And move on to next segment in interval.
      lo = seg;
    }
    // Finalize the sparse pointer structure at this dimension.
    if (isCompressedDim(d)) {
      pointers[d].push_back(indices[d].size());
    } else {
      // For dense storage we must fill in all the zero values after
      // the last element.
      for (uint64_t sz = sizes[d]; full < sz; full++)
        endDim(d + 1);
    }
  }

  /// Stores the sparse tensor storage scheme into a memory-resident sparse
  /// tensor in coordinate scheme.
  void toCOO(SparseTensorCOO<V> &tensor, std::vector<uint64_t> &reord,
             uint64_t pos, uint64_t d) {
    assert(d <= getRank());
    if (d == getRank()) {
      assert(pos < values.size());
      tensor.add(idx, values[pos]);
    } else if (isCompressedDim(d)) {
      // Sparse dimension.
      for (uint64_t ii = pointers[d][pos]; ii < pointers[d][pos + 1]; ii++) {
        idx[reord[d]] = indices[d][ii];
        toCOO(tensor, reord, ii, d + 1);
      }
    } else {
      // Dense dimension.
      for (uint64_t i = 0, sz = sizes[d], off = pos * sz; i < sz; i++) {
        idx[reord[d]] = i;
        toCOO(tensor, reord, off + i, d + 1);
      }
    }
  }

  /// Ends a deeper, never seen before dimension.
  void endDim(uint64_t d) {
    assert(d <= getRank());
    if (d == getRank()) {
      values.push_back(0);
    } else if (isCompressedDim(d)) {
      pointers[d].push_back(indices[d].size());
    } else {
      for (uint64_t full = 0, sz = sizes[d]; full < sz; full++)
        endDim(d + 1);
    }
  }

  /// Wraps up a single insertion path, inner to outer.
  void endPath(uint64_t diff) {
    uint64_t rank = getRank();
    assert(diff <= rank);
    for (uint64_t i = 0; i < rank - diff; i++) {
      uint64_t d = rank - i - 1;
      if (isCompressedDim(d)) {
        pointers[d].push_back(indices[d].size());
      } else {
        for (uint64_t full = idx[d] + 1, sz = sizes[d]; full < sz; full++)
          endDim(d + 1);
      }
    }
  }

  /// Continues a single insertion path, outer to inner.
  void insPath(const uint64_t *cursor, uint64_t diff, uint64_t top, V val) {
    uint64_t rank = getRank();
    assert(diff < rank);
    for (uint64_t d = diff; d < rank; d++) {
      uint64_t i = cursor[d];
      if (isCompressedDim(d)) {
        indices[d].push_back(i);
      } else {
        for (uint64_t full = top; full < i; full++)
          endDim(d + 1);
      }
      top = 0;
      idx[d] = i;
    }
    values.push_back(val);
  }

  /// Finds the lexicographic differing dimension.
  uint64_t lexDiff(const uint64_t *cursor) {
    for (uint64_t r = 0, rank = getRank(); r < rank; r++)
      if (cursor[r] > idx[r])
        return r;
      else
        assert(cursor[r] == idx[r] && "non-lexicographic insertion");
    assert(0 && "duplication insertion");
    return -1u;
  }

  /// Returns true if dimension is compressed.
  inline bool isCompressedDim(uint64_t d) const {
    return (!pointers[d].empty());
  }

private:
  std::vector<uint64_t> sizes; // per-dimension sizes
  std::vector<uint64_t> rev;   // "reverse" permutation
  std::vector<uint64_t> idx;   // index cursor
  std::vector<std::vector<P>> pointers;
  std::vector<std::vector<I>> indices;
  std::vector<V> values;

  //// -> MODIFIED
public:
  /*
  SparseTensorStorage(
      const std::vector<uint64_t> &other_sizes,
      const std::vector<std::vector<P>> &other_pointers,
      const std::vector<std::vector<I>> &other_indices,
      const std::vector<V> &other_values)
  :
      sizes(other_sizes),
      pointers(other_pointers),
      indices(other_indices),
      values(other_values)
  {}

  SparseTensorStorage(const SparseTensorStorage<P, I, V> &other)
  :
      sizes(other.sizes),
      pointers(other.pointers),
      indices(other.indices),
      values(other.values)
  {}
  */

  // Used by `empty_like`
  /*SparseTensorStorage(const std::vector<uint64_t> &other_sizes, void *other)
      : sizes(other_sizes),
        rev(static_cast<SparseTensorStorage<P, I, V> *>(other)->rev),
        pointers(other_sizes.size()), indices(other_sizes.size()) {
    // Update pointers to have same size as original tensor, but filled with
    // zeros
    SparseTensorStorage<P, I, V> *tensor =
        static_cast<SparseTensorStorage<P, I, V> *>(other);
    for (uint64_t dim = 0; dim < other_sizes.size(); dim++) {
      pointers[dim].resize(tensor->pointers[dim].size());
    }
  }*/

  // Used by `empty`
  // Note that `len(pointers[0]) == 0`!
  /*SparseTensorStorage(uint64_t ndims)
      : sizes(ndims), rev(ndims), pointers(ndims), indices(ndims) {}*/

  // Used by `dup`
  SparseTensorStorage(void *other)
      : sizes(static_cast<SparseTensorStorage<P, I, V> *>(other)->sizes),
        rev(static_cast<SparseTensorStorage<P, I, V> *>(other)->rev),
        pointers(static_cast<SparseTensorStorage<P, I, V> *>(other)->pointers),
        indices(static_cast<SparseTensorStorage<P, I, V> *>(other)->indices),
        values(static_cast<SparseTensorStorage<P, I, V> *>(other)->values) {}

  SparseTensorStorage(const std::vector<uint64_t> &other_sizes,
                      const std::vector<uint64_t> &other_rev, bool is_sparse)
      : sizes(other_sizes), rev(other_rev) {
    pointers.resize(sizes.size());
    if (is_sparse) {
      pointers[0].resize(2);
    }
    for (size_t i = 1; i < sizes.size(); ++i) {
      pointers[i].resize(1);
    }
    indices.resize(sizes.size());
  }

  void *get_rev_ptr() override { return &rev; }
  void *get_sizes_ptr() override { return &sizes; }
  void *get_pointers_ptr() override { return &pointers; }
  void *get_indices_ptr() override { return &indices; }
  void *get_values_ptr() override { return &values; }

  void swap_rev(void *new_rev) override {
    rev.swap(*(std::vector<uint64_t> *)new_rev);
  }
  void swap_sizes(void *new_sizes) override {
    sizes.swap(*(std::vector<uint64_t> *)new_sizes);
  }
  void swap_pointers(void *new_pointers) override {
    pointers.swap(*(std::vector<std::vector<P>> *)new_pointers);
  }
  void swap_indices(void *new_indices) override {
    indices.swap(*(std::vector<std::vector<I>> *)new_indices);
  }
  void swap_values(void *new_values) override {
    values.swap(*(std::vector<V> *)new_values);
  }
  void assign_rev(uint64_t d, uint64_t index) override { rev[d] = index; }
  void resize_pointers(uint64_t d, uint64_t size) override {
    pointers[d].resize(size);
  }
  void resize_index(uint64_t d, uint64_t size) override {
    indices[d].resize(size);
  }
  void resize_values(uint64_t size) override { values.resize(size); }
  void resize_dim(uint64_t d, uint64_t size) override { sizes[d] = size; }
  // New tensor of same type with same data
  void *dup() override {
    SparseTensorStorageBase *tensor = new SparseTensorStorage<P, I, V>(this);
    return tensor;
  }
  // New tensor of same type with same shape
  //void *empty_like() override {
  //  SparseTensorStorageBase *tensor =
  //      new SparseTensorStorage<P, I, V>(sizes, this);
  //  return tensor;
  //}
  // New tensor of dimensions `ndims` (no shape; must use `resize_dim`)
  //void *empty(uint64_t ndims) override {
  //  SparseTensorStorageBase *tensor = new SparseTensorStorage<P, I, V>(ndims);
  //  return tensor;
  //}

  bool verify() override {
    bool rv = true;
    uint64_t ndim = this->getRank();
    if (ndim == 0) {
      fprintf(stderr, "Bad tensor: ndim == 0\n");
      return false;
    }
    if (this->rev.size() != ndim) {
      fprintf(stderr, "Bad tensor: len(rev) != ndim\n");
      rv = false;
    } else {
      std::vector<uint64_t> zeros(ndim, 0);
      for (uint64_t i : this->rev) {
        if (i >= ndim) {
          fprintf(stderr, "Bad tensor: rev[i] >= ndim\n");
          rv = false;
        } else {
          zeros[i] = 1;
        }
      }
      for (uint64_t i : zeros) {
        if (i == 0) {
          fprintf(stderr, "Bad tensor: rev[i] == rev[j]\n");
          rv = false;
        }
      }
    }
    if (this->pointers.size() != ndim) {
      fprintf(stderr, "Bad tensor: len(pointers) != ndim\n");
      return false;
    }
    if (this->indices.size() != ndim) {
      fprintf(stderr, "Bad tensor: len(indices) != ndim\n");
      return false;
    }
    if (this->sizes.size() != ndim) {
      fprintf(stderr, "Bad tensor: len(sizes) != ndim\n");
      return false;
    }

    bool is_dense = true;
    uint64_t cum_size = 1;
    uint64_t prev_ptr_len = 0;
    uint64_t prev_idx_len = 0;
    for (size_t dim = 0; dim < ndim; ++dim) {
      auto &ptr = this->pointers[dim];
      auto &idx = this->indices[dim];
      auto &size = this->sizes[dim];
      if (size <= 0) {
        fprintf(stderr, "Bad tensor (dim=%lu): size <= 0\n", dim);
        return false;
      }
      cum_size = cum_size * size;
      if (ptr.size() == 0) {
        if (idx.size() != 0) {
          fprintf(stderr,
                  "Bad tensor (dim=%lu): len(ptr) == 0 and len(idx) != 0\n",
                  dim);
          rv = false;
        }
      } else {
        if (dim == 0) {
          if (ptr.size() < 2) {
            fprintf(stderr, "Bad tensor (dim=%lu): len(ptr) < 2\n", dim);
            rv = false;
          }
          if (ptr.size() > ((2 < idx.size() + 1) ? idx.size() + 1 : 2)) {
            // max(2, ...), because len(ptr) >= 2, and len(idx) >= 0 when dim ==
            // 0
            fprintf(stderr,
                    "Bad tensor (dim=%lu): len(ptr) > max(2, len(idx) + 1)\n",
                    dim);
            rv = false;
          }
        } else {
          if (is_dense) {
            if (ptr.size() != cum_size / size + 1) {
              fprintf(stderr,
                      "Bad tensor (dim=%lu): len(ptr) != cum_size // size + 1 "
                      "(previous dimensions were dense)\n",
                      dim);
              rv = false;
            }
          } else {
            // works for 2d
            if (ptr.size() > idx.size() + 1) {
              fprintf(stderr, "Bad tensor (dim=%lu): len(ptr) > len(idx) + 1\n",
                      dim);
              rv = false;
            }
          }
          if (ptr.size() < 1) {
            fprintf(stderr, "Bad tensor (dim=%lu): len(ptr) < 1\n", dim);
            rv = false;
          }
        }
        bool check_idx = true;
        if (ptr.size() > cum_size + 1) {
          fprintf(stderr, "Bad tensor (dim=%lu): len(ptr) > cum_size + 1\n",
                  dim);
          rv = false;
        }
        if (idx.size() > cum_size) {
          fprintf(stderr, "Bad tensor (dim=%lu): len(idx) > cum_size\n", dim);
          rv = false;
        }
        if (!issorted(ptr, 0, ptr.size())) {
          fprintf(stderr, "Bad tensor (dim=%lu): not issorted(ptr)\n", dim);
          rv = false;
          check_idx = false;
        }
        if (ptr[0] != 0) {
          fprintf(stderr, "Bad tensor (dim=%lu): ptr[0] != 0\n", dim);
          rv = false;
          check_idx = false;
        }
        if (ptr[ptr.size() - 1] != idx.size()) {
          fprintf(stderr, "Bad tensor (dim=%lu): ptr[-1] != len(idx)\n", dim);
          rv = false;
          check_idx = false;
        }
        for (auto i : idx) {
          if (i >= size) {
            fprintf(stderr, "Bad tensor (dim=%lu): idx[i] >= size\n", dim);
            rv = false;
            check_idx = false;
          }
        }
        if (check_idx) {
          auto start = ptr[0];
          for (size_t i = 1; i < ptr.size(); ++i) {
            auto end = ptr[i];
            if (end > idx.size()) {
              // Just in case.  Bad ptr should have been caught above.
              rv = false;
            } else if (!isincreasing(idx, start, end)) {
              fprintf(stderr, "Bad tensor (dim=%lu): not isincreasing(idx)\n",
                      dim);
              rv = false;
            }
            start = end;
          }
        }
        // These four checks may be redundant (and will they work for higher
        // rank?)
        if (prev_idx_len >= ptr.size()) {
          fprintf(stderr, "Bad tensor (dim=%lu): len(prev_idx) >= len(ptr)\n",
                  dim);
          rv = false;
        }
        if (prev_idx_len > idx.size()) {
          fprintf(stderr, "Bad tensor (dim=%lu): len(prev_idx) >= len(idx)\n",
                  dim);
          rv = false;
        }
        if (prev_ptr_len > ptr.size() + 1) {
          fprintf(stderr,
                  "Bad tensor (dim=%lu): len(prev_ptr) >= len(ptr) + 1\n", dim);
          rv = false;
        }
        if (prev_ptr_len > idx.size() + 2) {
          fprintf(stderr,
                  "Bad tensor (dim=%lu): len(prev_ptr) >= len(idx) + 2\n", dim);
          rv = false;
        }
        prev_ptr_len = ptr.size();
        prev_idx_len = idx.size();
        is_dense = false;
      }
    }
    if (is_dense) {
      if (cum_size != this->values.size()) {
        fprintf(stderr, "Bad tensor: cum_size != len(values)\n");
        rv = false;
      }
    } else {
      if (prev_idx_len != this->values.size()) {
        fprintf(stderr, "Bad tensor: len(last_idx) != len(values)\n");
        rv = false;
      }
    }
    return rv;
  }

  void print_components(int64_t level) override {
    // level 0 prints a dense tensor with '_' for missing values
    // level 1 prints the values array
    // level 2 prints indices and values
    // level 3 prints pointers, indices, and values
    // level 4 prints shape, pointers, indices, values
    // level 5 prints rev, shape, pointers, indices, values
    uint64_t rank = getRank();
    if (level >= 5) {
      // Print rev
      std::cout << "rev=(";
      for (uint64_t i=0; i<rank; i++) {
        if (i != 0)
          std::cout << ", ";
        std::cout << rev[i];
      }
      std::cout << ")\n";
    }
    if (level >= 4) {
      // Print shape
      std::cout << "shape=(";
      for (uint64_t i=0; i<rank; i++) {
        if (i != 0)
          std::cout << ", ";
        std::cout << sizes[rev[i]];
      }
      std::cout << ")\n";
    }
    if (level >= 3) {
      // Print pointers
      std::vector<P> ptrs = pointers[rank == 2 ? 1 : 0];
      std::cout << "pointers=(";
      for (uint64_t i=0; i<ptrs.size(); i++) {
        if (i != 0)
          std::cout << ", ";
        std::cout << ptrs[i];
      }
      std::cout << ")\n";
    }
    if (level >= 2) {
      // Print indices
      std::cout << "indices=(";
      std::vector<I> idx = indices[rank == 2 ? 1 : 0];
      for (uint64_t i=0; i<idx.size(); i++) {
        if (i != 0)
          std::cout << ", ";
        std::cout << idx[i];
      }
      std::cout << ")\n";
    }
    if (level >= 1) {
      // Print values
      std::cout << "values=(";
      for (uint64_t i=0; i<values.size(); i++) {
        if (i != 0)
          std::cout << ", ";
        std::cout << values[i];
      }
      std::cout << ")\n";
    }
  }

  void print_dense() override {
    uint64_t rank = sizes.size();
    if (rank == 1) {
      uint64_t length = sizes[0];
      std::cout << "[";
      std::vector<I> idx = indices[0];
      uint64_t idx_position = 0;
      uint64_t nnz = pointers[0][1];
      for (uint64_t i=0; i<length; i++) {
        if (i != 0)
          std::cout << ", ";
	if (idx_position < nnz && idx[idx_position] == i) {
          std::cout << values[idx_position];
	  idx_position++;
	} else {
          std::cout << "_";
	}
      }
      std::cout << "]";
    } else if (rank == 2) {
      uint64_t numRows = sizes[0];
      uint64_t numCols = sizes[1];
      std::vector<I> idx = indices[1];
      std::cout << "[";
      for (uint64_t r=0; r<numRows; r++) {
        if (r != 0)
          std::cout << ",";
	uint64_t first_ptr = pointers[1][r];
	uint64_t second_ptr = pointers[1][r+1];
	uint64_t ptr_delta = 0;
	std::cout << "\n" << "  [";
	for (uint64_t c=0; c<numCols; c++) {
	  if (c != 0)
	    std::cout << ", ";
	  uint64_t idx_position = first_ptr + ptr_delta;
	  if (idx_position < second_ptr && idx[idx_position] == c) {
	    std::cout << values[idx_position];
	    ptr_delta++;
	  } else {
	    std::cout << "_";
	  }
	}
	std::cout << "]";
      }
      std::cout << "\n" << "]";
    } else {
      std::cout << "Printing tensors of rank " << rank << " not yet supported.";
    }
  }
  //// <- MODIFIED
};

/// Helper to convert string to lower case.
static char *toLower(char *token) {
  for (char *c = token; *c; c++)
    *c = tolower(*c);
  return token;
}

/// Read the MME header of a general sparse matrix of type real.
static void readMMEHeader(FILE *file, char *filename, char *line,
                          uint64_t *idata, bool *isSymmetric) {
  char header[64];
  char object[64];
  char format[64];
  char field[64];
  char symmetry[64];
  // Read header line.
  if (fscanf(file, "%63s %63s %63s %63s %63s\n", header, object, format, field,
             symmetry) != 5) {
    fprintf(stderr, "Corrupt header in %s\n", filename);
    exit(1);
  }
  *isSymmetric = (strcmp(toLower(symmetry), "symmetric") == 0);
  // Make sure this is a general sparse matrix.
  if (strcmp(toLower(header), "%%matrixmarket") ||
      strcmp(toLower(object), "matrix") ||
      strcmp(toLower(format), "coordinate") || strcmp(toLower(field), "real") ||
      (strcmp(toLower(symmetry), "general") && !(*isSymmetric))) {
    fprintf(stderr,
            "Cannot find a general sparse matrix with type real in %s\n",
            filename);
    exit(1);
  }
  // Skip comments.
  while (true) {
    if (!fgets(line, kColWidth, file)) {
      fprintf(stderr, "Cannot find data in %s\n", filename);
      exit(1);
    }
    if (line[0] != '%')
      break;
  }
  // Next line contains M N NNZ.
  idata[0] = 2; // rank
  if (sscanf(line, "%" PRIu64 "%" PRIu64 "%" PRIu64 "\n", idata + 2, idata + 3,
             idata + 1) != 3) {
    fprintf(stderr, "Cannot find size in %s\n", filename);
    exit(1);
  }
}

/// Read the "extended" FROSTT header. Although not part of the documented
/// format, we assume that the file starts with optional comments followed
/// by two lines that define the rank, the number of nonzeros, and the
/// dimensions sizes (one per rank) of the sparse tensor.
static void readExtFROSTTHeader(FILE *file, char *filename, char *line,
                                uint64_t *idata) {
  // Skip comments.
  while (true) {
    if (!fgets(line, kColWidth, file)) {
      fprintf(stderr, "Cannot find data in %s\n", filename);
      exit(1);
    }
    if (line[0] != '#')
      break;
  }
  // Next line contains RANK and NNZ.
  if (sscanf(line, "%" PRIu64 "%" PRIu64 "\n", idata, idata + 1) != 2) {
    fprintf(stderr, "Cannot find metadata in %s\n", filename);
    exit(1);
  }
  // Followed by a line with the dimension sizes (one per rank).
  for (uint64_t r = 0; r < idata[0]; r++) {
    if (fscanf(file, "%" PRIu64, idata + 2 + r) != 1) {
      fprintf(stderr, "Cannot find dimension size %s\n", filename);
      exit(1);
    }
  }
  fgets(line, kColWidth, file); // end of line
}

/// Reads a sparse tensor with the given filename into a memory-resident
/// sparse tensor in coordinate scheme.
template <typename V>
static SparseTensorCOO<V> *openSparseTensorCOO(char *filename, uint64_t rank,
                                               const uint64_t *sizes,
                                               const uint64_t *perm) {
  // Open the file.
  FILE *file = fopen(filename, "r");
  if (!file) {
    fprintf(stderr, "Cannot find %s\n", filename);
    exit(1);
  }
  // Perform some file format dependent set up.
  char line[kColWidth];
  uint64_t idata[512];
  bool isSymmetric = false;
  if (strstr(filename, ".mtx")) {
    readMMEHeader(file, filename, line, idata, &isSymmetric);
  } else if (strstr(filename, ".tns")) {
    readExtFROSTTHeader(file, filename, line, idata);
  } else {
    fprintf(stderr, "Unknown format %s\n", filename);
    exit(1);
  }
  // Prepare sparse tensor object with per-dimension sizes
  // and the number of nonzeros as initial capacity.
  assert(rank == idata[0] && "rank mismatch");
  uint64_t nnz = idata[1];
  for (uint64_t r = 0; r < rank; r++)
    assert((sizes[r] == 0 || sizes[r] == idata[2 + r]) &&
           "dimension size mismatch");
  SparseTensorCOO<V> *tensor =
      SparseTensorCOO<V>::newSparseTensorCOO(rank, idata + 2, perm, nnz);
  //  Read all nonzero elements.
  std::vector<uint64_t> indices(rank);
  for (uint64_t k = 0; k < nnz; k++) {
    if (!fgets(line, kColWidth, file)) {
      fprintf(stderr, "Cannot find next line of data in %s\n", filename);
      exit(1);
    }
    char *linePtr = line;
    for (uint64_t r = 0; r < rank; r++) {
      uint64_t idx = strtoul(linePtr, &linePtr, 10);
      // Add 0-based index.
      indices[perm[r]] = idx - 1;
    }
    // The external formats always store the numerical values with the type
    // double, but we cast these values to the sparse tensor object type.
    double value = strtod(linePtr, &linePtr);
    tensor->add(indices, value);
    // We currently chose to deal with symmetric matrices by fully constructing
    // them. In the future, we may want to make symmetry implicit for storage
    // reasons.
    if (isSymmetric && indices[0] != indices[1])
      tensor->add({indices[1], indices[0]}, value);
  }
  // Close the file and return tensor.
  fclose(file);
  return tensor;
}

} // namespace

extern "C" {

//===----------------------------------------------------------------------===//
//
// Public API with methods that operate on MLIR buffers (memrefs) to interact
// with sparse tensors, which are only visible as opaque pointers externally.
// These methods should be used exclusively by MLIR compiler-generated code.
//
// Some macro magic is used to generate implementations for all required type
// combinations that can be called from MLIR compiler-generated code.
//
//===----------------------------------------------------------------------===//

#define CASE(p, i, v, P, I, V)                                                 \
  if (ptrTp == (p) && indTp == (i) && valTp == (v)) {                          \
    SparseTensorCOO<V> *tensor = nullptr;                                      \
    if (action <= Action::kFromCOO) {                                          \
      if (action == Action::kFromFile) {                                       \
        char *filename = static_cast<char *>(ptr);                             \
        tensor = openSparseTensorCOO<V>(filename, rank, sizes, perm);          \
      } else if (action == Action::kFromCOO) {                                 \
        tensor = static_cast<SparseTensorCOO<V> *>(ptr);                       \
      } else {                                                                 \
        assert(action == Action::kEmpty);                                      \
      }                                                                        \
      return SparseTensorStorage<P, I, V>::newSparseTensor(rank, sizes, perm,  \
                                                           sparsity, tensor);  \
    }                                                                          \
    if (action == Action::kEmptyCOO)                                           \
      return SparseTensorCOO<V>::newSparseTensorCOO(rank, sizes, perm);        \
    tensor = static_cast<SparseTensorStorage<P, I, V> *>(ptr)->toCOO(perm);    \
    if (action == Action::kToIterator) {                                       \
      tensor->startIterator();                                                 \
    } else {                                                                   \
      assert(action == Action::kToCOO);                                        \
    }                                                                          \
    return tensor;                                                             \
  }

#define CASE_SECSAME(p, v, P, V) CASE(p, p, v, P, P, V)

#define IMPL_SPARSEVALUES(NAME, TYPE, LIB)                                     \
  void _mlir_ciface_##NAME(StridedMemRefType<TYPE, 1> *ref, void *tensor) {    \
    assert(ref &&tensor);                                                      \
    std::vector<TYPE> *v;                                                      \
    static_cast<SparseTensorStorageBase *>(tensor)->LIB(&v);                   \
    ref->basePtr = ref->data = v->data();                                      \
    ref->offset = 0;                                                           \
    ref->sizes[0] = v->size();                                                 \
    ref->strides[0] = 1;                                                       \
  }

#define IMPL_GETOVERHEAD(NAME, TYPE, LIB)                                      \
  void _mlir_ciface_##NAME(StridedMemRefType<TYPE, 1> *ref, void *tensor,      \
                           index_t d) {                                        \
    assert(ref &&tensor);                                                      \
    std::vector<TYPE> *v;                                                      \
    static_cast<SparseTensorStorageBase *>(tensor)->LIB(&v, d);                \
    ref->basePtr = ref->data = v->data();                                      \
    ref->offset = 0;                                                           \
    ref->sizes[0] = v->size();                                                 \
    ref->strides[0] = 1;                                                       \
  }

#define IMPL_ADDELT(NAME, TYPE)                                                \
  void *_mlir_ciface_##NAME(void *tensor, TYPE value,                          \
                            StridedMemRefType<index_t, 1> *iref,               \
                            StridedMemRefType<index_t, 1> *pref) {             \
    assert(tensor &&iref &&pref);                                              \
    assert(iref->strides[0] == 1 && pref->strides[0] == 1);                    \
    assert(iref->sizes[0] == pref->sizes[0]);                                  \
    const index_t *indx = iref->data + iref->offset;                           \
    const index_t *perm = pref->data + pref->offset;                           \
    uint64_t isize = iref->sizes[0];                                           \
    std::vector<index_t> indices(isize);                                       \
    for (uint64_t r = 0; r < isize; r++)                                       \
      indices[perm[r]] = indx[r];                                              \
    static_cast<SparseTensorCOO<TYPE> *>(tensor)->add(indices, value);         \
    return tensor;                                                             \
  }

#define IMPL_GETNEXT(NAME, V)                                                  \
  bool _mlir_ciface_##NAME(void *tensor, StridedMemRefType<index_t, 1> *iref,  \
                           StridedMemRefType<V, 0> *vref) {                    \
    assert(tensor &&iref &&vref);                                              \
    assert(iref->strides[0] == 1);                                             \
    index_t *indx = iref->data + iref->offset;                                 \
    V *value = vref->data + vref->offset;                                      \
    const uint64_t isize = iref->sizes[0];                                     \
    auto iter = static_cast<SparseTensorCOO<V> *>(tensor);                     \
    const Element<V> *elem = iter->getNext();                                  \
    if (elem == nullptr) {                                                     \
      delete iter;                                                             \
      return false;                                                            \
    }                                                                          \
    for (uint64_t r = 0; r < isize; r++)                                       \
      indx[r] = elem->indices[r];                                              \
    *value = elem->value;                                                      \
    return true;                                                               \
  }

#define IMPL_LEXINSERT(NAME, V)                                                \
  void _mlir_ciface_##NAME(void *tensor, StridedMemRefType<index_t, 1> *cref,  \
                           V val) {                                            \
    assert(tensor &&cref);                                                     \
    assert(cref->strides[0] == 1);                                             \
    index_t *cursor = cref->data + cref->offset;                               \
    assert(cursor);                                                            \
    static_cast<SparseTensorStorageBase *>(tensor)->lexInsert(cursor, val);    \
  }

#define IMPL_EXPINSERT(NAME, V)                                                \
  void _mlir_ciface_##NAME(                                                    \
      void *tensor, StridedMemRefType<index_t, 1> *cref,                       \
      StridedMemRefType<V, 1> *vref, StridedMemRefType<bool, 1> *fref,         \
      StridedMemRefType<index_t, 1> *aref, index_t count) {                    \
    assert(tensor &&cref &&vref &&fref &&aref);                                \
    assert(cref->strides[0] == 1);                                             \
    assert(vref->strides[0] == 1);                                             \
    assert(fref->strides[0] == 1);                                             \
    assert(aref->strides[0] == 1);                                             \
    assert(vref->sizes[0] == fref->sizes[0]);                                  \
    index_t *cursor = cref->data + cref->offset;                               \
    V *values = vref->data + vref->offset;                                     \
    bool *filled = fref->data + fref->offset;                                  \
    index_t *added = aref->data + aref->offset;                                \
    static_cast<SparseTensorStorageBase *>(tensor)->expInsert(                 \
        cursor, values, filled, added, count);                                 \
  }

// Assume index_t is in fact uint64_t, so that _mlir_ciface_newSparseTensor
// can safely rewrite kIndex to kU64.  We make this assertion to guarantee
// that this file cannot get out of sync with its header.
static_assert(std::is_same<index_t, uint64_t>::value,
              "Expected index_t == uint64_t");

/// Constructs a new sparse tensor. This is the "swiss army knife"
/// method for materializing sparse tensors into the computation.
///
/// Action:
/// kEmpty = returns empty storage to fill later
/// kFromFile = returns storage, where ptr contains filename to read
/// kFromCOO = returns storage, where ptr contains coordinate scheme to assign
/// kEmptyCOO = returns empty coordinate scheme to fill and use with kFromCOO
/// kToCOO = returns coordinate scheme from storage in ptr to use with kFromCOO
/// kToIterator = returns iterator from storage in ptr (call getNext() to use)
void *
_mlir_ciface_newSparseTensor(StridedMemRefType<DimLevelType, 1> *aref, // NOLINT
                             StridedMemRefType<index_t, 1> *sref,
                             StridedMemRefType<index_t, 1> *pref,
                             OverheadType ptrTp, OverheadType indTp,
                             PrimaryType valTp, Action action, void *ptr) {
  assert(aref && sref && pref);
  assert(aref->strides[0] == 1 && sref->strides[0] == 1 &&
         pref->strides[0] == 1);
  assert(aref->sizes[0] == sref->sizes[0] && sref->sizes[0] == pref->sizes[0]);
  const DimLevelType *sparsity = aref->data + aref->offset;
  const index_t *sizes = sref->data + sref->offset;
  const index_t *perm = pref->data + pref->offset;
  uint64_t rank = aref->sizes[0];

  // Rewrite kIndex to kU64, to avoid introducing a bunch of new cases.
  // This is safe because of the static_assert above.
  if (ptrTp == OverheadType::kIndex)
    ptrTp = OverheadType::kU64;
  if (indTp == OverheadType::kIndex)
    indTp = OverheadType::kU64;

  // Double matrices with all combinations of overhead storage.
  CASE(OverheadType::kU64, OverheadType::kU64, PrimaryType::kF64, uint64_t,
       uint64_t, double);
  CASE(OverheadType::kU64, OverheadType::kU32, PrimaryType::kF64, uint64_t,
       uint32_t, double);
  CASE(OverheadType::kU64, OverheadType::kU16, PrimaryType::kF64, uint64_t,
       uint16_t, double);
  CASE(OverheadType::kU64, OverheadType::kU8, PrimaryType::kF64, uint64_t,
       uint8_t, double);
  CASE(OverheadType::kU32, OverheadType::kU64, PrimaryType::kF64, uint32_t,
       uint64_t, double);
  CASE(OverheadType::kU32, OverheadType::kU32, PrimaryType::kF64, uint32_t,
       uint32_t, double);
  CASE(OverheadType::kU32, OverheadType::kU16, PrimaryType::kF64, uint32_t,
       uint16_t, double);
  CASE(OverheadType::kU32, OverheadType::kU8, PrimaryType::kF64, uint32_t,
       uint8_t, double);
  CASE(OverheadType::kU16, OverheadType::kU64, PrimaryType::kF64, uint16_t,
       uint64_t, double);
  CASE(OverheadType::kU16, OverheadType::kU32, PrimaryType::kF64, uint16_t,
       uint32_t, double);
  CASE(OverheadType::kU16, OverheadType::kU16, PrimaryType::kF64, uint16_t,
       uint16_t, double);
  CASE(OverheadType::kU16, OverheadType::kU8, PrimaryType::kF64, uint16_t,
       uint8_t, double);
  CASE(OverheadType::kU8, OverheadType::kU64, PrimaryType::kF64, uint8_t,
       uint64_t, double);
  CASE(OverheadType::kU8, OverheadType::kU32, PrimaryType::kF64, uint8_t,
       uint32_t, double);
  CASE(OverheadType::kU8, OverheadType::kU16, PrimaryType::kF64, uint8_t,
       uint16_t, double);
  CASE(OverheadType::kU8, OverheadType::kU8, PrimaryType::kF64, uint8_t,
       uint8_t, double);

  // Float matrices with all combinations of overhead storage.
  CASE(OverheadType::kU64, OverheadType::kU64, PrimaryType::kF32, uint64_t,
       uint64_t, float);
  CASE(OverheadType::kU64, OverheadType::kU32, PrimaryType::kF32, uint64_t,
       uint32_t, float);
  CASE(OverheadType::kU64, OverheadType::kU16, PrimaryType::kF32, uint64_t,
       uint16_t, float);
  CASE(OverheadType::kU64, OverheadType::kU8, PrimaryType::kF32, uint64_t,
       uint8_t, float);
  CASE(OverheadType::kU32, OverheadType::kU64, PrimaryType::kF32, uint32_t,
       uint64_t, float);
  CASE(OverheadType::kU32, OverheadType::kU32, PrimaryType::kF32, uint32_t,
       uint32_t, float);
  CASE(OverheadType::kU32, OverheadType::kU16, PrimaryType::kF32, uint32_t,
       uint16_t, float);
  CASE(OverheadType::kU32, OverheadType::kU8, PrimaryType::kF32, uint32_t,
       uint8_t, float);
  CASE(OverheadType::kU16, OverheadType::kU64, PrimaryType::kF32, uint16_t,
       uint64_t, float);
  CASE(OverheadType::kU16, OverheadType::kU32, PrimaryType::kF32, uint16_t,
       uint32_t, float);
  CASE(OverheadType::kU16, OverheadType::kU16, PrimaryType::kF32, uint16_t,
       uint16_t, float);
  CASE(OverheadType::kU16, OverheadType::kU8, PrimaryType::kF32, uint16_t,
       uint8_t, float);
  CASE(OverheadType::kU8, OverheadType::kU64, PrimaryType::kF32, uint8_t,
       uint64_t, float);
  CASE(OverheadType::kU8, OverheadType::kU32, PrimaryType::kF32, uint8_t,
       uint32_t, float);
  CASE(OverheadType::kU8, OverheadType::kU16, PrimaryType::kF32, uint8_t,
       uint16_t, float);
  CASE(OverheadType::kU8, OverheadType::kU8, PrimaryType::kF32, uint8_t,
       uint8_t, float);

  // Integral matrices with both overheads of the same type.
  CASE_SECSAME(OverheadType::kU64, PrimaryType::kI64, uint64_t, int64_t);
  CASE_SECSAME(OverheadType::kU64, PrimaryType::kI32, uint64_t, int32_t);
  CASE_SECSAME(OverheadType::kU64, PrimaryType::kI16, uint64_t, int16_t);
  CASE_SECSAME(OverheadType::kU64, PrimaryType::kI8, uint64_t, int8_t);
  CASE_SECSAME(OverheadType::kU32, PrimaryType::kI32, uint32_t, int32_t);
  CASE_SECSAME(OverheadType::kU32, PrimaryType::kI16, uint32_t, int16_t);
  CASE_SECSAME(OverheadType::kU32, PrimaryType::kI8, uint32_t, int8_t);
  CASE_SECSAME(OverheadType::kU16, PrimaryType::kI32, uint16_t, int32_t);
  CASE_SECSAME(OverheadType::kU16, PrimaryType::kI16, uint16_t, int16_t);
  CASE_SECSAME(OverheadType::kU16, PrimaryType::kI8, uint16_t, int8_t);
  CASE_SECSAME(OverheadType::kU8, PrimaryType::kI32, uint8_t, int32_t);
  CASE_SECSAME(OverheadType::kU8, PrimaryType::kI16, uint8_t, int16_t);
  CASE_SECSAME(OverheadType::kU8, PrimaryType::kI8, uint8_t, int8_t);

  // Unsupported case (add above if needed).
  fputs("unsupported combination of types\n", stderr);
  exit(1);
}

/// Methods that provide direct access to pointers.
IMPL_GETOVERHEAD(sparsePointers, index_t, getPointers)
IMPL_GETOVERHEAD(sparsePointers64, uint64_t, getPointers)
IMPL_GETOVERHEAD(sparsePointers32, uint32_t, getPointers)
IMPL_GETOVERHEAD(sparsePointers16, uint16_t, getPointers)
IMPL_GETOVERHEAD(sparsePointers8, uint8_t, getPointers)

/// Methods that provide direct access to indices.
IMPL_GETOVERHEAD(sparseIndices, index_t, getIndices)
IMPL_GETOVERHEAD(sparseIndices64, uint64_t, getIndices)
IMPL_GETOVERHEAD(sparseIndices32, uint32_t, getIndices)
IMPL_GETOVERHEAD(sparseIndices16, uint16_t, getIndices)
IMPL_GETOVERHEAD(sparseIndices8, uint8_t, getIndices)

/// Methods that provide direct access to values.
IMPL_SPARSEVALUES(sparseValuesF64, double, getValues)
IMPL_SPARSEVALUES(sparseValuesF32, float, getValues)
IMPL_SPARSEVALUES(sparseValuesI64, int64_t, getValues)
IMPL_SPARSEVALUES(sparseValuesI32, int32_t, getValues)
IMPL_SPARSEVALUES(sparseValuesI16, int16_t, getValues)
IMPL_SPARSEVALUES(sparseValuesI8, int8_t, getValues)

/// Helper to add value to coordinate scheme, one per value type.
IMPL_ADDELT(addEltF64, double)
IMPL_ADDELT(addEltF32, float)
IMPL_ADDELT(addEltI64, int64_t)
IMPL_ADDELT(addEltI32, int32_t)
IMPL_ADDELT(addEltI16, int16_t)
IMPL_ADDELT(addEltI8, int8_t)

/// Helper to enumerate elements of coordinate scheme, one per value type.
IMPL_GETNEXT(getNextF64, double)
IMPL_GETNEXT(getNextF32, float)
IMPL_GETNEXT(getNextI64, int64_t)
IMPL_GETNEXT(getNextI32, int32_t)
IMPL_GETNEXT(getNextI16, int16_t)
IMPL_GETNEXT(getNextI8, int8_t)

/// Helper to insert elements in lexicographical index order, one per value
/// type.
IMPL_LEXINSERT(lexInsertF64, double)
IMPL_LEXINSERT(lexInsertF32, float)
IMPL_LEXINSERT(lexInsertI64, int64_t)
IMPL_LEXINSERT(lexInsertI32, int32_t)
IMPL_LEXINSERT(lexInsertI16, int16_t)
IMPL_LEXINSERT(lexInsertI8, int8_t)

/// Helper to insert using expansion, one per value type.
IMPL_EXPINSERT(expInsertF64, double)
IMPL_EXPINSERT(expInsertF32, float)
IMPL_EXPINSERT(expInsertI64, int64_t)
IMPL_EXPINSERT(expInsertI32, int32_t)
IMPL_EXPINSERT(expInsertI16, int16_t)
IMPL_EXPINSERT(expInsertI8, int8_t)

#undef CASE
#undef IMPL_SPARSEVALUES
#undef IMPL_GETOVERHEAD
#undef IMPL_ADDELT
#undef IMPL_GETNEXT
#undef IMPL_LEXINSERT
#undef IMPL_EXPINSERT

//===----------------------------------------------------------------------===//
//
// Public API with methods that accept C-style data structures to interact
// with sparse tensors, which are only visible as opaque pointers externally.
// These methods can be used both by MLIR compiler-generated code as well as by
// an external runtime that wants to interact with MLIR compiler-generated code.
//
//===----------------------------------------------------------------------===//

//// --> MODIFIED
uint64_t get_rank(void *tensor) {
  return static_cast<SparseTensorStorageBase *>(tensor)->getRank();
}
void *get_rev_ptr(void *tensor) {
  return static_cast<SparseTensorStorageBase *>(tensor)->get_rev_ptr();
}
void *get_sizes_ptr(void *tensor) {
  return static_cast<SparseTensorStorageBase *>(tensor)->get_sizes_ptr();
}
void *get_pointers_ptr(void *tensor) {
  return static_cast<SparseTensorStorageBase *>(tensor)->get_pointers_ptr();
}
void *get_indices_ptr(void *tensor) {
  return static_cast<SparseTensorStorageBase *>(tensor)->get_indices_ptr();
}
void *get_values_ptr(void *tensor) {
  return static_cast<SparseTensorStorageBase *>(tensor)->get_values_ptr();
}
void swap_rev(void *tensor, void *new_rev) {
  static_cast<SparseTensorStorageBase *>(tensor)->swap_rev(new_rev);
}
void swap_sizes(void *tensor, void *new_sizes) {
  static_cast<SparseTensorStorageBase *>(tensor)->swap_sizes(new_sizes);
}
void swap_pointers(void *tensor, void *new_pointers) {
  static_cast<SparseTensorStorageBase *>(tensor)->swap_pointers(new_pointers);
}
void swap_indices(void *tensor, void *new_indices) {
  static_cast<SparseTensorStorageBase *>(tensor)->swap_indices(new_indices);
}
void swap_values(void *tensor, void *new_values) {
  static_cast<SparseTensorStorageBase *>(tensor)->swap_values(new_values);
}
void assign_rev(void *tensor, uint64_t d, uint64_t index) {
  static_cast<SparseTensorStorageBase *>(tensor)->assign_rev(d, index);
}
void resize_pointers(void *tensor, uint64_t d, uint64_t size) {
  static_cast<SparseTensorStorageBase *>(tensor)->resize_pointers(d, size);
}
void resize_index(void *tensor, uint64_t d, uint64_t size) {
  static_cast<SparseTensorStorageBase *>(tensor)->resize_index(d, size);
}
void resize_values(void *tensor, uint64_t size) {
  static_cast<SparseTensorStorageBase *>(tensor)->resize_values(size);
}
void resize_dim(void *tensor, uint64_t d, uint64_t size) {
  static_cast<SparseTensorStorageBase *>(tensor)->resize_dim(d, size);
}
void *dup_tensor(void *tensor) {
  return static_cast<SparseTensorStorageBase *>(tensor)->dup();
}
//void *empty_like(void *tensor) {
//  return static_cast<SparseTensorStorageBase *>(tensor)->empty_like();
//}
//void *empty(void *tensor, uint64_t ndims) {
//  return static_cast<SparseTensorStorageBase *>(tensor)->empty(ndims);
//}
// Combinations of real types to !llvm.ptr<i8>
void *matrix_csr_f64_p64i64_to_ptr8(void *tensor) { return tensor; }
void *matrix_csc_f64_p64i64_to_ptr8(void *tensor) { return tensor; }
void *matrix_csr_f32_p64i64_to_ptr8(void *tensor) { return tensor; }
void *matrix_csc_f32_p64i64_to_ptr8(void *tensor) { return tensor; }
void *matrix_csr_i64_p64i64_to_ptr8(void *tensor) { return tensor; }
void *matrix_csc_i64_p64i64_to_ptr8(void *tensor) { return tensor; }
void *matrix_csr_i32_p64i64_to_ptr8(void *tensor) { return tensor; }
void *matrix_csc_i32_p64i64_to_ptr8(void *tensor) { return tensor; }
void *matrix_csr_i8_p64i64_to_ptr8(void *tensor) { return tensor; }
void *matrix_csc_i8_p64i64_to_ptr8(void *tensor) { return tensor; }
void *vector_f64_p64i64_to_ptr8(void *tensor) { return tensor; }
void *vector_f32_p64i64_to_ptr8(void *tensor) { return tensor; }
void *vector_i64_p64i64_to_ptr8(void *tensor) { return tensor; }
void *vector_i32_p64i64_to_ptr8(void *tensor) { return tensor; }
void *vector_i8_p64i64_to_ptr8(void *tensor) { return tensor; }
// Combinations of !llvm.ptr<i8> to real types
void *ptr8_to_matrix_csr_f64_p64i64(void *tensor) { return tensor; }
void *ptr8_to_matrix_csc_f64_p64i64(void *tensor) { return tensor; }
void *ptr8_to_matrix_csr_f32_p64i64(void *tensor) { return tensor; }
void *ptr8_to_matrix_csc_f32_p64i64(void *tensor) { return tensor; }
void *ptr8_to_matrix_csr_i64_p64i64(void *tensor) { return tensor; }
void *ptr8_to_matrix_csc_i64_p64i64(void *tensor) { return tensor; }
void *ptr8_to_matrix_csr_i32_p64i64(void *tensor) { return tensor; }
void *ptr8_to_matrix_csc_i32_p64i64(void *tensor) { return tensor; }
void *ptr8_to_matrix_csr_i8_p64i64(void *tensor) { return tensor; }
void *ptr8_to_matrix_csc_i8_p64i64(void *tensor) { return tensor; }
void *ptr8_to_vector_f64_p64i64(void *tensor) { return tensor; }
void *ptr8_to_vector_f32_p64i64(void *tensor) { return tensor; }
void *ptr8_to_vector_i64_p64i64(void *tensor) { return tensor; }
void *ptr8_to_vector_i32_p64i64(void *tensor) { return tensor; }
void *ptr8_to_vector_i8_p64i64(void *tensor) { return tensor; }

// Print functions
void print_int_as_char(int64_t character_int) {
  char character = (char)character_int;
  std::cout << character;
  return;
}
void print_index(uint64_t val) {
  std::cout << val;
  return;
}
void print_i1(bool val) {
  std::cout << val;
  return;
}
void print_i8(int8_t val) {
  // must cast since a char is an 8-bit int
  std::cout << (int16_t)val;
  return;
}
void print_i16(int16_t val) {
  std::cout << val;
  return;
}
void print_i32(int32_t val) {
  std::cout << val;
  return;
}
void print_i64(int64_t val) {
  std::cout << val;
  return;
}
void print_f32(float val) {
  std::cout << val;
  return;
}
void print_f64(double val) {
  std::cout << val;
  return;
}

bool verify(void *tensor) {
  return static_cast<SparseTensorStorageBase *>(tensor)->verify();
}
//// <- MODIFIED

/// Helper method to read a sparse tensor filename from the environment,
/// defined with the naming convention ${TENSOR0}, ${TENSOR1}, etc.
char *getTensorFilename(index_t id) {
  char var[80];
  sprintf(var, "TENSOR%" PRIu64, id);
  char *env = getenv(var);
  return env;
}

/// Returns size of sparse tensor in given dimension.
index_t sparseDimSize(void *tensor, index_t d) {
  return static_cast<SparseTensorStorageBase *>(tensor)->getDimSize(d);
}

/// Finalizes lexicographic insertions.
void endInsert(void *tensor) {
  return static_cast<SparseTensorStorageBase *>(tensor)->endInsert();
}

/// Releases sparse tensor storage.
void delSparseTensor(void *tensor) {
  delete static_cast<SparseTensorStorageBase *>(tensor);
}

/// Initializes sparse tensor from a COO-flavored format expressed using C-style
/// data structures. The expected parameters are:
///
///   rank:    rank of tensor
///   nse:     number of specified elements (usually the nonzeros)
///   shape:   array with dimension size for each rank
///   values:  a "nse" array with values for all specified elements
///   indices: a flat "nse x rank" array with indices for all specified elements
///
/// For example, the sparse matrix
///     | 1.0 0.0 0.0 |
///     | 0.0 5.0 3.0 |
/// can be passed as
///      rank    = 2
///      nse     = 3
///      shape   = [2, 3]
///      values  = [1.0, 5.0, 3.0]
///      indices = [ 0, 0,  1, 1,  1, 2]
//
// TODO: for now f64 tensors only, no dim ordering, all dimensions compressed
//
void *convertToMLIRSparseTensor(uint64_t rank, uint64_t nse, uint64_t *shape,
                                double *values, uint64_t *indices) {
  // Setup all-dims compressed and default ordering.
  std::vector<DimLevelType> sparse(rank, DimLevelType::kCompressed);
  std::vector<uint64_t> perm(rank);
  std::iota(perm.begin(), perm.end(), 0);
  // Convert external format to internal COO.
  SparseTensorCOO<double> *tensor = SparseTensorCOO<double>::newSparseTensorCOO(
      rank, shape, perm.data(), nse);
  std::vector<uint64_t> idx(rank);
  for (uint64_t i = 0, base = 0; i < nse; i++) {
    for (uint64_t r = 0; r < rank; r++)
      idx[r] = indices[base + r];
    tensor->add(idx, values[i]);
    base += rank;
  }
  // Return sparse tensor storage format as opaque pointer.
  return SparseTensorStorage<uint64_t, uint64_t, double>::newSparseTensor(
      rank, shape, perm.data(), sparse.data(), tensor);
}

/// Converts a sparse tensor to COO-flavored format expressed using C-style
/// data structures. The expected output parameters are pointers for these
/// values:
///
///   rank:    rank of tensor
///   nse:     number of specified elements (usually the nonzeros)
///   shape:   array with dimension size for each rank
///   values:  a "nse" array with values for all specified elements
///   indices: a flat "nse x rank" array with indices for all specified elements
///
/// The input is a pointer to SparseTensorStorage<P, I, V>, typically returned
/// from convertToMLIRSparseTensor.
///
//  TODO: Currently, values are copied from SparseTensorStorage to
//  SparseTensorCOO, then to the output. We may want to reduce the number of
//  copies.
//
//  TODO: for now f64 tensors only, no dim ordering, all dimensions compressed
//
void convertFromMLIRSparseTensor(void *tensor, uint64_t *pRank, uint64_t *pNse,
                                 uint64_t **pShape, double **pValues,
                                 uint64_t **pIndices) {
  SparseTensorStorage<uint64_t, uint64_t, double> *sparseTensor =
      static_cast<SparseTensorStorage<uint64_t, uint64_t, double> *>(tensor);
  uint64_t rank = sparseTensor->getRank();
  std::vector<uint64_t> perm(rank);
  std::iota(perm.begin(), perm.end(), 0);
  SparseTensorCOO<double> *coo = sparseTensor->toCOO(perm.data());

  const std::vector<Element<double>> &elements = coo->getElements();
  uint64_t nse = elements.size();

  uint64_t *shape = new uint64_t[rank];
  for (uint64_t i = 0; i < rank; i++)
    shape[i] = coo->getSizes()[i];

  double *values = new double[nse];
  uint64_t *indices = new uint64_t[rank * nse];

  for (uint64_t i = 0, base = 0; i < nse; i++) {
    values[i] = elements[i].value;
    for (uint64_t j = 0; j < rank; j++)
      indices[base + j] = elements[i].indices[j];
    base += rank;
  }

  delete coo;
  *pRank = rank;
  *pNse = nse;
  *pShape = shape;
  *pValues = values;
  *pIndices = indices;
}

//// -> MODIFIED
extern "C" MLIR_CRUNNERUTILS_EXPORT void
memrefCopy(int64_t elemSize, UnrankedMemRefType<char> *srcArg,
           UnrankedMemRefType<char> *dstArg) {
  DynamicMemRefType<char> src(*srcArg);
  DynamicMemRefType<char> dst(*dstArg);

  int64_t rank = src.rank;
  // Handle empty shapes -> nothing to copy.
  for (int rankp = 0; rankp < rank; ++rankp)
    if (src.sizes[rankp] == 0)
      return;

  char *srcPtr = src.data + src.offset * elemSize;
  char *dstPtr = dst.data + dst.offset * elemSize;

  if (rank == 0) {
    memcpy(dstPtr, srcPtr, elemSize);
    return;
  }

  int64_t *indices = static_cast<int64_t *>(alloca(sizeof(int64_t) * rank));
  int64_t *srcStrides = static_cast<int64_t *>(alloca(sizeof(int64_t) * rank));
  int64_t *dstStrides = static_cast<int64_t *>(alloca(sizeof(int64_t) * rank));

  // Initialize index and scale strides.
  for (int rankp = 0; rankp < rank; ++rankp) {
    indices[rankp] = 0;
    srcStrides[rankp] = src.strides[rankp] * elemSize;
    dstStrides[rankp] = dst.strides[rankp] * elemSize;
  }

  int64_t readIndex = 0, writeIndex = 0;
  for (;;) {
    // Copy over the element, byte by byte.
    memcpy(dstPtr + writeIndex, srcPtr + readIndex, elemSize);
    // Advance index and read position.
    for (int64_t axis = rank - 1; axis >= 0; --axis) {
      // Advance at current axis.
      auto newIndex = ++indices[axis];
      readIndex += srcStrides[axis];
      writeIndex += dstStrides[axis];
      // If this is a valid index, we have our next index, so continue copying.
      if (src.sizes[axis] != newIndex)
        break;
      // We reached the end of this axis. If this is axis 0, we are done.
      if (axis == 0)
        return;
      // Else, reset to 0 and undo the advancement of the linear index that
      // this axis had. Then continue with the axis one outer.
      indices[axis] = 0;
      readIndex -= src.sizes[axis] * srcStrides[axis];
      writeIndex -= dst.sizes[axis] * dstStrides[axis];
    }
  }
}

void print_tensor_dense(void *tensor) {
  static_cast<SparseTensorStorageBase *>(tensor)->print_dense();
}

void print_tensor(void *tensor, int64_t level) {
  if (level <= 0)
    static_cast<SparseTensorStorageBase *>(tensor)->print_dense();
  else
    static_cast<SparseTensorStorageBase *>(tensor)->print_components(level);
}
//// <- MODIFIED

} // extern "C"

#endif // MLIR_CRUNNERUTILS_DEFINE_FUNCTIONS
