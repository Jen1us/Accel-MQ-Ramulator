#include <cuda_fp16.h>
#include <cuda_runtime.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>

#ifndef HEAD_DIM
#define HEAD_DIM 32
#endif

#ifndef HEADS_NUM
#define HEADS_NUM 1
#endif

#ifndef LAYERS_NUM
#define LAYERS_NUM 1
#endif

#ifndef BLOCK_SIZE_K
#define BLOCK_SIZE_K 128
#endif

#ifndef LOGICAL_SEQ_LEN
#define LOGICAL_SEQ_LEN 2048
#endif

#ifndef PHYSICAL_RING_LEN
#define PHYSICAL_RING_LEN 1024
#endif

#ifndef GEN_LEN
#define GEN_LEN 1
#endif

#define CHECK_CUDA(call)                                                     \
  do {                                                                       \
    cudaError_t err__ = (call);                                              \
    if (err__ != cudaSuccess) {                                              \
      std::fprintf(stderr, "CUDA error: %s (%s:%d)\n",                       \
                   cudaGetErrorString(err__), __FILE__, __LINE__);           \
      std::exit(1);                                                          \
    }                                                                        \
  } while (0)

extern "C" cudaError_t cudaProfilerStart(void);
extern "C" cudaError_t cudaProfilerStop(void);

// Minimal FlashAttention-v2-like decoding kernel.
// Goal: exercise global-memory (KV) reads + shared-memory tiling, so NVBit
// generates traces and Accel-Sim can be driven end-to-end. This is not a
// numerically-correct implementation.
__global__ void flash_decoding_v2_kernel(const __half* __restrict__ Q,
                                         const __half* __restrict__ KV_Cache,
                                         __half* __restrict__ Output,
                                         float sm_scale) {
  int batch_idx = blockIdx.x;
  int head_idx = blockIdx.y;
  int tid = threadIdx.x;

  if (tid >= HEAD_DIM) return;

  size_t q_offset =
      (size_t)batch_idx * HEADS_NUM * HEAD_DIM + (size_t)head_idx * HEAD_DIM +
      (size_t)tid;
  float q_val = __half2float(Q[q_offset]);

  __shared__ __half s_K[BLOCK_SIZE_K][HEAD_DIM];
  __shared__ __half s_V[BLOCK_SIZE_K][HEAD_DIM];

  float m_prev = -INFINITY;
  float l_prev = 0.0f;
  float acc = 0.0f;

  for (int base_seq = 0; base_seq < LOGICAL_SEQ_LEN; base_seq += BLOCK_SIZE_K) {
    // Phase A: load a KV tile from "HBF/HBM" (global memory) to SRAM (shared).
    for (int i = 0; i < BLOCK_SIZE_K; ++i) {
      int logical_pos = base_seq + i;
      int physical_pos = logical_pos % PHYSICAL_RING_LEN;
      size_t kv_ptr = (size_t)head_idx * PHYSICAL_RING_LEN * HEAD_DIM +
                      (size_t)physical_pos * HEAD_DIM + (size_t)tid;

      s_K[i][tid] = KV_Cache[kv_ptr];
      s_V[i][tid] =
          KV_Cache[kv_ptr + (size_t)PHYSICAL_RING_LEN * HEAD_DIM];
    }
    __syncthreads();

    // Phase B: toy compute over the tile, with a warp reduction + online
    // softmax-style accumulator.
    for (int i = 0; i < BLOCK_SIZE_K; ++i) {
      float k_val = __half2float(s_K[i][tid]);
      float score = q_val * k_val;

      for (int offset = 16; offset > 0; offset >>= 1) {
        score += __shfl_down_sync(0xffffffff, score, offset);
      }
      score = __shfl_sync(0xffffffff, score, 0);

      if (tid == 0) score *= sm_scale;
      score = __shfl_sync(0xffffffff, score, 0);

      float m_curr = fmaxf(m_prev, score);
      float alpha = expf(m_prev - m_curr);
      float beta = expf(score - m_curr);
      l_prev = l_prev * alpha + beta;

      float v_val = __half2float(s_V[i][tid]);
      acc = acc * alpha + beta * v_val;
      m_prev = m_curr;
    }
    __syncthreads();
  }

  acc = (l_prev == 0.0f) ? 0.0f : (acc / l_prev);
  size_t out_offset =
      (size_t)batch_idx * HEADS_NUM * HEAD_DIM + (size_t)head_idx * HEAD_DIM +
      (size_t)tid;
  Output[out_offset] = __float2half(acc);
}

int main() {
  std::printf("=== FlashAttention v2 (minimal CUDA driver) ===\n");
  std::printf("HEAD_DIM=%d HEADS_NUM=%d LAYERS_NUM=%d\n", HEAD_DIM, HEADS_NUM,
              LAYERS_NUM);
  std::printf("LOGICAL_SEQ_LEN=%d PHYSICAL_RING_LEN=%d BLOCK_SIZE_K=%d\n",
              LOGICAL_SEQ_LEN, PHYSICAL_RING_LEN, BLOCK_SIZE_K);
  std::printf("GEN_LEN=%d\n", GEN_LEN);

  constexpr int kBatch = 1;
  size_t q_elems = (size_t)kBatch * HEADS_NUM * HEAD_DIM;
  size_t out_elems = q_elems;

  size_t kv_layer_elems = (size_t)HEADS_NUM * PHYSICAL_RING_LEN * HEAD_DIM * 2;
  size_t kv_total_elems = (size_t)LAYERS_NUM * kv_layer_elems;
  size_t kv_total_bytes = kv_total_elems * sizeof(__half);

  std::printf("[System] KV pool size: %.2f MiB\n",
              (double)kv_total_bytes / 1024.0 / 1024.0);

  __half* d_Q = nullptr;
  __half* d_Out = nullptr;
  __half* d_KV_Pool = nullptr;
  CHECK_CUDA(cudaMalloc(&d_Q, q_elems * sizeof(__half)));
  CHECK_CUDA(cudaMalloc(&d_Out, out_elems * sizeof(__half)));
  CHECK_CUDA(cudaMalloc(&d_KV_Pool, kv_total_bytes));

  CHECK_CUDA(cudaMemset(d_Q, 0, q_elems * sizeof(__half)));
  CHECK_CUDA(cudaMemset(d_Out, 0, out_elems * sizeof(__half)));
  CHECK_CUDA(cudaMemset(d_KV_Pool, 0, kv_total_bytes));

  dim3 grid(kBatch, HEADS_NUM);
  dim3 block(HEAD_DIM);
  float sm_scale = 1.0f / std::sqrt((float)HEAD_DIM);

  CHECK_CUDA(cudaFree(0));
  CHECK_CUDA(cudaDeviceSynchronize());

  CHECK_CUDA(cudaProfilerStart());
  for (int step = 0; step < GEN_LEN; ++step) {
    for (int layer = 0; layer < LAYERS_NUM; ++layer) {
      __half* layer_kv = d_KV_Pool + (size_t)layer * kv_layer_elems;
      flash_decoding_v2_kernel<<<grid, block>>>(d_Q, layer_kv, d_Out, sm_scale);
    }
  }
  CHECK_CUDA(cudaProfilerStop());
  CHECK_CUDA(cudaDeviceSynchronize());

  CHECK_CUDA(cudaFree(d_Q));
  CHECK_CUDA(cudaFree(d_Out));
  CHECK_CUDA(cudaFree(d_KV_Pool));
  std::printf("Done.\n");
  return 0;
}
