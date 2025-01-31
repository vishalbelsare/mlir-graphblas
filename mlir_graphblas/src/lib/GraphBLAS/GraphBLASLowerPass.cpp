//===- GraphBLASPasses.cpp - GraphBLAS dialect passes ---------*- C++ -*-===//
//
// TODO add documentation
//
//===--------------------------------------------------------------------===//
#include "mlir/Dialect/Bufferization/IR/Bufferization.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/Linalg/Transforms/Transforms.h"
#include "mlir/Dialect/SparseTensor/IR/SparseTensor.h"
#include "mlir/IR/Region.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/raw_ostream.h"

#include "GraphBLAS/GraphBLASArrayUtils.h"
#include "GraphBLAS/GraphBLASDialect.h"
#include "GraphBLAS/GraphBLASPasses.h"
#include "GraphBLAS/GraphBLASUtils.h"

using namespace ::mlir;
using namespace std::placeholders;

namespace {

//===----------------------------------------------------------------------===//
// Passes declaration.
//===----------------------------------------------------------------------===//

#define GEN_PASS_CLASSES
#include "GraphBLAS/GraphBLASPasses.h.inc"

//===----------------------------------------------------------------------===//
// Passes implementation.
//===----------------------------------------------------------------------===//

class LowerSizeRewrite : public OpRewritePattern<graphblas::SizeOp> {
public:
  using OpRewritePattern<graphblas::SizeOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::SizeOp op,
                                PatternRewriter &rewriter) const override {
    Location loc = op->getLoc();

    Value c0 = rewriter.create<arith::ConstantIndexOp>(loc, 0);
    Value inputTensor = op.input();
    Value size = rewriter.create<tensor::DimOp>(loc, inputTensor, c0);

    rewriter.replaceOp(op, size);
    return success();
  };
};

class LowerNumRowsRewrite : public OpRewritePattern<graphblas::NumRowsOp> {
public:
  using OpRewritePattern<graphblas::NumRowsOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::NumRowsOp op,
                                PatternRewriter &rewriter) const override {
    Location loc = op->getLoc();

    Value c0 = rewriter.create<arith::ConstantIndexOp>(loc, 0);
    Value inputTensor = op.input();
    Value nrows = rewriter.create<tensor::DimOp>(loc, inputTensor, c0);

    rewriter.replaceOp(op, nrows);
    return success();
  };
};

class LowerNumColsRewrite : public OpRewritePattern<graphblas::NumColsOp> {
public:
  using OpRewritePattern<graphblas::NumColsOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::NumColsOp op,
                                PatternRewriter &rewriter) const override {
    Location loc = op->getLoc();

    Value c1 = rewriter.create<arith::ConstantIndexOp>(loc, 1);
    Value inputTensor = op.input();
    Value ncols = rewriter.create<tensor::DimOp>(loc, inputTensor, c1);

    rewriter.replaceOp(op, ncols);
    return success();
  };
};

class LowerNumValsRewrite : public OpRewritePattern<graphblas::NumValsOp> {
public:
  using OpRewritePattern<graphblas::NumValsOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::NumValsOp op,
                                PatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    Value inputTensor = op.input();
    Type inputType = inputTensor.getType();

    sparse_tensor::SparseTensorEncodingAttr sparseEncoding =
        sparse_tensor::getSparseTensorEncoding(inputType);
    unsigned pointerBitWidth = sparseEncoding.getPointerBitWidth();
    Type pointerType = rewriter.getIntegerType(pointerBitWidth);
    Type indexType = rewriter.getIndexType();

    // Access the pointers
    Type memref1DPointerType = MemRefType::get({-1}, pointerType);
    unsigned rank = inputType.dyn_cast<RankedTensorType>().getRank();
    Value c_rank_minus_1 =
        rewriter.create<arith::ConstantIndexOp>(loc, rank - 1);
    Value ptrs = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memref1DPointerType, inputTensor, c_rank_minus_1);

    // Find length of pointer array
    Value npointers;
    if (rank == 1) {
      npointers = rewriter.create<arith::ConstantIndexOp>(loc, 1);
    } else {
      Value dimForPointers;
      if (hasRowOrdering(inputType)) {
        dimForPointers = rewriter.create<arith::ConstantIndexOp>(loc, 0);
      } else {
        dimForPointers = rewriter.create<arith::ConstantIndexOp>(loc, 1);
      }
      npointers =
          rewriter.create<tensor::DimOp>(loc, inputTensor, dimForPointers);
    }

    // The last value from the pointers is the number of nonzero values
    Value nnz_ptype = rewriter.create<memref::LoadOp>(loc, ptrs, npointers);
    Value nnz = rewriter.create<arith::IndexCastOp>(loc, nnz_ptype, indexType);

    rewriter.replaceOp(op, nnz);
    return success();
  };
};

class LowerDupRewrite : public OpRewritePattern<graphblas::DupOp> {
public:
  using OpRewritePattern<graphblas::DupOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::DupOp op,
                                PatternRewriter &rewriter) const override {
    ModuleOp module = op->getParentOfType<ModuleOp>();
    Location loc = op->getLoc();
    Value inputTensor = op.input();

    Value duplicate = callDupTensor(rewriter, module, loc, inputTensor);
    rewriter.replaceOp(op, duplicate);

    return success();
  };
};

class LowerConvertLayoutRewrite
    : public OpRewritePattern<graphblas::ConvertLayoutOp> {
public:
  using OpRewritePattern<graphblas::ConvertLayoutOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::ConvertLayoutOp op,
                                PatternRewriter &rewriter) const override {
    MLIRContext *context = op.getContext();
    ModuleOp module = op->getParentOfType<ModuleOp>();
    Location loc = op->getLoc();

    Value inputTensor = op.input();
    Type inputType = inputTensor.getType();
    Type outputType = op->getResultTypes().front();

    // Shortcut operation if no change
    if (inputType == outputType) {
      rewriter.replaceOp(op, inputTensor);
      return success();
    }

    // otherwise, the rest of this function changes the data layout
    RankedTensorType inputTensorType = inputType.dyn_cast<RankedTensorType>();
    sparse_tensor::SparseTensorEncodingAttr sparseEncoding =
        sparse_tensor::getSparseTensorEncoding(inputTensorType);
    unsigned ptrBitWidth = sparseEncoding.getPointerBitWidth();
    unsigned idxBitWidth = sparseEncoding.getIndexBitWidth();
    Type valueType = inputTensorType.getElementType();
    Type int64Type = rewriter.getIntegerType(64);
    Type indexType = rewriter.getIndexType();

    // Initial constants
    Value c0 = rewriter.create<arith::ConstantIndexOp>(loc, 0);
    Value c1 = rewriter.create<arith::ConstantIndexOp>(loc, 1);
    Value c0_64 = rewriter.create<arith::ConstantIntOp>(loc, 0, int64Type);
    Value c1_64 = rewriter.create<arith::ConstantIntOp>(loc, 1, int64Type);

    // Get sparse tensor info
    Type memref1DI64Type = MemRefType::get({-1}, int64Type);
    Type memref1DValueType = MemRefType::get({-1}, valueType);

    Value inputPtrs = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memref1DI64Type, inputTensor, c1);
    Value inputIndices = rewriter.create<sparse_tensor::ToIndicesOp>(
        loc, memref1DI64Type, inputTensor, c1);
    Value inputValues = rewriter.create<sparse_tensor::ToValuesOp>(
        loc, memref1DValueType, inputTensor);
    Value nrow = rewriter.create<graphblas::NumRowsOp>(loc, inputTensor);
    Value ncol = rewriter.create<graphblas::NumColsOp>(loc, inputTensor);
    Value nnz = rewriter.create<graphblas::NumValsOp>(loc, inputTensor);

    Value duplicate = callEmptyLike(rewriter, module, loc, inputTensor);

    // Beyond this point, the algorithm assumes csr->csc,
    // so swap nrow/ncol for csc->csr
    bool outputIsCSC = hasColumnOrdering(outputType);

    // update the reverse index map and dimensions for CSR or CSC
    if (outputIsCSC) {
      callAssignRev(rewriter, module, loc, duplicate, c0, c1);
      callAssignRev(rewriter, module, loc, duplicate, c1, c0);

      callResizeDim(rewriter, module, loc, duplicate, c0, ncol);
      callResizeDim(rewriter, module, loc, duplicate, c1, nrow);
    } else {
      callAssignRev(rewriter, module, loc, duplicate, c0, c0);
      callAssignRev(rewriter, module, loc, duplicate, c1, c1);

      callResizeDim(rewriter, module, loc, duplicate, c0, nrow);
      callResizeDim(rewriter, module, loc, duplicate, c1, ncol);

      Value tmp = nrow;
      nrow = ncol;
      ncol = tmp;
    }

    Value ncols_plus_one = rewriter.create<arith::AddIOp>(loc, ncol, c1);
    callResizePointers(rewriter, module, loc, duplicate, c1, ncols_plus_one);
    callResizeIndex(rewriter, module, loc, duplicate, c1, nnz);
    callResizeValues(rewriter, module, loc, duplicate, nnz);

    // the verify function will ensure that this is CSR->CSC or CSC->CSR
    Value output = castToPtr8(rewriter, module, loc, duplicate);
    RankedTensorType flippedType = getSingleCompressedMatrixType(
        context, inputTensorType.getShape(), outputIsCSC, valueType,
        ptrBitWidth, idxBitWidth);
    output = castToTensor(rewriter, module, loc, output, flippedType);

    Value outputPtrs = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memref1DI64Type, output, c1);
    Value outputIndices = rewriter.create<sparse_tensor::ToIndicesOp>(
        loc, memref1DI64Type, output, c1);
    Value outputValues = rewriter.create<sparse_tensor::ToValuesOp>(
        loc, memref1DValueType, output);

    // compute number of non-zero entries per column of A

    // init B.pointers to zero
    scf::ForOp initLoop = rewriter.create<scf::ForOp>(loc, c0, ncol, c1);
    Value initLoopIdx = initLoop.getInductionVar();
    rewriter.setInsertionPointToStart(initLoop.getBody());
    rewriter.create<memref::StoreOp>(loc, c0_64, outputPtrs, initLoopIdx);
    rewriter.setInsertionPointAfter(initLoop);

    // store pointers
    scf::ForOp ptrLoop = rewriter.create<scf::ForOp>(loc, c0, nnz, c1);
    Value ptrLoopIdx = ptrLoop.getInductionVar();

    rewriter.setInsertionPointToStart(ptrLoop.getBody());
    Value colA64 =
        rewriter.create<memref::LoadOp>(loc, inputIndices, ptrLoopIdx);
    Value colA = rewriter.create<arith::IndexCastOp>(loc, colA64, indexType);
    Value colB = rewriter.create<memref::LoadOp>(loc, outputPtrs, colA);
    Value colB1 = rewriter.create<arith::AddIOp>(loc, colB, c1_64);
    rewriter.create<memref::StoreOp>(loc, colB1, outputPtrs, colA);

    rewriter.setInsertionPointAfter(ptrLoop);

    // cumsum the nnz per column to get Bp
    rewriter.create<memref::StoreOp>(loc, c0_64, outputPtrs, ncol);

    scf::ForOp colAccLoop = rewriter.create<scf::ForOp>(loc, c0, ncol, c1);
    Value colAccLoopIdx = colAccLoop.getInductionVar();

    rewriter.setInsertionPointToStart(colAccLoop.getBody());
    Value temp =
        rewriter.create<memref::LoadOp>(loc, outputPtrs, colAccLoopIdx);
    Value cumsum = rewriter.create<memref::LoadOp>(loc, outputPtrs, ncol);
    rewriter.create<memref::StoreOp>(loc, cumsum, outputPtrs, colAccLoopIdx);
    Value cumsum2 = rewriter.create<arith::AddIOp>(loc, cumsum, temp);
    rewriter.create<memref::StoreOp>(loc, cumsum2, outputPtrs, ncol);

    rewriter.setInsertionPointAfter(colAccLoop);

    // copy values
    scf::ForOp outerLoop = rewriter.create<scf::ForOp>(loc, c0, nrow, c1);
    Value rowIdx = outerLoop.getInductionVar();

    rewriter.setInsertionPointToStart(outerLoop.getBody());
    Value row_64 = rewriter.create<arith::IndexCastOp>(loc, rowIdx, int64Type);
    Value j_start_64 = rewriter.create<memref::LoadOp>(loc, inputPtrs, rowIdx);
    Value j_start =
        rewriter.create<arith::IndexCastOp>(loc, j_start_64, indexType);
    Value row_plus1 = rewriter.create<arith::AddIOp>(loc, rowIdx, c1);
    Value j_end_64 = rewriter.create<memref::LoadOp>(loc, inputPtrs, row_plus1);
    Value j_end = rewriter.create<arith::IndexCastOp>(loc, j_end_64, indexType);

    scf::ForOp innerLoop = rewriter.create<scf::ForOp>(loc, j_start, j_end, c1);
    Value jj = innerLoop.getInductionVar();

    rewriter.setInsertionPointToStart(innerLoop.getBody());

    Value col_64 = rewriter.create<memref::LoadOp>(loc, inputIndices, jj);
    Value col = rewriter.create<arith::IndexCastOp>(loc, col_64, indexType);
    Value dest_64 = rewriter.create<memref::LoadOp>(loc, outputPtrs, col);
    Value dest = rewriter.create<arith::IndexCastOp>(loc, dest_64, indexType);
    rewriter.create<memref::StoreOp>(loc, row_64, outputIndices, dest);
    Value axjj = rewriter.create<memref::LoadOp>(loc, inputValues, jj);
    rewriter.create<memref::StoreOp>(loc, axjj, outputValues, dest);

    // Bp[col]++
    Value bp_inc = rewriter.create<memref::LoadOp>(loc, outputPtrs, col);
    Value bp_inc1 = rewriter.create<arith::AddIOp>(loc, bp_inc, c1_64);
    rewriter.create<memref::StoreOp>(loc, bp_inc1, outputPtrs, col);

    rewriter.setInsertionPointAfter(outerLoop);

    Value last_last = rewriter.create<memref::LoadOp>(loc, outputPtrs, ncol);
    rewriter.create<memref::StoreOp>(loc, c0_64, outputPtrs, ncol);

    scf::ForOp finalLoop = rewriter.create<scf::ForOp>(loc, c0, ncol, c1);
    Value iCol = finalLoop.getInductionVar();

    rewriter.setInsertionPointToStart(finalLoop.getBody());

    Value swapTemp = rewriter.create<memref::LoadOp>(loc, outputPtrs, iCol);
    Value last = rewriter.create<memref::LoadOp>(loc, outputPtrs, ncol);
    rewriter.create<memref::StoreOp>(loc, last, outputPtrs, iCol);
    rewriter.create<memref::StoreOp>(loc, swapTemp, outputPtrs, ncol);

    rewriter.setInsertionPointAfter(finalLoop);

    rewriter.create<memref::StoreOp>(loc, last_last, outputPtrs, ncol);

    rewriter.replaceOp(op, output);

    cleanupIntermediateTensor(rewriter, module, loc, output);

    return success();
  };
};

class LowerCastRewrite : public OpRewritePattern<graphblas::CastOp> {
public:
  using OpRewritePattern<graphblas::CastOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::CastOp op,
                                PatternRewriter &rewriter) const override {
    // MLIRContext *context = op.getContext();
    ModuleOp module = op->getParentOfType<ModuleOp>();
    Location loc = op->getLoc();

    Value input = op.input();
    Type inputType = input.getType();
    Type outputType = op->getResultTypes().front();

    // Shortcut operation if no change
    if (inputType == outputType) {
      rewriter.replaceOp(op, input);
      return success();
    }

    RankedTensorType inputTensorType = inputType.cast<RankedTensorType>();
    // sparse_tensor::SparseTensorEncodingAttr inputSparseEncoding =
    //    sparse_tensor::getSparseTensorEncoding(inputTensorType);
    // unsigned inputPtrBitWidth = inputSparseEncoding.getPointerBitWidth();
    // unsigned inputIdxBitWidth = inputSparseEncoding.getIndexBitWidth();
    Type inputValueType = inputTensorType.getElementType();

    RankedTensorType outputTensorType = outputType.cast<RankedTensorType>();
    // sparse_tensor::SparseTensorEncodingAttr outputSparseEncoding =
    //    sparse_tensor::getSparseTensorEncoding(outputTensorType);
    // unsigned outputPtrBitWidth = outputSparseEncoding.getPointerBitWidth();
    // unsigned outputIdxBitWidth = outputSparseEncoding.getIndexBitWidth();
    Type outputValueType = outputTensorType.getElementType();

    unsigned rank = inputTensorType.getRank();
    Type memref1DIValueType = MemRefType::get({-1}, inputValueType);
    Type memref1DOValueType = MemRefType::get({-1}, outputValueType);

    // Initial constants
    Value c0 = rewriter.create<arith::ConstantIndexOp>(loc, 0);
    Value c1 = rewriter.create<arith::ConstantIndexOp>(loc, 1);

    // Get the shape as a ValueRange
    ValueRange shape;
    if (rank == 1) {
      Value size = rewriter.create<graphblas::SizeOp>(loc, input);
      shape = ValueRange{size};
    } else {
      Value nrows = rewriter.create<graphblas::NumRowsOp>(loc, input);
      Value ncols = rewriter.create<graphblas::NumColsOp>(loc, input);
      shape = ValueRange{nrows, ncols};
    }

    // Create a new tensor with the correct output value type
    Value output =
        rewriter.create<sparse_tensor::InitOp>(loc, outputType, shape);

    // Make a copy of the input so we can swap the pointers and indices
    Value duplicate = callDupTensor(rewriter, module, loc, input);
    callSwapPointers(rewriter, module, loc, duplicate, output);
    callSwapIndices(rewriter, module, loc, duplicate, output);
    rewriter.create<sparse_tensor::ReleaseOp>(loc, duplicate);

    // Cast values to new dtype
    Value nnz = rewriter.create<graphblas::NumValsOp>(loc, input);
    callResizeValues(rewriter, module, loc, output, nnz);
    Value inputValues = rewriter.create<sparse_tensor::ToValuesOp>(
        loc, memref1DIValueType, input);
    Value outputValues = rewriter.create<sparse_tensor::ToValuesOp>(
        loc, memref1DOValueType, output);
    scf::ParallelOp loop = rewriter.create<scf::ParallelOp>(loc, c0, nnz, c1);
    Value loopIdx = loop.getInductionVars().front();
    {
      rewriter.setInsertionPointToStart(loop.getBody());
      Value val = rewriter.create<memref::LoadOp>(loc, inputValues, loopIdx);
      Value newVal;
      if (auto itype = inputValueType.dyn_cast<IntegerType>()) {
        newVal = llvm::TypeSwitch<Type, Value>(outputValueType)
                     .Case<IntegerType>([&](IntegerType otype) {
                       // int -> int
                       unsigned iBitWidth = itype.getWidth();
                       unsigned oBitWidth = otype.getWidth();
                       if (iBitWidth < oBitWidth)
                         return rewriter
                             .create<arith::ExtSIOp>(loc, outputValueType, val)
                             .getResult();
                       else if (iBitWidth > oBitWidth)
                         return rewriter
                             .create<arith::TruncIOp>(loc, outputValueType, val)
                             .getResult();
                       else
                         return val;
                     })
                     .Case<FloatType>([&](FloatType otype) {
                       // int -> float
                       return rewriter.create<arith::SIToFPOp>(
                           loc, outputValueType, val);
                     });
      } else {
        newVal = llvm::TypeSwitch<Type, Value>(outputValueType)
                     .Case<IntegerType>([&](IntegerType otype) {
                       // float -> int
                       return rewriter.create<arith::FPToSIOp>(
                           loc, outputValueType, val);
                     })
                     .Case<FloatType>([&](FloatType otype) {
                       // float -> float
                       unsigned iBitWidth =
                           inputValueType.dyn_cast<FloatType>().getWidth();
                       unsigned oBitWidth = otype.getWidth();
                       if (iBitWidth < oBitWidth)
                         return rewriter
                             .create<arith::ExtFOp>(loc, outputValueType, val)
                             .getResult();
                       else if (iBitWidth > oBitWidth)
                         return rewriter
                             .create<arith::TruncFOp>(loc, outputValueType, val)
                             .getResult();
                       else
                         return val;
                     });
      }
      rewriter.create<memref::StoreOp>(loc, newVal, outputValues, loopIdx);
      rewriter.setInsertionPointAfter(loop);
    }

    rewriter.replaceOp(op, output);

    cleanupIntermediateTensor(rewriter, module, loc, output);

    return success();
  };
};

class TransposeDWIMRewrite : public OpRewritePattern<graphblas::TransposeOp> {
public:
  using OpRewritePattern<graphblas::TransposeOp>::OpRewritePattern;

  static bool needsDWIM(graphblas::TransposeOp op) {

    Value inputTensor = op.input();
    RankedTensorType inputType =
        inputTensor.getType().dyn_cast<RankedTensorType>();
    RankedTensorType outputType =
        op->getResultTypes().front().dyn_cast<RankedTensorType>();

    bool inputTypeIsCSR = hasRowOrdering(inputType);
    bool outputTypeIsCSR = hasRowOrdering(outputType);

    return (inputTypeIsCSR == outputTypeIsCSR);
  };

  LogicalResult match(graphblas::TransposeOp op) const override {
    if (needsDWIM(op))
      return success();
    else
      return failure();
  };

  void rewrite(graphblas::TransposeOp op,
               PatternRewriter &rewriter) const override {
    MLIRContext *context = op.getContext();
    Location loc = op->getLoc();

    Value inputTensor = op.input();
    RankedTensorType outputType =
        op->getResultTypes().front().dyn_cast<RankedTensorType>();

    RankedTensorType flippedInputType =
        getFlippedLayoutType(context, inputTensor.getType());

    Value flippedInput = rewriter.create<graphblas::ConvertLayoutOp>(
        loc, flippedInputType, inputTensor);
    Value transposed =
        rewriter.create<graphblas::TransposeOp>(loc, outputType, flippedInput);

    rewriter.replaceOp(op, transposed);
  };
};

class LowerTransposeRewrite : public OpRewritePattern<graphblas::TransposeOp> {
public:
  using OpRewritePattern<graphblas::TransposeOp>::OpRewritePattern;

  LogicalResult match(graphblas::TransposeOp op) const override {
    if (TransposeDWIMRewrite::needsDWIM(op))
      return failure();
    else
      return success();
  };

  void rewrite(graphblas::TransposeOp op,
               PatternRewriter &rewriter) const override {

    ModuleOp module = op->getParentOfType<ModuleOp>();
    Location loc = op->getLoc();

    Value inputTensor = op.input();
    RankedTensorType inputType =
        inputTensor.getType().dyn_cast<RankedTensorType>();
    bool inputTypeIsCSR = hasRowOrdering(inputType);

    RankedTensorType flippedInputType =
        op.getResult().getType().cast<RankedTensorType>();

    // Cast types
    Value output = callDupTensor(rewriter, module, loc, inputTensor);
    Value c0 = rewriter.create<arith::ConstantIndexOp>(loc, 0);
    Value c1 = rewriter.create<arith::ConstantIndexOp>(loc, 1);
    if (inputTypeIsCSR) {
      callAssignRev(rewriter, module, loc, output, c0, c1);
      callAssignRev(rewriter, module, loc, output, c1, c0);
    } else {
      callAssignRev(rewriter, module, loc, output, c0, c0);
      callAssignRev(rewriter, module, loc, output, c1, c1);
    }
    output = castToPtr8(rewriter, module, loc, output);
    output = castToTensor(rewriter, module, loc, output, flippedInputType);

    // TODO we get an error when we have hard-coded/known sizes at compile time.

    rewriter.replaceOp(op, output);

    cleanupIntermediateTensor(rewriter, module, loc, output);
  };
};

