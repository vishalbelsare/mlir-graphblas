"""
Various ops written in MLIR which implement dialects or other utilities
"""

import itertools
import math
from typing import Tuple, Sequence, Optional, Union
from .mlir_builder import MLIRFunctionBuilder, MLIRVar
from .types import (
    MemrefType,
    TensorType,
    SparseTensorType,
    SparseEncodingType,
    IndexType,
    IntType,
    LlvmPtrType,
)


class BaseOp:
    dialect = None  # This is optional if op is in the std dialect; otherwise define it
    name = None

    @classmethod
    def call(
        cls, irbuilder: MLIRFunctionBuilder, *args, **kwargs
    ) -> Tuple[MLIRVar, str]:
        raise NotImplementedError()

    @classmethod
    def ensure_mlirvar(cls, obj, type_=None):
        """
        Raises a TypeError is obj is not an MLIRVar
        If type_ is specified, raises a TypeError if obj.type is not of type type_
        """
        if not isinstance(obj, MLIRVar):
            raise TypeError(f"{cls.name} expects an MLIRVar, but got {obj}.")
        if type_ is not None:
            if not isinstance(obj.type, type_):
                raise TypeError(
                    f"{cls.name} expects an MLIRVar with type {type_}, but got {obj!r}"
                )

    def __init_subclass__(cls):
        MLIRFunctionBuilder.register_op(cls)


###########################################
# std ops
###########################################


class SelectOp(BaseOp):
    name = "select"

    @classmethod
    def call(cls, irbuilder, cond, lhs, rhs):
        cls.ensure_mlirvar(cond, IntType)
        cls.ensure_mlirvar(lhs)
        cls.ensure_mlirvar(rhs)
        if cond.type.num != 1:
            raise TypeError(f"Type of cond must be i1, not {cond.type}")
        if lhs.type != rhs.type:
            raise TypeError(f"Type mismatch: {lhs.type} != {rhs.type}")
        ret_val = irbuilder.new_var(lhs.type)
        return ret_val, (f"{ret_val.assign} = select {cond}, {lhs}, {rhs}: {lhs.type}")


###########################################
# arith ops
###########################################


class ConstantOp(BaseOp):
    dialect = "arith"
    name = "constant"

    # Special constants
    INFINITY = {
        "+inf": {
            "f64": "0x7FF0000000000000",
            "f32": "0x7F800000",
            "f16": "0x7C00",
        },
        "-inf": {
            "f64": "0xFFF0000000000000",
            "f32": "0xFF800000",
            "f16": "0xFC00",
        },
    }

    @classmethod
    def call(cls, irbuilder, value, type):
        # Special handling in case value is *already* an MLIR constant
        if isinstance(value, MLIRVar):
            return value, None

        if str(type) in {"bf16", "f16", "f32", "f64", "f80", "f128"}:
            if value == math.inf:
                value = cls.INFINITY["+inf"][str(type)]
            elif value == -math.inf:
                value = cls.INFINITY["-inf"][str(type)]
            else:
                value = float(value)
        ret_val = irbuilder.new_var(type)
        return ret_val, (f"{ret_val.assign} = arith.constant {value} : {type}")


class IndexCastOp(BaseOp):
    dialect = "arith"
    name = "index_cast"

    @classmethod
    def call(cls, irbuilder, value: MLIRVar, result_type):
        cls.ensure_mlirvar(value)
        ret_val = irbuilder.new_var(result_type)
        return ret_val, (
            f"{ret_val.assign} = arith.index_cast {value} : {value.type} to {ret_val.type}"
        )


class BitCastOp(BaseOp):
    dialect = "arith"
    name = "bitcast"

    @classmethod
    def call(cls, irbuilder, value: MLIRVar, result_type):
        cls.ensure_mlirvar(value)
        ret_val = irbuilder.new_var(result_type)
        return ret_val, (
            f"{ret_val.assign} = arith.bitcast {value} : {value.type} to {ret_val.type}"
        )


class SignedIntToFloatOp(BaseOp):
    dialect = "arith"
    name = "sitofp"

    @classmethod
    def call(cls, irbuilder, value: MLIRVar, result_type):
        cls.ensure_mlirvar(value)
        ret_val = irbuilder.new_var(result_type)
        return ret_val, (
            f"{ret_val.assign} = arith.sitofp {value} : {value.type} to {ret_val.type}"
        )


class FloatToSignedIntOp(BaseOp):
    dialect = "arith"
    name = "fptosi"

    @classmethod
    def call(cls, irbuilder, value: MLIRVar, result_type):
        cls.ensure_mlirvar(value)
        ret_val = irbuilder.new_var(result_type)
        return ret_val, (
            f"{ret_val.assign} = arith.fptosi {value} : {value.type} to {ret_val.type}"
        )


class AddIOp(BaseOp):
    dialect = "arith"
    name = "addi"

    @classmethod
    def call(cls, irbuilder, lhs, rhs):
        cls.ensure_mlirvar(lhs)
        cls.ensure_mlirvar(rhs)
        if lhs.type != rhs.type:
            raise TypeError(f"Type mismatch: {lhs.type} != {rhs.type}")
        ret_val = irbuilder.new_var(lhs.type)
        return ret_val, (f"{ret_val.assign} = arith.addi {lhs}, {rhs} : {lhs.type}")


class SubIOp(BaseOp):
    dialect = "arith"
    name = "subi"

    @classmethod
    def call(cls, irbuilder, lhs, rhs):
        cls.ensure_mlirvar(lhs)
        cls.ensure_mlirvar(rhs)
        if lhs.type != rhs.type:
            raise TypeError(f"Type mismatch: {lhs.type} != {rhs.type}")
        ret_val = irbuilder.new_var(lhs.type)
        return ret_val, (f"{ret_val.assign} = arith.subi {lhs}, {rhs} : {lhs.type}")


