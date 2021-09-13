""" This wraps RandomUtils.cpp """
cimport cython
from libc.stdint cimport int8_t, int16_t, int32_t, int64_t, uint8_t, uint16_t, uint32_t, uint64_t, uintptr_t


cdef extern from "RandomUtils.cpp" nogil:
    void *create_choose_uniform_context()
    void choose_uniform(void *rngContext, int64_t n, int64_t maxIndex,
                  int64_t *outAlloc, int64_t *outBase, int64_t outOffset,
                  int64_t outSize, int64_t outStride, double *valAlloc,
                  double *valBase, int64_t valOffset, int64_t valSize,
                  int64_t valStride)
    void destroy_choose_uniform_context(void *rngContext)


cdef class ChooseUniformContext:
    cdef void *_data

    def __init__(self):
        self._data = create_choose_uniform_context()

    def __dealloc__(self):
        destroy_choose_uniform_context(self._data)
    
    @property
    def __mlir_void_ptr__(self):
        return <uintptr_t>self._data