class LowerSelectRewrite : public OpRewritePattern<graphblas::SelectOp> {
public:
  using OpRewritePattern<graphblas::SelectOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::SelectOp op,
                                PatternRewriter &rewriter) const override {

    std::string selector = op.selector().str();
    OperandRange thunks = op.thunks();

    if (selector == "probability") {
      Value thunk = thunks[0];
      Value rngContext = thunks[1];
      auto probBlock = std::bind(probabilityBlock, _1, _2, _3, _4, _5, _6, _7,
                                 thunk, rngContext);
      return buildAlgorithm<graphblas::SelectOp>(op, rewriter, probBlock);
    } else {
      Location loc = op->getLoc();

      Value input = op.input();
      RankedTensorType inputType = input.getType().cast<RankedTensorType>();
      Type valueType = inputType.getElementType();

      // Replace with SelectGenericOp
      graphblas::SelectGenericOp newSelectOp =
          rewriter.create<graphblas::SelectGenericOp>(loc, op->getResultTypes(),
                                                      input, 1);

      // Populate based on operator kind
      LogicalResult popResult = failure();
      if (unary1.contains(selector) || unary3.contains(selector)) {
        popResult = populateUnary(rewriter, loc, selector, valueType,
                                  newSelectOp.getRegions().slice(0, 1),
                                  graphblas::YieldKind::SELECT_OUT,
                                  /* boolAsI8 */ false);
      } else {
        popResult = populateBinary(rewriter, loc, selector, valueType,
                                   newSelectOp.getRegions().slice(0, 1),
                                   graphblas::YieldKind::SELECT_OUT,
                                   /* boolAsI8 */ false);
      }
      if (failed(popResult))
        return failure();

      // Remove thunk from populated block
      if (binary2.contains(selector) || binary4.contains(selector)) {
        Value thunk = thunks[0];
        Block &block = newSelectOp.getRegion(0).front();
        Value thunkArg = block.getArgument(1);
        thunkArg.replaceAllUsesWith(thunk);
        block.eraseArgument(1);
      }

      rewriter.setInsertionPointAfter(newSelectOp);
      rewriter.replaceOp(op, newSelectOp.getResult());
    }

    return success();
  };

  template <class T>
  static LogicalResult
  buildAlgorithm(T op, PatternRewriter &rewriter,
                 std::function<LogicalResult(T, PatternRewriter &, Location,
                                             Value &, Value, Value, Value)>
                     func) {
    ModuleOp module = op->template getParentOfType<ModuleOp>();
    Location loc = op->getLoc();

    Value input = op.input();
    RankedTensorType inputType = input.getType().cast<RankedTensorType>();
    Type valueType = inputType.getElementType();
    Type int64Type = rewriter.getIntegerType(64);
    Type indexType = rewriter.getIndexType();
    Type memref1DI64Type = MemRefType::get({-1}, int64Type);
    Type memref1DValueType = MemRefType::get({-1}, valueType);

    // Initial constants
    Value c0 = rewriter.create<arith::ConstantIndexOp>(loc, 0);
    Value c1 = rewriter.create<arith::ConstantIndexOp>(loc, 1);
    Value c1_64 = rewriter.create<arith::ConstantIntOp>(loc, 1, int64Type);

    // Get sparse tensor info
    unsigned rank = inputType.getRank();
    Value nrow;
    if (rank == 2)
      nrow = rewriter.create<graphblas::NumRowsOp>(loc, input);
    else
      // Vectors are stored as a 1xn matrix
      // so the code works correctly if we assume a single row
      nrow = c1;

    Value indexPos = (rank == 2 ? c1 : c0);
    Value Ap = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memref1DI64Type, input, indexPos);
    Value Aj = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memref1DI64Type,
                                                           input, indexPos);
    Value Ax = rewriter.create<sparse_tensor::ToValuesOp>(
        loc, memref1DValueType, input);

    // Create output
    Value output = rewriter.create<graphblas::DupOp>(loc, input);
    bool colWise = false;
    if (rank == 2)
      colWise = hasColumnOrdering(inputType);

    Value Bp = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memref1DI64Type, output, indexPos);
    Value Bj = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memref1DI64Type,
                                                           output, indexPos);
    Value Bx = rewriter.create<sparse_tensor::ToValuesOp>(
        loc, memref1DValueType, output);

    // Loop
    scf::ForOp outerLoop = rewriter.create<scf::ForOp>(loc, c0, nrow, c1);
    Value row = outerLoop.getInductionVar();
    {
      rewriter.setInsertionPointToStart(outerLoop.getBody());
      Value row_plus1 = rewriter.create<arith::AddIOp>(loc, row, c1);

      Value bp_curr_count = rewriter.create<memref::LoadOp>(loc, Bp, row);
      rewriter.create<memref::StoreOp>(loc, bp_curr_count, Bp, row_plus1);

      Value j_start_64 = rewriter.create<memref::LoadOp>(loc, Ap, row);
      Value j_end_64 = rewriter.create<memref::LoadOp>(loc, Ap, row_plus1);
      Value j_start =
          rewriter.create<arith::IndexCastOp>(loc, j_start_64, indexType);
      Value j_end =
          rewriter.create<arith::IndexCastOp>(loc, j_end_64, indexType);

      scf::ForOp innerLoop =
          rewriter.create<scf::ForOp>(loc, j_start, j_end, c1);
      Value jj = innerLoop.getInductionVar();
      {
        rewriter.setInsertionPointToStart(innerLoop.getBody());
        Value col_64 = rewriter.create<memref::LoadOp>(loc, Aj, jj);
        Value col = rewriter.create<arith::IndexCastOp>(loc, col_64, indexType);
        Value val = rewriter.create<memref::LoadOp>(loc, Ax, jj);

        // Inject code from func
        Value keep = nullptr;
        LogicalResult funcResult = failure();
        if (rank == 1)
          funcResult = func(op, rewriter, loc, keep, val, col, col);
        else if (colWise)
          funcResult = func(op, rewriter, loc, keep, val, col, row);
        else
          funcResult = func(op, rewriter, loc, keep, val, row, col);
        if (funcResult.failed()) {
          return funcResult;
        }

        scf::IfOp ifKeep =
            rewriter.create<scf::IfOp>(loc, keep, false /* no else region */);
        {
          rewriter.setInsertionPointToStart(ifKeep.thenBlock());

          Value bj_pos_64 = rewriter.create<memref::LoadOp>(loc, Bp, row_plus1);
          Value bj_pos =
              rewriter.create<arith::IndexCastOp>(loc, bj_pos_64, indexType);

          rewriter.create<memref::StoreOp>(loc, col_64, Bj, bj_pos);
          rewriter.create<memref::StoreOp>(loc, val, Bx, bj_pos);

          Value bj_pos_plus1 =
              rewriter.create<arith::AddIOp>(loc, bj_pos_64, c1_64);
          rewriter.create<memref::StoreOp>(loc, bj_pos_plus1, Bp, row_plus1);

          rewriter.setInsertionPointAfter(ifKeep);
        }
        // rewriter.setInsertionPointAfter(innerLoop);
      }

      rewriter.setInsertionPointAfter(outerLoop);
    }

    // trim excess values
    Value nnz = rewriter.create<graphblas::NumValsOp>(loc, output);
    callResizeIndex(rewriter, module, loc, output, indexPos, nnz);
    callResizeValues(rewriter, module, loc, output, nnz);

    rewriter.replaceOp(op, output);

    cleanupIntermediateTensor(rewriter, module, loc, output);

    return success();
  };

private:
  static LogicalResult
  probabilityBlock(graphblas::SelectOp op, PatternRewriter &rewriter,
                   Location loc, Value &keep, Value val, Value row, Value col,
                   // These are not part of the standard signature
                   // and will be passed using `bind`
                   Value thunk, Value rngContext) {
    Type f64Type = rewriter.getF64Type();
    SymbolRefAttr random_double =
        SymbolRefAttr::get(rewriter.getContext(), "random_double");
    // Get a random double between [0, 1)
    CallOp randCall = rewriter.create<mlir::CallOp>(
        loc, random_double, TypeRange{f64Type}, ArrayRef<Value>({rngContext}));
    Value rand = randCall.getResult(0);
    keep = rewriter.create<arith::CmpFOp>(loc, arith::CmpFPredicate::OLT, rand,
                                          thunk);

    return success();
  };
};

class LowerSelectGenericRewrite
    : public OpRewritePattern<graphblas::SelectGenericOp> {
public:
  using OpRewritePattern<graphblas::SelectGenericOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::SelectGenericOp op,
                                PatternRewriter &rewriter) const override {
    LogicalResult callResult =
        LowerSelectRewrite::buildAlgorithm<graphblas::SelectGenericOp>(
            op, rewriter, genericBlock);
    if (callResult.failed()) {
      return callResult;
    }

    return success();
  };

private:
  static LogicalResult genericBlock(graphblas::SelectGenericOp op,
                                    PatternRewriter &rewriter, Location loc,
                                    Value &keep, Value val, Value row,
                                    Value col) {
    // Required blocks
    RegionRange extensions = op.extensions();
    ExtensionBlocks extBlocks;
    std::set<graphblas::YieldKind> required = {
        graphblas::YieldKind::SELECT_OUT};
    LogicalResult extractResult =
        extBlocks.extractBlocks(op, extensions, required, {});

    if (extractResult.failed()) {
      return extractResult;
    }

    int numArguments = extBlocks.selectOut->getArguments().size();

    // scf::ForOp automatically gets an empty scf.yield at the end which
    // we need to insert before
    Operation *scfYield = rewriter.getBlock()->getTerminator();

    // insert selectOut block
    graphblas::YieldOp selectOutYield =
        llvm::dyn_cast_or_null<graphblas::YieldOp>(
            extBlocks.selectOut->getTerminator());

    ValueRange subVals = ValueRange{val, row, col}.slice(0, numArguments);
    rewriter.mergeBlockBefore(extBlocks.selectOut, scfYield, subVals);
    keep = selectOutYield.values().front();
    rewriter.eraseOp(selectOutYield);

    return success();
  };
};

class ReduceToVectorDWIMRewrite
    : public OpRewritePattern<graphblas::ReduceToVectorOp> {
public:
  using OpRewritePattern<graphblas::ReduceToVectorOp>::OpRewritePattern;

  static bool needsDWIM(graphblas::ReduceToVectorOp op) {
    int axis = op.axis();
    bool isCSR = hasRowOrdering(op.input().getType());
    return ((axis == 0 && isCSR) || (axis == 1 && !isCSR));
  };

  LogicalResult matchAndRewrite(graphblas::ReduceToVectorOp op,
                                PatternRewriter &rewriter) const override {
    if (!needsDWIM(op))
      return failure();

    MLIRContext *context = op.getContext();
    Location loc = op->getLoc();

    Value input = op.input();
    RankedTensorType flippedInputType =
        getFlippedLayoutType(context, input.getType());

    rewriter.setInsertionPoint(op);
    Value flippedInput = rewriter.create<graphblas::ConvertLayoutOp>(
        loc, flippedInputType, input);
    op.inputMutable().assign(flippedInput);

    return success();
  };
};

class LowerReduceToVectorRewrite
    : public OpRewritePattern<graphblas::ReduceToVectorOp> {
public:
  using OpRewritePattern<graphblas::ReduceToVectorOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::ReduceToVectorOp op,
                                PatternRewriter &rewriter) const override {
    if (ReduceToVectorDWIMRewrite::needsDWIM(op))
      return failure();

    Value input = op.input();
    StringRef aggregator = op.aggregator();
    RankedTensorType inputType = input.getType().dyn_cast<RankedTensorType>();
    Type elementType = inputType.getElementType();
    Type i64Type = rewriter.getI64Type();

    if (aggregator == "count") {
      return buildAlgorithm<graphblas::ReduceToVectorOp>(op, rewriter, i64Type,
                                                         countBlock);
    } else if (aggregator == "argmin" or aggregator == "argmax") {
      return buildAlgorithm<graphblas::ReduceToVectorOp>(op, rewriter, i64Type,
                                                         argminmaxBlock);
    } else if (aggregator == "first" or aggregator == "last") {
      return buildAlgorithm<graphblas::ReduceToVectorOp>(
          op, rewriter, elementType, firstLastBlock);
    } else {
      Location loc = op->getLoc();

      NamedAttrList attributes = {};
      attributes.append(StringRef("axis"),
                        rewriter.getIntegerAttr(i64Type, op.axis()));
      attributes.append(StringRef("mask_complement"),
                        rewriter.getBoolAttr(op.mask_complement()));
      graphblas::ReduceToVectorGenericOp newReduceOp =
          rewriter.create<graphblas::ReduceToVectorGenericOp>(
              loc, op->getResultTypes(), input, attributes.getAttrs(), 2);

      if (failed(populateMonoid(rewriter, loc, op.aggregator(), elementType,
                                newReduceOp.getRegions().slice(0, 2),
                                graphblas::YieldKind::AGG_IDENTITY,
                                graphblas::YieldKind::AGG)))
        return failure();

      rewriter.setInsertionPointAfter(newReduceOp);
      rewriter.replaceOp(op, newReduceOp.getResult());
    }

    return success();
  };

  template <class T>
  static LogicalResult buildAlgorithm(
      T op, PatternRewriter &rewriter, Type outputType,
      std::function<LogicalResult(T, PatternRewriter &, Location, Value &,
                                  Value, Value, Value, Value)>
          func) {
    MLIRContext *context = op.getContext();
    ModuleOp module = op->template getParentOfType<ModuleOp>();
    Location loc = op->getLoc();

    // Inputs
    Value input = op.input();
    Value mask = op.mask();
    int axis = op.axis();
    bool maskComplement = op.mask_complement();

    // Types
    Type indexType = rewriter.getIndexType();
    Type i64Type = rewriter.getIntegerType(64);
    RankedTensorType inputType = input.getType().dyn_cast<RankedTensorType>();
    Type memrefPointerType = getMemrefPointerType(inputType);
    Type memrefIndexType = getMemrefIndexType(inputType);
    Type memrefIValueType = getMemrefValueType(inputType);
    Type memrefOValueType = MemRefType::get({-1}, outputType);

    // Constants
    Value c0 = rewriter.create<arith::ConstantIndexOp>(loc, 0);
    Value c1 = rewriter.create<arith::ConstantIndexOp>(loc, 1);

    // Sparse pointers
    Value Ip = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memrefPointerType, input, c1);
    Value Ii = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memrefIndexType,
                                                           input, c1);
    Value Ix = rewriter.create<sparse_tensor::ToValuesOp>(loc, memrefIValueType,
                                                          input);

    // Compute output sizes
    Value size;
    if (axis == 1)
      size = rewriter.create<graphblas::NumRowsOp>(loc, input);
    else
      size = rewriter.create<graphblas::NumColsOp>(loc, input);

    // Compute sparse array of valid output indices
    ValueRange sdpRet = sparsifyDensePointers(rewriter, loc, size, Ip);
    Value sparsePointers = sdpRet[0];
    Value nnz = sdpRet[1];
    if (mask) {
      Value Mi = rewriter.create<sparse_tensor::ToIndicesOp>(
          loc, memrefIndexType, mask, c0);
      Value mNnz = rewriter.create<graphblas::NumValsOp>(loc, mask);
      if (maskComplement) {
        ValueRange bmcRet =
            buildMaskComplement(rewriter, loc, size, Mi, c0, mNnz);
        Mi = bmcRet[0];
        mNnz = bmcRet[1];
      }
      Value prevSparsePointers = sparsePointers;
      ValueRange bioRet =
          buildIndexOverlap(rewriter, loc, nnz, prevSparsePointers, mNnz, Mi);
      sparsePointers = bioRet[0];
      nnz = bioRet[1];
      if (maskComplement)
        rewriter.create<memref::DeallocOp>(loc, Mi);
      rewriter.create<memref::DeallocOp>(loc, prevSparsePointers);
    }
    Value nnz64 = rewriter.create<arith::IndexCastOp>(loc, nnz, i64Type);

    // Build output vector
    Value output = callNewTensor(rewriter, module, loc, ValueRange{size},
                                 getCompressedVectorType(context, outputType));

    callResizeIndex(rewriter, module, loc, output, c0, nnz);
    callResizeValues(rewriter, module, loc, output, nnz);

    Value Op = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memrefPointerType, output, c0);
    Value Oi = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memrefIndexType,
                                                           output, c0);
    Value Ox = rewriter.create<sparse_tensor::ToValuesOp>(loc, memrefOValueType,
                                                          output);

    // Populate output
    rewriter.create<memref::StoreOp>(loc, nnz64, Op, c1);

    // Loop over sparse array of valid output indices
    scf::ForOp reduceLoop = rewriter.create<scf::ForOp>(loc, c0, nnz, c1);
    {
      rewriter.setInsertionPointToStart(reduceLoop.getBody());
      Value outputPos = reduceLoop.getInductionVar();
      Value rowIndex64 =
          rewriter.create<memref::LoadOp>(loc, sparsePointers, outputPos);
      Value rowIndex =
          rewriter.create<arith::IndexCastOp>(loc, rowIndex64, indexType);
      Value nextRowIndex =
          rewriter.create<arith::AddIOp>(loc, rowIndex, c1).getResult();
      Value ptr64 = rewriter.create<memref::LoadOp>(loc, Ip, rowIndex);
      Value nextPtr64 = rewriter.create<memref::LoadOp>(loc, Ip, nextRowIndex);

      // At this point, we know the row is not empty, so nextPtr64 > ptr64
      Value ptr = rewriter.create<arith::IndexCastOp>(loc, ptr64, indexType);
      Value nextPtr =
          rewriter.create<arith::IndexCastOp>(loc, nextPtr64, indexType);

      // Inject code from func
      Value aggVal = nullptr;
      LogicalResult funcResult =
          func(op, rewriter, loc, aggVal, ptr, nextPtr, Ii, Ix);
      if (funcResult.failed()) {
        return funcResult;
      }

      rewriter.create<memref::StoreOp>(loc, aggVal, Ox, outputPos);
      rewriter.create<memref::StoreOp>(loc, rowIndex64, Oi, outputPos);
    }
    rewriter.setInsertionPointAfter(reduceLoop);
    rewriter.create<memref::DeallocOp>(loc, sparsePointers);
    rewriter.replaceOp(op, output);

    cleanupIntermediateTensor(rewriter, module, loc, output);

    return success();
  };

private:
  static LogicalResult countBlock(graphblas::ReduceToVectorOp op,
                                  PatternRewriter &rewriter, Location loc,
                                  Value &aggVal, Value ptr, Value nextPtr,
                                  Value Ii, Value Ix) {
    Type i64Type = rewriter.getI64Type();
    Value diff = rewriter.create<arith::SubIOp>(loc, nextPtr, ptr);
    aggVal = rewriter.create<arith::IndexCastOp>(loc, diff, i64Type);

    return success();
  }

  static LogicalResult argminmaxBlock(graphblas::ReduceToVectorOp op,
                                      PatternRewriter &rewriter, Location loc,
                                      Value &aggVal, Value ptr, Value nextPtr,
                                      Value Ii, Value Ix) {
    StringRef aggregator = op.aggregator();
    Value c1 = rewriter.create<arith::ConstantIndexOp>(loc, 1);
    RankedTensorType inputType =
        op.input().getType().dyn_cast<RankedTensorType>();
    Type elementType = inputType.getElementType();
    Type i64Type = rewriter.getI64Type();

    Value initVal = rewriter.create<memref::LoadOp>(loc, Ix, ptr);
    Value initIdx = rewriter.create<memref::LoadOp>(loc, Ii, ptr);
    Value ptrPlusOne = rewriter.create<arith::AddIOp>(loc, ptr, c1);
    scf::ForOp loop = rewriter.create<scf::ForOp>(loc, ptrPlusOne, nextPtr, c1,
                                                  ValueRange{initVal, initIdx});
    {
      rewriter.setInsertionPointToStart(loop.getBody());
      Value curVal = loop.getLoopBody().getArgument(1);
      Value curIdx = loop.getLoopBody().getArgument(2);
      Value curPtr = loop.getInductionVar();
      Value rowValue = rewriter.create<memref::LoadOp>(loc, Ix, curPtr);

      bool useMinimum = aggregator == "argmin";
      Value mustUpdate = llvm::TypeSwitch<Type, Value>(elementType)
                             .Case<IntegerType>([&](IntegerType type) {
                               return rewriter.create<arith::CmpIOp>(
                                   loc,
                                   useMinimum ? arith::CmpIPredicate::slt
                                              : arith::CmpIPredicate::sgt,
                                   rowValue, curVal);
                             })
                             .Case<FloatType>([&](FloatType type) {
                               return rewriter.create<arith::CmpFOp>(
                                   loc,
                                   useMinimum ? arith::CmpFPredicate::OLT
                                              : arith::CmpFPredicate::OGT,
                                   rowValue, curVal);
                             });

      scf::IfOp ifMustUpdateBlock = rewriter.create<scf::IfOp>(
          loc, TypeRange{elementType, i64Type}, mustUpdate, true);
      {
        rewriter.setInsertionPointToStart(ifMustUpdateBlock.thenBlock());
        Value newIdx = rewriter.create<memref::LoadOp>(loc, Ii, curPtr);
        rewriter.create<scf::YieldOp>(loc, ValueRange{rowValue, newIdx});
      }
      {
        rewriter.setInsertionPointToStart(ifMustUpdateBlock.elseBlock());
        rewriter.create<scf::YieldOp>(loc, ValueRange{curVal, curIdx});
        rewriter.setInsertionPointAfter(ifMustUpdateBlock);
      }
      rewriter.create<scf::YieldOp>(loc, ifMustUpdateBlock.getResults());

      rewriter.setInsertionPointAfter(loop);
    }

    aggVal = loop.getResult(1);

    return success();
  }

  static LogicalResult firstLastBlock(graphblas::ReduceToVectorOp op,
                                      PatternRewriter &rewriter, Location loc,
                                      Value &aggVal, Value ptr, Value nextPtr,
                                      Value Ii, Value Ix) {
    StringRef aggregator = op.aggregator();
    Value c1 = rewriter.create<arith::ConstantIndexOp>(loc, 1);

    if (aggregator == "first") {
      aggVal = rewriter.create<memref::LoadOp>(loc, Ix, ptr);
    } else {
      Value lastPtr = rewriter.create<arith::SubIOp>(loc, nextPtr, c1);
      aggVal = rewriter.create<memref::LoadOp>(loc, Ix, lastPtr);
    }

    return success();
  }
};

class LowerReduceToVectorGenericRewrite
    : public OpRewritePattern<graphblas::ReduceToVectorGenericOp> {
public:
  using OpRewritePattern<graphblas::ReduceToVectorGenericOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::ReduceToVectorGenericOp op,
                                PatternRewriter &rewriter) const override {
    Type elementType =
        op.input().getType().cast<RankedTensorType>().getElementType();
    LogicalResult callResult = LowerReduceToVectorRewrite::buildAlgorithm<
        graphblas::ReduceToVectorGenericOp>(op, rewriter, elementType,
                                            genericBlock);
    if (callResult.failed()) {
      return callResult;
    }

    return success();
  };