class MulIOp(BaseOp):
    dialect = "arith"
    name = "muli"

    @classmethod
    def call(cls, irbuilder, lhs, rhs):
        cls.ensure_mlirvar(lhs)
        cls.ensure_mlirvar(rhs)
        if lhs.type != rhs.type:
            raise TypeError(f"Type mismatch: {lhs.type} != {rhs.type}")
        ret_val = irbuilder.new_var(lhs.type)
        return ret_val, (f"{ret_val.assign} = arith.muli {lhs}, {rhs} : {lhs.type}")


class AddFOp(BaseOp):
    dialect = "arith"
    name = "addf"

    @classmethod
    def call(cls, irbuilder, lhs, rhs):
        cls.ensure_mlirvar(lhs)
        cls.ensure_mlirvar(rhs)
        if lhs.type != rhs.type:
            raise TypeError(f"Type mismatch: {lhs.type} != {rhs.type}")
        ret_val = irbuilder.new_var(lhs.type)
        return ret_val, (f"{ret_val.assign} = arith.addf {lhs}, {rhs} : {lhs.type}")


class SubFOp(BaseOp):
    dialect = "arith"
    name = "subf"

    @classmethod
    def call(cls, irbuilder, lhs, rhs):
        cls.ensure_mlirvar(lhs)
        cls.ensure_mlirvar(rhs)
        if lhs.type != rhs.type:
            raise TypeError(f"Type mismatch: {lhs.type} != {rhs.type}")
        ret_val = irbuilder.new_var(lhs.type)
        return ret_val, (f"{ret_val.assign} = arith.subf {lhs}, {rhs} : {lhs.type}")


class MulFOp(BaseOp):
    dialect = "arith"
    name = "mulf"

    @classmethod
    def call(cls, irbuilder, lhs, rhs):
        cls.ensure_mlirvar(lhs)
        cls.ensure_mlirvar(rhs)
        if lhs.type != rhs.type:
            raise TypeError(f"Type mismatch: {lhs.type} != {rhs.type}")
        ret_val = irbuilder.new_var(lhs.type)
        return ret_val, (f"{ret_val.assign} = arith.mulf {lhs}, {rhs} : {lhs.type}")


class DivFOp(BaseOp):
    dialect = "arith"
    name = "divf"

    @classmethod
    def call(cls, irbuilder, lhs, rhs):
        cls.ensure_mlirvar(lhs)
        cls.ensure_mlirvar(rhs)
        if lhs.type != rhs.type:
            raise TypeError(f"Type mismatch: {lhs.type} != {rhs.type}")
        ret_val = irbuilder.new_var(lhs.type)
        return ret_val, (f"{ret_val.assign} = arith.divf {lhs}, {rhs} : {lhs.type}")


class CmpIOp(BaseOp):
    dialect = "arith"
    name = "cmpi"
    # fmt: off
    allowed_cmpstr = {
        "eq", "ne",
        "slt", "sle", "sgt", "sge",
        "ult", "ule", "ugt", "uge"
    }
    # fmt: on

    @classmethod
    def call(cls, irbuilder, lhs, rhs, cmpstr):
        cls.ensure_mlirvar(lhs)
        cls.ensure_mlirvar(rhs)
        if lhs.type != rhs.type:
            raise TypeError(f"Type mismatch: {lhs.type} != {rhs.type}")
        cmpstr = cmpstr.lower()
        if cmpstr not in cls.allowed_cmpstr:
            raise ValueError(f"Unknown cmpstr: {cmpstr}")
        ret_val = irbuilder.new_var("i1")
        return ret_val, (
            f'{ret_val.assign} = arith.cmpi "{cmpstr}", {lhs}, {rhs} : {lhs.type}'
        )


class CmpFOp(BaseOp):
    dialect = "arith"
    name = "cmpf"
    # fmt: off
    # See https://llvm.org/docs/LangRef.html#fcmp-instruction for explanation
    allowed_cmpstr = {
        "false", "oeq", "ogt", "oge", "olt", "ole", "one", "ord",
        "ueq", "ugt", "uge", "ult", "ule", "une", "uno", "true"
    }
    # fmt: on

    @classmethod
    def call(cls, irbuilder, lhs, rhs, cmpstr):
        cls.ensure_mlirvar(lhs)
        cls.ensure_mlirvar(rhs)
        if lhs.type != rhs.type:
            raise TypeError(f"Type mismatch: {lhs.type} != {rhs.type}")
        cmpstr = cmpstr.lower()
        if cmpstr not in cls.allowed_cmpstr:
            raise ValueError(f"Unknown cmpstr: {cmpstr}")
        ret_val = irbuilder.new_var("i1")
        return ret_val, (
            f'{ret_val.assign} = arith.cmpf "{cmpstr}", {lhs}, {rhs} : {lhs.type}'
        )


class AndIOp(BaseOp):
    dialect = "arith"
    name = "andi"

    @classmethod
    def call(cls, irbuilder, lhs, rhs):
        cls.ensure_mlirvar(lhs)
        cls.ensure_mlirvar(rhs)
        if lhs.type != rhs.type:
            raise TypeError(f"Type mismatch: {lhs.type} != {rhs.type}")
        ret_val = irbuilder.new_var("i1")
        return ret_val, (f"{ret_val.assign} = arith.andi {lhs}, {rhs} : {lhs.type}")


class OrIOp(BaseOp):
    dialect = "arith"
    name = "ori"

    @classmethod
    def call(cls, irbuilder, lhs, rhs):
        cls.ensure_mlirvar(lhs)
        cls.ensure_mlirvar(rhs)
        if lhs.type != rhs.type:
            raise TypeError(f"Type mismatch: {lhs.type} != {rhs.type}")
        ret_val = irbuilder.new_var("i1")
        return ret_val, (f"{ret_val.assign} = arith.ori {lhs}, {rhs} : {lhs.type}")


