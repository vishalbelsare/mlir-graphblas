// RUN: graphblas-opt %s | graphblas-exec main | FileCheck %s

#CSR64 = #sparse_tensor.encoding<{
  dimLevelType = [ "dense", "compressed" ],
  dimOrdering = affine_map<(i,j) -> (i,j)>,
  pointerBitWidth = 64,
  indexBitWidth = 64
}>

#CSC64 = #sparse_tensor.encoding<{
  dimLevelType = [ "dense", "compressed" ],
  dimOrdering = affine_map<(i,j) -> (j,i)>,
  pointerBitWidth = 64,
  indexBitWidth = 64
}>

func @main() -> () {
    %a_dense = arith.constant dense<[
        [0, 1, 2, 0],
        [0, 0, 0, 3]
      ]> : tensor<2x4xi64>
    %a_csr = sparse_tensor.convert %a_dense : tensor<2x4xi64> to tensor<?x?xi64, #CSR64>
    %a_csc = sparse_tensor.convert %a_dense : tensor<2x4xi64> to tensor<?x?xi64, #CSC64>
    
    %b_dense = arith.constant dense<[
        [0, 7],
        [4, 0],
        [5, 0],
        [6, 8]
      ]> : tensor<4x2xi64>
    %b_csr = sparse_tensor.convert %b_dense : tensor<4x2xi64> to tensor<?x?xi64, #CSR64>
    %b_csc = sparse_tensor.convert %b_dense : tensor<4x2xi64> to tensor<?x?xi64, #CSC64>
    
    %mask_dense = arith.constant dense<[
        [9, 0],
        [0, 8]
      ]> : tensor<2x2xi64>
    %mask_csr = sparse_tensor.convert %mask_dense : tensor<2x2xi64> to tensor<?x?xi64, #CSR64>
    %mask_csc = sparse_tensor.convert %mask_dense : tensor<2x2xi64> to tensor<?x?xi64, #CSC64>

    %answer_1 = graphblas.matrix_multiply %a_csr, %b_csr { semiring = "plus_times" } : (tensor<?x?xi64, #CSR64>, tensor<?x?xi64, #CSR64>) to tensor<?x?xi64, #CSR64>
    // CHECK: answer_1 [
    // CHECK:   [14, _],
    // CHECK:   [18, 24],
    // CHECK: ]
    graphblas.print %answer_1 { strings = ["answer_1 "] } : tensor<?x?xi64, #CSR64>

    %answer_2 = graphblas.matrix_multiply %a_csr, %b_csc { semiring = "min_plus" } : (tensor<?x?xi64, #CSR64>, tensor<?x?xi64, #CSC64>) to tensor<?x?xi64, #CSR64>
    // CHECK: answer_2 [
    // CHECK:   [5, _],
    // CHECK:   [9, 11],
    // CHECK: ]
    graphblas.print %answer_2 { strings = ["answer_2 "] } : tensor<?x?xi64, #CSR64>

    %answer_3 = graphblas.matrix_multiply %a_csc, %b_csr { semiring = "any_overlapi" } : (tensor<?x?xi64, #CSC64>, tensor<?x?xi64, #CSR64>) to tensor<?x?xi64, #CSR64>
    // CHECK: answer_3 [
    // CHECK:   [2, _],
    // CHECK:   [3, 3],
    // CHECK: ]
    graphblas.print %answer_3 { strings = ["answer_3 "] } : tensor<?x?xi64, #CSR64>

    %answer_4 = graphblas.matrix_multiply %a_csc, %b_csc { semiring = "any_pair" } : (tensor<?x?xi64, #CSC64>, tensor<?x?xi64, #CSC64>) to tensor<?x?xi64, #CSR64>
    // CHECK: answer_4 [
    // CHECK:   [1, _],
    // CHECK:   [1, 1],
    // CHECK: ]
    graphblas.print %answer_4 { strings = ["answer_4 "] } : tensor<?x?xi64, #CSR64>
    
    %answer_5 = graphblas.matrix_multiply %a_csr, %b_csr, %mask_csr { semiring = "plus_times" } : (tensor<?x?xi64, #CSR64>, tensor<?x?xi64, #CSR64>, tensor<?x?xi64, #CSR64>) to tensor<?x?xi64, #CSR64>
    // CHECK: answer_5 [
    // CHECK:   [14, _],
    // CHECK:   [_, 24],
    // CHECK: ]
    graphblas.print %answer_5 { strings = ["answer_5 "] } : tensor<?x?xi64, #CSR64>

    %answer_6 = graphblas.matrix_multiply %a_csr, %b_csc, %mask_csr { semiring = "min_plus" } : (tensor<?x?xi64, #CSR64>, tensor<?x?xi64, #CSC64>, tensor<?x?xi64, #CSR64>) to tensor<?x?xi64, #CSR64>
    // CHECK: answer_6 [
    // CHECK:   [5, _],
    // CHECK:   [_, 11],
    // CHECK: ]
    graphblas.print %answer_6 { strings = ["answer_6 "] } : tensor<?x?xi64, #CSR64>

    %answer_7 = graphblas.matrix_multiply %a_csc, %b_csr, %mask_csr { semiring = "any_overlapi" } : (tensor<?x?xi64, #CSC64>, tensor<?x?xi64, #CSR64>, tensor<?x?xi64, #CSR64>) to tensor<?x?xi64, #CSR64>
    // CHECK: answer_7 [
    // CHECK:   [2, _],
    // CHECK:   [_, 3],
    // CHECK: ]
    graphblas.print %answer_7 { strings = ["answer_7 "] } : tensor<?x?xi64, #CSR64>

    %answer_8 = graphblas.matrix_multiply %a_csc, %b_csc, %mask_csr { semiring = "any_pair" } : (tensor<?x?xi64, #CSC64>, tensor<?x?xi64, #CSC64>, tensor<?x?xi64, #CSR64>) to tensor<?x?xi64, #CSR64>
    // CHECK: answer_8 [
    // CHECK:   [1, _],
    // CHECK:   [_, 1],
    // CHECK: ]
    graphblas.print %answer_8 { strings = ["answer_8 "] } : tensor<?x?xi64, #CSR64>
    
    %answer_9 = graphblas.matrix_multiply %a_csr, %b_csr, %mask_csc { semiring = "plus_times" } : (tensor<?x?xi64, #CSR64>, tensor<?x?xi64, #CSR64>, tensor<?x?xi64, #CSC64>) to tensor<?x?xi64, #CSR64>
    // CHECK: answer_9 [
    // CHECK:   [14, _],
    // CHECK:   [_, 24],
    // CHECK: ]
    graphblas.print %answer_9 { strings = ["answer_9 "] } : tensor<?x?xi64, #CSR64>

    %answer_10 = graphblas.matrix_multiply %a_csr, %b_csc, %mask_csc { semiring = "min_plus" } : (tensor<?x?xi64, #CSR64>, tensor<?x?xi64, #CSC64>, tensor<?x?xi64, #CSC64>) to tensor<?x?xi64, #CSR64>
    // CHECK: answer_10 [
    // CHECK:   [5, _],
    // CHECK:   [_, 11],
    // CHECK: ]
    graphblas.print %answer_10 { strings = ["answer_10 "] } : tensor<?x?xi64, #CSR64>

    %answer_11 = graphblas.matrix_multiply %a_csc, %b_csr, %mask_csc { semiring = "any_overlapi" } : (tensor<?x?xi64, #CSC64>, tensor<?x?xi64, #CSR64>, tensor<?x?xi64, #CSC64>) to tensor<?x?xi64, #CSR64>
    // CHECK: answer_11 [
    // CHECK:   [2, _],
    // CHECK:   [_, 3],
    // CHECK: ]
    graphblas.print %answer_11 { strings = ["answer_11 "] } : tensor<?x?xi64, #CSR64>

    %answer_12 = graphblas.matrix_multiply %a_csc, %b_csc, %mask_csc { semiring = "any_pair", mask_complement = true } : (tensor<?x?xi64, #CSC64>, tensor<?x?xi64, #CSC64>, tensor<?x?xi64, #CSC64>) to tensor<?x?xi64, #CSR64>
    // CHECK: answer_12 [
    // CHECK:   [_, _],
    // CHECK:   [1, _],
    // CHECK: ]
    graphblas.print %answer_12 { strings = ["answer_12 "] } : tensor<?x?xi64, #CSR64>

    return
}