private:
  static LogicalResult genericBlock(graphblas::ReduceToVectorGenericOp op,
                                    PatternRewriter &rewriter, Location loc,
                                    Value &aggVal, Value ptr, Value nextPtr,
                                    Value Ii, Value Ix) {
    // Required blocks
    RegionRange extensions = op.extensions();
    ExtensionBlocks extBlocks;
    std::set<graphblas::YieldKind> required = {
        graphblas::YieldKind::AGG_IDENTITY, graphblas::YieldKind::AGG};
    LogicalResult extractResult =
        extBlocks.extractBlocks(op, extensions, required, {});

    if (extractResult.failed()) {
      return extractResult;
    }

    // Build inner block
    Value c1 = rewriter.create<arith::ConstantIndexOp>(loc, 1);

    // insert agg identity
    rewriter.mergeBlocks(extBlocks.aggIdentity, rewriter.getBlock(), {});
    graphblas::YieldOp aggIdentityYield =
        llvm::dyn_cast_or_null<graphblas::YieldOp>(
            rewriter.getBlock()->getTerminator());
    Value c0Accumulator = aggIdentityYield.values().front();
    rewriter.eraseOp(aggIdentityYield);

    // reduce in a loop
    scf::ParallelOp aggLoop =
        rewriter.create<scf::ParallelOp>(loc, ptr, nextPtr, c1, c0Accumulator);
    ValueRange aggIdx = aggLoop.getInductionVars();

    rewriter.setInsertionPointToStart(aggLoop.getBody());
    Value x = rewriter.create<memref::LoadOp>(loc, Ix, aggIdx);

    scf::ReduceOp reducer = rewriter.create<scf::ReduceOp>(loc, x);
    BlockArgument lhs = reducer.getRegion().getArgument(0);
    BlockArgument rhs = reducer.getRegion().getArgument(1);

    rewriter.setInsertionPointToStart(&reducer.getRegion().front());

    rewriter.mergeBlocks(extBlocks.agg, rewriter.getBlock(), {lhs, rhs});
    graphblas::YieldOp aggYield = llvm::dyn_cast_or_null<graphblas::YieldOp>(
        rewriter.getBlock()->getTerminator());
    Value result = aggYield.values().front();
    rewriter.eraseOp(aggYield);

    rewriter.create<scf::ReduceReturnOp>(loc, result);

    rewriter.setInsertionPointAfter(aggLoop);

    aggVal = aggLoop.getResult(0);

    return success();
  };
};

class LowerReduceToScalarRewrite
    : public OpRewritePattern<graphblas::ReduceToScalarOp> {
public:
  using OpRewritePattern<graphblas::ReduceToScalarOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::ReduceToScalarOp op,
                                PatternRewriter &rewriter) const override {
    StringRef aggregator = op.aggregator();

    if (aggregator == "count") {
      return rewriteCount(op, rewriter);
    } else if (aggregator == "argmin" or aggregator == "argmax") {
      return rewriteArgMinMax(op, rewriter);
    } else {
      return rewriteStandard(op, rewriter);
    }
  };

private:
  LogicalResult rewriteCount(graphblas::ReduceToScalarOp op,
                             PatternRewriter &rewriter) const {
    Value input = op.input();
    Location loc = op->getLoc();
    Type int64Type = rewriter.getIntegerType(64);

    Value countOp = rewriter.create<graphblas::NumValsOp>(loc, input);
    Value countOp_64 =
        rewriter.create<arith::IndexCastOp>(loc, countOp, int64Type);
    rewriter.replaceOp(op, countOp_64);

    return success();
  }

  LogicalResult rewriteArgMinMax(graphblas::ReduceToScalarOp op,
                                 PatternRewriter &rewriter) const {
    // TODO we get seg faults if given a size 0 vector or a sparse vector with
    // no non-zero values. Probably should return a -1 for these cases.
    Location loc = op->getLoc();
    StringRef aggregator = op.aggregator();

    Value input = op.input();
    RankedTensorType inputType = input.getType().cast<RankedTensorType>();

    Value c0 = rewriter.create<arith::ConstantIndexOp>(loc, 0);
    Value c1 = rewriter.create<arith::ConstantIndexOp>(loc, 1);
    Type indexType = rewriter.getIndexType();
    Type int64Type = rewriter.getIntegerType(64);
    Type memref1DI64Type = MemRefType::get({-1}, int64Type);

    Value pointers = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memref1DI64Type, input, c0);
    Value endPosition64 = rewriter.create<memref::LoadOp>(loc, pointers, c1);
    Value endPosition =
        rewriter.create<arith::IndexCastOp>(loc, endPosition64, indexType);

    Type inputElementType = inputType.getElementType();
    Type memref1DValueType = MemRefType::get({-1}, inputElementType);
    Value values = rewriter.create<sparse_tensor::ToValuesOp>(
        loc, memref1DValueType, input);

    Value initialExtremum = rewriter.create<memref::LoadOp>(loc, values, c0);

    scf::ForOp loop = rewriter.create<scf::ForOp>(
        loc, c1, endPosition, c1, ValueRange{initialExtremum, c0});
    Value currentValuePosition = loop.getInductionVar();
    Value currentExtremum = loop.getLoopBody().getArgument(1);
    Value currentExtremumPosition = loop.getLoopBody().getArgument(2);
    rewriter.setInsertionPointToStart(loop.getBody());

    Value currentValue =
        rewriter.create<memref::LoadOp>(loc, values, currentValuePosition);
    bool useMinimum = aggregator == "argmin";
    Value replace = llvm::TypeSwitch<Type, Value>(inputElementType)
                        .Case<IntegerType>([&](IntegerType type) {
                          return rewriter.create<arith::CmpIOp>(
                              loc,
                              useMinimum ? arith::CmpIPredicate::slt
                                         : arith::CmpIPredicate::sgt,
                              currentValue, currentExtremum);
                        })
                        .Case<FloatType>([&](FloatType type) {
                          return rewriter.create<arith::CmpFOp>(
                              loc,
                              useMinimum ? arith::CmpFPredicate::OLT
                                         : arith::CmpFPredicate::OGT,
                              currentValue, currentExtremum);
                        });

    scf::IfOp ifBlock = rewriter.create<scf::IfOp>(
        loc, TypeRange{inputElementType, indexType}, replace, true);
    rewriter.setInsertionPointToStart(ifBlock.thenBlock());
    rewriter.create<scf::YieldOp>(
        loc, ValueRange{currentValue, currentValuePosition});
    rewriter.setInsertionPointToStart(ifBlock.elseBlock());
    rewriter.create<scf::YieldOp>(
        loc, ValueRange{currentExtremum, currentExtremumPosition});
    rewriter.setInsertionPointAfter(ifBlock);

    rewriter.create<scf::YieldOp>(loc, ifBlock.getResults());
    rewriter.setInsertionPointAfter(loop);

    Value finalExtremumPosition = loop.getResult(1);
    Value indices = rewriter.create<sparse_tensor::ToIndicesOp>(
        loc, memref1DI64Type, input, c0);
    Value argExtremum =
        rewriter.create<memref::LoadOp>(loc, indices, finalExtremumPosition);
    rewriter.replaceOp(op, argExtremum);

    return success();
  }

  LogicalResult rewriteStandard(graphblas::ReduceToScalarOp op,
                                PatternRewriter &rewriter) const {
    Value input = op.input();
    Location loc = op->getLoc();
    Type valueType = input.getType().cast<RankedTensorType>().getElementType();

    graphblas::ReduceToScalarGenericOp newReduceOp =
        rewriter.create<graphblas::ReduceToScalarGenericOp>(
            loc, op->getResultTypes(), input, 2);

    if (failed(populateMonoid(rewriter, loc, op.aggregator(), valueType,
                              newReduceOp.getRegions().slice(0, 2),
                              graphblas::YieldKind::AGG_IDENTITY,
                              graphblas::YieldKind::AGG)))
      return failure();

    rewriter.setInsertionPointAfter(newReduceOp);
    rewriter.replaceOp(op, newReduceOp.getResult());

    return success();
  }
};

class LowerReduceToScalarGenericRewrite
    : public OpRewritePattern<graphblas::ReduceToScalarGenericOp> {
public:
  using OpRewritePattern<graphblas::ReduceToScalarGenericOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::ReduceToScalarGenericOp op,
                                PatternRewriter &rewriter) const override {
    Value input = op.input();
    Location loc = op->getLoc();

    RankedTensorType operandType =
        op.input().getType().dyn_cast<RankedTensorType>();
    Type valueType = operandType.getElementType();

    // Required blocks
    RegionRange extensions = op.extensions();
    ExtensionBlocks extBlocks;
    std::set<graphblas::YieldKind> required = {
        graphblas::YieldKind::AGG_IDENTITY, graphblas::YieldKind::AGG};
    LogicalResult extractResult =
        extBlocks.extractBlocks(op, extensions, required, {});

    if (extractResult.failed()) {
      return extractResult;
    }

    // insert agg identity
    rewriter.mergeBlocks(extBlocks.aggIdentity, rewriter.getBlock(), {});
    graphblas::YieldOp aggIdentityYield =
        llvm::dyn_cast_or_null<graphblas::YieldOp>(
            rewriter.getBlock()->getTerminator());
    Value c0Accumulator = aggIdentityYield.values().front();
    rewriter.eraseOp(aggIdentityYield);

    // initial constants
    Value c0 = rewriter.create<arith::ConstantIndexOp>(loc, 0);
    Value c1 = rewriter.create<arith::ConstantIndexOp>(loc, 1);

    // Get sparse tensor info
    MemRefType memref1DValueType = MemRefType::get({-1}, valueType);

    sparse_tensor::ToValuesOp inputValues =
        rewriter.create<sparse_tensor::ToValuesOp>(loc, memref1DValueType,
                                                   input);

    Value nnz = rewriter.create<graphblas::NumValsOp>(loc, input);

    // begin loop
    scf::ParallelOp valueLoop =
        rewriter.create<scf::ParallelOp>(loc, c0, nnz, c1, c0Accumulator);
    ValueRange valueLoopIdx = valueLoop.getInductionVars();

    rewriter.setInsertionPointToStart(valueLoop.getBody());
    memref::LoadOp y =
        rewriter.create<memref::LoadOp>(loc, inputValues, valueLoopIdx);

    scf::ReduceOp reducer = rewriter.create<scf::ReduceOp>(loc, y);
    BlockArgument lhs = reducer.getRegion().getArgument(0);
    BlockArgument rhs = reducer.getRegion().getArgument(1);

    rewriter.setInsertionPointToStart(&reducer.getRegion().front());

    rewriter.mergeBlocks(extBlocks.agg, rewriter.getBlock(), {lhs, rhs});
    graphblas::YieldOp aggYield = llvm::dyn_cast_or_null<graphblas::YieldOp>(
        rewriter.getBlock()->getTerminator());
    Value result = aggYield.values().front();
    rewriter.eraseOp(aggYield);

    rewriter.create<scf::ReduceReturnOp>(loc, result);

    rewriter.setInsertionPointAfter(reducer);

    rewriter.replaceOp(op, valueLoop.getResult(0));

    return success();
  };
};

class LowerApplyRewrite : public OpRewritePattern<graphblas::ApplyOp> {
public:
  using OpRewritePattern<graphblas::ApplyOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::ApplyOp op,
                                PatternRewriter &rewriter) const override {
    Value input, thunk;
    LogicalResult extractArgResult = extractApplyOpArgs(op, input, thunk);
    assert(!extractArgResult.failed() &&
           "Assumption that extractApplyOpArgs succeeded (due to verify "
           "method) has been violated.");

    StringRef apply_operator = op.apply_operator();
    if (apply_operator == "identity") {
      // This doesn't produce a copy like we do for all the other operators
      rewriter.replaceOp(op, input);
      return success();
    }

    ModuleOp module = op->getParentOfType<ModuleOp>(); /* ignore unused variable
                                                          for debugging */
    (void)module;
    Location loc = op->getLoc();

    Type valueType =
        input.getType().dyn_cast<RankedTensorType>().getElementType();

    // New op
    graphblas::ApplyGenericOp newApplyOp =
        rewriter.create<graphblas::ApplyGenericOp>(loc, op->getResultTypes(),
                                                   input, 1);

    // Populate based on operator kind
    LogicalResult popResult = failure();
    if (unary1.contains(apply_operator) || unary3.contains(apply_operator))
      popResult = populateUnary(rewriter, loc, apply_operator, valueType,
                                newApplyOp.getRegions().slice(0, 1),
                                graphblas::YieldKind::TRANSFORM_OUT);
    else
      popResult = populateBinary(rewriter, loc, apply_operator, valueType,
                                 newApplyOp.getRegions().slice(0, 1),
                                 graphblas::YieldKind::TRANSFORM_OUT);
    if (failed(popResult))
      return failure();

    // Remove thunk from populated block
    if (binary2.contains(apply_operator) || binary4.contains(apply_operator)) {
      Block &block = newApplyOp.getRegion(0).front();
      int thunkPos = thunk == op.left() ? 0 : 1;
      Value thunkArg = block.getArgument(thunkPos);
      thunkArg.replaceAllUsesWith(thunk);
      block.eraseArgument(thunkPos);
    }

    rewriter.replaceOp(op, newApplyOp.getResult());

    return success();
  };
};

class LowerApplyGenericRewrite
    : public OpRewritePattern<graphblas::ApplyGenericOp> {
public:
  using OpRewritePattern<graphblas::ApplyGenericOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::ApplyGenericOp op,
                                PatternRewriter &rewriter) const override {
    ModuleOp module = op->getParentOfType<ModuleOp>();
    Location loc = op->getLoc();

    Value inputTensor = op.input();
    RankedTensorType inputTensorType =
        inputTensor.getType().cast<RankedTensorType>();
    RankedTensorType outputTensorType =
        op.getResult().getType().cast<RankedTensorType>();
    unsigned rank = inputTensorType.getRank();

    Type indexType = rewriter.getIndexType();
    Type memrefPointerType = getMemrefPointerType(inputTensorType);
    Type memrefIndexType = getMemrefIndexType(inputTensorType);
    Type memrefIValueType = getMemrefValueType(inputTensorType);
    Type memrefOValueType = getMemrefValueType(outputTensorType);

    // Required blocks
    RegionRange extensions = op.extensions();
    ExtensionBlocks extBlocks;
    std::set<graphblas::YieldKind> required = {
        graphblas::YieldKind::TRANSFORM_OUT};
    LogicalResult extractResult =
        extBlocks.extractBlocks(op, extensions, required, {});

    if (extractResult.failed()) {
      return extractResult;
    }

    // Initial constants
    Value c0 = rewriter.create<arith::ConstantIndexOp>(loc, 0);
    Value c1 = rewriter.create<arith::ConstantIndexOp>(loc, 1);

    // Build output with same shape as input, but possibly different output type
    Value output = rewriter.create<graphblas::DupOp>(loc, inputTensor);
    if (inputTensorType != outputTensorType)
      output =
          rewriter.create<graphblas::CastOp>(loc, outputTensorType, output);
    // Get sparse tensor info
    Value inputValues = rewriter.create<sparse_tensor::ToValuesOp>(
        loc, memrefIValueType, inputTensor);
    Value outputValues = rewriter.create<sparse_tensor::ToValuesOp>(
        loc, memrefOValueType, output);

    Value nnz = rewriter.create<graphblas::NumValsOp>(loc, inputTensor);

    int numArguments = extBlocks.transformOut->getArguments().size();
    if (numArguments == 3) {
      // Loop over pointers, indices, values
      // This works for
      // - vector -> passes in (val, index, index)
      // - CSR or CSC -> passes in (val, row, col)
      Value inputPointers = rewriter.create<sparse_tensor::ToPointersOp>(
          loc, memrefPointerType, inputTensor);
      Value inputIndices = rewriter.create<sparse_tensor::ToIndicesOp>(
          loc, memrefIndexType, inputTensor);
      bool byCols = false;
      Value npointers;
      if (rank == 1) {
        npointers = c1;
      } else if (hasRowOrdering(inputTensorType)) {
        npointers = rewriter.create<graphblas::NumRowsOp>(loc, inputTensor);
      } else {
        npointers = rewriter.create<graphblas::NumColsOp>(loc, inputTensor);
        byCols = true;
      }
      scf::ParallelOp pointerLoop =
          rewriter.create<scf::ParallelOp>(loc, c0, npointers, c1);
      Value pointerIdx = pointerLoop.getInductionVars().front();

      rewriter.setInsertionPointToStart(pointerLoop.getBody());
      Value pointerIdx_plus1 =
          rewriter.create<arith::AddIOp>(loc, pointerIdx, c1);

      Value indexStart_64 =
          rewriter.create<memref::LoadOp>(loc, inputPointers, pointerIdx);
      Value indexEnd_64 =
          rewriter.create<memref::LoadOp>(loc, inputPointers, pointerIdx_plus1);
      Value indexStart =
          rewriter.create<arith::IndexCastOp>(loc, indexStart_64, indexType);
      Value indexEnd =
          rewriter.create<arith::IndexCastOp>(loc, indexEnd_64, indexType);

      scf::ForOp innerLoop =
          rewriter.create<scf::ForOp>(loc, indexStart, indexEnd, c1);
      Value jj = innerLoop.getInductionVar();
      {
        rewriter.setInsertionPointToStart(innerLoop.getBody());
        Value col_64 = rewriter.create<memref::LoadOp>(loc, inputIndices, jj);
        Value col = rewriter.create<arith::IndexCastOp>(loc, col_64, indexType);
        Value val = rewriter.create<memref::LoadOp>(loc, inputValues, jj);

        // insert transformOut block
        graphblas::YieldOp transformOutYield =
            llvm::dyn_cast_or_null<graphblas::YieldOp>(
                extBlocks.transformOut->getTerminator());

        ValueRange subVals;
        if (rank == 1)
          subVals = ValueRange{val, col, col};
        else if (byCols)
          subVals = ValueRange{val, col, pointerIdx};
        else
          subVals = ValueRange{val, pointerIdx, col};

        rewriter.mergeBlocks(extBlocks.transformOut, rewriter.getBlock(),
                             subVals);
        Value result = transformOutYield.values().front();
        rewriter.eraseOp(transformOutYield);

        rewriter.create<memref::StoreOp>(loc, result, outputValues, jj);
        // rewriter.setInsertionPointAfter(innerLoop);
      }

      // end row loop
      rewriter.setInsertionPointAfter(pointerLoop);
    } else if (numArguments == 1) {
      // Fast path: only loop over values because we don't need indices
      scf::ParallelOp valueLoop =
          rewriter.create<scf::ParallelOp>(loc, c0, nnz, c1);
      ValueRange valueLoopIdx = valueLoop.getInductionVars();

      rewriter.setInsertionPointToStart(valueLoop.getBody());
      Value val =
          rewriter.create<memref::LoadOp>(loc, inputValues, valueLoopIdx);

      // scf::ParallelOp automatically gets an empty scf.yield at the end which
      // we need to insert before
      Operation *scfYield = valueLoop.getBody()->getTerminator();

      // insert transformOut block
      graphblas::YieldOp transformOutYield =
          llvm::dyn_cast_or_null<graphblas::YieldOp>(
              extBlocks.transformOut->getTerminator());

      rewriter.mergeBlockBefore(extBlocks.transformOut, scfYield, {val});
      Value result = transformOutYield.values().front();
      rewriter.eraseOp(transformOutYield);

      rewriter.create<memref::StoreOp>(loc, result, outputValues, valueLoopIdx);

      // end value loop
      rewriter.setInsertionPointAfter(valueLoop);
    } else
      assert(0);

    // Add return op
    rewriter.replaceOp(op, output);

    cleanupIntermediateTensor(rewriter, module, loc, output);

    return success();
  };
};

class LowerMatrixMultiplyRewrite
    : public OpRewritePattern<graphblas::MatrixMultiplyOp> {
public:
  using OpRewritePattern<graphblas::MatrixMultiplyOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::MatrixMultiplyOp op,
                                PatternRewriter &rewriter) const override {
    ModuleOp module = op->getParentOfType<ModuleOp>(); /* ignore unused variable
                                                          for debugging */
    (void)module;
    Location loc = op->getLoc();

    // Inputs
    ValueRange operands = op.getOperands();
    StringRef semiring = op.semiring();
    bool maskComplement = op.mask_complement();

    // Types
    // Can't use result here because it might be a scalar (vector-vector)
    Type valueType =
        op.a().getType().dyn_cast<RankedTensorType>().getElementType();

    // New op
    NamedAttrList attributes = {};
    attributes.append(StringRef("mask_complement"),
                      rewriter.getBoolAttr(maskComplement));
    graphblas::MatrixMultiplyGenericOp newMultOp =
        rewriter.create<graphblas::MatrixMultiplyGenericOp>(
            loc, op->getResultTypes(), operands, attributes.getAttrs(), 3);

    if (failed(populateSemiring(rewriter, loc, semiring, valueType,
                                newMultOp.getRegions().slice(0, 3))))
      return failure();

    rewriter.setInsertionPointAfter(newMultOp);

    rewriter.replaceOp(op, newMultOp.getResult());

    return success();
  };
};

class MatrixMultiplyGenericDWIMFirstArgRewrite
    : public OpRewritePattern<graphblas::MatrixMultiplyGenericOp> {
public:
  using OpRewritePattern<graphblas::MatrixMultiplyGenericOp>::OpRewritePattern;

  template <class T>
  static bool needsDWIM(T op) {
    return hasColumnOrdering(op.a().getType());
  };

  LogicalResult matchAndRewrite(graphblas::MatrixMultiplyGenericOp op,
                                PatternRewriter &rewriter) const override {
    if (!needsDWIM(op))
      return failure();

    MLIRContext *context = op.getContext();
    Location loc = op->getLoc();
    Value A = op.a();
    RankedTensorType aType = A.getType().cast<RankedTensorType>();
    RankedTensorType flippedMatrixType = getFlippedLayoutType(context, aType);

    rewriter.setInsertionPoint(op);
    Value flippedA =
        rewriter.create<graphblas::ConvertLayoutOp>(loc, flippedMatrixType, A);
    op.aMutable().assign(flippedA);

    return success();
  };
};

class MatrixMultiplyGenericDWIMSecondArgRewrite
    : public OpRewritePattern<graphblas::MatrixMultiplyGenericOp> {
public:
  using OpRewritePattern<graphblas::MatrixMultiplyGenericOp>::OpRewritePattern;

  template <class T>
  static bool needsDWIM(T op) {
    return hasRowOrdering(op.b().getType());
  };

  LogicalResult matchAndRewrite(graphblas::MatrixMultiplyGenericOp op,
                                PatternRewriter &rewriter) const override {
    if (!needsDWIM(op))
      return failure();

    MLIRContext *context = op.getContext();
    Location loc = op->getLoc();
    Value B = op.b();
    RankedTensorType bType = B.getType().cast<RankedTensorType>();
    RankedTensorType flippedMatrixType = getFlippedLayoutType(context, bType);

    rewriter.setInsertionPoint(op);
    Value flippedB =
        rewriter.create<graphblas::ConvertLayoutOp>(loc, flippedMatrixType, B);
    op.bMutable().assign(flippedB);

    return success();
  };
};

class MatrixMultiplyGenericDWIMMaskRewrite
    : public OpRewritePattern<graphblas::MatrixMultiplyGenericOp> {
public:
  using OpRewritePattern<graphblas::MatrixMultiplyGenericOp>::OpRewritePattern;

  template <class T>
  static bool needsDWIM(T op) {
    Value mask = op.mask();
    if (!mask)
      return false;
    return hasColumnOrdering(mask.getType());
  };

  LogicalResult matchAndRewrite(graphblas::MatrixMultiplyGenericOp op,
                                PatternRewriter &rewriter) const override {
    if (!needsDWIM(op))
      return failure();

    MLIRContext *context = op.getContext();
    Location loc = op->getLoc();

    Value mask = op.mask();
    RankedTensorType maskType = mask.getType().cast<RankedTensorType>();
    RankedTensorType flippedMatrixType =
        getFlippedLayoutType(context, maskType);

    rewriter.setInsertionPoint(op);
    Value flippedMask = rewriter.create<graphblas::ConvertLayoutOp>(
        loc, flippedMatrixType, mask);
    op.maskMutable().assign(flippedMask);

    return success();
  };
};