class TruncIOp(BaseOp):
    dialect = "arith"
    name = "trunci"

    @classmethod
    def call(cls, irbuilder, val, return_type):
        cls.ensure_mlirvar(val)
        ret_val = irbuilder.new_var(return_type)
        if isinstance(ret_val.type, SparseTensorType):
            if not isinstance(val.type, SparseTensorType):
                raise TypeError(
                    f"val type is not compatible with return_type: {val.type} vs {return_type}"
                )
            in_bits = val.type.value_type.num
            out_bits = ret_val.type.value_type.num
        else:
            if not isinstance(val.type, IntType):
                raise TypeError(
                    f"val type is not compatible with return_type: {val.type} vs {return_type}"
                )
            in_bits = val.type.num
            out_bits = ret_val.type.num
        if in_bits < out_bits:
            raise ValueError(
                f"return_type must have fewer bits than val type: {in_bits} < {out_bits}"
            )
        return ret_val, (
            f"{ret_val.assign} = arith.trunci {val} : {val.type} to {ret_val.type}"
        )


###########################################
# math ops
###########################################


class PowFOp(BaseOp):
    dialect = "math"
    name = "powf"

    @classmethod
    def call(cls, irbuilder, lhs, rhs):
        cls.ensure_mlirvar(lhs)
        cls.ensure_mlirvar(rhs)
        if lhs.type != rhs.type:
            raise TypeError(f"Type mismatch: {lhs.type} != {rhs.type}")
        ret_val = irbuilder.new_var(lhs.type)
        return ret_val, (f"{ret_val.assign} = math.powf {lhs}, {rhs} : {lhs.type}")


###########################################
# memref ops
###########################################


class MemrefAllocOp(BaseOp):
    dialect = "memref"
    name = "alloc"

    @classmethod
    def call(cls, irbuilder, type: str, *dim_sizes):
        ret_val = irbuilder.new_var(type)
        for ds in dim_sizes:
            cls.ensure_mlirvar(ds, IndexType)
        shape = MemrefType.parse(type, irbuilder.aliases).shape
        expected_num_dim_sizes = sum((d == -1 for d in shape))
        if expected_num_dim_sizes != len(dim_sizes):
            raise ValueError(
                f"{type} expected {expected_num_dim_sizes} given dim sizes; got {len(dim_sizes)}."
            )
        return ret_val, (
            f"{ret_val.assign} = memref.alloc({', '.join(str(ds) for ds in dim_sizes)}) : {ret_val.type}"
        )


class MemrefAllocaOp(BaseOp):
    dialect = "memref"
    name = "alloca"

    @classmethod
    def call(cls, irbuilder, type: str, *dim_sizes):
        ret_val = irbuilder.new_var(type)
        for ds in dim_sizes:
            cls.ensure_mlirvar(ds, IndexType)
        shape = MemrefType.parse(type, irbuilder.aliases).shape
        expected_num_dim_sizes = sum((d == -1 for d in shape))
        if expected_num_dim_sizes != len(dim_sizes):
            raise ValueError(
                f"{type} expected {expected_num_dim_sizes} given dim sizes; got {len(dim_sizes)}."
            )
        return ret_val, (
            f"{ret_val.assign} = memref.alloca({', '.join(str(ds) for ds in dim_sizes)}) : {ret_val.type}"
        )


class MemrefStoreOp(BaseOp):
    dialect = "memref"
    name = "store"

    @classmethod
    def call(
        cls,
        irbuilder,
        value,
        destination: MLIRVar,
        indices: Sequence[Union[MLIRVar, int]],
    ):
        cls.ensure_mlirvar(destination)
        # Be forgiving if a single index is provided
        if not hasattr(indices, "__len__"):
            indices = [indices]
        indices_string = ", ".join(map(str, indices))
        return None, (
            f"memref.store {value}, {destination}[{indices_string}] : {destination.type}"
        )


class MemrefLoadOp(BaseOp):
    dialect = "memref"
    name = "load"

    @classmethod
    def call(
        cls, irbuilder, input_memref: MLIRVar, indices: Sequence[Union[MLIRVar, int]]
    ):
        cls.ensure_mlirvar(input_memref, MemrefType)
        # Be forgiving if a single index is provided
        if not hasattr(indices, "__len__"):
            indices = [indices]
        indices_string = ", ".join(map(str, indices))
        ret_val = irbuilder.new_var(input_memref.type.value_type)
        return ret_val, (
            f"{ret_val.assign} = memref.load {input_memref}[{indices_string}] : {input_memref.type}"
        )


###########################################
# bufferization ops
###########################################


class BufferizationToTensorOp(BaseOp):
    dialect = "bufferization"
    name = "to_tensor"

    @classmethod
    def call(cls, irbuilder, input_memref: MLIRVar, return_type: str):
        cls.ensure_mlirvar(input_memref, MemrefType)
        # TODO infer return_type from input_memref
        ret_val = irbuilder.new_var(return_type)
        return ret_val, (
            f"{ret_val.assign} = bufferization.to_tensor {input_memref} : {input_memref.type}"
        )


###########################################
# linalg ops
###########################################


class LinalgInitOp(BaseOp):
    dialect = "linalg"
    name = "init_tensor"

    @classmethod
    def call(cls, irbuilder, return_type: str, *dim_sizes: str):
        for dim_size in dim_sizes:
            if isinstance(dim_size, int):
                cls.ensure_mlirvar(dim_size)
        ret_val = irbuilder.new_var(return_type)
        dim_string = ", ".join(map(str, dim_sizes))
        return ret_val, (
            f"{ret_val.assign} = linalg.init_tensor[{dim_string}] : {ret_val.type}"
        )


###########################################
# tensor ops
###########################################


