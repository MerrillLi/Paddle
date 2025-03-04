/* Copyright (c) 2022 PaddlePaddle Authors. All Rights Reserved.
Copyright (c) 2022 NVIDIA Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#pragma once

#include <algorithm>
#include <mutex>
#include <unordered_map>

#ifdef PADDLE_WITH_CUDA

#include <cuda_runtime_api.h>  // NOLINT
#include "cuda.h"              // NOLINT

#if CUDA_VERSION >= 11060

#include "gflags/gflags.h"
#include "paddle/fluid/framework/scope_guard.h"
#include "paddle/fluid/memory/memory.h"
#include "paddle/fluid/platform/dynload/cublasLt.h"
#include "paddle/fluid/platform/enforce.h"
#include "paddle/fluid/platform/float16.h"
#include "paddle/phi/backends/all_context.h"
#include "paddle/phi/backends/gpu/cuda/cuda_helper.h"
#include "paddle/phi/common/amp_type_traits.h"
#include "paddle/phi/core/dense_tensor.h"
#include "paddle/utils/optional.h"

DECLARE_int64(cublaslt_exhaustive_search_times);

namespace paddle {
namespace operators {

class GemmEpilogueAlgoCache {
 public:
  static GemmEpilogueAlgoCache& Instance() {
    static GemmEpilogueAlgoCache instance(
        FLAGS_cublaslt_exhaustive_search_times);
    return instance;
  }

  GemmEpilogueAlgoCache(GemmEpilogueAlgoCache const&) = delete;
  void operator=(GemmEpilogueAlgoCache const&) = delete;

  cublasLtMatmulAlgo_t* GetGemmAlgo(cublasLtHandle_t lt_handle,
                                    cublasLtMatmulDesc_t op_desc,
                                    cublasLtMatrixLayout_t a_desc,
                                    cublasLtMatrixLayout_t b_desc,
                                    cublasLtMatrixLayout_t c_desc,
                                    const void* alpha,
                                    const void* beta,
                                    const void* a,
                                    const void* b,
                                    void* c,
                                    cudaStream_t stream,
                                    void* workspace,
                                    size_t workspace_size) {
    if (search_times_ <= 0) return nullptr;

    int64_t seed = 0;
    std::hash<int64_t> hash_fn;

    HashMatmulDesc_(op_desc, &seed, hash_fn);
    HashMatrixLayoutDesc_(a_desc, &seed, hash_fn);
    HashMatrixLayoutDesc_(b_desc, &seed, hash_fn);
    HashMatrixLayoutDesc_(c_desc, &seed, hash_fn);

    cublasLtMatmulAlgo_t ret;
    {
      std::lock_guard<std::mutex> lock(cache_mutex_);
      auto it = map_.find(seed);
      if (it != map_.end()) {
        return &(it->second);
      }
    }

    cublasLtMatmulPreference_t preference;
    PADDLE_ENFORCE_GPU_SUCCESS(
        platform::dynload::cublasLtMatmulPreferenceCreate(&preference));
    PADDLE_ENFORCE_GPU_SUCCESS(
        platform::dynload::cublasLtMatmulPreferenceSetAttribute(
            preference,
            CUBLASLT_MATMUL_PREF_MAX_WORKSPACE_BYTES,
            &workspace_size,
            sizeof(workspace_size)));

    int returned_results = 0;
    std::vector<cublasLtMatmulHeuristicResult_t> heuristic_results(
        requested_algo_count_);
    PADDLE_ENFORCE_GPU_SUCCESS(
        platform::dynload::cublasLtMatmulAlgoGetHeuristic(
            lt_handle,
            op_desc,
            a_desc,
            b_desc,
            c_desc,
            c_desc,
            preference,
            requested_algo_count_,
            heuristic_results.data(),
            &returned_results));

    PADDLE_ENFORCE_GT(
        returned_results,
        0,
        platform::errors::Unavailable("No GEMM epilogue algorithm support!"));

    PADDLE_ENFORCE_GPU_SUCCESS(
        platform::dynload::cublasLtMatmulPreferenceDestroy(preference));

    int best_algo_idx = -1;
    float best_algo_time = 0;

    // Run 100 times for warmup
    int warmup_algo_idx = 0;
    for (int t = 0; t < 100; t++) {
      cublasStatus_t status = platform::dynload::cublasLtMatmul(
          lt_handle,
          op_desc,
          alpha,
          a,
          a_desc,
          b,
          b_desc,
          beta,
          c,
          c_desc,
          c,
          c_desc,
          &heuristic_results[warmup_algo_idx].algo,
          workspace,
          workspace_size,
          stream);
      if (status != CUBLAS_STATUS_SUCCESS) {
        t = -1;
        warmup_algo_idx += 1;
        if (warmup_algo_idx == requested_algo_count_) {
          PADDLE_THROW(platform::errors::Unavailable(
              "No GEMM epilogue algorithm support!"));
        }
      }
    }

    cudaEvent_t start_event, stop_event;
    PADDLE_ENFORCE_GPU_SUCCESS(cudaEventCreate(&start_event));
    PADDLE_ENFORCE_GPU_SUCCESS(cudaEventCreate(&stop_event));

    for (int algo_idx = 0; algo_idx < returned_results; ++algo_idx) {
      float curr_time = 0;
      for (int check_idx = 0; check_idx < search_times_; check_idx++) {
        float time = 0;
        PADDLE_ENFORCE_GPU_SUCCESS(cudaEventRecord(start_event, stream));

        cublasStatus_t status =
            platform::dynload::cublasLtMatmul(lt_handle,
                                              op_desc,
                                              alpha,
                                              a,
                                              a_desc,
                                              b,
                                              b_desc,
                                              beta,
                                              c,
                                              c_desc,
                                              c,
                                              c_desc,
                                              &heuristic_results[algo_idx].algo,
                                              workspace,
                                              workspace_size,
                                              stream);

        PADDLE_ENFORCE_GPU_SUCCESS(cudaEventRecord(stop_event, stream));
        PADDLE_ENFORCE_GPU_SUCCESS(cudaEventSynchronize(stop_event));
        PADDLE_ENFORCE_GPU_SUCCESS(
            cudaEventElapsedTime(&time, start_event, stop_event));
        curr_time += time;
        if (status != CUBLAS_STATUS_SUCCESS) {
          curr_time = 3.40282e+038;  // Max Value of float
          break;
        }
      }

      curr_time = curr_time / search_times_;
      if (curr_time < best_algo_time || algo_idx == 0) {
        best_algo_idx = algo_idx;
        best_algo_time = curr_time;
      }
    }

    PADDLE_ENFORCE_GPU_SUCCESS(cudaEventDestroy(start_event));
    PADDLE_ENFORCE_GPU_SUCCESS(cudaEventDestroy(stop_event));

    if (best_algo_idx == -1) {
      PADDLE_THROW(
          platform::errors::Unavailable("No GEMM epilogue algorithm support!"));
    }

    ret = heuristic_results[best_algo_idx].algo;

    VLOG(4) << "Search time:" << search_times_ << ", hash-key (" << seed
            << ") not found in GemmEpilogueAlgoCache";

    std::lock_guard<std::mutex> lock(cache_mutex_);
    auto& algo_in_map = map_[seed];
    algo_in_map = ret;
    return &algo_in_map;
  }

 private:
  explicit GemmEpilogueAlgoCache(int search_times)
      : search_times_(search_times) {
    map_.clear();
  }
  std::unordered_map<int64_t, cublasLtMatmulAlgo_t> map_;
  int search_times_;
  const int requested_algo_count_ = 10;
  std::mutex cache_mutex_;

  void HashMatmulDesc_(cublasLtMatmulDesc_t desc,
                       int64_t* seed,
                       const std::hash<int64_t>& hash_fn) {
    size_t size_to_write;
    int trans_a, trans_b;
    uint32_t epilogue;

    PADDLE_ENFORCE_GPU_SUCCESS(
        platform::dynload::cublasLtMatmulDescGetAttribute(
            desc,
            CUBLASLT_MATMUL_DESC_TRANSA,
            &trans_a,
            sizeof(trans_a),
            &size_to_write));
    HashValue_(seed, hash_fn, static_cast<int64_t>(trans_a));

    PADDLE_ENFORCE_GPU_SUCCESS(
        platform::dynload::cublasLtMatmulDescGetAttribute(
            desc,
            CUBLASLT_MATMUL_DESC_TRANSB,
            &trans_b,
            sizeof(trans_b),
            &size_to_write));
    HashValue_(seed, hash_fn, static_cast<int64_t>(trans_b));

    PADDLE_ENFORCE_GPU_SUCCESS(
        platform::dynload::cublasLtMatmulDescGetAttribute(
            desc,
            CUBLASLT_MATMUL_DESC_EPILOGUE,
            &epilogue,
            sizeof(epilogue),
            &size_to_write));
    HashValue_(seed, hash_fn, static_cast<int64_t>(epilogue));
  }

  void HashMatrixLayoutDesc_(cublasLtMatrixLayout_t desc,
                             int64_t* seed,
                             const std::hash<int64_t>& hash_fn) {
    size_t size_to_write;
    uint32_t dtype;
    int32_t batch;
    uint64_t row, col;
    int64_t ld, batch_offset;

    PADDLE_ENFORCE_GPU_SUCCESS(
        platform::dynload::cublasLtMatrixLayoutGetAttribute(
            desc,
            CUBLASLT_MATRIX_LAYOUT_TYPE,
            &dtype,
            sizeof(dtype),
            &size_to_write));
    HashValue_(seed, hash_fn, static_cast<int64_t>(dtype));

    PADDLE_ENFORCE_GPU_SUCCESS(
        platform::dynload::cublasLtMatrixLayoutGetAttribute(
            desc,
            CUBLASLT_MATRIX_LAYOUT_BATCH_COUNT,
            &batch,
            sizeof(batch),
            &size_to_write));
    HashValue_(seed, hash_fn, static_cast<int64_t>(batch));

    PADDLE_ENFORCE_GPU_SUCCESS(
        platform::dynload::cublasLtMatrixLayoutGetAttribute(
            desc,
            CUBLASLT_MATRIX_LAYOUT_ROWS,
            &row,
            sizeof(row),
            &size_to_write));
    HashValue_(seed, hash_fn, static_cast<int64_t>(row));

    PADDLE_ENFORCE_GPU_SUCCESS(
        platform::dynload::cublasLtMatrixLayoutGetAttribute(
            desc,
            CUBLASLT_MATRIX_LAYOUT_COLS,
            &col,
            sizeof(col),
            &size_to_write));
    HashValue_(seed, hash_fn, static_cast<int64_t>(col));

    PADDLE_ENFORCE_GPU_SUCCESS(
        platform::dynload::cublasLtMatrixLayoutGetAttribute(
            desc, CUBLASLT_MATRIX_LAYOUT_LD, &ld, sizeof(ld), &size_to_write));
    HashValue_(seed, hash_fn, static_cast<int64_t>(ld));

    PADDLE_ENFORCE_GPU_SUCCESS(
        platform::dynload::cublasLtMatrixLayoutGetAttribute(
            desc,
            CUBLASLT_MATRIX_LAYOUT_STRIDED_BATCH_OFFSET,
            &batch_offset,
            sizeof(batch_offset),
            &size_to_write));
    HashValue_(seed, hash_fn, static_cast<int64_t>(batch_offset));
  }

  void HashValue_(int64_t* seed,
                  const std::hash<int64_t>& hash_fn,
                  int64_t value) {
    *seed ^= hash_fn(value) + 0x9e3779b9 + (*seed << 6) + (*seed >> 2);
  }
};

static cublasLtEpilogue_t GetEpilogueType(const std::string& activation,
                                          bool enable_auxiliary) {
  if (activation == "relu") {
    return enable_auxiliary ? CUBLASLT_EPILOGUE_RELU_AUX_BIAS
                            : CUBLASLT_EPILOGUE_RELU_BIAS;
  } else if (activation == "gelu") {
    return enable_auxiliary ? CUBLASLT_EPILOGUE_GELU_AUX_BIAS
                            : CUBLASLT_EPILOGUE_GELU_BIAS;
  } else if (activation == "none") {
    return CUBLASLT_EPILOGUE_BIAS;
  } else {
    PADDLE_THROW(platform::errors::InvalidArgument(
        "The activation attribute of fused_gemm_epilogue op should be"
        " one of {\"none\", \"relu\", \"gelu\"}. But received %s."
        "But received activation=%s.",
        activation));
  }
}

template <typename T>
void ComputeFusedGemmEpilogueForward(const phi::GPUContext& dev_ctx,
                                     const phi::DenseTensor* x,
                                     const phi::DenseTensor* y,
                                     const phi::DenseTensor* bias,
                                     int64_t M,
                                     int64_t N,
                                     int64_t K,
                                     bool trans_x,
                                     bool trans_y,
                                     const std::string& activation,
                                     phi::DenseTensor* out,
                                     phi::DenseTensor* reserve_space) {
  using MT = typename phi::dtype::MPTypeTrait<T>::Type;

  VLOG(6) << "x.shape={" << x->dims() << "}, y.shape={" << y->dims()
          << "}, out.shape={" << out->dims() << "}, M=" << M << ", N=" << N
          << ", K=" << K << ", trans_x=" << trans_x << ", trans_y=" << trans_y
          << ", activation=" << activation
          << ", reserve_space=" << reserve_space;

  bool enable_auxiliary = reserve_space == nullptr ? false : true;
  auto* out_data = out->data<T>();

  cudaDataType_t mat_type = phi::backends::gpu::ToCudaDataType<T>();
  cudaDataType_t scale_type = phi::backends::gpu::ToCudaDataType<MT>();
  cublasComputeType_t compute_type = CUBLAS_COMPUTE_32F;
  if (std::is_same<T, double>::value) {
    compute_type = CUBLAS_COMPUTE_64F;
  }

  cublasLtMatmulDesc_t operation_desc = NULL;
  PADDLE_ENFORCE_GPU_SUCCESS(platform::dynload::cublasLtMatmulDescCreate(
      &operation_desc, compute_type, scale_type));
  cublasOperation_t transx = trans_x ? CUBLAS_OP_T : CUBLAS_OP_N;
  cublasOperation_t transy = trans_y ? CUBLAS_OP_T : CUBLAS_OP_N;
  PADDLE_ENFORCE_GPU_SUCCESS(platform::dynload::cublasLtMatmulDescSetAttribute(
      operation_desc, CUBLASLT_MATMUL_DESC_TRANSB, &transx, sizeof(transx)));
  PADDLE_ENFORCE_GPU_SUCCESS(platform::dynload::cublasLtMatmulDescSetAttribute(
      operation_desc, CUBLASLT_MATMUL_DESC_TRANSA, &transy, sizeof(transy)));

  cublasLtEpilogue_t epiloque_func =
      GetEpilogueType(activation, enable_auxiliary);
  PADDLE_ENFORCE_GPU_SUCCESS(platform::dynload::cublasLtMatmulDescSetAttribute(
      operation_desc,
      CUBLASLT_MATMUL_DESC_EPILOGUE,
      &epiloque_func,
      sizeof(epiloque_func)));
  const T* bias_data = bias->data<T>();
  PADDLE_ENFORCE_GPU_SUCCESS(platform::dynload::cublasLtMatmulDescSetAttribute(
      operation_desc,
      CUBLASLT_MATMUL_DESC_BIAS_POINTER,
      &bias_data,
      sizeof(bias_data)));

  if (enable_auxiliary && activation != "none") {
    // Note (Ming Huang): The initialization of ReseveSpace is happened in the
    // dev_ctx.Alloc. Therefore, we set real date type up here.
    if (activation == "relu") {
      phi::DataType rs_type = phi::DataType::BOOL;
      size_t reserve_space_size =
          phi::product(reserve_space->dims()) * SizeOf(rs_type);
      dev_ctx.Alloc(reserve_space, rs_type, reserve_space_size);
    } else {
      size_t reserve_space_size =
          phi::product(reserve_space->dims()) * sizeof(T);
      dev_ctx.Alloc<T>(reserve_space, reserve_space_size);
    }

    void* aux_data = reserve_space->data();

    PADDLE_ENFORCE_GPU_SUCCESS(
        platform::dynload::cublasLtMatmulDescSetAttribute(
            operation_desc,
            CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_POINTER,
            &aux_data,
            sizeof(aux_data)));
    int64_t aux_ld = N;
    PADDLE_ENFORCE_GPU_SUCCESS(
        platform::dynload::cublasLtMatmulDescSetAttribute(
            operation_desc,
            CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_LD,
            &aux_ld,
            sizeof(aux_ld)));
  }

  cublasLtMatrixLayout_t x_desc = NULL, y_desc = NULL, out_desc = NULL;
  if (trans_x) {
    PADDLE_ENFORCE_GPU_SUCCESS(platform::dynload::cublasLtMatrixLayoutCreate(
        &x_desc, mat_type, M, K, M));
  } else {
    PADDLE_ENFORCE_GPU_SUCCESS(platform::dynload::cublasLtMatrixLayoutCreate(
        &x_desc, mat_type, K, M, K));
  }
  if (trans_y) {
    PADDLE_ENFORCE_GPU_SUCCESS(platform::dynload::cublasLtMatrixLayoutCreate(
        &y_desc, mat_type, K, N, K));
  } else {
    PADDLE_ENFORCE_GPU_SUCCESS(platform::dynload::cublasLtMatrixLayoutCreate(
        &y_desc, mat_type, N, K, N));
  }
  PADDLE_ENFORCE_GPU_SUCCESS(platform::dynload::cublasLtMatrixLayoutCreate(
      &out_desc, mat_type, N, M, N));

  cublasLtHandle_t lt_handle = dev_ctx.cublaslt_handle();
  // NOTE(zengjinle): I do not know whether the 4MB workspace size is
  // "enough". I just followed the settings from the NVIDIA MLPerf BERT code.
  size_t workspace_size = static_cast<size_t>(4) * 1024 * 1024;
  cudaStream_t stream = dev_ctx.stream();
  memory::allocation::AllocationPtr workspace = memory::Alloc(
      dev_ctx.GetPlace(),
      workspace_size,
      phi::Stream(reinterpret_cast<phi::StreamId>(dev_ctx.stream())));

  MT alpha = static_cast<MT>(1);
  MT beta = static_cast<MT>(0);

  const auto* y_data = y->data<T>();
  const auto* x_data = x->data<T>();

  auto algo = GemmEpilogueAlgoCache::Instance().GetGemmAlgo(lt_handle,
                                                            operation_desc,
                                                            y_desc,
                                                            x_desc,
                                                            out_desc,
                                                            &alpha,
                                                            &beta,
                                                            y_data,
                                                            x_data,
                                                            out_data,
                                                            stream,
                                                            workspace->ptr(),
                                                            workspace_size);
  PADDLE_ENFORCE_GPU_SUCCESS(platform::dynload::cublasLtMatmul(lt_handle,
                                                               operation_desc,
                                                               &alpha,
                                                               y_data,
                                                               y_desc,
                                                               x_data,
                                                               x_desc,
                                                               &beta,
                                                               out_data,
                                                               out_desc,
                                                               out_data,
                                                               out_desc,
                                                               algo,
                                                               workspace->ptr(),
                                                               workspace_size,
                                                               stream));

  PADDLE_ENFORCE_GPU_SUCCESS(
      platform::dynload::cublasLtMatmulDescDestroy(operation_desc));
  PADDLE_ENFORCE_GPU_SUCCESS(
      platform::dynload::cublasLtMatrixLayoutDestroy(y_desc));
  PADDLE_ENFORCE_GPU_SUCCESS(
      platform::dynload::cublasLtMatrixLayoutDestroy(x_desc));
  PADDLE_ENFORCE_GPU_SUCCESS(
      platform::dynload::cublasLtMatrixLayoutDestroy(out_desc));
}

enum FusedGEMMGradInType { kDX = 0, kDY = 1, kDZ = 2 };

template <bool TransX, bool TransY>
struct FusedGEMMGradTrait;

template <>
struct FusedGEMMGradTrait<false, false> {
  static constexpr auto kXGradA = FusedGEMMGradInType::kDZ;
  static constexpr auto kXGradB = FusedGEMMGradInType::kDY;
  static constexpr auto kXGradATrans = false;
  static constexpr auto kXGradBTrans = true;

  static constexpr auto kYGradA = FusedGEMMGradInType::kDX;
  static constexpr auto kYGradB = FusedGEMMGradInType::kDZ;
  static constexpr auto kYGradATrans = true;
  static constexpr auto kYGradBTrans = false;
};

template <>
struct FusedGEMMGradTrait<true, false> {
  static constexpr auto kXGradA = FusedGEMMGradInType::kDY;
  static constexpr auto kXGradB = FusedGEMMGradInType::kDZ;
  static constexpr auto kXGradATrans = false;
  static constexpr auto kXGradBTrans = true;

  static constexpr auto kYGradA = FusedGEMMGradInType::kDX;
  static constexpr auto kYGradB = FusedGEMMGradInType::kDZ;
  static constexpr auto kYGradATrans = false;
  static constexpr auto kYGradBTrans = false;
};

template <>
struct FusedGEMMGradTrait<false, true> {
  static constexpr auto kXGradA = FusedGEMMGradInType::kDZ;
  static constexpr auto kXGradB = FusedGEMMGradInType::kDY;
  static constexpr auto kXGradATrans = false;
  static constexpr auto kXGradBTrans = false;

  static constexpr auto kYGradA = FusedGEMMGradInType::kDZ;
  static constexpr auto kYGradB = FusedGEMMGradInType::kDX;
  static constexpr auto kYGradATrans = true;
  static constexpr auto kYGradBTrans = false;
};

template <>
struct FusedGEMMGradTrait<true, true> {
  static constexpr auto kXGradA = FusedGEMMGradInType::kDY;
  static constexpr auto kXGradB = FusedGEMMGradInType::kDZ;
  static constexpr auto kXGradATrans = true;
  static constexpr auto kXGradBTrans = true;

  static constexpr auto kYGradA = FusedGEMMGradInType::kDZ;
  static constexpr auto kYGradB = FusedGEMMGradInType::kDX;
  static constexpr auto kYGradATrans = true;
  static constexpr auto kYGradBTrans = true;
};

static constexpr auto BoolToCuBlasEnum(bool transpose) {
  return transpose ? CUBLAS_OP_T : CUBLAS_OP_N;
}

static cublasLtEpilogue_t GetEpilogueGradType(
    const std::string& activation_grad) {
  if (activation_grad == "none") {
    return CUBLASLT_EPILOGUE_DEFAULT;
  } else if (activation_grad == "relu_grad") {
    return CUBLASLT_EPILOGUE_DRELU;
  } else if (activation_grad == "gelu_grad") {
    return CUBLASLT_EPILOGUE_DGELU;
  } else {
    PADDLE_THROW(platform::errors::InvalidArgument(
        "The activation_grad attribute of fused_gemm_epilogue op should "
        "be one of {\"none\", \"relu\", \"gelu\"}. But received %s."
        "But received activation_grad=%s.",
        activation_grad));
  }
}

template <typename T, typename DXT, typename DYT, bool TransX, bool TransY>
void ComputeFusedGemmEpilogueBackwardImpl(const phi::GPUContext& dev_ctx,
                                          const phi::DenseTensor* dout,
                                          const phi::DenseTensor* x,
                                          const phi::DenseTensor* y,
                                          const phi::DenseTensor* reserve_space,
                                          int64_t M,
                                          int64_t N,
                                          int64_t K,
                                          const std::string activation_grad,
                                          phi::DenseTensor* dx,
                                          phi::DenseTensor* dy,
                                          phi::DenseTensor* dbias,
                                          bool use_addto_dx,
                                          bool use_addto_dy) {
  using MT = typename phi::dtype::MPTypeTrait<T>::Type;
  static_assert(std::is_same<DXT, T>::value || std::is_same<DXT, MT>::value);
  static_assert(std::is_same<DYT, T>::value || std::is_same<DYT, MT>::value);

  using Trait = FusedGEMMGradTrait<TransX, TransY>;

  cudaDataType_t mat_type = phi::backends::gpu::ToCudaDataType<T>();
  cudaDataType_t scale_type = phi::backends::gpu::ToCudaDataType<MT>();
  cublasComputeType_t compute_type = CUBLAS_COMPUTE_32F;
  if (std::is_same<T, double>::value) {
    compute_type = CUBLAS_COMPUTE_64F;
  }

  cublasLtHandle_t lt_handle = dev_ctx.cublaslt_handle();
  // NOTE(zengjinle): I do not know whether the 4MB workspace size is
  // "enough". I just followed the settings from the NVIDIA MLPerf BERT code.
  size_t workspace_size = static_cast<size_t>(4) * 1024 * 1024;
  const cublasLtMatmulAlgo_t* algo = nullptr;
  cudaStream_t stream = dev_ctx.stream();

  MT alpha = static_cast<MT>(1.0);
  MT beta_dx = use_addto_dx ? static_cast<MT>(1.0) : static_cast<MT>(0.0);
  MT beta_dy = use_addto_dy ? static_cast<MT>(1.0) : static_cast<MT>(0.0);

  cublasLtMatrixLayout_t dout_desc = nullptr, dout_trans_desc = nullptr;
  cublasLtMatrixLayout_t x_desc = nullptr, x_trans_desc = nullptr;
  cublasLtMatrixLayout_t y_desc = nullptr, y_trans_desc = nullptr;
  cublasLtMatrixLayout_t dx_desc = nullptr, dy_desc = nullptr;
  cublasLtMatmulDesc_t dx_operation_desc = nullptr, dy_operation_desc = nullptr;

  DEFINE_PADDLE_SCOPE_GUARD([&] {
    auto descs = {dout_desc,
                  dout_trans_desc,
                  x_desc,
                  x_trans_desc,
                  y_desc,
                  y_trans_desc,
                  dx_desc,
                  dy_desc};
    for (auto desc : descs) {
      if (desc) {
        PADDLE_ENFORCE_GPU_SUCCESS(
            platform::dynload::cublasLtMatrixLayoutDestroy(desc));
      }
    }

    if (dx_operation_desc) {
      PADDLE_ENFORCE_GPU_SUCCESS(
          platform::dynload::cublasLtMatmulDescDestroy(dx_operation_desc));
    }

    if (dy_operation_desc) {
      PADDLE_ENFORCE_GPU_SUCCESS(
          platform::dynload::cublasLtMatmulDescDestroy(dy_operation_desc));
    }
  });

  auto x_row = TransX ? K : M;
  auto x_col = TransX ? M : K;
  auto y_row = TransY ? N : K;
  auto y_col = TransY ? K : N;
  auto z_row = TransX ? N : M;
  auto z_col = TransX ? M : N;

  // dx = func(dout, y)
  if (dx) {
    constexpr auto kXGradAIsDZ = (Trait::kXGradA == FusedGEMMGradInType::kDZ);
    cublasLtMatrixLayout_t *dx_dout_desc, *dx_y_desc;

    if (TransX) {
      dx_dout_desc = &dout_trans_desc;
      PADDLE_ENFORCE_GPU_SUCCESS(platform::dynload::cublasLtMatrixLayoutCreate(
          dx_dout_desc, mat_type, z_row, z_col, z_row));
    } else {
      dx_dout_desc = &dout_desc;
      PADDLE_ENFORCE_GPU_SUCCESS(platform::dynload::cublasLtMatrixLayoutCreate(
          dx_dout_desc, mat_type, z_col, z_row, z_col));
    }

    dx_y_desc = &y_trans_desc;
    PADDLE_ENFORCE_GPU_SUCCESS(platform::dynload::cublasLtMatrixLayoutCreate(
        dx_y_desc, mat_type, y_col, y_row, y_col));

    auto& a_desc = kXGradAIsDZ ? (*dx_dout_desc) : (*dx_y_desc);
    auto& b_desc = kXGradAIsDZ ? (*dx_y_desc) : (*dx_dout_desc);
    auto a_trans = BoolToCuBlasEnum(Trait::kXGradATrans);
    auto b_trans = BoolToCuBlasEnum(Trait::kXGradBTrans);

    PADDLE_ENFORCE_GPU_SUCCESS(platform::dynload::cublasLtMatrixLayoutCreate(
        &dx_desc,
        phi::backends::gpu::ToCudaDataType<DXT>(),
        x_col,
        x_row,
        x_col));

    PADDLE_ENFORCE_GPU_SUCCESS(platform::dynload::cublasLtMatmulDescCreate(
        &dx_operation_desc, compute_type, scale_type));
    PADDLE_ENFORCE_GPU_SUCCESS(
        platform::dynload::cublasLtMatmulDescSetAttribute(
            dx_operation_desc,
            CUBLASLT_MATMUL_DESC_TRANSB,
            &a_trans,
            sizeof(a_trans)));
    PADDLE_ENFORCE_GPU_SUCCESS(
        platform::dynload::cublasLtMatmulDescSetAttribute(
            dx_operation_desc,
            CUBLASLT_MATMUL_DESC_TRANSA,
            &b_trans,
            sizeof(b_trans)));

    cublasLtEpilogue_t epiloque_func_for_dx =
        GetEpilogueGradType(activation_grad);
    PADDLE_ENFORCE_GPU_SUCCESS(
        platform::dynload::cublasLtMatmulDescSetAttribute(
            dx_operation_desc,
            CUBLASLT_MATMUL_DESC_EPILOGUE,
            &epiloque_func_for_dx,
            sizeof(epiloque_func_for_dx)));

    if (activation_grad != "none") {
      auto* aux_data = reserve_space->data();
      PADDLE_ENFORCE_GPU_SUCCESS(
          platform::dynload::cublasLtMatmulDescSetAttribute(
              dx_operation_desc,
              CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_POINTER,
              &aux_data,
              sizeof(aux_data)));
      int64_t aux_ld = TransX ? M : K;
      PADDLE_ENFORCE_GPU_SUCCESS(
          platform::dynload::cublasLtMatmulDescSetAttribute(
              dx_operation_desc,
              CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_LD,
              &aux_ld,
              sizeof(aux_ld)));
    }

    auto dx_workspace = memory::Alloc(
        dev_ctx.GetPlace(),
        workspace_size,
        phi::Stream(reinterpret_cast<phi::StreamId>(dev_ctx.stream())));

    auto* dx_data = dev_ctx.Alloc<DXT>(dx, dx->numel() * sizeof(DXT));
    const auto* y_data = y->data<T>();
    const auto* dout_data = dout->data<T>();
    const auto* a_data = kXGradAIsDZ ? dout_data : y_data;
    const auto* b_data = kXGradAIsDZ ? y_data : dout_data;

    auto algo =
        GemmEpilogueAlgoCache::Instance().GetGemmAlgo(lt_handle,
                                                      dx_operation_desc,
                                                      b_desc,
                                                      a_desc,
                                                      dx_desc,
                                                      &alpha,
                                                      &beta_dx,
                                                      b_data,
                                                      a_data,
                                                      dx_data,
                                                      stream,
                                                      dx_workspace->ptr(),
                                                      workspace_size);

    PADDLE_ENFORCE_GPU_SUCCESS(
        platform::dynload::cublasLtMatmul(lt_handle,
                                          dx_operation_desc,
                                          &alpha,
                                          b_data,
                                          b_desc,
                                          a_data,
                                          a_desc,
                                          &beta_dx,
                                          dx_data,
                                          dx_desc,
                                          dx_data,
                                          dx_desc,
                                          algo,
                                          dx_workspace->ptr(),
                                          workspace_size,
                                          stream));
  }

  // dy = func(dout, x)
  if (dy) {
    constexpr auto kYGradAIsDZ = (Trait::kYGradA == FusedGEMMGradInType::kDZ);

    cublasLtMatrixLayout_t *dy_dout_desc = nullptr, *dy_x_desc = nullptr;
    if (TransX) {
      dy_dout_desc = &dout_trans_desc;
      if (dout_trans_desc == nullptr) {
        PADDLE_ENFORCE_GPU_SUCCESS(
            platform::dynload::cublasLtMatrixLayoutCreate(
                dy_dout_desc, mat_type, z_row, z_col, z_row));
      }
    } else {
      dy_dout_desc = &dout_desc;
      if (dout_desc == nullptr) {
        PADDLE_ENFORCE_GPU_SUCCESS(
            platform::dynload::cublasLtMatrixLayoutCreate(
                dy_dout_desc, mat_type, z_col, z_row, z_col));
      }
    }

    dy_x_desc = &x_trans_desc;
    PADDLE_ENFORCE_GPU_SUCCESS(platform::dynload::cublasLtMatrixLayoutCreate(
        dy_x_desc, mat_type, x_col, x_row, x_col));

    auto& a_desc = kYGradAIsDZ ? (*dy_dout_desc) : (*dy_x_desc);
    auto& b_desc = kYGradAIsDZ ? (*dy_x_desc) : (*dy_dout_desc);
    auto a_trans = BoolToCuBlasEnum(Trait::kYGradATrans);
    auto b_trans = BoolToCuBlasEnum(Trait::kYGradBTrans);

    PADDLE_ENFORCE_GPU_SUCCESS(platform::dynload::cublasLtMatrixLayoutCreate(
        &dy_desc,
        phi::backends::gpu::ToCudaDataType<DYT>(),
        y_col,
        y_row,
        y_col));

    PADDLE_ENFORCE_GPU_SUCCESS(platform::dynload::cublasLtMatmulDescCreate(
        &dy_operation_desc, compute_type, scale_type));

    PADDLE_ENFORCE_GPU_SUCCESS(
        platform::dynload::cublasLtMatmulDescSetAttribute(
            dy_operation_desc,
            CUBLASLT_MATMUL_DESC_TRANSB,
            &a_trans,
            sizeof(a_trans)));
    PADDLE_ENFORCE_GPU_SUCCESS(
        platform::dynload::cublasLtMatmulDescSetAttribute(
            dy_operation_desc,
            CUBLASLT_MATMUL_DESC_TRANSA,
            &b_trans,
            sizeof(b_trans)));

    cublasLtEpilogue_t epiloque_func_for_dy;
    if (dbias == nullptr) {
      epiloque_func_for_dy = CUBLASLT_EPILOGUE_DEFAULT;
    } else {
      if (TransY) {
        epiloque_func_for_dy = CUBLASLT_EPILOGUE_BGRADB;
      } else {
        epiloque_func_for_dy = CUBLASLT_EPILOGUE_BGRADA;
      }
    }

    PADDLE_ENFORCE_GPU_SUCCESS(
        platform::dynload::cublasLtMatmulDescSetAttribute(
            dy_operation_desc,
            CUBLASLT_MATMUL_DESC_EPILOGUE,
            &epiloque_func_for_dy,
            sizeof(epiloque_func_for_dy)));

    if (dbias) {
      auto* dbias_data =
          dev_ctx.Alloc<DYT>(dbias, dbias->numel() * sizeof(DYT));
      PADDLE_ENFORCE_GPU_SUCCESS(
          platform::dynload::cublasLtMatmulDescSetAttribute(
              dy_operation_desc,
              CUBLASLT_MATMUL_DESC_BIAS_POINTER,
              &dbias_data,
              sizeof(dbias_data)));
    }

    auto dy_workspace = memory::Alloc(
        dev_ctx.GetPlace(),
        workspace_size,
        phi::Stream(reinterpret_cast<phi::StreamId>(dev_ctx.stream())));
    auto* dy_data = dev_ctx.Alloc<DYT>(dy, dy->numel() * sizeof(DYT));
    const auto* dout_data = dout->data<T>();
    const auto* x_data = x->data<T>();
    const auto* a_data = kYGradAIsDZ ? dout_data : x_data;
    const auto* b_data = kYGradAIsDZ ? x_data : dout_data;

    auto algo =
        GemmEpilogueAlgoCache::Instance().GetGemmAlgo(lt_handle,
                                                      dy_operation_desc,
                                                      b_desc,
                                                      a_desc,
                                                      dy_desc,
                                                      &alpha,
                                                      &beta_dy,
                                                      b_data,
                                                      a_data,
                                                      dy_data,
                                                      stream,
                                                      dy_workspace->ptr(),
                                                      workspace_size);

    PADDLE_ENFORCE_GPU_SUCCESS(
        platform::dynload::cublasLtMatmul(lt_handle,
                                          dy_operation_desc,
                                          &alpha,
                                          b_data,
                                          b_desc,
                                          a_data,
                                          a_desc,
                                          &beta_dy,
                                          dy_data,
                                          dy_desc,
                                          dy_data,
                                          dy_desc,
                                          algo,
                                          dy_workspace->ptr(),
                                          workspace_size,
                                          stream));
  }
}

template <typename T, typename DXT = T, typename DYT = T>
void ComputeFusedGemmEpilogueBackward(const phi::GPUContext& dev_ctx,
                                      const phi::DenseTensor* dout,
                                      const phi::DenseTensor* x,
                                      const phi::DenseTensor* y,
                                      const phi::DenseTensor* reserve_space,
                                      int64_t M,
                                      int64_t N,
                                      int64_t K,
                                      bool trans_x,
                                      bool trans_y,
                                      const std::string& activation_grad,
                                      phi::DenseTensor* dx,
                                      phi::DenseTensor* dy,
                                      phi::DenseTensor* dbias,
                                      bool use_addto_dx = false,
                                      bool use_addto_dy = false) {
  VLOG(10) << "M=" << M << ", K=" << K << ", N=" << N << ", trans_x=" << trans_x
           << ", trans_y=" << trans_y
           << ", activation_grad=" << activation_grad;

  if (trans_x) {
    if (trans_y) {
      ComputeFusedGemmEpilogueBackwardImpl<T, DXT, DYT, true, true>(
          dev_ctx,
          dout,
          x,
          y,
          reserve_space,
          M,
          N,
          K,
          activation_grad,
          dx,
          dy,
          dbias,
          use_addto_dx,
          use_addto_dy);
    } else {
      ComputeFusedGemmEpilogueBackwardImpl<T, DXT, DYT, true, false>(
          dev_ctx,
          dout,
          x,
          y,
          reserve_space,
          M,
          N,
          K,
          activation_grad,
          dx,
          dy,
          dbias,
          use_addto_dx,
          use_addto_dy);
    }
  } else {
    if (trans_y) {
      ComputeFusedGemmEpilogueBackwardImpl<T, DXT, DYT, false, true>(
          dev_ctx,
          dout,
          x,
          y,
          reserve_space,
          M,
          N,
          K,
          activation_grad,
          dx,
          dy,
          dbias,
          use_addto_dx,
          use_addto_dy);
    } else {
      ComputeFusedGemmEpilogueBackwardImpl<T, DXT, DYT, false, false>(
          dev_ctx,
          dout,
          x,
          y,
          reserve_space,
          M,
          N,
          K,
          activation_grad,
          dx,
          dy,
          dbias,
          use_addto_dx,
          use_addto_dy);
    }
  }
}

}  // namespace operators
}  // namespace paddle
#endif
#endif