class LowerMatrixMultiplyGenericRewrite
    : public OpRewritePattern<graphblas::MatrixMultiplyGenericOp> {
public:
  using OpRewritePattern<graphblas::MatrixMultiplyGenericOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::MatrixMultiplyGenericOp op,
                                PatternRewriter &rewriter) const override {
    if (MatrixMultiplyGenericDWIMFirstArgRewrite::needsDWIM(op) ||
        MatrixMultiplyGenericDWIMSecondArgRewrite::needsDWIM(op) ||
        MatrixMultiplyGenericDWIMMaskRewrite::needsDWIM(op))
      return failure();

    // Required blocks
    RegionRange extensions = op.extensions();
    ExtensionBlocks extBlocks;
    std::set<graphblas::YieldKind> required = {
        graphblas::YieldKind::ADD_IDENTITY, graphblas::YieldKind::ADD,
        graphblas::YieldKind::MULT};
    std::set<graphblas::YieldKind> optional = {
        graphblas::YieldKind::TRANSFORM_OUT};
    LogicalResult extractResult =
        extBlocks.extractBlocks(op, extensions, required, optional);

    if (extractResult.failed()) {
      return extractResult;
    }

    // Inputs
    Value A = op.a();
    Value B = op.b();

    unsigned aRank = A.getType().dyn_cast<RankedTensorType>().getRank();
    unsigned bRank = B.getType().dyn_cast<RankedTensorType>().getRank();

    if (aRank == 2 && bRank == 2)
      return rewriteMatrixMatrixMultiplication(op, rewriter, extBlocks);
    else if (aRank == 2 && bRank == 1)
      return rewriteMatrixVectorMultiplication(op, rewriter, extBlocks);
    else if (aRank == 1 && bRank == 2)
      return rewriteVectorMatrixMultiplication(op, rewriter, extBlocks);
    else
      return rewriteVectorVectorMultiplication(op, rewriter, extBlocks);
  };

private:
  LogicalResult
  rewriteMatrixMatrixMultiplication(graphblas::MatrixMultiplyGenericOp op,
                                    PatternRewriter &rewriter,
                                    ExtensionBlocks extBlocks) const {
    ModuleOp module = op->getParentOfType<ModuleOp>();
    Location loc = op->getLoc();

    // Inputs
    Value A = op.a();
    Value B = op.b();
    Value mask = op.mask();
    bool isMaskComplement = op.mask_complement();

    // Types
    Type indexType = rewriter.getIndexType();
    Type int64Type = rewriter.getIntegerType(64);
    Type valueType =
        op.getResult().getType().dyn_cast<RankedTensorType>().getElementType();

    MemRefType memref1DI64Type = MemRefType::get({-1}, int64Type);
    MemRefType memref1DValueType = MemRefType::get({-1}, valueType);

    // Initial constants
    Value c0 = rewriter.create<arith::ConstantIndexOp>(loc, 0);
    Value c1 = rewriter.create<arith::ConstantIndexOp>(loc, 1);
    Value ci0 = rewriter.create<arith::ConstantIntOp>(loc, 0, int64Type);

    Value nrow = rewriter.create<graphblas::NumRowsOp>(loc, A);
    Value ncol = rewriter.create<graphblas::NumColsOp>(loc, B);
    Value nk = rewriter.create<graphblas::NumColsOp>(
        loc, A); // guaranteed equal to B.rows
    Value nrow_plus_one = rewriter.create<arith::AddIOp>(loc, nrow, c1);

    Value C = callEmptyLike(rewriter, module, loc, A);
    callResizeDim(rewriter, module, loc, C, c0, nrow);
    callResizeDim(rewriter, module, loc, C, c1, ncol);
    callResizePointers(rewriter, module, loc, C, c1, nrow_plus_one);

    Value Ap = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memref1DI64Type, A, c1);
    Value Aj = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memref1DI64Type,
                                                           A, c1);
    Value Ax =
        rewriter.create<sparse_tensor::ToValuesOp>(loc, memref1DValueType, A);
    Value Bp = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memref1DI64Type, B, c1);
    Value Bi = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memref1DI64Type,
                                                           B, c1);
    Value Bx =
        rewriter.create<sparse_tensor::ToValuesOp>(loc, memref1DValueType, B);
    Value Cp = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memref1DI64Type, C, c1);
    Value Mp, Mj;
    if (mask) {
      Mp = rewriter.create<sparse_tensor::ToPointersOp>(loc, memref1DI64Type,
                                                        mask, c1);
      Mj = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memref1DI64Type,
                                                       mask, c1);
    }

    // 1st pass
    //   Compute the number of nonzero entries per row.
    //   Store results in Cp
    //   The rows in A are the fixed elements, while the columns of B are the
    //   iteration element
    scf::ParallelOp rowLoop1 =
        rewriter.create<scf::ParallelOp>(loc, c0, nrow, c1);
    Value row = rowLoop1.getInductionVars().front();
    rewriter.setInsertionPointToStart(rowLoop1.getBody());

    Value colStart64 = rewriter.create<memref::LoadOp>(loc, Ap, row);
    Value rowPlus1 = rewriter.create<arith::AddIOp>(loc, row, c1);
    Value colEnd64 = rewriter.create<memref::LoadOp>(loc, Ap, rowPlus1);
    Value cmpColSame = rewriter.create<arith::CmpIOp>(
        loc, arith::CmpIPredicate::eq, colStart64, colEnd64);

    scf::IfOp ifBlock_rowTotal =
        rewriter.create<scf::IfOp>(loc, int64Type, cmpColSame, true);
    // if cmpColSame
    rewriter.setInsertionPointToStart(ifBlock_rowTotal.thenBlock());
    rewriter.create<scf::YieldOp>(loc, ci0);

    // else
    rewriter.setInsertionPointToStart(ifBlock_rowTotal.elseBlock());
    Value colStart =
        rewriter.create<arith::IndexCastOp>(loc, colStart64, indexType);
    Value colEnd =
        rewriter.create<arith::IndexCastOp>(loc, colEnd64, indexType);
    Value total;
    if (mask) {
      Value mcolStart64 = rewriter.create<memref::LoadOp>(loc, Mp, row);
      Value mcolEnd64 = rewriter.create<memref::LoadOp>(loc, Mp, rowPlus1);
      Value mcolStart =
          rewriter.create<arith::IndexCastOp>(loc, mcolStart64, indexType);
      Value mcolEnd =
          rewriter.create<arith::IndexCastOp>(loc, mcolEnd64, indexType);
      if (isMaskComplement) {
        ValueRange mcResult =
            buildMaskComplement(rewriter, loc, ncol, Mj, mcolStart, mcolEnd);
        Value maskComplement = mcResult[0];
        Value mcSize = mcResult[1];
        total = computeNumOverlaps(rewriter, loc, nk, Aj, colStart, colEnd, Bp,
                                   Bi, maskComplement, c0, mcSize, valueType);
        rewriter.create<memref::DeallocOp>(loc, maskComplement);
      } else {
        total = computeNumOverlaps(rewriter, loc, nk, Aj, colStart, colEnd, Bp,
                                   Bi, Mj, mcolStart, mcolEnd, valueType);
      }
    } else {
      total = computeNumOverlaps(rewriter, loc, nk, Aj, colStart, colEnd, Bp,
                                 Bi, nullptr, c0, ncol, valueType);
    }
    rewriter.create<scf::YieldOp>(loc, total);

    // end if cmpColSame
    rewriter.setInsertionPointAfter(ifBlock_rowTotal);
    Value rowTotal = ifBlock_rowTotal.getResult(0);
    rewriter.create<memref::StoreOp>(loc, rowTotal, Cp, row);

    // end row loop
    rewriter.setInsertionPointAfter(rowLoop1);

    // 2nd pass
    //   Compute the cumsum of values in Cp to build the final Cp
    //   Then resize C's indices and values
    scf::ForOp rowLoop2 = rewriter.create<scf::ForOp>(loc, c0, nrow, c1);
    Value cs_i = rowLoop2.getInductionVar();
    rewriter.setInsertionPointToStart(rowLoop2.getBody());

    Value csTemp = rewriter.create<memref::LoadOp>(loc, Cp, cs_i);
    Value cumsum = rewriter.create<memref::LoadOp>(loc, Cp, nrow);
    rewriter.create<memref::StoreOp>(loc, cumsum, Cp, cs_i);
    Value cumsum2 = rewriter.create<arith::AddIOp>(loc, cumsum, csTemp);
    rewriter.create<memref::StoreOp>(loc, cumsum2, Cp, nrow);

    // end row loop
    rewriter.setInsertionPointAfter(rowLoop2);

    Value nnz = rewriter.create<graphblas::NumValsOp>(loc, C);
    callResizeIndex(rewriter, module, loc, C, c1, nnz);
    callResizeValues(rewriter, module, loc, C, nnz);
    Value Cj = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memref1DI64Type,
                                                           C, c1);
    Value Cx =
        rewriter.create<sparse_tensor::ToValuesOp>(loc, memref1DValueType, C);

    // 3rd pass
    //   In parallel over the rows,
    //   compute the nonzero columns and associated values.
    //   Store in Cj and Cx
    //   The rows in A are the fixed elements, while the columns of B are the
    //   iteration element
    scf::ParallelOp rowLoop3 =
        rewriter.create<scf::ParallelOp>(loc, c0, nrow, c1);
    row = rowLoop3.getInductionVars().front();
    rewriter.setInsertionPointToStart(rowLoop3.getBody());

    rowPlus1 = rewriter.create<arith::AddIOp>(loc, row, c1);
    Value cpStart64 = rewriter.create<memref::LoadOp>(loc, Cp, row);
    Value cpEnd64 = rewriter.create<memref::LoadOp>(loc, Cp, rowPlus1);
    Value cmp_cpDifferent = rewriter.create<arith::CmpIOp>(
        loc, arith::CmpIPredicate::ne, cpStart64, cpEnd64);
    scf::IfOp ifBlock_cmpDiff =
        rewriter.create<scf::IfOp>(loc, cmp_cpDifferent);
    rewriter.setInsertionPointToStart(ifBlock_cmpDiff.thenBlock());

    Value baseIndex64 = rewriter.create<memref::LoadOp>(loc, Cp, row);
    Value baseIndex =
        rewriter.create<arith::IndexCastOp>(loc, baseIndex64, indexType);

    colStart64 = rewriter.create<memref::LoadOp>(loc, Ap, row);
    colEnd64 = rewriter.create<memref::LoadOp>(loc, Ap, rowPlus1);
    colStart = rewriter.create<arith::IndexCastOp>(loc, colStart64, indexType);
    colEnd = rewriter.create<arith::IndexCastOp>(loc, colEnd64, indexType);

    if (mask) {
      Value mcolStart64 = rewriter.create<memref::LoadOp>(loc, Mp, row);
      Value mcolEnd64 = rewriter.create<memref::LoadOp>(loc, Mp, rowPlus1);
      Value mcolStart =
          rewriter.create<arith::IndexCastOp>(loc, mcolStart64, indexType);
      Value mcolEnd =
          rewriter.create<arith::IndexCastOp>(loc, mcolEnd64, indexType);
      if (isMaskComplement) {
        ValueRange mcResult =
            buildMaskComplement(rewriter, loc, ncol, Mj, mcolStart, mcolEnd);
        Value maskComplement = mcResult[0];
        Value mcSize = mcResult[1];
        computeInnerProduct(rewriter, loc, nk, row, Aj, Ax, colStart, colEnd,
                            Bp, Bi, Bx, maskComplement, c0, mcSize, valueType,
                            extBlocks, Cj, Cx, baseIndex, false);
        rewriter.create<memref::DeallocOp>(loc, maskComplement);
      } else {
        computeInnerProduct(rewriter, loc, nk, row, Aj, Ax, colStart, colEnd,
                            Bp, Bi, Bx, Mj, mcolStart, mcolEnd, valueType,
                            extBlocks, Cj, Cx, baseIndex, false);
      }
    } else {
      computeInnerProduct(rewriter, loc, nk, row, Aj, Ax, colStart, colEnd, Bp,
                          Bi, Bx, nullptr, c0, ncol, valueType, extBlocks, Cj,
                          Cx, baseIndex, false);
    }

    // end if cmpDiff
    rewriter.setInsertionPointAfter(ifBlock_cmpDiff);

    // end row loop
    rewriter.setInsertionPointAfter(rowLoop3);

    rewriter.replaceOp(op, C);

    cleanupIntermediateTensor(rewriter, module, loc, C);

    return success();
  }

  LogicalResult
  rewriteMatrixVectorMultiplication(graphblas::MatrixMultiplyGenericOp op,
                                    PatternRewriter &rewriter,
                                    ExtensionBlocks extBlocks) const {
    ModuleOp module = op->getParentOfType<ModuleOp>();
    Location loc = op->getLoc();

    // Inputs
    Value A = op.a();
    Value B = op.b();
    Value mask = op.mask();
    bool isMaskComplement = op.mask_complement();

    // Types
    Type indexType = rewriter.getIndexType();
    Type int64Type = rewriter.getIntegerType(64);
    Type valueType =
        op.getResult().getType().dyn_cast<RankedTensorType>().getElementType();

    MemRefType memref1DI64Type = MemRefType::get({-1}, int64Type);
    MemRefType memref1DValueType = MemRefType::get({-1}, valueType);

    // Initial constants
    Value c0 = rewriter.create<arith::ConstantIndexOp>(loc, 0);
    Value c1 = rewriter.create<arith::ConstantIndexOp>(loc, 1);
    Value c2 = rewriter.create<arith::ConstantIndexOp>(loc, 2);
    Value ci0 = rewriter.create<arith::ConstantIntOp>(loc, 0, int64Type);

    Value size = rewriter.create<graphblas::NumRowsOp>(loc, A);
    Value nk = rewriter.create<graphblas::SizeOp>(loc, B);
    // TODO: how do I check nk == nk_check and raise an exception if they don't
    // match? Value nk_check = rewriter.create<graphblas::NumColsOp>(loc, A);

    Value C = callEmptyLike(rewriter, module, loc, B);
    callResizeDim(rewriter, module, loc, C, c0, size);
    callResizePointers(rewriter, module, loc, C, c0, c2);

    Value Ap = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memref1DI64Type, A, c1);
    Value Aj = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memref1DI64Type,
                                                           A, c1);
    Value Ax =
        rewriter.create<sparse_tensor::ToValuesOp>(loc, memref1DValueType, A);
    Value Bp = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memref1DI64Type, B, c0);
    Value Bi = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memref1DI64Type,
                                                           B, c0);
    Value Bx =
        rewriter.create<sparse_tensor::ToValuesOp>(loc, memref1DValueType, B);
    Value Cp = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memref1DI64Type, C, c0);
    Value Mp, Mi, maskStart, maskEnd;
    if (mask) {
      Mp = rewriter.create<sparse_tensor::ToPointersOp>(loc, memref1DI64Type,
                                                        mask, c0);
      Mi = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memref1DI64Type,
                                                       mask, c0);
      Value maskStart64 = rewriter.create<memref::LoadOp>(loc, Mp, c0);
      Value maskEnd64 = rewriter.create<memref::LoadOp>(loc, Mp, c1);
      maskStart =
          rewriter.create<arith::IndexCastOp>(loc, maskStart64, indexType);
      maskEnd = rewriter.create<arith::IndexCastOp>(loc, maskEnd64, indexType);
    }

    // 1st pass
    //   Compute the number of nonzero entries in the result
    //   Store results in Cp
    //   The vector B is the fixed element, while the rows of A are the
    //   iteration element
    Value fixedIndexEnd64 = rewriter.create<memref::LoadOp>(loc, Bp, c1);
    Value fixedIndexEnd =
        rewriter.create<arith::IndexCastOp>(loc, fixedIndexEnd64, indexType);
    Value cmpColSame = rewriter.create<arith::CmpIOp>(
        loc, arith::CmpIPredicate::eq, c0, fixedIndexEnd);

    scf::IfOp ifBlock_rowTotal =
        rewriter.create<scf::IfOp>(loc, int64Type, cmpColSame, true);
    // if cmpColSame
    rewriter.setInsertionPointToStart(ifBlock_rowTotal.thenBlock());
    rewriter.create<scf::YieldOp>(loc, ci0);

    // else
    rewriter.setInsertionPointToStart(ifBlock_rowTotal.elseBlock());
    Value total;
    if (mask) {
      if (isMaskComplement) {
        ValueRange mcResult =
            buildMaskComplement(rewriter, loc, size, Mi, maskStart, maskEnd);
        Value maskComplement = mcResult[0];
        Value mcSize = mcResult[1];
        total = computeNumOverlaps(rewriter, loc, nk, Bi, c0, fixedIndexEnd, Ap,
                                   Aj, maskComplement, c0, mcSize, valueType);
        rewriter.create<memref::DeallocOp>(loc, maskComplement);
      } else {
        total = computeNumOverlaps(rewriter, loc, nk, Bi, c0, fixedIndexEnd, Ap,
                                   Aj, Mi, maskStart, maskEnd, valueType);
      }
    } else {
      total = computeNumOverlaps(rewriter, loc, nk, Bi, c0, fixedIndexEnd, Ap,
                                 Aj, nullptr, c0, size, valueType);
    }
    rewriter.create<scf::YieldOp>(loc, total);

    // end if cmpColSame
    rewriter.setInsertionPointAfter(ifBlock_rowTotal);
    Value nnzTotal = ifBlock_rowTotal.getResult(0);
    Value nnz = rewriter.create<arith::IndexCastOp>(loc, nnzTotal, indexType);
    rewriter.create<memref::StoreOp>(loc, nnzTotal, Cp, c1);

    callResizeIndex(rewriter, module, loc, C, c0, nnz);
    callResizeValues(rewriter, module, loc, C, nnz);
    Value Ci = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memref1DI64Type,
                                                           C, c0);
    Value Cx =
        rewriter.create<sparse_tensor::ToValuesOp>(loc, memref1DValueType, C);

    // 2nd pass
    //   Compute the nonzero values.
    //   Store in Ci and Cx
    //   The vector B is the fixed element, while the rows of A are the
    //   iteration element
    Value cmp_cpDifferent =
        rewriter.create<arith::CmpIOp>(loc, arith::CmpIPredicate::ne, c0, nnz);
    scf::IfOp ifBlock_cmpDiff =
        rewriter.create<scf::IfOp>(loc, cmp_cpDifferent);
    rewriter.setInsertionPointToStart(ifBlock_cmpDiff.thenBlock());

    if (mask) {
      if (isMaskComplement) {
        ValueRange mcResult =
            buildMaskComplement(rewriter, loc, size, Mi, maskStart, maskEnd);
        Value maskComplement = mcResult[0];
        Value mcSize = mcResult[1];
        computeInnerProduct(rewriter, loc, nk, c0, Bi, Bx, c0, fixedIndexEnd,
                            Ap, Aj, Ax, maskComplement, c0, mcSize, valueType,
                            extBlocks, Ci, Cx, c0, true);
        rewriter.create<memref::DeallocOp>(loc, maskComplement);
      } else {
        computeInnerProduct(rewriter, loc, nk, c0, Bi, Bx, c0, fixedIndexEnd,
                            Ap, Aj, Ax, Mi, maskStart, maskEnd, valueType,
                            extBlocks, Ci, Cx, c0, true);
      }
    } else {
      computeInnerProduct(rewriter, loc, nk, c0, Bi, Bx, c0, fixedIndexEnd, Ap,
                          Aj, Ax, nullptr, c0, size, valueType, extBlocks, Ci,
                          Cx, c0, true);
    }

    // end if cmpDiff
    rewriter.setInsertionPointAfter(ifBlock_cmpDiff);

    rewriter.replaceOp(op, C);

    cleanupIntermediateTensor(rewriter, module, loc, C);

    return success();
  }

  LogicalResult
  rewriteVectorMatrixMultiplication(graphblas::MatrixMultiplyGenericOp op,
                                    PatternRewriter &rewriter,
                                    ExtensionBlocks extBlocks) const {
    ModuleOp module = op->getParentOfType<ModuleOp>();
    Location loc = op->getLoc();

    // Inputs
    Value A = op.a();
    Value B = op.b();
    Value mask = op.mask();
    bool isMaskComplement = op.mask_complement();

    // Types
    Type indexType = rewriter.getIndexType();
    Type int64Type = rewriter.getIntegerType(64);
    Type valueType =
        op.getResult().getType().dyn_cast<RankedTensorType>().getElementType();

    MemRefType memref1DI64Type = MemRefType::get({-1}, int64Type);
    MemRefType memref1DValueType = MemRefType::get({-1}, valueType);

    // Initial constants
    Value c0 = rewriter.create<arith::ConstantIndexOp>(loc, 0);
    Value c1 = rewriter.create<arith::ConstantIndexOp>(loc, 1);
    Value c2 = rewriter.create<arith::ConstantIndexOp>(loc, 2);
    Value ci0 = rewriter.create<arith::ConstantIntOp>(loc, 0, int64Type);

    Value size = rewriter.create<graphblas::NumColsOp>(loc, B);
    Value nk = rewriter.create<graphblas::SizeOp>(
        loc, A); // guaranteed equal to B.rows

    Value C = callEmptyLike(rewriter, module, loc, A);
    callResizeDim(rewriter, module, loc, C, c0, size);
    callResizePointers(rewriter, module, loc, C, c0, c2);

    Value Ap = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memref1DI64Type, A, c0);
    Value Ai = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memref1DI64Type,
                                                           A, c0);
    Value Ax =
        rewriter.create<sparse_tensor::ToValuesOp>(loc, memref1DValueType, A);
    Value Bp = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memref1DI64Type, B, c1);
    Value Bi = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memref1DI64Type,
                                                           B, c1);
    Value Bx =
        rewriter.create<sparse_tensor::ToValuesOp>(loc, memref1DValueType, B);
    Value Cp = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memref1DI64Type, C, c0);
    Value Mp, Mi, maskStart, maskEnd;
    if (mask) {
      Mp = rewriter.create<sparse_tensor::ToPointersOp>(loc, memref1DI64Type,
                                                        mask, c0);
      Mi = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memref1DI64Type,
                                                       mask, c0);
      Value maskStart64 = rewriter.create<memref::LoadOp>(loc, Mp, c0);
      Value maskEnd64 = rewriter.create<memref::LoadOp>(loc, Mp, c1);
      maskStart =
          rewriter.create<arith::IndexCastOp>(loc, maskStart64, indexType);
      maskEnd = rewriter.create<arith::IndexCastOp>(loc, maskEnd64, indexType);
    }

    // 1st pass
    //   Compute the number of nonzero entries in the result
    //   Store results in Cp
    //   The vector A is the fixed element, while the columns of B are the
    //   iteration element
    Value fixedIndexEnd64 = rewriter.create<memref::LoadOp>(loc, Ap, c1);
    Value fixedIndexEnd =
        rewriter.create<arith::IndexCastOp>(loc, fixedIndexEnd64, indexType);
    Value cmpColSame = rewriter.create<arith::CmpIOp>(
        loc, arith::CmpIPredicate::eq, c0, fixedIndexEnd);

    scf::IfOp ifBlock_rowTotal =
        rewriter.create<scf::IfOp>(loc, int64Type, cmpColSame, true);
    // if cmpColSame
    rewriter.setInsertionPointToStart(ifBlock_rowTotal.thenBlock());
    rewriter.create<scf::YieldOp>(loc, ci0);

    // else
    rewriter.setInsertionPointToStart(ifBlock_rowTotal.elseBlock());
    Value total;
    if (mask) {
      if (isMaskComplement) {
        ValueRange mcResult =
            buildMaskComplement(rewriter, loc, size, Mi, maskStart, maskEnd);
        Value maskComplement = mcResult[0];
        Value mcSize = mcResult[1];
        total = computeNumOverlaps(rewriter, loc, nk, Ai, c0, fixedIndexEnd, Bp,
                                   Bi, maskComplement, c0, mcSize, valueType);
        rewriter.create<memref::DeallocOp>(loc, maskComplement);
      } else {
        total = computeNumOverlaps(rewriter, loc, nk, Ai, c0, fixedIndexEnd, Bp,
                                   Bi, Mi, maskStart, maskEnd, valueType);
      }
    } else {
      total = computeNumOverlaps(rewriter, loc, nk, Ai, c0, fixedIndexEnd, Bp,
                                 Bi, nullptr, c0, size, valueType);
    }
    rewriter.create<scf::YieldOp>(loc, total);

    // end if cmpColSame
    rewriter.setInsertionPointAfter(ifBlock_rowTotal);
    Value nnzTotal = ifBlock_rowTotal.getResult(0);
    Value nnz = rewriter.create<arith::IndexCastOp>(loc, nnzTotal, indexType);
    rewriter.create<memref::StoreOp>(loc, nnzTotal, Cp, c1);

    callResizeIndex(rewriter, module, loc, C, c0, nnz);
    callResizeValues(rewriter, module, loc, C, nnz);
    Value Ci = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memref1DI64Type,
                                                           C, c0);
    Value Cx =
        rewriter.create<sparse_tensor::ToValuesOp>(loc, memref1DValueType, C);

    // 2nd pass
    //   Compute the nonzero values.
    //   Store in Ci and Cx
    //   The vector A is the fixed element, while the columns of B are the
    //   iteration element
    Value cmp_cpDifferent =
        rewriter.create<arith::CmpIOp>(loc, arith::CmpIPredicate::ne, c0, nnz);
    scf::IfOp ifBlock_cmpDiff =
        rewriter.create<scf::IfOp>(loc, cmp_cpDifferent);
    rewriter.setInsertionPointToStart(ifBlock_cmpDiff.thenBlock());

    if (mask) {
      if (isMaskComplement) {
        ValueRange mcResult =
            buildMaskComplement(rewriter, loc, size, Mi, maskStart, maskEnd);
        Value maskComplement = mcResult[0];
        Value mcSize = mcResult[1];
        computeInnerProduct(rewriter, loc, nk, c0, Ai, Ax, c0, fixedIndexEnd,
                            Bp, Bi, Bx, maskComplement, c0, mcSize, valueType,
                            extBlocks, Ci, Cx, c0, false);
        rewriter.create<memref::DeallocOp>(loc, maskComplement);
      } else {
        computeInnerProduct(rewriter, loc, nk, c0, Ai, Ax, c0, fixedIndexEnd,
                            Bp, Bi, Bx, Mi, maskStart, maskEnd, valueType,
                            extBlocks, Ci, Cx, c0, false);
      }
    } else {
      computeInnerProduct(rewriter, loc, nk, c0, Ai, Ax, c0, fixedIndexEnd, Bp,
                          Bi, Bx, nullptr, c0, size, valueType, extBlocks, Ci,
                          Cx, c0, false);
    }

    // end if cmpDiff
    rewriter.setInsertionPointAfter(ifBlock_cmpDiff);

    rewriter.replaceOp(op, C);

    cleanupIntermediateTensor(rewriter, module, loc, C);

    return success();
  }

  LogicalResult
  rewriteVectorVectorMultiplication(graphblas::MatrixMultiplyGenericOp op,
                                    PatternRewriter &rewriter,
                                    ExtensionBlocks extBlocks) const {
    ModuleOp module = op->getParentOfType<ModuleOp>();
    Location loc = op->getLoc();

    // Inputs
    Value A = op.a();
    Value B = op.b();

    // Types
    Type indexType = rewriter.getIndexType();
    Type int64Type = rewriter.getIntegerType(64);
    Type valueType = A.getType().dyn_cast<RankedTensorType>().getElementType();

    MemRefType memref1DI64Type = MemRefType::get({-1}, int64Type);
    MemRefType memref1DValueType = MemRefType::get({-1}, valueType);

    // Initial constants
    Value c0 = rewriter.create<arith::ConstantIndexOp>(loc, 0);
    Value c1 = rewriter.create<arith::ConstantIndexOp>(loc, 1);
    Value c2 = rewriter.create<arith::ConstantIndexOp>(loc, 2);

    Value size = rewriter.create<graphblas::SizeOp>(loc, A);

    Value C = callEmptyLike(rewriter, module, loc, A);
    callResizeDim(
        rewriter, module, loc, C, c0,
        c1); // exactly one entry because this is a vector representing a scalar
    callResizePointers(rewriter, module, loc, C, c0, c2);
    callResizeIndex(rewriter, module, loc, C, c0, c1);
    callResizeValues(rewriter, module, loc, C, c1);

    Value Ap = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memref1DI64Type, A, c0);
    Value Ai = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memref1DI64Type,
                                                           A, c0);
    Value Ax =
        rewriter.create<sparse_tensor::ToValuesOp>(loc, memref1DValueType, A);
    Value Bp = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memref1DI64Type, B, c0);
    Value Bi = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memref1DI64Type,
                                                           B, c0);
    Value Bx =
        rewriter.create<sparse_tensor::ToValuesOp>(loc, memref1DValueType, B);
    Value Ci = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memref1DI64Type,
                                                           C, c0);
    Value Cx =
        rewriter.create<sparse_tensor::ToValuesOp>(loc, memref1DValueType, C);

    // Single pass
    //   Compute the nonzero values.
    //   Store in Ci and Cx (single-element vector representing a scalar)
    //   The vector A is the fixed element, while the vector B is treated as the
    //   iteration element
    Value fixedIndexEnd64 = rewriter.create<memref::LoadOp>(loc, Ap, c1);
    Value fixedIndexEnd =
        rewriter.create<arith::IndexCastOp>(loc, fixedIndexEnd64, indexType);

    computeInnerProduct(rewriter, loc, size, c0, Ai, Ax, c0, fixedIndexEnd, Bp,
                        Bi, Bx, nullptr, c0, c1, valueType, extBlocks, Ci, Cx,
                        c0, false);

    // extract scalar from C
    Value cScalar = rewriter.create<memref::LoadOp>(loc, Cx, c0);

    rewriter.replaceOp(op, cScalar);

    cleanupIntermediateTensor(rewriter, module, loc, C);

    return success();
  }
};