class TensorDimOp(BaseOp):
    dialect = "tensor"
    name = "dim"

    @classmethod
    def call(cls, irbuilder, input_tensor: MLIRVar, index: Union[MLIRVar, int]):
        cls.ensure_mlirvar(input_tensor, (TensorType, SparseTensorType))
        index = irbuilder.arith.constant(index, "index")
        ret_val = irbuilder.new_var("index")
        return ret_val, (
            f"{ret_val.assign} = tensor.dim {input_tensor}, {index} : "
            f"{input_tensor.type}"
        )


class TensorExtractOp(BaseOp):
    dialect = "tensor"
    name = "extract"

    @classmethod
    def call(cls, irbuilder, input, dim):
        cls.ensure_mlirvar(input, TensorType)
        cls.ensure_mlirvar(dim, IndexType)
        ret_val = irbuilder.new_var(input.type.value_type)
        return ret_val, (
            f"{ret_val.assign} = tensor.extract {input}[{dim}] : {input.type}"
        )


class TensorFromElementsOp(BaseOp):
    dialect = "tensor"
    name = "from_elements"

    @classmethod
    def call(cls, irbuilder, *elements):
        common_type = None
        for elem in elements:
            cls.ensure_mlirvar(elem)
            if common_type is None:
                common_type = elem.type
            elif common_type != elem.type:
                raise TypeError(
                    f"All elements must have the same type: {elem.type} != {common_type}"
                )
        ret_val = irbuilder.new_var(f"tensor<{len(elements)}x{common_type}>")
        elemstr = ", ".join(map(str, elements))
        return ret_val, (
            f"{ret_val.assign} = tensor.from_elements {elemstr} : {ret_val.type}"
        )


###########################################
# llvm ops
###########################################


class LLVMGetElementPtrOp(BaseOp):
    dialect = "llvm"
    name = "getelementptr"

    @classmethod
    def call(cls, irbuilder, list_, index):
        cls.ensure_mlirvar(list_)
        ret_val = irbuilder.new_var(list_.type)
        return ret_val, (
            f"{ret_val.assign} = llvm.getelementptr {list_}[{index}] : "
            f"({list_.type}, {index.type}) -> {list_.type}"
        )


class LLVMAllocaOp(BaseOp):
    dialect = "llvm"
    name = "alloca"

    @classmethod
    def call(cls, irbuilder, size: MLIRVar, element_type: str):
        cls.ensure_mlirvar(size)
        return_type = f"!llvm.ptr<{element_type}>"
        ret_val = irbuilder.new_var(return_type)
        return ret_val, (
            f"{ret_val.assign} = llvm.alloca {size} x {size.type} : "
            f"({size.type}) -> !llvm.ptr<{element_type}>"
        )


class LLVMLoadOp(BaseOp):
    dialect = "llvm"
    name = "load"

    @classmethod
    def call(cls, irbuilder, pointer, return_type: str):
        cls.ensure_mlirvar(pointer)
        ret_val = irbuilder.new_var(return_type)
        return ret_val, (f"{ret_val.assign} = llvm.load {pointer} : {pointer.type}")


class LLVMStoreOp(BaseOp):
    dialect = "llvm"
    name = "store"

    @classmethod
    def call(cls, irbuilder, value: MLIRVar, pointer: MLIRVar):
        cls.ensure_mlirvar(value)
        cls.ensure_mlirvar(pointer)
        if value.type != pointer.type.internal_type:
            raise TypeError("Value type and pointer internal type must match.")
        return None, f"llvm.store {value}, {pointer} : {pointer.type}"


###########################################
# sparse_tensor ops
###########################################


class SparseTensorConvert(BaseOp):
    dialect = "sparse_tensor"
    name = "convert"

    @classmethod
    def call(cls, irbuilder, input, return_type):
        cls.ensure_mlirvar(input, SparseTensorType)
        ret_val = irbuilder.new_var(return_type)
        return ret_val, (
            f"{ret_val.assign} = sparse_tensor.convert {input} : "
            f"{input.type} to {ret_val.type}"
        )


class SparseTensorPointers(BaseOp):
    dialect = "sparse_tensor"
    name = "pointers"

    @classmethod
    def call(cls, irbuilder, input, dim):
        cls.ensure_mlirvar(input, SparseTensorType)
        cls.ensure_mlirvar(dim, IndexType)
        ret_val = irbuilder.new_var("memref<?xi64>")
        return ret_val, (
            f"{ret_val.assign} = sparse_tensor.pointers {input}, {dim} : "
            f"{input.type} to memref<?xi64>"
        )


class SparseTensorIndices(BaseOp):
    dialect = "sparse_tensor"
    name = "indices"

    @classmethod
    def call(cls, irbuilder, input, dim):
        cls.ensure_mlirvar(input, SparseTensorType)
        cls.ensure_mlirvar(dim, IndexType)
        ret_val = irbuilder.new_var("memref<?xi64>")
        return ret_val, (
            f"{ret_val.assign} = sparse_tensor.indices {input}, {dim} : "
            f"{input.type} to memref<?xi64>"
        )


class SparseTensorValues(BaseOp):
    dialect = "sparse_tensor"
    name = "values"

    @classmethod
    def call(cls, irbuilder, input):
        cls.ensure_mlirvar(input, SparseTensorType)
        ret_val = irbuilder.new_var(f"memref<?x{input.type.value_type}>")
        return ret_val, (
            f"{ret_val.assign} = sparse_tensor.values {input} : "
            f"{input.type} to memref<?x{input.type.value_type}>"
        )


###########################################
# graphblas ops
###########################################


class GraphBLAS_Size(BaseOp):
    dialect = "graphblas"
    name = "size"

    @classmethod
    def call(cls, irbuilder, input):
        cls.ensure_mlirvar(input, SparseTensorType)
        ret_val = irbuilder.new_var("index")
        return ret_val, (f"{ret_val.assign} = graphblas.size {input} : {input.type}")


class GraphBLAS_NumRows(BaseOp):
    dialect = "graphblas"
    name = "num_rows"

    @classmethod
    def call(cls, irbuilder, input):
        cls.ensure_mlirvar(input, SparseTensorType)
        ret_val = irbuilder.new_var("index")
        return ret_val, (
            f"{ret_val.assign} = graphblas.num_rows {input} : {input.type}"
        )


class GraphBLAS_NumCols(BaseOp):
    dialect = "graphblas"
    name = "num_cols"

    @classmethod
    def call(cls, irbuilder, input):
        cls.ensure_mlirvar(input, SparseTensorType)
        ret_val = irbuilder.new_var("index")
        return ret_val, (
            f"{ret_val.assign} = graphblas.num_cols {input} : {input.type}"
        )


class GraphBLAS_NumVals(BaseOp):
    dialect = "graphblas"
    name = "num_vals"

    @classmethod
    def call(cls, irbuilder, input):
        cls.ensure_mlirvar(input, SparseTensorType)
        ret_val = irbuilder.new_var("index")
        return ret_val, (
            f"{ret_val.assign} = graphblas.num_vals {input} : {input.type}"
        )


class GraphBLAS_Dup(BaseOp):
    dialect = "graphblas"
    name = "dup"

    @classmethod
    def call(cls, irbuilder, input):
        cls.ensure_mlirvar(input, SparseTensorType)
        ret_val = irbuilder.new_var(input.type)
        return ret_val, (f"{ret_val.assign} = graphblas.dup {input} : {input.type}")


class GraphBLAS_ConvertLayout(BaseOp):
    dialect = "graphblas"
    name = "convert_layout"

    @classmethod
    def call(cls, irbuilder, input, return_type: str):
        cls.ensure_mlirvar(input, SparseTensorType)
        ret_val = irbuilder.new_var(return_type)
        return ret_val, (
            f"{ret_val.assign} = graphblas.convert_layout {input} : "
            f"{input.type} to {ret_val.type}"
        )


class GraphBLAS_Cast(BaseOp):
    dialect = "graphblas"
    name = "cast"

    @classmethod
    def call(cls, irbuilder, input, return_type: str):
        cls.ensure_mlirvar(input, SparseTensorType)
        ret_val = irbuilder.new_var(return_type)
        return ret_val, (
            f"{ret_val.assign} = graphblas.cast {input} : "
            f"{input.type} to {ret_val.type}"
        )


class GraphBLAS_Transpose(BaseOp):
    dialect = "graphblas"
    name = "transpose"

    @classmethod
    def call(cls, irbuilder, input, return_type: str):
        cls.ensure_mlirvar(input, SparseTensorType)
        ret_val = irbuilder.new_var(return_type)
        return ret_val, (
            f"{ret_val.assign} = graphblas.transpose {input} : "
            f"{input.type} to {ret_val.type}"
        )


class GraphBLAS_Union(BaseOp):
    dialect = "graphblas"
    name = "union"

    @classmethod
    def call(cls, irbuilder, lhs, rhs, operator):
        cls.ensure_mlirvar(lhs, SparseTensorType)
        cls.ensure_mlirvar(rhs, SparseTensorType)
        ret_val = irbuilder.new_var(lhs.type)
        return ret_val, (
            f'{ret_val.assign} = graphblas.union {lhs}, {rhs} {{ union_operator = "{operator}" }} :'
            f"({lhs.type}, {rhs.type}) to {ret_val.type}"
        )


class GraphBLAS_Intersect(BaseOp):
    dialect = "graphblas"
    name = "intersect"

    @classmethod
    def call(cls, irbuilder, lhs, rhs, operator):
        cls.ensure_mlirvar(lhs, SparseTensorType)
        cls.ensure_mlirvar(rhs, SparseTensorType)
        # Figure out correct return type
        return_type = lhs.type
        if operator in {"eq", "ge", "gt", "le", "lt", "ne"}:
            return_type = SparseTensorType(lhs.type.shape, "i8", lhs.type.encoding)
        ret_val = irbuilder.new_var(return_type)
        return ret_val, (
            f'{ret_val.assign} = graphblas.intersect {lhs}, {rhs} {{ intersect_operator = "{operator}" }} :'
            f"({lhs.type}, {rhs.type}) to {ret_val.type}"
        )


class GraphBLAS_Update(BaseOp):
    dialect = "graphblas"
    name = "update"

    @classmethod
    def call(
        cls,
        irbuilder,
        input,
        output,
        accumulate=None,
        *,
        mask=None,
        mask_complement=False,
        replace=False,
    ):
        cls.ensure_mlirvar(input, SparseTensorType)
        cls.ensure_mlirvar(output, SparseTensorType)
        accum_str = f'accumulate_operator = "{accumulate}", ' if accumulate else ""
        mask_str = f"({mask})" if mask is not None else ""
        mask_type_str = f"({mask.type})" if mask is not None else ""
        return None, (
            f"graphblas.update {input} -> {output}{mask_str} "
            f'{{ {accum_str}mask_complement = {"true" if mask_complement else "false"}, '
            f'replace = {"true" if replace else "false"} }} : {input.type} -> {output.type}{mask_type_str}'
        )


class GraphBLAS_UniformComplement(BaseOp):
    dialect = "graphblas"
    name = "uniform_complement"

    @classmethod
    def call(cls, irbuilder, input, value):
        cls.ensure_mlirvar(input, SparseTensorType)
        value = irbuilder.arith.constant(value, input.type.value_type)
        ret_val = irbuilder.new_var(
            SparseTensorType(input.type.shape, value.type, input.type.encoding)
        )
        return ret_val, (
            f"{ret_val.assign} = graphblas.uniform_complement {input}, {value} : "
            f"{input.type}, {value.type} to {ret_val.type}"
        )