class MatrixMultiplyReduceToScalarGenericDWIMFirstArgRewrite
    : public OpRewritePattern<
          graphblas::MatrixMultiplyReduceToScalarGenericOp> {
public:
  using OpRewritePattern<
      graphblas::MatrixMultiplyReduceToScalarGenericOp>::OpRewritePattern;

  LogicalResult
  matchAndRewrite(graphblas::MatrixMultiplyReduceToScalarGenericOp op,
                  PatternRewriter &rewriter) const override {
    if (!MatrixMultiplyGenericDWIMFirstArgRewrite::needsDWIM(op))
      return failure();

    MLIRContext *context = op.getContext();
    Location loc = op->getLoc();
    Value A = op.a();
    RankedTensorType aType = A.getType().cast<RankedTensorType>();
    RankedTensorType flippedMatrixType = getFlippedLayoutType(context, aType);

    rewriter.setInsertionPoint(op);
    Value flippedA =
        rewriter.create<graphblas::ConvertLayoutOp>(loc, flippedMatrixType, A);
    op.aMutable().assign(flippedA);

    return success();
  };
};

class MatrixMultiplyReduceToScalarGenericDWIMSecondArgRewrite
    : public OpRewritePattern<
          graphblas::MatrixMultiplyReduceToScalarGenericOp> {
public:
  using OpRewritePattern<
      graphblas::MatrixMultiplyReduceToScalarGenericOp>::OpRewritePattern;

  LogicalResult
  matchAndRewrite(graphblas::MatrixMultiplyReduceToScalarGenericOp op,
                  PatternRewriter &rewriter) const override {
    if (!MatrixMultiplyGenericDWIMSecondArgRewrite::needsDWIM(op))
      return failure();

    MLIRContext *context = op.getContext();
    Location loc = op->getLoc();
    Value B = op.b();
    RankedTensorType bType = B.getType().cast<RankedTensorType>();
    RankedTensorType flippedMatrixType = getFlippedLayoutType(context, bType);

    rewriter.setInsertionPoint(op);
    Value flippedB =
        rewriter.create<graphblas::ConvertLayoutOp>(loc, flippedMatrixType, B);
    op.bMutable().assign(flippedB);

    return success();
  };
};

class MatrixMultiplyReduceToScalarGenericDWIMMaskRewrite
    : public OpRewritePattern<
          graphblas::MatrixMultiplyReduceToScalarGenericOp> {
public:
  using OpRewritePattern<
      graphblas::MatrixMultiplyReduceToScalarGenericOp>::OpRewritePattern;

  LogicalResult
  matchAndRewrite(graphblas::MatrixMultiplyReduceToScalarGenericOp op,
                  PatternRewriter &rewriter) const override {
    if (!MatrixMultiplyGenericDWIMMaskRewrite::needsDWIM(op))
      return failure();

    MLIRContext *context = op.getContext();
    Location loc = op->getLoc();

    Value mask = op.mask();
    RankedTensorType maskType = mask.getType().cast<RankedTensorType>();
    RankedTensorType flippedMatrixType =
        getFlippedLayoutType(context, maskType);

    rewriter.setInsertionPoint(op);
    Value flippedMask = rewriter.create<graphblas::ConvertLayoutOp>(
        loc, flippedMatrixType, mask);
    op.maskMutable().assign(flippedMask);

    return success();
  };
};

class LowerMatrixMultiplyReduceToScalarGenericRewrite
    : public OpRewritePattern<
          graphblas::MatrixMultiplyReduceToScalarGenericOp> {
public:
  using OpRewritePattern<
      graphblas::MatrixMultiplyReduceToScalarGenericOp>::OpRewritePattern;
  LogicalResult
  matchAndRewrite(graphblas::MatrixMultiplyReduceToScalarGenericOp op,
                  PatternRewriter &rewriter) const override {
    if (MatrixMultiplyGenericDWIMFirstArgRewrite::needsDWIM(op) ||
        MatrixMultiplyGenericDWIMSecondArgRewrite::needsDWIM(op) ||
        MatrixMultiplyGenericDWIMMaskRewrite::needsDWIM(op))
      return failure();

    ModuleOp module = op->getParentOfType<ModuleOp>(); /* ignore unused variable
                                                          for debugging */
    (void)module;
    Location loc = op->getLoc();

    // Inputs
    Value A = op.a();
    Value B = op.b();
    Value mask = op.mask();

    // Required blocks
    RegionRange extensions = op.extensions();
    ExtensionBlocks extBlocks;
    std::set<graphblas::YieldKind> required = {
        graphblas::YieldKind::ADD_IDENTITY, graphblas::YieldKind::ADD,
        graphblas::YieldKind::MULT, graphblas::YieldKind::AGG_IDENTITY,
        graphblas::YieldKind::AGG};
    std::set<graphblas::YieldKind> optional = {};
    LogicalResult extractResult =
        extBlocks.extractBlocks(op, extensions, required, optional);

    if (extractResult.failed()) {
      return extractResult;
    }

    // Types
    Type indexType = rewriter.getIndexType();
    Type int64Type = rewriter.getIntegerType(64);
    Type boolType = rewriter.getI1Type();
    Type valueType = A.getType().dyn_cast<RankedTensorType>().getElementType();

    MemRefType memref1DI64Type = MemRefType::get({-1}, int64Type);
    MemRefType memref1DBoolType = MemRefType::get({-1}, boolType);
    MemRefType memref1DValueType = MemRefType::get({-1}, valueType);

    // Initial constants
    Value c0 = rewriter.create<arith::ConstantIndexOp>(loc, 0);
    Value c1 = rewriter.create<arith::ConstantIndexOp>(loc, 1);
    // TODO: make cf0 value dependent on the aggregator
    Value cf0 = llvm::TypeSwitch<Type, Value>(valueType)
                    .Case<IntegerType>([&](IntegerType type) {
                      return rewriter.create<arith::ConstantIntOp>(
                          loc, 0, type.getWidth());
                    })
                    .Case<FloatType>([&](FloatType type) {
                      return rewriter.create<arith::ConstantFloatOp>(
                          loc, APFloat(0.0), type);
                    });
    Value ctrue = rewriter.create<arith::ConstantIntOp>(loc, 1, boolType);
    Value cfalse = rewriter.create<arith::ConstantIntOp>(loc, 0, boolType);

    // Get sparse tensor info
    Value Ap = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memref1DI64Type, A, c1);
    Value Aj = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memref1DI64Type,
                                                           A, c1);
    Value Ax =
        rewriter.create<sparse_tensor::ToValuesOp>(loc, memref1DValueType, A);
    Value Bp = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memref1DI64Type, B, c1);
    Value Bi = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memref1DI64Type,
                                                           B, c1);
    Value Bx =
        rewriter.create<sparse_tensor::ToValuesOp>(loc, memref1DValueType, B);

    Value nrow = rewriter.create<graphblas::NumRowsOp>(loc, A);
    Value ncol = rewriter.create<graphblas::NumColsOp>(loc, B);
    Value nk = rewriter.create<graphblas::NumColsOp>(
        loc, A); // guaranteed equal to B.rows

    Value Mp, Mj;
    if (mask) {
      Mp = rewriter.create<sparse_tensor::ToPointersOp>(loc, memref1DI64Type,
                                                        mask, c1);
      Mj = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memref1DI64Type,
                                                       mask, c1);
    }

    // In parallel over the rows and columns,
    //   compute the nonzero values and accumulate
    scf::ParallelOp rowLoop =
        rewriter.create<scf::ParallelOp>(loc, c0, nrow, c1, cf0);
    Value row = rowLoop.getInductionVars().front();
    rewriter.setInsertionPointToStart(rowLoop.getBody());

    Value rowPlus1 = rewriter.create<arith::AddIOp>(loc, row, c1);
    Value apStart64 = rewriter.create<memref::LoadOp>(loc, Ap, row);
    Value apEnd64 = rewriter.create<memref::LoadOp>(loc, Ap, rowPlus1);
    Value cmp_cpSame = rewriter.create<arith::CmpIOp>(
        loc, arith::CmpIPredicate::eq, apStart64, apEnd64);

    scf::IfOp ifBlock_cmpSame =
        rewriter.create<scf::IfOp>(loc, valueType, cmp_cpSame, true);
    // if cmpSame
    rewriter.setInsertionPointToStart(ifBlock_cmpSame.thenBlock());
    rewriter.create<scf::YieldOp>(loc, cf0);

    // else
    rewriter.setInsertionPointToStart(ifBlock_cmpSame.elseBlock());

    // Construct a dense array of row values
    Value colStart =
        rewriter.create<arith::IndexCastOp>(loc, apStart64, indexType);
    Value colEnd = rewriter.create<arith::IndexCastOp>(loc, apEnd64, indexType);
    Value kvec = rewriter.create<memref::AllocOp>(loc, memref1DValueType, nk);
    Value kvec_i1 = rewriter.create<memref::AllocOp>(loc, memref1DBoolType, nk);
    rewriter.create<linalg::FillOp>(loc, cfalse, kvec_i1);

    scf::ParallelOp colLoop1 =
        rewriter.create<scf::ParallelOp>(loc, colStart, colEnd, c1);
    Value jj = colLoop1.getInductionVars().front();
    rewriter.setInsertionPointToStart(colLoop1.getBody());
    Value col64 = rewriter.create<memref::LoadOp>(loc, Aj, jj);
    Value col = rewriter.create<arith::IndexCastOp>(loc, col64, indexType);
    rewriter.create<memref::StoreOp>(loc, ctrue, kvec_i1, col);
    Value val = rewriter.create<memref::LoadOp>(loc, Ax, jj);
    rewriter.create<memref::StoreOp>(loc, val, kvec, col);

    // end col loop 1
    rewriter.setInsertionPointAfter(colLoop1);

    // Loop thru all columns of B; accumulate values
    scf::ParallelOp colLoop2;
    if (mask) {
      Value mcolStart64 = rewriter.create<memref::LoadOp>(loc, Mp, row);
      Value mcolEnd64 = rewriter.create<memref::LoadOp>(loc, Mp, rowPlus1);
      Value mcolStart =
          rewriter.create<arith::IndexCastOp>(loc, mcolStart64, indexType);
      Value mcolEnd =
          rewriter.create<arith::IndexCastOp>(loc, mcolEnd64, indexType);

      colLoop2 =
          rewriter.create<scf::ParallelOp>(loc, mcolStart, mcolEnd, c1, cf0);
      Value mm = colLoop2.getInductionVars().front();
      rewriter.setInsertionPointToStart(colLoop2.getBody());
      col64 = rewriter.create<memref::LoadOp>(loc, Mj, mm);
      col = rewriter.create<arith::IndexCastOp>(loc, col64, indexType);
    } else {
      colLoop2 = rewriter.create<scf::ParallelOp>(loc, c0, ncol, c1, cf0);
      col = colLoop2.getInductionVars().front();
      rewriter.setInsertionPointToStart(colLoop2.getBody());
      col64 = rewriter.create<arith::IndexCastOp>(loc, col, int64Type);
    }

    Value colPlus1 = rewriter.create<arith::AddIOp>(loc, col, c1);
    Value iStart64 = rewriter.create<memref::LoadOp>(loc, Bp, col);
    Value iEnd64 = rewriter.create<memref::LoadOp>(loc, Bp, colPlus1);
    Value iStart =
        rewriter.create<arith::IndexCastOp>(loc, iStart64, indexType);
    Value iEnd = rewriter.create<arith::IndexCastOp>(loc, iEnd64, indexType);

    // insert add identity block
    rewriter.mergeBlocks(extBlocks.addIdentity, rewriter.getBlock(), {});
    graphblas::YieldOp addIdentityYield =
        llvm::dyn_cast_or_null<graphblas::YieldOp>(
            rewriter.getBlock()->getTerminator());
    Value addIdentity = addIdentityYield.values().front();
    rewriter.eraseOp(addIdentityYield);

    scf::ForOp kLoop =
        rewriter.create<scf::ForOp>(loc, iStart, iEnd, c1, addIdentity);
    Value ii = kLoop.getInductionVar();
    Value curr = kLoop.getLoopBody().getArgument(1);
    rewriter.setInsertionPointToStart(kLoop.getBody());

    Value kk64 = rewriter.create<memref::LoadOp>(loc, Bi, ii);
    Value kk = rewriter.create<arith::IndexCastOp>(loc, kk64, indexType);
    Value cmpPair = rewriter.create<memref::LoadOp>(loc, kvec_i1, kk);
    scf::IfOp ifBlock_cmpPair =
        rewriter.create<scf::IfOp>(loc, valueType, cmpPair, true);
    // if cmpPair
    rewriter.setInsertionPointToStart(ifBlock_cmpPair.thenBlock());

    Value aVal = rewriter.create<memref::LoadOp>(loc, kvec, kk);
    Value bVal = rewriter.create<memref::LoadOp>(loc, Bx, ii);

    // insert multiply operation block
    ValueRange injectVals = ValueRange{aVal, bVal, row, col, kk};
    rewriter.mergeBlocks(
        extBlocks.mult, rewriter.getBlock(),
        injectVals.slice(0, extBlocks.mult->getArguments().size()));
    graphblas::YieldOp multYield = llvm::dyn_cast_or_null<graphblas::YieldOp>(
        rewriter.getBlock()->getTerminator());
    Value multResult = multYield.values().front();
    rewriter.eraseOp(multYield);

    // insert add operation block
    rewriter.mergeBlocks(extBlocks.add, rewriter.getBlock(),
                         {curr, multResult});
    graphblas::YieldOp addYield = llvm::dyn_cast_or_null<graphblas::YieldOp>(
        rewriter.getBlock()->getTerminator());
    Value addResult = addYield.values().front();
    rewriter.eraseOp(addYield);

    rewriter.create<scf::YieldOp>(loc, addResult);

    // else
    rewriter.setInsertionPointToStart(ifBlock_cmpPair.elseBlock());
    rewriter.create<scf::YieldOp>(loc, curr);

    // end if cmpPair
    rewriter.setInsertionPointAfter(ifBlock_cmpPair);
    Value newCurr = ifBlock_cmpPair.getResult(0);
    rewriter.create<scf::YieldOp>(loc, newCurr);

    // end k loop
    rewriter.setInsertionPointAfter(kLoop);

    Value colVal = kLoop.getResult(0);

    // FIXME: this is where transform_out goes

    scf::ReduceOp colReducer = rewriter.create<scf::ReduceOp>(loc, colVal);
    BlockArgument lhs = colReducer.getRegion().getArgument(0);
    BlockArgument rhs = colReducer.getRegion().getArgument(1);

    rewriter.setInsertionPointToStart(&colReducer.getRegion().front());

    Region *aggRegion = extBlocks.agg->getParent();
    BlockAndValueMapping mapper;
    // Clone blocks into front of region to displace existing entry block, which
    // will be removed by canonicalization later
    aggRegion->cloneInto(&colReducer.getRegion(),
                         colReducer.getRegion().begin(), mapper);
    graphblas::YieldOp colYield = llvm::dyn_cast_or_null<graphblas::YieldOp>(
        colReducer.getRegion().front().getTerminator());
    Value colAggResult = colYield.values().front();
    rewriter.setInsertionPointAfter(colYield);
    rewriter.create<scf::ReduceReturnOp>(loc, colAggResult);
    rewriter.eraseOp(colYield);

    rewriter.setInsertionPointAfter(colReducer);

    // end col loop 2
    rewriter.setInsertionPointAfter(colLoop2);

    Value subtotal = colLoop2.getResult(0);
    rewriter.create<memref::DeallocOp>(loc, kvec);
    rewriter.create<memref::DeallocOp>(loc, kvec_i1);
    rewriter.create<scf::YieldOp>(loc, subtotal);

    // end if cmpSame
    rewriter.setInsertionPointAfter(ifBlock_cmpSame);

    Value rowTotal = ifBlock_cmpSame.getResult(0);

    scf::ReduceOp rowReducer = rewriter.create<scf::ReduceOp>(loc, rowTotal);
    lhs = rowReducer.getRegion().getArgument(0);
    rhs = rowReducer.getRegion().getArgument(1);

    rewriter.setInsertionPointToStart(&rowReducer.getRegion().front());

    graphblas::YieldOp yield = llvm::dyn_cast_or_null<graphblas::YieldOp>(
        extBlocks.agg->getTerminator());
    Value aggResult = yield.values().front();

    // we can safely merge this agg block now, since the previous agg instance
    // was cloned above
    rewriter.mergeBlocks(extBlocks.agg, rewriter.getBlock(), {lhs, rhs});
    rewriter.create<scf::ReduceReturnOp>(loc, aggResult);
    rewriter.eraseOp(yield);

    // end row loop
    rewriter.setInsertionPointAfter(rowLoop);

    Value total = rowLoop.getResult(0);

    rewriter.replaceOp(op, total);

    return success();
  };
};