class GraphBLAS_Equal(BaseOp):
    dialect = "graphblas"
    name = "equal"

    @classmethod
    def call(cls, irbuilder, lhs, rhs):
        cls.ensure_mlirvar(lhs, SparseTensorType)
        cls.ensure_mlirvar(rhs, SparseTensorType)
        ret_val = irbuilder.new_var("i1")
        return ret_val, (
            f"{ret_val.assign} = graphblas.equal {lhs}, {rhs} : {lhs.type}, {rhs.type}"
        )


class GraphBLAS_Select(BaseOp):
    dialect = "graphblas"
    name = "select"

    @classmethod
    def call(
        cls,
        irbuilder,
        input,
        selector: str,
        thunk: MLIRVar = None,
        *,
        rng_context: MLIRVar = None,
    ):
        cls.ensure_mlirvar(input, SparseTensorType)
        if thunk is not None:
            cls.ensure_mlirvar(thunk)
        if rng_context is not None:
            cls.ensure_mlirvar(rng_context)
        if selector == "probability":
            irbuilder.needed_function_table["random_double"] = (
                f"func private @random_double(!llvm.ptr<i8>) -> (f64)",
                ["!llvm.ptr<i8>"],
                "",
            )

        ret_val = irbuilder.new_var(input.type)
        text = [
            f"{ret_val.assign} = graphblas.select {input}",
            f' {{ selector = "{selector}" }}',
            f" : {input.type}",
            f" to {ret_val.type}",
        ]
        if thunk is not None:
            text.insert(1, f", {thunk}")
            text.insert(-1, f", {thunk.type}")
        if rng_context is not None:
            if thunk is None:
                raise ValueError("Thunk must be provided when rng_context is provided")
            text.insert(2, f", {rng_context}")
            text.insert(-1, f", {rng_context.type}")

        return ret_val, "".join(text)


class GraphBLAS_ReduceToVector(BaseOp):
    dialect = "graphblas"
    name = "reduce_to_vector"

    @classmethod
    def call(cls, irbuilder, input, aggregator, axis):
        cls.ensure_mlirvar(input, SparseTensorType)
        if axis not in (0, 1):
            raise TypeError(f"Illegal axis: {axis}, must be 0 or 1")
        sparse_vec_encoding = SparseEncodingType(
            ["compressed"],
            None,
            input.type.encoding.pointer_bit_width,
            input.type.encoding.index_bit_width,
        )
        return_element_type = (
            IntType(64)
            if aggregator in ("argmin", "argmax", "count")
            else input.type.value_type
        )
        return_type = SparseTensorType([-1], return_element_type, sparse_vec_encoding)
        ret_val = irbuilder.new_var(return_type)
        return ret_val, (
            f"{ret_val.assign} = graphblas.reduce_to_vector {input} "
            f'{{ aggregator = "{aggregator}" , axis = {axis} }} : {input.type} to {ret_val.type}'
        )


class GraphBLAS_ReduceToScalar(BaseOp):
    dialect = "graphblas"
    name = "reduce_to_scalar"

    @classmethod
    def call(cls, irbuilder, input, aggregator):
        cls.ensure_mlirvar(input, SparseTensorType)
        if aggregator in {"count", "argmin", "argmax"}:
            return_type = "i64"
        else:
            return_type = input.type.value_type
        ret_val = irbuilder.new_var(return_type)
        return ret_val, (
            f"{ret_val.assign} = graphblas.reduce_to_scalar {input} "
            f'{{ aggregator = "{aggregator}" }} : {input.type} to {ret_val.type}'
        )


class GraphBLAS_Apply(BaseOp):
    dialect = "graphblas"
    name = "apply"

    @classmethod
    def call(
        cls, irbuilder, input, apply_op, right=None, *, left=None, return_type=None
    ):
        cls.ensure_mlirvar(input, SparseTensorType)

        if return_type is None:
            return_type = input.type
        ret_val = irbuilder.new_var(return_type)

        if left is not None:
            if right is not None:
                raise TypeError("Only one thunk allowed.")
            cls.ensure_mlirvar(left)
            code = (
                f"{ret_val.assign} = graphblas.apply {left}, {input} "
                f'{{ apply_operator = "{apply_op}" }} : ({left.type}, {input.type}) to {ret_val.type}'
            )
        elif right is not None:
            cls.ensure_mlirvar(right)
            code = (
                f"{ret_val.assign} = graphblas.apply {input}, {right} "
                f'{{ apply_operator = "{apply_op}" }} : ({input.type}, {right.type}) to {ret_val.type}'
            )
        else:
            code = (
                f"{ret_val.assign} = graphblas.apply {input} "
                f'{{ apply_operator = "{apply_op}" }} : ({input.type}) to {ret_val.type}'
            )

        return ret_val, code


class GraphBLAS_MatrixMultiply(BaseOp):
    dialect = "graphblas"
    name = "matrix_multiply"

    @classmethod
    def call(cls, irbuilder, a, b, semiring, *, mask=None, mask_complement=False):
        cls.ensure_mlirvar(a, SparseTensorType)
        cls.ensure_mlirvar(b, SparseTensorType)
        if len(b.type.shape) == 1:
            return_type = b.type
        else:
            return_type = a.type
        # TODO: make the return type more robust; may depend on a, b, and/or semiring
        ret_val = irbuilder.new_var(return_type)
        if mask:
            cls.ensure_mlirvar(mask)
            mlir = (
                f"{ret_val.assign} = graphblas.matrix_multiply {a}, {b}, {mask} "
                f'{{ semiring = "{semiring}", mask_complement = {"true" if mask_complement else "false"} }}'
                f": ({a.type}, {b.type}, {mask.type}) to {ret_val.type}"
            )
        else:
            mlir = (
                f"{ret_val.assign} = graphblas.matrix_multiply {a}, {b} "
                f'{{ semiring = "{semiring}" }} : ({a.type}, {b.type}) to {ret_val.type}'
            )
        return ret_val, mlir