class LowerUnionRewrite : public OpRewritePattern<graphblas::UnionOp> {
public:
  using OpRewritePattern<graphblas::UnionOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::UnionOp op,
                                PatternRewriter &rewriter) const override {
    Location loc = op->getLoc();

    Value a = op.a();
    Value b = op.b();
    Value mask = op.mask();
    Type valueType = a.getType().cast<RankedTensorType>().getElementType();

    if (mask) {
      NamedAttrList attributes = {};
      attributes.append(StringRef("mask_complement"),
                        rewriter.getBoolAttr(op.mask_complement()));
      a = rewriter.create<graphblas::SelectMaskOp>(
          loc, a.getType(), ValueRange{a, mask}, attributes.getAttrs());
      b = rewriter.create<graphblas::SelectMaskOp>(
          loc, b.getType(), ValueRange{b, mask}, attributes.getAttrs());
    }

    // New op
    NamedAttrList attributes = {};
    graphblas::UnionGenericOp newUnionOp =
        rewriter.create<graphblas::UnionGenericOp>(loc, op->getResultTypes(),
                                                   ValueRange{a, b},
                                                   attributes.getAttrs(), 1);

    if (failed(populateBinary(rewriter, loc, op.union_operator(), valueType,
                              newUnionOp.getRegions().slice(0, 1),
                              graphblas::YieldKind::MULT)))
      return failure();

    rewriter.setInsertionPointAfter(newUnionOp);

    rewriter.replaceOp(op, newUnionOp.getResult());

    return success();
  };
};

class LowerUnionGenericRewrite
    : public OpRewritePattern<graphblas::UnionGenericOp> {
public:
  using OpRewritePattern<graphblas::UnionGenericOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::UnionGenericOp op,
                                PatternRewriter &rewriter) const override {
    ModuleOp module = op->getParentOfType<ModuleOp>();
    Location loc = op->getLoc();

    // Inputs
    Value a = op.a();
    Value b = op.b();

    // Required block
    RegionRange extensions = op.extensions();
    ExtensionBlocks extBlocks;
    std::set<graphblas::YieldKind> required = {graphblas::YieldKind::MULT};
    LogicalResult extractResult =
        extBlocks.extractBlocks(op, extensions, required, {});

    if (extractResult.failed()) {
      return extractResult;
    }

    // Types
    RankedTensorType aType = a.getType().dyn_cast<RankedTensorType>();

    unsigned rank = aType.getRank(); // ranks guaranteed to be equal

    Type outputType =
        op.getResult().getType().dyn_cast<RankedTensorType>().getElementType();
    Value output = callEmptyLike(rewriter, module, loc, a, outputType);
    if (rank == 2) {
      computeMatrixElementWise(rewriter, loc, module, a, b, output,
                               extBlocks.mult, UNION);
    } else {
      computeVectorElementWise(rewriter, loc, module, a, b, output,
                               extBlocks.mult, UNION);
    }

    rewriter.replaceOp(op, output);

    cleanupIntermediateTensor(rewriter, module, loc, output);

    return success();
  };
};

class LowerIntersectRewrite : public OpRewritePattern<graphblas::IntersectOp> {
public:
  using OpRewritePattern<graphblas::IntersectOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::IntersectOp op,
                                PatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    Value a = op.a();
    Value b = op.b();
    Value mask = op.mask();
    Type valueType = a.getType().cast<RankedTensorType>().getElementType();
    StringRef opstr = op.intersect_operator();

    if (mask) {
      NamedAttrList attributes = {};
      attributes.append(StringRef("mask_complement"),
                        rewriter.getBoolAttr(op.mask_complement()));
      a = rewriter.create<graphblas::SelectMaskOp>(
          loc, a.getType(), ValueRange{a, mask}, attributes.getAttrs());
      b = rewriter.create<graphblas::SelectMaskOp>(
          loc, b.getType(), ValueRange{b, mask}, attributes.getAttrs());
    }

    // Special handling for "first" and "second"
    if (opstr == "first") {
      graphblas::SelectMaskOp newIntersectOp =
          rewriter.create<graphblas::SelectMaskOp>(loc, op->getResultTypes(),
                                                   ValueRange{a, b});
      rewriter.replaceOp(op, newIntersectOp.getResult());
    } else if (opstr == "second") {
      graphblas::SelectMaskOp newIntersectOp =
          rewriter.create<graphblas::SelectMaskOp>(loc, op->getResultTypes(),
                                                   ValueRange{b, a});
      rewriter.replaceOp(op, newIntersectOp.getResult());
    } else {
      // New op
      NamedAttrList attributes = {};
      graphblas::IntersectGenericOp newIntersectOp =
          rewriter.create<graphblas::IntersectGenericOp>(
              loc, op->getResultTypes(), ValueRange{a, b},
              attributes.getAttrs(), 1);

      if (failed(populateBinary(rewriter, loc, op.intersect_operator(),
                                valueType,
                                newIntersectOp.getRegions().slice(0, 1),
                                graphblas::YieldKind::MULT)))
        return failure();

      rewriter.setInsertionPointAfter(newIntersectOp);
      rewriter.replaceOp(op, newIntersectOp.getResult());
    }

    return success();
  };
};

class LowerIntersectGenericRewrite
    : public OpRewritePattern<graphblas::IntersectGenericOp> {
public:
  using OpRewritePattern<graphblas::IntersectGenericOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::IntersectGenericOp op,
                                PatternRewriter &rewriter) const override {
    ModuleOp module = op->getParentOfType<ModuleOp>();
    Location loc = op->getLoc();

    // Inputs
    Value a = op.a();
    Value b = op.b();

    // Required block
    RegionRange extensions = op.extensions();
    ExtensionBlocks extBlocks;
    std::set<graphblas::YieldKind> required = {graphblas::YieldKind::MULT};
    LogicalResult extractResult =
        extBlocks.extractBlocks(op, extensions, required, {});

    if (extractResult.failed()) {
      return extractResult;
    }

    // Types
    RankedTensorType aType = a.getType().dyn_cast<RankedTensorType>();

    unsigned rank = aType.getRank(); // ranks guaranteed to be equal

    Type outputType =
        op.getResult().getType().dyn_cast<RankedTensorType>().getElementType();
    Value output = callEmptyLike(rewriter, module, loc, a, outputType);
    if (rank == 2) {
      computeMatrixElementWise(rewriter, loc, module, a, b, output,
                               extBlocks.mult, INTERSECT);
    } else {
      computeVectorElementWise(rewriter, loc, module, a, b, output,
                               extBlocks.mult, INTERSECT);
    }

    rewriter.replaceOp(op, output);

    cleanupIntermediateTensor(rewriter, module, loc, output);

    return success();
  };
};

class LowerUpdateRewrite : public OpRewritePattern<graphblas::UpdateOp> {
public:
  using OpRewritePattern<graphblas::UpdateOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::UpdateOp op,
                                PatternRewriter &rewriter) const override {
    ModuleOp module = op->getParentOfType<ModuleOp>();
    Location loc = op->getLoc();

    Type valueType =
        op.input().getType().cast<RankedTensorType>().getElementType();
    bool maskComplement = op.mask_complement();
    bool replace = op.replace();
    llvm::Optional<llvm::StringRef> accumulateOperator =
        op.accumulate_operator();

    // Use generic for accumulator
    if (accumulateOperator) {
      // New op
      NamedAttrList attributes = {};
      attributes.append(StringRef("mask_complement"),
                        rewriter.getBoolAttr(maskComplement));
      attributes.append(StringRef("replace"), rewriter.getBoolAttr(replace));
      graphblas::UpdateGenericOp newUpdateOp =
          rewriter.create<graphblas::UpdateGenericOp>(loc, op->getResultTypes(),
                                                      op.getOperands(),
                                                      attributes.getAttrs(), 1);

      if (failed(populateBinary(rewriter, loc, accumulateOperator->str(),
                                valueType, newUpdateOp.getRegions().slice(0, 1),
                                graphblas::YieldKind::ACCUMULATE)))
        return failure();

      rewriter.setInsertionPointAfter(newUpdateOp);
      rewriter.eraseOp(op);

      return success();
    }

    // No accumulator; lower without generic op

    Value input = op.input();
    Value output = op.output();
    Value mask = op.mask();

    // Types
    RankedTensorType outputType = output.getType().dyn_cast<RankedTensorType>();

    unsigned rank = outputType.getRank(); // ranks guaranteed to be equal
    auto computeEwise =
        rank == 2 ? computeMatrixElementWise : computeVectorElementWise;

    if (mask) {
      EwiseBehavior maskBehavior = (maskComplement ? MASK_COMPLEMENT : MASK);
      if (replace) {
        // input -> output(mask) { replace }

        computeEwise(rewriter, loc, module, input, mask, output, nullptr,
                     maskBehavior);
      } else {
        // input -> output(mask)

        // Step 1: apply the mask inverse to the output
        EwiseBehavior maskInverseBehavior =
            (maskComplement ? MASK : MASK_COMPLEMENT);
        Value maskedOutput = callEmptyLike(rewriter, module, loc, output);
        computeEwise(rewriter, loc, module, output, mask, maskedOutput, nullptr,
                     maskInverseBehavior);
        // Step 2: apply the mask to the input
        Value maskedInput = callEmptyLike(rewriter, module, loc, input);
        computeEwise(rewriter, loc, module, input, mask, maskedInput, nullptr,
                     maskBehavior);
        // Step 3: union the two masked results
        // Note that there should be zero overlaps, so we do not provide
        //      an accumulation block
        computeEwise(rewriter, loc, module, maskedInput, maskedOutput, output,
                     nullptr, UNION);
        rewriter.create<sparse_tensor::ReleaseOp>(loc, maskedOutput);
        rewriter.create<sparse_tensor::ReleaseOp>(loc, maskedInput);
      }
    } else {
      // input -> output { replace? }

      Value inputCopy = callDupTensor(rewriter, module, loc, input);
      callSwapPointers(rewriter, module, loc, inputCopy, output);
      callSwapIndices(rewriter, module, loc, inputCopy, output);
      callSwapValues(rewriter, module, loc, inputCopy, output);
      rewriter.create<sparse_tensor::ReleaseOp>(loc, inputCopy);
    }

    rewriter.eraseOp(op);

    return success();
  };
};

class LowerUpdateGenericRewrite
    : public OpRewritePattern<graphblas::UpdateGenericOp> {
public:
  using OpRewritePattern<graphblas::UpdateGenericOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::UpdateGenericOp op,
                                PatternRewriter &rewriter) const override {
    ModuleOp module = op->getParentOfType<ModuleOp>();
    Location loc = op->getLoc();

    // Inputs
    Value input = op.input();
    Value output = op.output();
    Value mask = op.mask();
    bool maskComplement = op.mask_complement();
    bool replace = op.replace();

    // Extension blocks
    RegionRange extensions = op.extensions();
    ExtensionBlocks extBlocks;
    std::set<graphblas::YieldKind> optional = {
        graphblas::YieldKind::ACCUMULATE};
    LogicalResult extractResult =
        extBlocks.extractBlocks(op, extensions, {}, optional);

    if (extractResult.failed()) {
      return extractResult;
    }

    // Types
    RankedTensorType outputType = output.getType().dyn_cast<RankedTensorType>();

    unsigned rank = outputType.getRank(); // ranks guaranteed to be equal
    auto computeEwise =
        rank == 2 ? computeMatrixElementWise : computeVectorElementWise;

    if (mask) {
      EwiseBehavior maskBehavior = (maskComplement ? MASK_COMPLEMENT : MASK);
      if (replace) {
        // input -> output(mask) { accumulate, replace }
        // Must think of this as `output(mask) << input` so ordering is correct

        // Step 1: apply the mask to the output
        Value maskedOutput = callEmptyLike(rewriter, module, loc, output);
        computeEwise(rewriter, loc, module, output, mask, maskedOutput, nullptr,
                     maskBehavior);
        // Step 2: apply the mask to the input
        Value maskedInput = callEmptyLike(rewriter, module, loc, input);
        computeEwise(rewriter, loc, module, input, mask, maskedInput, nullptr,
                     maskBehavior);
        // Step 3: union the two masked results
        computeEwise(rewriter, loc, module, maskedOutput, maskedInput, output,
                     extBlocks.accumulate, UNION);
        rewriter.create<sparse_tensor::ReleaseOp>(loc, maskedOutput);
        rewriter.create<sparse_tensor::ReleaseOp>(loc, maskedInput);
      } else {
        // input -> output(mask) { accumulate }
        // Must think of this as `output(mask) << input` so ordering is correct

        // Step 1: apply the mask to the input
        Value maskedInput = callEmptyLike(rewriter, module, loc, input);
        computeEwise(rewriter, loc, module, input, mask, maskedInput, nullptr,
                     maskBehavior);
        // Step 2: union the two masked results
        Value outputCopy = callDupTensor(rewriter, module, loc, output);
        computeEwise(rewriter, loc, module, outputCopy, maskedInput, output,
                     extBlocks.accumulate, UNION);
        rewriter.create<sparse_tensor::ReleaseOp>(loc, outputCopy);
        rewriter.create<sparse_tensor::ReleaseOp>(loc, maskedInput);
      }
    } else {
      // input -> output { accumulate, replace? }
      // Must think of this as `output << input` so ordering is correct

      Value outputCopy = callDupTensor(rewriter, module, loc, output);
      computeEwise(rewriter, loc, module, outputCopy, input, output,
                   extBlocks.accumulate, UNION);
      rewriter.create<sparse_tensor::ReleaseOp>(loc, outputCopy);
    }

    rewriter.eraseOp(op);

    return success();
  };
};

class LowerEqualRewrite : public OpRewritePattern<graphblas::EqualOp> {
public:
  using OpRewritePattern<graphblas::EqualOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::EqualOp op,
                                PatternRewriter &rewriter) const override {
    Location loc = op->getLoc();

    // Inputs
    Value A = op.a();
    Value B = op.b();
    RankedTensorType aType = A.getType().dyn_cast<RankedTensorType>();

    // Types
    Type boolType = rewriter.getI1Type();
    // Need to use a standard word size in AND-reduction for OpenMP
    // This could be i8, i32, or i64, but we pick i32
    Type intReduceType = rewriter.getIntegerType(32);
    Type int64Type = rewriter.getIntegerType(64);
    Type valueType = aType.getElementType();
    MemRefType memref1DI64Type = MemRefType::get({-1}, int64Type);
    MemRefType memref1DValueType = MemRefType::get({-1}, valueType);

    // Initial constants
    Value c0 = rewriter.create<arith::ConstantIndexOp>(loc, 0);
    Value c1 = rewriter.create<arith::ConstantIndexOp>(loc, 1);
    Value cfalse = rewriter.create<arith::ConstantIntOp>(loc, 0, boolType);
    Value c1_reduce =
        rewriter.create<arith::ConstantIntOp>(loc, 1, intReduceType);

    unsigned rank = aType.getRank(); // ranks guaranteed to be equal

    Value dimIndex;
    Value cmpShape;
    if (rank == 2) {
      // Matrix check
      dimIndex = c1;
      Value aNrows = rewriter.create<graphblas::NumRowsOp>(loc, A);
      Value bNrows = rewriter.create<graphblas::NumRowsOp>(loc, B);
      Value aNcols = rewriter.create<graphblas::NumColsOp>(loc, A);
      Value bNcols = rewriter.create<graphblas::NumColsOp>(loc, B);
      Value cmpNrows = rewriter.create<arith::CmpIOp>(
          loc, arith::CmpIPredicate::eq, aNrows, bNrows);
      Value cmpNcols = rewriter.create<arith::CmpIOp>(
          loc, arith::CmpIPredicate::eq, aNcols, bNcols);
      cmpShape = rewriter.create<arith::AndIOp>(loc, cmpNrows, cmpNcols);
    } else {
      // Vector check
      dimIndex = c0;
      // Check size
      Value aSize = rewriter.create<graphblas::SizeOp>(loc, A);
      Value bSize = rewriter.create<graphblas::SizeOp>(loc, B);
      cmpShape = rewriter.create<arith::CmpIOp>(loc, arith::CmpIPredicate::eq,
                                                aSize, bSize);
    }

    scf::IfOp ifOuter =
        rewriter.create<scf::IfOp>(loc, boolType, cmpShape, true);
    // if cmpSize
    rewriter.setInsertionPointToStart(ifOuter.thenBlock());

    // Check number of non-zeros
    Value aNnz = rewriter.create<graphblas::NumValsOp>(loc, A);
    Value bNnz = rewriter.create<graphblas::NumValsOp>(loc, B);
    Value cmpNnz = rewriter.create<arith::CmpIOp>(loc, arith::CmpIPredicate::eq,
                                                  aNnz, bNnz);
    scf::IfOp ifNnz = rewriter.create<scf::IfOp>(loc, boolType, cmpNnz, true);
    // if cmpNnz
    rewriter.setInsertionPointToStart(ifNnz.thenBlock());

    // Check index positions and values
    Value Ai = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memref1DI64Type,
                                                           A, dimIndex);
    Value Bi = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memref1DI64Type,
                                                           B, dimIndex);
    Value Ax =
        rewriter.create<sparse_tensor::ToValuesOp>(loc, memref1DValueType, A);
    Value Bx =
        rewriter.create<sparse_tensor::ToValuesOp>(loc, memref1DValueType, B);

    scf::ParallelOp indexLoop =
        rewriter.create<scf::ParallelOp>(loc, c0, aNnz, c1, c1_reduce);
    Value loopIdx = indexLoop.getInductionVars().front();
    rewriter.setInsertionPointToStart(indexLoop.getBody());

    Value aIndex = rewriter.create<memref::LoadOp>(loc, Ai, loopIdx);
    Value bIndex = rewriter.create<memref::LoadOp>(loc, Bi, loopIdx);
    Value aValue = rewriter.create<memref::LoadOp>(loc, Ax, loopIdx);
    Value bValue = rewriter.create<memref::LoadOp>(loc, Bx, loopIdx);
    Value cmpIndex = rewriter.create<arith::CmpIOp>(
        loc, arith::CmpIPredicate::eq, aIndex, bIndex);
    Value cmpValue = llvm::TypeSwitch<Type, Value>(valueType)
                         .Case<IntegerType>([&](IntegerType type) {
                           return rewriter.create<arith::CmpIOp>(
                               loc, arith::CmpIPredicate::eq, aValue, bValue);
                         })
                         .Case<FloatType>([&](FloatType type) {
                           return rewriter.create<arith::CmpFOp>(
                               loc, arith::CmpFPredicate::OEQ, aValue, bValue);
                         });
    Value cmpCombined = rewriter.create<arith::AndIOp>(loc, cmpIndex, cmpValue);
    // Need to do reduction with a standard word size (rather than i1) for
    // OpenMP
    Value cmpCombined_ext =
        rewriter.create<arith::ExtSIOp>(loc, cmpCombined, intReduceType);

    scf::ReduceOp reducer =
        rewriter.create<scf::ReduceOp>(loc, cmpCombined_ext);
    BlockArgument lhs = reducer.getRegion().getArgument(0);
    BlockArgument rhs = reducer.getRegion().getArgument(1);
    rewriter.setInsertionPointToStart(&reducer.getRegion().front());
    Value cmpFinal = rewriter.create<arith::AndIOp>(loc, lhs, rhs);
    rewriter.create<scf::ReduceReturnOp>(loc, cmpFinal);

    rewriter.setInsertionPointAfter(indexLoop);
    Value boolResult =
        rewriter.create<arith::TruncIOp>(loc, indexLoop.getResult(0), boolType);
    rewriter.create<scf::YieldOp>(loc, boolResult);

    // else cmpNnz
    rewriter.setInsertionPointToStart(ifNnz.elseBlock());
    rewriter.create<scf::YieldOp>(loc, cfalse);
    // end cmpNnz
    rewriter.setInsertionPointAfter(ifNnz);
    Value nnzReturn = ifNnz.getResult(0);
    rewriter.create<scf::YieldOp>(loc, nnzReturn);

    // else cmpSize
    rewriter.setInsertionPointToStart(ifOuter.elseBlock());
    rewriter.create<scf::YieldOp>(loc, cfalse);
    // end cmpSize
    rewriter.setInsertionPointAfter(ifOuter);
    Value isEqual = ifOuter.getResult(0);

    rewriter.replaceOp(op, isEqual);

    return success();
  };
};

class LowerSelectMaskRewrite
    : public OpRewritePattern<graphblas::SelectMaskOp> {
public:
  using OpRewritePattern<graphblas::SelectMaskOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::SelectMaskOp op,
                                PatternRewriter &rewriter) const override {
    ModuleOp module = op->getParentOfType<ModuleOp>();
    Location loc = op->getLoc();

    // Inputs
    Value input = op.input();
    Value mask = op.mask();
    bool maskComplement = op.mask_complement();
    RankedTensorType inputTensorType =
        input.getType().dyn_cast<RankedTensorType>();

    Value output = callEmptyLike(rewriter, module, loc, input);
    EwiseBehavior maskBehavior = (maskComplement ? MASK_COMPLEMENT : MASK);

    unsigned rank = inputTensorType.getRank();
    if (rank == 2) {
      computeMatrixElementWise(rewriter, loc, module, input, mask, output,
                               nullptr, maskBehavior);
    } else {
      computeVectorElementWise(rewriter, loc, module, input, mask, output,
                               nullptr, maskBehavior);
    }

    rewriter.replaceOp(op, output);

    cleanupIntermediateTensor(rewriter, module, loc, output);

    return success();
  };
};

class LowerUniformComplementRewrite
    : public OpRewritePattern<graphblas::UniformComplementOp> {
public:
  using OpRewritePattern<graphblas::UniformComplementOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::UniformComplementOp op,
                                PatternRewriter &rewriter) const override {
    ModuleOp module = op->getParentOfType<ModuleOp>();
    Location loc = op->getLoc();

    // Inputs
    Value input = op.input();
    Value value = op.value();
    RankedTensorType inputTensorType =
        input.getType().dyn_cast<RankedTensorType>();
    RankedTensorType outputTensorType =
        op.getResult().getType().dyn_cast<RankedTensorType>();
    Type outputElementType = outputTensorType.getElementType();
    Type indexType = rewriter.getIndexType();
    Type i64Type = rewriter.getI64Type();
    Type memrefPointerType = getMemrefPointerType(inputTensorType);
    Type memrefIndexType = getMemrefIndexType(inputTensorType);
    Type memrefOValueType = getMemrefValueType(outputTensorType);
    unsigned rank = inputTensorType.getRank();

    // Initial constants
    Value c0 = rewriter.create<arith::ConstantIndexOp>(loc, 0);
    Value c1 = rewriter.create<arith::ConstantIndexOp>(loc, 1);

    Value output =
        callEmptyLike(rewriter, module, loc, input, outputElementType);

    // Resize output (max size - nnz)
    Value size, npointers, compSize, dimIndex;
    if (rank == 1) {
      npointers = c1;
      size = rewriter.create<graphblas::SizeOp>(loc, input);
      compSize = size;
      dimIndex = c0;
    } else {
      Value nrows = rewriter.create<graphblas::NumRowsOp>(loc, input);
      Value ncols = rewriter.create<graphblas::NumColsOp>(loc, input);
      size = rewriter.create<arith::MulIOp>(loc, nrows, ncols);
      dimIndex = c1;
      if (hasRowOrdering(inputTensorType)) {
        npointers = nrows;
        compSize = ncols;
      } else {
        npointers = ncols;
        compSize = nrows;
      }
    }
    Value nnz = rewriter.create<graphblas::NumValsOp>(loc, input);
    Value newSize = rewriter.create<arith::SubIOp>(loc, size, nnz);
    callResizeIndex(rewriter, module, loc, output, dimIndex, newSize);
    callResizeValues(rewriter, module, loc, output, newSize);

    // Get sparse tensor info
    Value Ip = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memrefPointerType, input, dimIndex);
    Value Ii = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memrefIndexType,
                                                           input, dimIndex);
    Value Op = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memrefPointerType, output, dimIndex);
    Value Oi = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memrefIndexType,
                                                           output, dimIndex);
    Value Ox = rewriter.create<sparse_tensor::ToValuesOp>(loc, memrefOValueType,
                                                          output);

    scf::ForOp loop =
        rewriter.create<scf::ForOp>(loc, c0, npointers, c1, ValueRange{c0});
    {
      rewriter.setInsertionPointToStart(loop.getBody());
      Value rowCount = loop.getLoopBody().getArgument(1);
      Value rowIndex = loop.getInductionVar();

      Value row_plus1 = rewriter.create<arith::AddIOp>(loc, rowIndex, c1);
      Value idxStart_64 = rewriter.create<memref::LoadOp>(loc, Ip, rowIndex);
      Value idxEnd_64 = rewriter.create<memref::LoadOp>(loc, Ip, row_plus1);
      Value idxStart =
          rewriter.create<arith::IndexCastOp>(loc, idxStart_64, indexType);
      Value idxEnd =
          rewriter.create<arith::IndexCastOp>(loc, idxEnd_64, indexType);

      ValueRange mcResults =
          buildMaskComplement(rewriter, loc, compSize, Ii, idxStart, idxEnd);
      Value maskComplement = mcResults[0];
      Value maskComplementSize = mcResults[1];

      Value newCount =
          rewriter.create<arith::AddIOp>(loc, rowCount, maskComplementSize);
      Value newCount_64 =
          rewriter.create<arith::IndexCastOp>(loc, newCount, i64Type);
      rewriter.create<memref::StoreOp>(loc, newCount_64, Op, row_plus1);

      scf::ForOp innerLoop =
          rewriter.create<scf::ForOp>(loc, c0, maskComplementSize, c1);
      {
        rewriter.setInsertionPointToStart(innerLoop.getBody());
        Value mcIndex = innerLoop.getInductionVar();
        Value colIndex = rewriter.create<arith::AddIOp>(loc, mcIndex, rowCount);

        Value innerIdx =
            rewriter.create<memref::LoadOp>(loc, maskComplement, mcIndex);
        rewriter.create<memref::StoreOp>(loc, innerIdx, Oi, colIndex);
        rewriter.create<memref::StoreOp>(loc, value, Ox, colIndex);

        rewriter.setInsertionPointAfter(innerLoop);
      }

      rewriter.create<scf::YieldOp>(loc, ValueRange{newCount});
      rewriter.setInsertionPointAfter(loop);
    }

    rewriter.replaceOp(op, output);

    return success();
  };
};

class LowerDiagOpRewrite : public OpRewritePattern<graphblas::DiagOp> {
public:
  using OpRewritePattern<graphblas::DiagOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::DiagOp op,
                                PatternRewriter &rewriter) const override {

    RankedTensorType resultTensorType =
        op.getResult().getType().dyn_cast<RankedTensorType>();

    if (resultTensorType.getRank() == 1) {
      return lowerMatrixToVecDiagOp(op, rewriter, resultTensorType);
    } else if (resultTensorType.getRank() == 2) {
      return lowerVecToMatrixDiagOp(op, rewriter, resultTensorType);
    }

    return failure();
  };

private:
  LogicalResult
  lowerVecToMatrixDiagOp(graphblas::DiagOp op, PatternRewriter &rewriter,
                         RankedTensorType &resultTensorType) const {

    ModuleOp module = op->getParentOfType<ModuleOp>();
    Location loc = op->getLoc();

    Value vector = op.input();

    Type valueType = resultTensorType.getElementType();

    Type indexType = rewriter.getIndexType();
    Type int64Type = rewriter.getIntegerType(64);
    Type memref1DI64Type = MemRefType::get({-1}, int64Type);
    Type memref1DValueType = MemRefType::get({-1}, valueType);

    Value c0_i64 =
        rewriter.create<ConstantOp>(loc, rewriter.getIntegerAttr(int64Type, 0));
    Value c1_i64 =
        rewriter.create<ConstantOp>(loc, rewriter.getIntegerAttr(int64Type, 1));

    Value c0 = rewriter.create<arith::ConstantIndexOp>(loc, 0);
    Value c1 = rewriter.create<arith::ConstantIndexOp>(loc, 1);

    Value vectorLength = rewriter.create<graphblas::SizeOp>(loc, vector);
    Value vectorIndices = rewriter.create<sparse_tensor::ToIndicesOp>(
        loc, memref1DI64Type, vector, c0);
    Value vectorValues = rewriter.create<sparse_tensor::ToValuesOp>(
        loc, memref1DValueType, vector);

    Value output = callNewTensor(
        rewriter, module, loc, {vectorLength, vectorLength}, resultTensorType);

    Value outputNNZ = rewriter.create<graphblas::NumValsOp>(loc, vector);
    Value hasValues = rewriter.create<arith::CmpIOp>(
        loc, arith::CmpIPredicate::ugt, outputNNZ, c0);
    scf::IfOp ifHasValues = rewriter.create<scf::IfOp>(loc, hasValues, false);
    {
      rewriter.setInsertionPointToStart(ifHasValues.thenBlock());

      callResizeIndex(rewriter, module, loc, output, c1, outputNNZ);
      callResizeValues(rewriter, module, loc, output, outputNNZ);

      Value outputIndices = rewriter.create<sparse_tensor::ToIndicesOp>(
          loc, memref1DI64Type, output, c1);
      Value outputValues = rewriter.create<sparse_tensor::ToValuesOp>(
          loc, memref1DValueType, output);

      scf::ForOp copyValuesAndIndicesLoop =
          rewriter.create<scf::ForOp>(loc, c0, outputNNZ, c1);
      {
        rewriter.setInsertionPointToStart(copyValuesAndIndicesLoop.getBody());
        Value outputPosition = copyValuesAndIndicesLoop.getInductionVar();
        Value vectorIndex =
            rewriter.create<memref::LoadOp>(loc, vectorIndices, outputPosition);
        rewriter.create<memref::StoreOp>(loc, vectorIndex, outputIndices,
                                         outputPosition);
        Value vectorValue =
            rewriter.create<memref::LoadOp>(loc, vectorValues, outputPosition);
        rewriter.create<memref::StoreOp>(loc, vectorValue, outputValues,
                                         outputPosition);
        rewriter.setInsertionPointAfter(copyValuesAndIndicesLoop);
      }

      Value outputPointers = rewriter.create<sparse_tensor::ToPointersOp>(
          loc, memref1DI64Type, output, c1);
      Value initialVectorIndicesValue =
          rewriter.create<memref::LoadOp>(loc, vectorIndices, c0);
      Value vectorLengthMinusOne =
          rewriter.create<arith::SubIOp>(loc, vectorLength, c1);
      scf::ForOp pointersUpdateLoop = rewriter.create<scf::ForOp>(
          loc, c0, vectorLength, c1,
          ValueRange{c0_i64, c0, initialVectorIndicesValue});
      {
        rewriter.setInsertionPointToStart(pointersUpdateLoop.getBody());
        Value pointersPosition = pointersUpdateLoop.getInductionVar();
        Value ptr_i64 = pointersUpdateLoop.getLoopBody().getArgument(1);
        Value vectorIndicesPosition =
            pointersUpdateLoop.getLoopBody().getArgument(2);
        Value vectorIndicesValue =
            pointersUpdateLoop.getLoopBody().getArgument(3);

        rewriter.create<memref::StoreOp>(loc, ptr_i64, outputPointers,
                                         pointersPosition);
        Value pointersPosition_i64 = rewriter.create<arith::IndexCastOp>(
            loc, pointersPosition, int64Type);
        Value rowHasValue = rewriter.create<arith::CmpIOp>(
            op.getLoc(), arith::CmpIPredicate::eq, vectorIndicesValue,
            pointersPosition_i64);
        Value notAtLastIteration = rewriter.create<arith::CmpIOp>(
            op.getLoc(), arith::CmpIPredicate::ne, pointersPosition,
            vectorLengthMinusOne);
        Value mustUpdate = rewriter.create<arith::AndIOp>(
            loc, notAtLastIteration, rowHasValue);

        scf::IfOp ifMustUpdateBlock = rewriter.create<scf::IfOp>(
            loc, TypeRange{int64Type, indexType, int64Type}, mustUpdate, true);
        {
          rewriter.setInsertionPointToStart(ifMustUpdateBlock.thenBlock());
          Value nextPtr_i64 =
              rewriter.create<arith::AddIOp>(loc, ptr_i64, c1_i64);
          Value nextVectorIndicesPosition =
              rewriter.create<arith::AddIOp>(loc, vectorIndicesPosition, c1);
          Value nextUpdatedVectorIndicesValue = rewriter.create<memref::LoadOp>(
              loc, vectorIndices, nextVectorIndicesPosition);

          rewriter.create<scf::YieldOp>(
              loc, ValueRange{nextPtr_i64, nextVectorIndicesPosition,
                              nextUpdatedVectorIndicesValue});
        }
        {
          rewriter.setInsertionPointToStart(ifMustUpdateBlock.elseBlock());
          rewriter.create<scf::YieldOp>(
              loc,
              ValueRange{ptr_i64, vectorIndicesPosition, vectorIndicesValue});
        }
        rewriter.setInsertionPointAfter(ifMustUpdateBlock);

        Value updatedPtr_i64 = ifMustUpdateBlock.getResult(0);
        Value updatedVectorIndicesPosition = ifMustUpdateBlock.getResult(1);
        Value updatedVectorIndicesValue = ifMustUpdateBlock.getResult(2);

        rewriter.create<scf::YieldOp>(
            loc, ValueRange{updatedPtr_i64, updatedVectorIndicesPosition,
                            updatedVectorIndicesValue});

        rewriter.setInsertionPointAfter(pointersUpdateLoop);
      }

      Value outputNNZ_i64 =
          rewriter.create<arith::IndexCastOp>(loc, outputNNZ, int64Type);
      rewriter.create<memref::StoreOp>(loc, outputNNZ_i64, outputPointers,
                                       vectorLength);
      rewriter.setInsertionPointAfter(ifHasValues);
    }

    rewriter.replaceOp(op, output);

    cleanupIntermediateTensor(rewriter, module, loc, output);

    return success();
  }

  LogicalResult
  lowerMatrixToVecDiagOp(graphblas::DiagOp op, PatternRewriter &rewriter,
                         RankedTensorType &resultTensorType) const {

    // This implementation reads as assuming the input matrix is CSR,
    // but it will work for CSC as well.

    ModuleOp module = op->getParentOfType<ModuleOp>();
    Location loc = op->getLoc();

    Value matrix = op.input();

    Type valueType = resultTensorType.getElementType();

    Type indexType = rewriter.getIndexType();
    Type int64Type = rewriter.getIntegerType(64);
    Type int1Type = rewriter.getIntegerType(1);
    Type memref1DI64Type = MemRefType::get({-1}, int64Type);
    Type memref1DValueType = MemRefType::get({-1}, valueType);

    Value c1_i1 =
        rewriter.create<ConstantOp>(loc, rewriter.getIntegerAttr(int1Type, 1));
    Value c0_valueType = llvm::TypeSwitch<Type, Value>(valueType)
                             .Case<IntegerType>([&](IntegerType type) {
                               return rewriter.create<ConstantOp>(
                                   loc, rewriter.getIntegerAttr(valueType, 1));
                             })
                             .Case<FloatType>([&](FloatType type) {
                               return rewriter.create<ConstantOp>(
                                   loc, rewriter.getFloatAttr(valueType, 1.0));
                             });

    Value c0 = rewriter.create<arith::ConstantIndexOp>(loc, 0);
    Value c1 = rewriter.create<arith::ConstantIndexOp>(loc, 1);

    Value nrows = rewriter.create<graphblas::NumRowsOp>(loc, matrix);

    Value matrixPointers = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memref1DI64Type, matrix, c1);
    Value matrixIndices = rewriter.create<sparse_tensor::ToIndicesOp>(
        loc, memref1DI64Type, matrix, c1);
    Value matrixValues = rewriter.create<sparse_tensor::ToValuesOp>(
        loc, memref1DValueType, matrix);

    Value output = callNewTensor(rewriter, module, loc, ValueRange{nrows},
                                 resultTensorType);

    // We do two loops, one to find the output vector's nnz
    // and one to fill up the output's indices and values.
    // We have to get the nnz first to allocate space in the
    // output vector correctly.
    // TODO We could do one loop where we do both.
    //  1) assume that the output nnz is some arbitrary size
    //  2) resize accordingly
    //  3) on each iteration,
    //        - store the values and indices
    //        - track the correct nnz
    //        - if we reach the limit of whatever size the
    //          output vector is, resize (double it or
    //          something like that)
    //  4) resize the output vector to the correct nnz
    // It's unclear which approach is more performant since
    // it depends on how accurate our arbitrary guesses are
    // for initial size and how much we should resize.

    scf::ForOp outputNNZLoop =
        rewriter.create<scf::ForOp>(loc, c0, nrows, c1, ValueRange{c0});
    {
      Value numDiagonalContainingRows =
          outputNNZLoop.getLoopBody().getArgument(1);
      rewriter.setInsertionPointToStart(outputNNZLoop.getBody());

      Value matrixRowIndex = outputNNZLoop.getInductionVar();
      Value nextMatrixRowIndex =
          rewriter.create<arith::AddIOp>(loc, matrixRowIndex, c1);

      Value firstPtr_i64 =
          rewriter.create<memref::LoadOp>(loc, matrixPointers, matrixRowIndex);
      Value secondPtr_i64 = rewriter.create<memref::LoadOp>(loc, matrixPointers,
                                                            nextMatrixRowIndex);

      Value firstPtr =
          rewriter.create<arith::IndexCastOp>(loc, firstPtr_i64, indexType);
      Value secondPtr =
          rewriter.create<arith::IndexCastOp>(loc, secondPtr_i64, indexType);

      Value matrixRowIndex_i64 =
          rewriter.create<arith::IndexCastOp>(loc, matrixRowIndex, int64Type);

      scf::WhileOp findDiagonalWhileLoop = rewriter.create<scf::WhileOp>(
          loc, TypeRange{indexType, int1Type}, ValueRange{firstPtr, c1_i1});
      Block *findDiagonalWhileLoopBefore =
          rewriter.createBlock(&findDiagonalWhileLoop.getBefore(), {},
                               TypeRange{indexType, int1Type});
      Block *findDiagonalWhileLoopAfter =
          rewriter.createBlock(&findDiagonalWhileLoop.getAfter(), {},
                               TypeRange{indexType, int1Type});
      Value diagonalNotFound = findDiagonalWhileLoop.getResult(1);
      {
        rewriter.setInsertionPointToStart(
            &findDiagonalWhileLoop.getBefore().front());
        Value ptr = findDiagonalWhileLoopBefore->getArgument(0);
        Value diagonalPositionNotFound =
            findDiagonalWhileLoopBefore->getArgument(1);
        Value morePtrs = rewriter.create<arith::CmpIOp>(
            op.getLoc(), arith::CmpIPredicate::ult, ptr, secondPtr);
        Value continueCondition = rewriter.create<arith::AndIOp>(
            loc, diagonalPositionNotFound, morePtrs);
        rewriter.create<scf::ConditionOp>(
            loc, continueCondition, ValueRange{ptr, diagonalPositionNotFound});
      }
      {
        rewriter.setInsertionPointToStart(
            &findDiagonalWhileLoop.getAfter().front());
        Value currentPtr = findDiagonalWhileLoopAfter->getArgument(0);
        Value elementColumnIndex_i64 =
            rewriter.create<memref::LoadOp>(loc, matrixIndices, currentPtr);
        Value isNotDiagonalPosition = rewriter.create<arith::CmpIOp>(
            op.getLoc(), arith::CmpIPredicate::ne, elementColumnIndex_i64,
            matrixRowIndex_i64);
        Value nextPtr = rewriter.create<arith::AddIOp>(loc, currentPtr, c1);
        rewriter.create<scf::YieldOp>(
            loc, ValueRange{nextPtr, isNotDiagonalPosition});
        rewriter.setInsertionPointAfter(findDiagonalWhileLoop);
      }

      scf::IfOp ifDiagonalNotFoundBlock = rewriter.create<scf::IfOp>(
          loc, TypeRange{indexType}, diagonalNotFound, true);
      {
        rewriter.setInsertionPointToStart(ifDiagonalNotFoundBlock.thenBlock());
        rewriter.create<scf::YieldOp>(loc,
                                      ValueRange{numDiagonalContainingRows});
      }
      {
        rewriter.setInsertionPointToStart(ifDiagonalNotFoundBlock.elseBlock());
        Value nextNumDiagonalContainingRows =
            rewriter.create<arith::AddIOp>(loc, numDiagonalContainingRows, c1);
        rewriter.create<scf::YieldOp>(
            loc, ValueRange{nextNumDiagonalContainingRows});
      }
      rewriter.setInsertionPointAfter(ifDiagonalNotFoundBlock);
      Value updatedNumDiagonalContainingRows =
          ifDiagonalNotFoundBlock.getResult(0);

      rewriter.create<scf::YieldOp>(
          loc, ValueRange{updatedNumDiagonalContainingRows});

      rewriter.setInsertionPointAfter(outputNNZLoop);
    }
    Value outputNNZ = outputNNZLoop.getResult(0);

    callResizeIndex(rewriter, module, loc, output, c0, outputNNZ);
    callResizeValues(rewriter, module, loc, output, outputNNZ);

    Value outputPointers = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memref1DI64Type, output, c0);
    Value outputNNZ_i64 =
        rewriter.create<arith::IndexCastOp>(loc, outputNNZ, int64Type);
    rewriter.create<memref::StoreOp>(loc, outputNNZ_i64, outputPointers, c1);

    Value outputIndices = rewriter.create<sparse_tensor::ToIndicesOp>(
        loc, memref1DI64Type, output, c0);
    Value outputValues = rewriter.create<sparse_tensor::ToValuesOp>(
        loc, memref1DValueType, output);

    scf::ForOp outputValueAndIndicesFillingLoop =
        rewriter.create<scf::ForOp>(loc, c0, nrows, c1, ValueRange{c0});
    {
      Value outputValuesPosition =
          outputValueAndIndicesFillingLoop.getLoopBody().getArgument(1);
      Value rowIndex = outputValueAndIndicesFillingLoop.getInductionVar();
      rewriter.setInsertionPointToStart(
          outputValueAndIndicesFillingLoop.getBody());

      Value nextRowIndex = rewriter.create<arith::AddIOp>(loc, rowIndex, c1);
      Value firstPtr_i64 =
          rewriter.create<memref::LoadOp>(loc, matrixPointers, rowIndex);
      Value secondPtr_i64 =
          rewriter.create<memref::LoadOp>(loc, matrixPointers, nextRowIndex);

      Value firstPtr =
          rewriter.create<arith::IndexCastOp>(loc, firstPtr_i64, indexType);
      Value secondPtr =
          rewriter.create<arith::IndexCastOp>(loc, secondPtr_i64, indexType);

      Value rowIndex_i64 =
          rewriter.create<arith::IndexCastOp>(loc, rowIndex, int64Type);

      // instead of having a var for whether or not a diagonal value was found
      // and the value itself, we could just track whether or not the diagonal
      // value (see the C++ variable diagonalValue) is zero (or whatever the
      // missing value represents).
      // This will cause bugs with malformed sparse tensors that have the
      // missing value in the values array.

      // c0_valueType is just used as a dummmy initial value here ; any garbage
      // value would work
      scf::WhileOp findDiagonalWhileLoop = rewriter.create<scf::WhileOp>(
          loc, TypeRange{indexType, int1Type, valueType},
          ValueRange{firstPtr, c1_i1, c0_valueType});
      Block *findDiagonalWhileLoopBefore =
          rewriter.createBlock(&findDiagonalWhileLoop.getBefore(), {},
                               TypeRange{indexType, int1Type, valueType});
      Block *findDiagonalWhileLoopAfter =
          rewriter.createBlock(&findDiagonalWhileLoop.getAfter(), {},
                               TypeRange{indexType, int1Type, valueType});
      Value diagonalNotFound = findDiagonalWhileLoop.getResult(1);
      Value diagonalValue = findDiagonalWhileLoop.getResult(2);
      {
        Value ptr = findDiagonalWhileLoopBefore->getArgument(0);
        Value diagonalPositionNotFound =
            findDiagonalWhileLoopBefore->getArgument(1);
        Value currentDiagonalValue =
            findDiagonalWhileLoopBefore->getArgument(2);
        rewriter.setInsertionPointToStart(
            &findDiagonalWhileLoop.getBefore().front());
        Value morePtrs = rewriter.create<arith::CmpIOp>(
            op.getLoc(), arith::CmpIPredicate::ult, ptr, secondPtr);
        Value continueCondition = rewriter.create<arith::AndIOp>(
            loc, diagonalPositionNotFound, morePtrs);
        rewriter.create<scf::ConditionOp>(
            loc, continueCondition,
            ValueRange{ptr, diagonalPositionNotFound, currentDiagonalValue});
      }
      {
        rewriter.setInsertionPointToStart(
            &findDiagonalWhileLoop.getAfter().front());
        Value currentPtr = findDiagonalWhileLoopAfter->getArgument(0);
        Value previousDiagonalValue =
            findDiagonalWhileLoopAfter->getArgument(2);
        Value elementColumnIndex_i64 =
            rewriter.create<memref::LoadOp>(loc, matrixIndices, currentPtr);
        Value isNotDiagonalPosition = rewriter.create<arith::CmpIOp>(
            op.getLoc(), arith::CmpIPredicate::ne, elementColumnIndex_i64,
            rowIndex_i64);

        scf::IfOp ifDiagonalNotFoundBlock = rewriter.create<scf::IfOp>(
            loc, TypeRange{valueType}, isNotDiagonalPosition, true);
        {
          rewriter.setInsertionPointToStart(
              ifDiagonalNotFoundBlock.thenBlock());
          // TODO yielding a dummy value (e.g. c0_valueType) works as well ;
          // unsure which creates more optimal code
          rewriter.create<scf::YieldOp>(loc, ValueRange{previousDiagonalValue});
        }
        {
          rewriter.setInsertionPointToStart(
              ifDiagonalNotFoundBlock.elseBlock());
          Value actualDiagonalValue =
              rewriter.create<memref::LoadOp>(loc, matrixValues, currentPtr);
          rewriter.create<scf::YieldOp>(loc, ValueRange{actualDiagonalValue});
        }
        rewriter.setInsertionPointAfter(ifDiagonalNotFoundBlock);
        Value updatedDiagonalValue = ifDiagonalNotFoundBlock.getResult(0);

        Value nextPtr = rewriter.create<arith::AddIOp>(loc, currentPtr, c1);
        rewriter.create<scf::YieldOp>(
            loc,
            ValueRange{nextPtr, isNotDiagonalPosition, updatedDiagonalValue});
        rewriter.setInsertionPointAfter(findDiagonalWhileLoop);
      }

      scf::IfOp ifDiagonalNotFoundBlock = rewriter.create<scf::IfOp>(
          loc, TypeRange{indexType}, diagonalNotFound, true);
      {
        rewriter.setInsertionPointToStart(ifDiagonalNotFoundBlock.thenBlock());
        rewriter.create<scf::YieldOp>(loc, ValueRange{outputValuesPosition});
      }
      {
        rewriter.setInsertionPointToStart(ifDiagonalNotFoundBlock.elseBlock());

        rewriter.create<memref::StoreOp>(loc, diagonalValue, outputValues,
                                         outputValuesPosition);
        rewriter.create<memref::StoreOp>(loc, rowIndex_i64, outputIndices,
                                         outputValuesPosition);

        Value nextOutputValuesPosition =
            rewriter.create<arith::AddIOp>(loc, outputValuesPosition, c1);
        rewriter.create<scf::YieldOp>(loc,
                                      ValueRange{nextOutputValuesPosition});
      }
      rewriter.setInsertionPointAfter(ifDiagonalNotFoundBlock);
      Value nextOutputValuesPosition = ifDiagonalNotFoundBlock.getResult(0);

      rewriter.create<scf::YieldOp>(loc, ValueRange{nextOutputValuesPosition});
      rewriter.setInsertionPointAfter(outputValueAndIndicesFillingLoop);
    }

    rewriter.replaceOp(op, output);

    return success();
  }
};