class GraphBLAS_Diag(BaseOp):
    dialect = "graphblas"
    name = "diag"

    @classmethod
    def call(cls, irbuilder, input, return_type: str):
        cls.ensure_mlirvar(input, SparseTensorType)
        ret_val = irbuilder.new_var(return_type)
        return ret_val, (
            f"{ret_val.assign} = graphblas.diag {input} : {input.type} to {ret_val.type}"
        )


class GraphBLAS_MatrixSelectRandom(BaseOp):
    dialect = "graphblas"
    name = "matrix_select_random"
    allowed_choose_n = {
        "choose_first": "i64",
        "choose_uniform": "!llvm.ptr<i8>",
        "choose_weighted": "!llvm.ptr<i8>",
    }

    @classmethod
    def call(cls, irbuilder, input, n: MLIRVar, rng_context: MLIRVar, choose_n: str):
        cls.ensure_mlirvar(input, SparseTensorType)
        cls.ensure_mlirvar(n, IntType)
        cls.ensure_mlirvar(rng_context)
        if choose_n not in cls.allowed_choose_n:
            raise TypeError(
                f"Illegal choose_n function: {choose_n}, must be one of {cls.allowed_choose_n}"
            )

        first_rand_ret_type = cls.allowed_choose_n[choose_n]
        irbuilder.needed_function_table[choose_n] = (
            f"func private @{choose_n}({first_rand_ret_type}, i64, i64,"
            "memref<?xi64, #map1d>, memref<?xf64, #map1d>) -> ()",
            [
                first_rand_ret_type,
                "i64",
                "i64",
                "memref<?xi64, #map1d>",
                "memref<?xf64, #map1d>",
            ],
            "",
        )

        ret_val = irbuilder.new_var(input.type)
        return ret_val, (
            f"{ret_val.assign} = graphblas.matrix_select_random {input}, {n}, {rng_context} "
            + f"{{ choose_n = @{choose_n} }}"
            + f" : ({input.type}, {n.type}, {rng_context.type}) to {input.type}"
        )


class GraphBLAS_FromCOO(BaseOp):
    dialect = "graphblas"
    name = "from_coo"

    @classmethod
    def call(cls, irbuilder, indices, values, shape):
        cls.ensure_mlirvar(indices, TensorType)
        cls.ensure_mlirvar(values, TensorType)
        if not hasattr(shape, "__len__"):
            shape = [shape]
        shape = [irbuilder.arith.constant(sh, "index") for sh in shape]
        if len(shape) == 2:
            ret_type = f"tensor<?x?x{values.type.value_type}, #CSR64>"
        else:
            ret_type = f"tensor<?x{values.type.value_type}, #CV64>"
        ret_val = irbuilder.new_var(ret_type)
        dimstr = ", ".join(map(str, shape))
        return ret_val, (
            f"{ret_val.assign} = graphblas.from_coo {indices}, {values} [{dimstr}]"
            + f" : {indices.type}, {values.type} to {ret_val.type}"
        )


class GraphBLAS_ToCOO(BaseOp):
    dialect = "graphblas"
    name = "to_coo"

    @classmethod
    def call(cls, irbuilder, tensor):
        cls.ensure_mlirvar(tensor, SparseTensorType)
        ret_val = irbuilder.new_tuple(
            "tensor<?x?xindex>", f"tensor<?x{tensor.type.value_type}>"
        )
        return ret_val, (
            f"{ret_val.assign} = graphblas.to_coo {tensor} : "
            f"{tensor.type} to tensor<?x?xindex>, tensor<?x{tensor.type.value_type}>"
        )


class GraphBLAS_Print(BaseOp):
    dialect = "graphblas"
    name = "print"

    @classmethod
    def call(cls, irbuilder, *original_printables):
        printables = [""]
        for printable in original_printables:
            if isinstance(printable, str) and isinstance(printables[-1], str):
                printables[-1] = printables[-1] + printable
            elif not isinstance(printable, str) and not isinstance(printables[-1], str):
                printables.append(" ")
                printables.append(printable)
            else:
                printables.append(printable)

            if not isinstance(printable, str):
                cls.ensure_mlirvar(printable)
        values = printables[1::2]
        string_attributes = printables[::2]
        return None, (
            "graphblas.print "
            + ", ".join(str(v) for v in values)
            + " { strings = ["
            + ", ".join(
                '"' + s.replace("\n", "\\n").replace('"', '\\"') + '"'
                for s in string_attributes
            )
            + "] } : "
            + ", ".join(str(v.type) for v in values)
        )


class GraphBLAS_PrintTensor(BaseOp):
    dialect = "graphblas"
    name = "print_tensor"

    @classmethod
    def call(cls, irbuilder, tensor, level: int):
        cls.ensure_mlirvar(tensor, SparseTensorType)
        return (
            None,
            f"graphblas.print_tensor {tensor} {{ level = {level} }} : {tensor.type}",
        )


###########################################
# util ops
###########################################


class PtrToTensorOp(BaseOp):
    dialect = "util"
    name = "ptr8_to_tensor"

    @classmethod
    def call(cls, irbuilder, input, return_type: str):
        cls.ensure_mlirvar(input)
        tensor_type = SparseTensorType.parse(return_type, irbuilder.aliases)

        ret_val = irbuilder.new_var(return_type)
        funcname = f"ptr8_to_{tensor_type.to_short_string()}"
        irbuilder.needed_function_table[funcname] = (
            f"func private @{funcname}(!llvm.ptr<i8>) -> {ret_val.type}",
            ["!llvm.ptr<i8>"],
            return_type,
        )

        return ret_val, (
            f"{ret_val.assign} = call @{funcname}({input}) : "
            f"(!llvm.ptr<i8>) -> {ret_val.type}"
        )


class TensorToPtrOp(BaseOp):
    dialect = "util"
    name = "tensor_to_ptr8"

    @classmethod
    def call(cls, irbuilder, input):
        cls.ensure_mlirvar(input, SparseTensorType)

        ret_val = irbuilder.new_var("!llvm.ptr<i8>")
        funcname = f"{input.type.to_short_string()}_to_ptr8"
        irbuilder.needed_function_table[funcname] = (
            f"func private @{funcname}({input.type}) -> !llvm.ptr<i8>",
            [str(input.type)],
            "!llvm.ptr<i8>",
        )

        return ret_val, (
            f"{ret_val.assign} = call @{funcname}({input}) : "
            f"({input.type}) -> !llvm.ptr<i8>"
        )


class NewSparseTensor(BaseOp):
    dialect = "util"
    name = "new_sparse_tensor"

    @classmethod
    def call(cls, irbuilder, tensor_type: str, *dim_sizes):
        """
        Vectors take a single dim_size
        Matrices need nrows, ncols
        """
        ret_val = irbuilder.new_var(tensor_type)
        cls.ensure_mlirvar(ret_val, SparseTensorType)
        rank = ret_val.type.encoding.rank
        if len(dim_sizes) != rank:
            raise ValueError(
                f"Type {tensor_type} implies rank {rank}."
                "Must provide exactly that many dim_sizes."
            )
        for ds in dim_sizes:
            cls.ensure_mlirvar(ds, IndexType)

        statements = [
            f"{ret_val.assign} = sparse_tensor.init [{', '.join(str(ds) for ds in dim_sizes)}] :"
            f"{ret_val.type}"
        ]
        c1_var, stmt = ConstantOp.call(irbuilder, 1, "index")
        statements.append(stmt)
        # Resize pointers to make this a valid sparse tensor structure
        if rank == 1:
            dim_var, stmt = ConstantOp.call(irbuilder, 0, "index")
            statements.append(stmt)
            npointers_var = c1_var
        else:
            dim_var = c1_var
            if ret_val.type.encoding.ordering == [1, 0]:
                npointers_var, stmt = GraphBLAS_NumCols.call(irbuilder, ret_val)
            else:
                npointers_var, stmt = GraphBLAS_NumRows.call(irbuilder, ret_val)
            statements.append(stmt)
        npointers_plus_1_var, stmt = AddIOp.call(irbuilder, npointers_var, c1_var)
        statements.append(stmt)
        ret_val_ptr, stmt = TensorToPtrOp.call(irbuilder, ret_val)
        statements.append(stmt)
        _, stmt = ResizeSparsePointers.call(
            irbuilder, ret_val_ptr, dim_var, npointers_plus_1_var
        )
        statements.append(stmt)

        return ret_val, "\n".join(statements)


class DelSparseTensor(BaseOp):
    dialect = "util"
    name = "del_sparse_tensor"

    @classmethod
    def call(cls, irbuilder, input):
        cls.ensure_mlirvar(input, SparseTensorType)
        input, cast_string = TensorToPtrOp.call(irbuilder, input)
        cast_string += "\n"
        irbuilder.needed_function_table["delSparseTensor"] = (
            f"func private @delSparseTensor(!llvm.ptr<i8>) -> ()",
            ["!llvm.ptr<i8>"],
            "",
        )

        return None, cast_string + (
            f"call @delSparseTensor({input}) : (!llvm.ptr<i8>) -> ()"
        )


class ResizeSparseDim(BaseOp):
    dialect = "util"
    name = "resize_sparse_dim"

    @classmethod
    def call(cls, irbuilder, input, dim, size):
        cls.ensure_mlirvar(input, LlvmPtrType)
        cls.ensure_mlirvar(dim, IndexType)
        cls.ensure_mlirvar(size, IndexType)
        irbuilder.needed_function_table["resize_dim"] = (
            f"func private @resize_dim(!llvm.ptr<i8>, index, index)",
            ["!llvm.ptr<i8>", "index", "index"],
            "",
        )

        return None, (
            f"call @resize_dim({input}, {dim}, {size}) : (!llvm.ptr<i8>, index, index) -> ()"
        )


class ResizeSparsePointers(BaseOp):
    dialect = "util"
    name = "resize_sparse_pointers"

    @classmethod
    def call(cls, irbuilder, input, dim, size):
        cls.ensure_mlirvar(input, LlvmPtrType)
        cls.ensure_mlirvar(dim, IndexType)
        cls.ensure_mlirvar(size, IndexType)
        irbuilder.needed_function_table["resize_pointers"] = (
            f"func private @resize_pointers(!llvm.ptr<i8>, index, index)",
            ["!llvm.ptr<i8>", "index", "index"],
            "",
        )

        return None, (
            f"call @resize_pointers({input}, {dim}, {size}) : (!llvm.ptr<i8>, index, index) -> ()"
        )


class ResizeSparseIndex(BaseOp):
    dialect = "util"
    name = "resize_sparse_index"

    @classmethod
    def call(cls, irbuilder, input, dim, size):
        cls.ensure_mlirvar(input, LlvmPtrType)
        cls.ensure_mlirvar(dim, IndexType)
        cls.ensure_mlirvar(size, IndexType)
        irbuilder.needed_function_table["resize_index"] = (
            f"func private @resize_index(!llvm.ptr<i8>, index, index)",
            ["!llvm.ptr<i8>", "index", "index"],
            "",
        )

        return None, (
            f"call @resize_index({input}, {dim}, {size}) : (!llvm.ptr<i8>, index, index) -> ()"
        )


class ResizeSparseValues(BaseOp):
    dialect = "util"
    name = "resize_sparse_values"

    @classmethod
    def call(cls, irbuilder, input, size):
        cls.ensure_mlirvar(input, LlvmPtrType)
        cls.ensure_mlirvar(size, IndexType)
        irbuilder.needed_function_table["resize_values"] = (
            f"func private @resize_values(!llvm.ptr<i8>, index)",
            ["!llvm.ptr<i8>", "index"],
            "",
        )

        return None, (
            f"call @resize_values({input}, {size}) : (!llvm.ptr<i8>, index) -> ()"
        )