class LowerCommentRewrite : public OpRewritePattern<graphblas::CommentOp> {
public:
  using OpRewritePattern<graphblas::CommentOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::CommentOp op,
                                PatternRewriter &rewriter) const override {
    rewriter.eraseOp(op);
    return success();
  };
};

class LowerPrintRewrite : public OpRewritePattern<graphblas::PrintOp> {
public:
  using OpRewritePattern<graphblas::PrintOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::PrintOp op,
                                PatternRewriter &rewriter) const override {

    ModuleOp module = op->getParentOfType<ModuleOp>();
    Location loc = op->getLoc();

    for (auto enumerated_pair :
         llvm::enumerate(llvm::zip_longest(op.strings(), op.values()))) {
      auto pair = enumerated_pair.value();
      Optional<Attribute> stringAttribute = std::get<0>(pair);
      Optional<Value> val = std::get<1>(pair);

      if (stringAttribute) {
        StringRef currentString =
            stringAttribute.getValue().dyn_cast<StringAttr>().getValue();
        callPrintString(rewriter, module, loc, currentString);
      } else if (enumerated_pair.index() != 0)
        callPrintString(rewriter, module, loc, " ");

      if (!val)
        callPrintString(rewriter, module, loc, " ");
      else if (val.getValue().getType().dyn_cast_or_null<RankedTensorType>())
        callPrintTensor(rewriter, module, loc, val.getValue());
      else
        callPrintValue(rewriter, module, loc, val.getValue());
    }
    callPrintString(rewriter, module, loc, "\n");

    rewriter.eraseOp(op);

    return success();
  };
};

class LowerPrintTensorRewrite
    : public OpRewritePattern<graphblas::PrintTensorOp> {
public:
  using OpRewritePattern<graphblas::PrintTensorOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::PrintTensorOp op,
                                PatternRewriter &rewriter) const override {
    ModuleOp module = op->getParentOfType<ModuleOp>();
    Location loc = op->getLoc();
    Value input = op.input();
    int64_t level = op.level();

    callPrintTensorComponents(rewriter, module, loc, input, level);

    rewriter.eraseOp(op);

    return success();
  };
};

class LowerMatrixSelectRandomRewrite
    : public OpRewritePattern<graphblas::MatrixSelectRandomOp> {
public:
  using OpRewritePattern<graphblas::MatrixSelectRandomOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::MatrixSelectRandomOp op,
                                PatternRewriter &rewriter) const override {
    Location loc = op->getLoc();

    ModuleOp module = op->getParentOfType<ModuleOp>();

    Value input = op.input();
    Value n = op.n();
    Value rngContext = op.rng_context();
    SymbolRefAttr chooseNSymbol = op.choose_n();

    Type valueType = input.getType().dyn_cast<TensorType>().getElementType();
    Type int64Type = rewriter.getIntegerType(64);
    Type indexType = rewriter.getIndexType();
    Type memref1DI64Type = MemRefType::get({-1}, int64Type);
    Type memref1DValueType = MemRefType::get({-1}, valueType);

    // Initial constants
    Value c0 = rewriter.create<arith::ConstantIndexOp>(loc, 0);
    Value c0_64 = rewriter.create<arith::ConstantIntOp>(loc, 0, int64Type);
    Value c1 = rewriter.create<arith::ConstantIndexOp>(loc, 1);

    // Get sparse tensor info
    Value nrow = rewriter.create<graphblas::NumRowsOp>(loc, input);
    Value Ap = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memref1DI64Type, input, c1);
    Value Aj = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memref1DI64Type,
                                                           input, c1);
    Value Ax = rewriter.create<sparse_tensor::ToValuesOp>(
        loc, memref1DValueType, input);

    // Create output tensor
    Value output = rewriter.create<graphblas::DupOp>(loc, input);
    Value Bp = rewriter.create<sparse_tensor::ToPointersOp>(
        loc, memref1DI64Type, output, c1);
    Value Bj = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memref1DI64Type,
                                                           output, c1);
    Value Bx = rewriter.create<sparse_tensor::ToValuesOp>(
        loc, memref1DValueType, output);
    rewriter.create<memref::StoreOp>(loc, c0_64, Bp, c0);

    // Pass 1: Scan input tensor to compute offsets
    scf::ForOp scanLoop = rewriter.create<scf::ForOp>(loc, c0, nrow, c1);
    Value row = scanLoop.getInductionVar();

    rewriter.setInsertionPointToStart(scanLoop.getBody());
    Value row_plus1 = rewriter.create<arith::AddIOp>(loc, row, c1);
    Value Aj_start_64 = rewriter.create<memref::LoadOp>(loc, Ap, row);
    Value Aj_end_64 = rewriter.create<memref::LoadOp>(loc, Ap, row_plus1);

    // Limit number of row values in output to n
    Value Aj_size_64 =
        rewriter.create<arith::SubIOp>(loc, Aj_end_64, Aj_start_64);
    Value isRowSmall = rewriter.create<arith::CmpIOp>(
        loc, arith::CmpIPredicate::ule, Aj_size_64, n);
    Value Bj_size_64 =
        rewriter.create<SelectOp>(loc, isRowSmall, Aj_size_64, n);

    Value Bj_start_64 = rewriter.create<memref::LoadOp>(loc, Bp, row);
    Value Bj_end_64 =
        rewriter.create<arith::AddIOp>(loc, Bj_start_64, Bj_size_64);
    rewriter.create<memref::StoreOp>(loc, Bj_end_64, Bp, row_plus1);

    rewriter.setInsertionPointAfter(scanLoop);

    // Pass 2: Parallel select and compute output
    scf::ParallelOp rowLoop =
        rewriter.create<scf::ParallelOp>(loc, c0, nrow, c1);
    row = rowLoop.getInductionVars()[0];

    rewriter.setInsertionPointToStart(rowLoop.getBody());

    row_plus1 = rewriter.create<arith::AddIOp>(loc, row, c1);
    Aj_start_64 = rewriter.create<memref::LoadOp>(loc, Ap, row);
    Value Aj_start =
        rewriter.create<arith::IndexCastOp>(loc, Aj_start_64, indexType);
    Aj_end_64 = rewriter.create<memref::LoadOp>(loc, Ap, row_plus1);
    Value Aj_end =
        rewriter.create<arith::IndexCastOp>(loc, Aj_end_64, indexType);
    Bj_start_64 = rewriter.create<memref::LoadOp>(loc, Bp, row);
    Value Bj_start =
        rewriter.create<arith::IndexCastOp>(loc, Bj_start_64, indexType);
    Bj_end_64 = rewriter.create<memref::LoadOp>(loc, Bp, row_plus1);
    Value Bj_end =
        rewriter.create<arith::IndexCastOp>(loc, Bj_end_64, indexType);

    Value Aj_size = rewriter.create<arith::SubIOp>(loc, Aj_end, Aj_start);
    Aj_size_64 = rewriter.create<arith::IndexCastOp>(loc, Aj_size, int64Type);
    Value Bj_size = rewriter.create<arith::SubIOp>(loc, Bj_end, Bj_start);
    Bj_size_64 = rewriter.create<arith::IndexCastOp>(loc, Bj_size, int64Type);
    Value copyRow = rewriter.create<arith::CmpIOp>(
        loc, arith::CmpIPredicate::eq, Aj_size, Bj_size);

    // Create output subviews
    Value Bj_view =
        rewriter.create<memref::SubViewOp>(loc, Bj, Bj_start, Bj_size, c1);
    Value Bx_view =
        rewriter.create<memref::SubViewOp>(loc, Bx, Bj_start, Bj_size, c1);
    Value Aj_view =
        rewriter.create<memref::SubViewOp>(loc, Aj, Aj_start, Aj_size, c1);
    Value Ax_view =
        rewriter.create<memref::SubViewOp>(loc, Ax, Aj_start, Aj_size, c1);

    // If number of row values less than or equal to n, copy row directly
    scf::IfOp ifCopy = rewriter.create<scf::IfOp>(loc, copyRow, true);

    rewriter.setInsertionPointToStart(ifCopy.thenBlock());

    // copy contents
    rewriter.create<memref::CopyOp>(loc, Aj_view, Bj_view);
    rewriter.create<memref::CopyOp>(loc, Ax_view, Bx_view);

    // Else, fill output row with random selection from input row
    rewriter.setInsertionPointToStart(ifCopy.elseBlock());

    // These are unused. Should they be removed?
    // MLIRContext *context = module.getContext();
    // FuncOp chooseFunc = module.lookupSymbol<FuncOp>(chooseNSymbol);

    // TODO: Verify signature of this function is what we expect

    // Call function using output Bj row as temporary storage
    rewriter.create<mlir::CallOp>(
        loc, chooseNSymbol, TypeRange(),
        ArrayRef<Value>(
            {rngContext, Bj_size_64, Aj_size_64, Bj_view, Ax_view}));

    // Loop over randomly selected offsets
    scf::ParallelOp colLoop =
        rewriter.create<scf::ParallelOp>(loc, c0, Bj_size, c1);
    Value offset = colLoop.getInductionVars()[0];

    rewriter.setInsertionPointToStart(colLoop.getBody());

    Value sourceOffset_64 =
        rewriter.create<memref::LoadOp>(loc, Bj_view, offset);
    Value sourceOffset =
        rewriter.create<arith::IndexCastOp>(loc, sourceOffset_64, indexType);
    Value colIndex =
        rewriter.create<memref::LoadOp>(loc, Aj_view, sourceOffset);
    Value colValue =
        rewriter.create<memref::LoadOp>(loc, Ax_view, sourceOffset);
    // overwrite the randomly selected offset with the actual column index
    rewriter.create<memref::StoreOp>(loc, colIndex, Bj_view, offset);
    // write the corresponding value from source matrix
    rewriter.create<memref::StoreOp>(loc, colValue, Bx_view, offset);

    // end loop over columns

    // end loop over rows

    // Output array is populated
    rewriter.setInsertionPointAfter(rowLoop);
    // Resize output index and values to match total number of elements
    Value outputNNZ_64 = rewriter.create<memref::LoadOp>(loc, Bp, nrow);
    Value outputNNZ =
        rewriter.create<arith::IndexCastOp>(loc, outputNNZ_64, indexType);
    callResizeIndex(rewriter, module, loc, output, c1, outputNNZ);
    callResizeValues(rewriter, module, loc, output, outputNNZ);

    rewriter.replaceOp(op, output);

    return success();
  };
};

class LowerFromCoordinatesRewrite
    : public OpRewritePattern<graphblas::FromCoordinatesOp> {
public:
  using OpRewritePattern<graphblas::FromCoordinatesOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::FromCoordinatesOp op,
                                PatternRewriter &rewriter) const override {
    ModuleOp module = op->getParentOfType<ModuleOp>();
    Location loc = op->getLoc();
    Value indices = op.indices();
    Value values = op.values();
    ValueRange sizes = op.sizes();

    // Types
    RankedTensorType resultType =
        op.getResult().getType().cast<RankedTensorType>();
    Type int64Type = rewriter.getIntegerType(64);
    MemRefType memrefI64Type = MemRefType::get({-1}, int64Type);
    MemRefType memrefValueType =
        MemRefType::get({-1}, resultType.getElementType());

    unsigned rank = resultType.getRank();

    // Initial constants
    Value c0 = rewriter.create<arith::ConstantIndexOp>(loc, 0);
    Value c1 = rewriter.create<arith::ConstantIndexOp>(loc, 1);
    Value ci0 = rewriter.create<arith::ConstantIntOp>(loc, 0, int64Type);
    Value ci1 = rewriter.create<arith::ConstantIntOp>(loc, 1, int64Type);

    Value output =
        rewriter.create<sparse_tensor::InitOp>(loc, resultType, sizes);

    // Sparse Tensor info
    Value npointers, dimIndex;
    if (rank == 1) {
      npointers = c1;
      dimIndex = c0;
    } else {
      npointers = sizes[0];
      dimIndex = c1;
    }

    // Size sparse arrays
    Value nnz = rewriter.create<tensor::DimOp>(loc, indices, c0);
    Value npointers_plus1 = rewriter.create<arith::AddIOp>(loc, npointers, c1);
    callResizePointers(rewriter, module, loc, output, dimIndex,
                       npointers_plus1);
    callResizeIndex(rewriter, module, loc, output, dimIndex, nnz);
    callResizeValues(rewriter, module, loc, output, nnz);

    Value Op = rewriter.create<sparse_tensor::ToPointersOp>(loc, memrefI64Type,
                                                            output, dimIndex);
    Value Oi = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memrefI64Type,
                                                           output, dimIndex);
    Value Ox = rewriter.create<sparse_tensor::ToValuesOp>(loc, memrefValueType,
                                                          output);

    // Populate from indices and values
    // We assume everything is in the correct order
    // Increment the pointer count and fill in the index and value
    scf::ForOp loop = rewriter.create<scf::ForOp>(loc, c0, nnz, c1);
    {
      rewriter.setInsertionPointToStart(loop.getBody());
      Value pos = loop.getInductionVar();

      if (rank == 2) {
        Value row = rewriter.create<tensor::ExtractOp>(loc, indices,
                                                       ValueRange{pos, c0});
        Value currRowCount = rewriter.create<memref::LoadOp>(loc, Op, row);
        Value rowCount_plus1 =
            rewriter.create<arith::AddIOp>(loc, currRowCount, ci1);
        rewriter.create<memref::StoreOp>(loc, rowCount_plus1, Op, row);
      }
      Value idx = rewriter.create<tensor::ExtractOp>(loc, indices,
                                                     ValueRange{pos, dimIndex});
      Value idx64 = rewriter.create<arith::IndexCastOp>(loc, idx, int64Type);
      Value val = rewriter.create<tensor::ExtractOp>(loc, values, pos);
      rewriter.create<memref::StoreOp>(loc, idx64, Oi, pos);
      rewriter.create<memref::StoreOp>(loc, val, Ox, pos);

      rewriter.setInsertionPointAfter(loop);
    }

    if (rank == 2) {
      // Update pointers using cumsum
      scf::ForOp cumSumLoop =
          rewriter.create<scf::ForOp>(loc, c0, npointers, c1, ValueRange{ci0});
      {
        rewriter.setInsertionPointToStart(cumSumLoop.getBody());
        Value pos = cumSumLoop.getInductionVar();
        Value base = cumSumLoop.getLoopBody().getArgument(1);

        Value numEntries = rewriter.create<memref::LoadOp>(loc, Op, pos);
        rewriter.create<memref::StoreOp>(loc, base, Op, pos);
        Value nextBase = rewriter.create<arith::AddIOp>(loc, base, numEntries);
        rewriter.create<scf::YieldOp>(loc, nextBase);

        rewriter.setInsertionPointAfter(cumSumLoop);
      }
    }
    // Update last pointer with nnz
    Value nnz64 = rewriter.create<arith::IndexCastOp>(loc, nnz, int64Type);
    rewriter.create<memref::StoreOp>(loc, nnz64, Op, npointers);

    rewriter.replaceOp(op, output);

    return success();
  };
};

class LowerToCoordinatesRewrite
    : public OpRewritePattern<graphblas::ToCoordinatesOp> {
public:
  using OpRewritePattern<graphblas::ToCoordinatesOp>::OpRewritePattern;
  LogicalResult matchAndRewrite(graphblas::ToCoordinatesOp op,
                                PatternRewriter &rewriter) const override {
    Location loc = op->getLoc();
    Value input = op.input();
    RankedTensorType inputType = input.getType().cast<RankedTensorType>();
    RankedTensorType valuesType =
        op.getResult(1).getType().cast<RankedTensorType>();

    unsigned rank = inputType.getRank();
    Value nvals = rewriter.create<graphblas::NumValsOp>(loc, input);
    Value nrank = rewriter.create<arith::ConstantIndexOp>(loc, rank);

    Type indexType = rewriter.getIndexType();
    Type int64Type = rewriter.getIntegerType(64);
    MemRefType memrefI64Type = MemRefType::get({-1}, int64Type);
    MemRefType memrefIndicesType = MemRefType::get({-1, -1}, indexType);
    MemRefType memrefValueType =
        MemRefType::get({-1}, valuesType.getElementType());

    Value indices = rewriter.create<memref::AllocOp>(loc, memrefIndicesType,
                                                     ValueRange{nvals, nrank});
    Value values =
        rewriter.create<memref::AllocOp>(loc, memrefValueType, nvals);

    // Initial constants
    Value c0 = rewriter.create<arith::ConstantIndexOp>(loc, 0);
    Value c1 = rewriter.create<arith::ConstantIndexOp>(loc, 1);

    // Get sparse tensor info
    Value npointers, dimIndex;
    if (rank == 1) {
      npointers = c1;
      dimIndex = c0;
    } else {
      npointers = rewriter.create<graphblas::NumRowsOp>(loc, input);
      dimIndex = c1;
    }

    Value Ip = rewriter.create<sparse_tensor::ToPointersOp>(loc, memrefI64Type,
                                                            input, dimIndex);
    Value Ii = rewriter.create<sparse_tensor::ToIndicesOp>(loc, memrefI64Type,
                                                           input, dimIndex);
    Value Ix =
        rewriter.create<sparse_tensor::ToValuesOp>(loc, memrefValueType, input);

    // Iterate through input, populating indices and values
    scf::ForOp rowLoop = rewriter.create<scf::ForOp>(loc, c0, npointers, c1);
    {
      rewriter.setInsertionPointToStart(rowLoop.getBody());
      Value row = rowLoop.getInductionVar();
      Value row_plus1 = rewriter.create<arith::AddIOp>(loc, row, c1);

      Value j_start_64 = rewriter.create<memref::LoadOp>(loc, Ip, row);
      Value j_end_64 = rewriter.create<memref::LoadOp>(loc, Ip, row_plus1);
      Value j_start =
          rewriter.create<arith::IndexCastOp>(loc, j_start_64, indexType);
      Value j_end =
          rewriter.create<arith::IndexCastOp>(loc, j_end_64, indexType);

      scf::ForOp colLoop = rewriter.create<scf::ForOp>(loc, j_start, j_end, c1);
      {
        rewriter.setInsertionPointToStart(colLoop.getBody());
        Value jj = colLoop.getInductionVar();

        Value col_64 = rewriter.create<memref::LoadOp>(loc, Ii, jj);
        Value col = rewriter.create<arith::IndexCastOp>(loc, col_64, indexType);
        Value val = rewriter.create<memref::LoadOp>(loc, Ix, jj);

        if (rank == 2)
          rewriter.create<memref::StoreOp>(loc, row, indices,
                                           ValueRange{jj, c0});
        rewriter.create<memref::StoreOp>(loc, col, indices,
                                         ValueRange{jj, dimIndex});
        rewriter.create<memref::StoreOp>(loc, val, values, jj);

        rewriter.setInsertionPointAfter(colLoop);
      }

      rewriter.setInsertionPointAfter(rowLoop);
    }

    // Convert memrefs to tensors
    Value indicesTensor =
        rewriter.create<bufferization::ToTensorOp>(loc, indices);
    Value valuesTensor =
        rewriter.create<bufferization::ToTensorOp>(loc, values);

    rewriter.replaceOp(op, ValueRange{indicesTensor, valuesTensor});

    return success();
  };
};

void populateGraphBLASLoweringPatterns(RewritePatternSet &patterns) {
  patterns
      .add<LowerMatrixSelectRandomRewrite, LowerSelectRewrite,
           LowerSelectGenericRewrite, LowerReduceToVectorRewrite,
           LowerReduceToVectorGenericRewrite, LowerReduceToScalarRewrite,
           LowerReduceToScalarGenericRewrite, LowerConvertLayoutRewrite,
           LowerCastRewrite, LowerTransposeRewrite, LowerApplyRewrite,
           LowerApplyGenericRewrite, LowerUniformComplementRewrite,
           LowerMatrixMultiplyReduceToScalarGenericRewrite,
           LowerMatrixMultiplyRewrite, LowerMatrixMultiplyGenericRewrite,
           LowerUnionRewrite, LowerUnionGenericRewrite, LowerIntersectRewrite,
           LowerIntersectGenericRewrite, LowerUpdateRewrite,
           LowerUpdateGenericRewrite, LowerEqualRewrite, LowerDiagOpRewrite,
           LowerSelectMaskRewrite, LowerCommentRewrite, LowerPrintRewrite,
           LowerPrintTensorRewrite, LowerSizeRewrite, LowerNumRowsRewrite,
           LowerNumColsRewrite, LowerNumValsRewrite, LowerDupRewrite,
           LowerFromCoordinatesRewrite, LowerToCoordinatesRewrite>(
          patterns.getContext());
}

struct GraphBLASLoweringPass
    : public GraphBLASLoweringBase<GraphBLASLoweringPass> {
  void runOnOperation() override {
    MLIRContext *ctx = &getContext();
    RewritePatternSet patterns(ctx);
    ConversionTarget target(*ctx);
    populateGraphBLASLoweringPatterns(patterns);
    (void)applyPatternsAndFoldGreedily(getOperation(), std::move(patterns));
    target.addIllegalDialect<graphblas::GraphBLASDialect>();
  }
};

void populateGraphBLASStructuralizePatterns(RewritePatternSet &patterns) {
  patterns
      .add<TransposeDWIMRewrite, ReduceToVectorDWIMRewrite,
           MatrixMultiplyGenericDWIMFirstArgRewrite,
           MatrixMultiplyGenericDWIMSecondArgRewrite,
           MatrixMultiplyGenericDWIMMaskRewrite,
           MatrixMultiplyReduceToScalarGenericDWIMFirstArgRewrite,
           MatrixMultiplyReduceToScalarGenericDWIMSecondArgRewrite,
           MatrixMultiplyReduceToScalarGenericDWIMMaskRewrite,
           LowerMatrixMultiplyRewrite, LowerApplyRewrite, LowerSelectRewrite,
           LowerUnionRewrite, LowerIntersectRewrite, LowerUpdateRewrite,
           LowerReduceToVectorRewrite, LowerReduceToScalarRewrite>(
          patterns.getContext());
}

struct GraphBLASStructuralizePass
    : public GraphBLASStructuralizeBase<GraphBLASStructuralizePass> {
  void runOnOperation() override {
    MLIRContext *ctx = &getContext();
    RewritePatternSet patterns(ctx);
    ConversionTarget target(*ctx);
    populateGraphBLASStructuralizePatterns(patterns);
    (void)applyPatternsAndFoldGreedily(getOperation(), std::move(patterns));
  }
};
} // end anonymous namespace

std::unique_ptr<OperationPass<ModuleOp>> mlir::createGraphBLASLoweringPass() {
  return std::make_unique<GraphBLASLoweringPass>();
}

std::unique_ptr<OperationPass<ModuleOp>>
mlir::createGraphBLASStructuralizePass() {
  return std::make_unique<GraphBLASStructuralizePass>();
}
