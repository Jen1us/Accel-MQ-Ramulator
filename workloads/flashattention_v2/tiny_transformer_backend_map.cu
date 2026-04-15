#include <cuda_fp16.h>
#include <cuda_runtime.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#ifndef HEAD_DIM
#define HEAD_DIM 32
#endif

#ifndef HEADS_NUM
#define HEADS_NUM 4
#endif

#ifndef LAYERS_NUM
#define LAYERS_NUM 2
#endif

#ifndef LOGICAL_SEQ_LEN
#define LOGICAL_SEQ_LEN 64
#endif

#ifndef PHYSICAL_RING_LEN
#define PHYSICAL_RING_LEN 64
#endif

#ifndef GEN_LEN
#define GEN_LEN 1
#endif

#ifndef MLP_EXPAND
#define MLP_EXPAND 2
#endif

#ifndef PROJ_TAPS
#define PROJ_TAPS 8
#endif

#ifndef MLP_TAPS
#define MLP_TAPS 8
#endif

#ifndef ATTN_WINDOW
#define ATTN_WINDOW 8
#endif

constexpr int kHiddenDim = HEADS_NUM * HEAD_DIM;
constexpr int kMlpHiddenDim = kHiddenDim * MLP_EXPAND;

static_assert(HEAD_DIM == 32, "decoder block validation expects HEAD_DIM=32");
static_assert(kHiddenDim <= 1024, "hidden dimension must fit in one CUDA block");

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

struct RegionSpec {
  const char* name;
  const void* ptr;
  size_t bytes;
  const char* backend;
  const char* label;
};

__device__ __forceinline__ float synth_input_value(int tid, int layer, int step) {
  const int seed = tid + layer * 7 + step * 13;
  return 0.0625f * static_cast<float>((seed & 0xf) + 1);
}

__device__ __forceinline__ float silu(float x) {
  return x / (1.0f + expf(-x));
}

__device__ float sampled_dot_half_row(const __half* row, const float* vec, int len,
                                      int taps, int seed) {
  float acc = 0.0f;
  for (int tap = 0; tap < taps; ++tap) {
    const int idx = (seed + tap * 17) % len;
    acc += __half2float(row[idx]) * vec[idx];
  }
  return acc;
}

__global__ void decoder_block_kernel(
    const __half* __restrict__ weights_q,
    const __half* __restrict__ weights_k,
    const __half* __restrict__ weights_v,
    const __half* __restrict__ weights_o,
    const __half* __restrict__ weights_mlp_up,
    const __half* __restrict__ weights_mlp_gate,
    const __half* __restrict__ weights_mlp_down,
    __half* __restrict__ kv_cache, int layer, int step, float sm_scale) {
  const int tid = threadIdx.x;
  if (tid >= kHiddenDim) return;

  const int head_idx = tid / HEAD_DIM;
  const int head_lane = tid % HEAD_DIM;
  constexpr int kKvPerHead = PHYSICAL_RING_LEN * HEAD_DIM;
  const int current_logical_pos = LOGICAL_SEQ_LEN - 1 + step;
  const int active_tokens = (current_logical_pos + 1 < PHYSICAL_RING_LEN)
                                ? (current_logical_pos + 1)
                                : PHYSICAL_RING_LEN;
  const int attention_tokens =
      (active_tokens < ATTN_WINDOW) ? active_tokens : ATTN_WINDOW;
  const int logical_start = current_logical_pos - attention_tokens + 1;
  const int current_physical_pos = current_logical_pos % PHYSICAL_RING_LEN;

  __shared__ float s_input[kHiddenDim];
  __shared__ float s_q[kHiddenDim];
  __shared__ float s_k[kHiddenDim];
  __shared__ float s_v[kHiddenDim];
  __shared__ float s_ctx[kHiddenDim];
  __shared__ float s_attn_resid[kHiddenDim];
  __shared__ float s_mlp_hidden[kMlpHiddenDim];
  __shared__ float s_final[kHiddenDim];
  __shared__ volatile float s_checksum_sink;

  s_input[tid] = synth_input_value(tid, layer, step);
  __syncthreads();

  s_q[tid] = sampled_dot_half_row(
      weights_q + static_cast<size_t>(tid) * kHiddenDim, s_input, kHiddenDim,
      PROJ_TAPS, tid + layer * 11 + step * 19);
  s_k[tid] = sampled_dot_half_row(
      weights_k + static_cast<size_t>(tid) * kHiddenDim, s_input, kHiddenDim,
      PROJ_TAPS, tid + layer * 13 + step * 23);
  s_v[tid] = sampled_dot_half_row(
      weights_v + static_cast<size_t>(tid) * kHiddenDim, s_input, kHiddenDim,
      PROJ_TAPS, tid + layer * 17 + step * 29);
  __syncthreads();

  const size_t head_base = static_cast<size_t>(head_idx) * kKvPerHead * 2;
  const size_t k_write_ptr =
      head_base + static_cast<size_t>(current_physical_pos) * HEAD_DIM + head_lane;
  const size_t v_write_ptr = head_base + kKvPerHead +
                             static_cast<size_t>(current_physical_pos) * HEAD_DIM +
                             head_lane;
  kv_cache[k_write_ptr] = __float2half(s_k[tid]);
  kv_cache[v_write_ptr] = __float2half(s_v[tid]);
  __syncthreads();

  float m_prev = -INFINITY;
  float l_prev = 0.0f;
  float ctx = 0.0f;
  for (int logical_pos = logical_start; logical_pos <= current_logical_pos;
       ++logical_pos) {
    const int physical_pos = logical_pos % PHYSICAL_RING_LEN;
    const size_t k_read_ptr =
        head_base + static_cast<size_t>(physical_pos) * HEAD_DIM + head_lane;
    const size_t v_read_ptr = head_base + kKvPerHead +
                              static_cast<size_t>(physical_pos) * HEAD_DIM +
                              head_lane;

    const float k_val = (logical_pos == current_logical_pos)
                            ? s_k[tid]
                            : __half2float(kv_cache[k_read_ptr]);
    const float v_val = (logical_pos == current_logical_pos)
                            ? s_v[tid]
                            : __half2float(kv_cache[v_read_ptr]);

    float score = s_q[tid] * k_val;
    for (int offset = HEAD_DIM / 2; offset > 0; offset >>= 1) {
      score += __shfl_down_sync(0xffffffff, score, offset, HEAD_DIM);
    }
    score = __shfl_sync(0xffffffff, score, 0, HEAD_DIM) * sm_scale;

    const float m_curr = fmaxf(m_prev, score);
    const float alpha = expf(m_prev - m_curr);
    const float beta = expf(score - m_curr);
    l_prev = l_prev * alpha + beta;
    ctx = ctx * alpha + beta * v_val;
    m_prev = m_curr;
  }
  s_ctx[tid] = (l_prev == 0.0f) ? 0.0f : (ctx / l_prev);
  __syncthreads();

  s_attn_resid[tid] =
      s_input[tid] +
      sampled_dot_half_row(weights_o + static_cast<size_t>(tid) * kHiddenDim,
                           s_ctx, kHiddenDim, PROJ_TAPS,
                           tid + layer * 31 + step * 37);
  __syncthreads();

  for (int mlp_idx = tid; mlp_idx < kMlpHiddenDim; mlp_idx += kHiddenDim) {
    const float up = sampled_dot_half_row(
        weights_mlp_up + static_cast<size_t>(mlp_idx) * kHiddenDim, s_attn_resid,
        kHiddenDim, MLP_TAPS, mlp_idx + layer * 41 + step * 43);
    const float gate = sampled_dot_half_row(
        weights_mlp_gate + static_cast<size_t>(mlp_idx) * kHiddenDim,
        s_attn_resid, kHiddenDim, MLP_TAPS, mlp_idx + layer * 47 + step * 53);
    s_mlp_hidden[mlp_idx] = silu(gate) * up;
  }
  __syncthreads();

  s_final[tid] = s_attn_resid[tid] +
                 sampled_dot_half_row(
                     weights_mlp_down +
                         static_cast<size_t>(tid) * kMlpHiddenDim,
                     s_mlp_hidden, kMlpHiddenDim, MLP_TAPS,
                     tid + layer * 59 + step * 61);
  __syncthreads();

  if (tid == 0) {
    float checksum = 0.0f;
    for (int i = 0; i < kHiddenDim; ++i) {
      checksum += s_final[i];
    }
    s_checksum_sink = checksum;
  }
}

static void write_region(FILE* fp, const RegionSpec& region) {
  if (fp == nullptr || region.ptr == nullptr || region.bytes == 0) return;
  std::fprintf(fp, "%s 0x%llx 0x%llx %s %s\n", region.name,
               static_cast<unsigned long long>(
                   reinterpret_cast<uintptr_t>(region.ptr)),
               static_cast<unsigned long long>(region.bytes), region.backend,
               region.label);
}

static void maybe_write_backend_metadata(const char* path,
                                         const RegionSpec* regions,
                                         size_t region_count) {
  if (path == nullptr || path[0] == '\0') return;

  FILE* fp = std::fopen(path, "w");
  if (fp == nullptr) {
    std::perror("fopen(metadata)");
    std::exit(1);
  }

  std::fprintf(fp, "# region base size backend label\n");
  for (size_t i = 0; i < region_count; ++i) {
    write_region(fp, regions[i]);
  }
  std::fclose(fp);
}

int main() {
  std::printf("=== Tiny Decoder Block Backend-Map Driver ===\n");
  std::printf("HEAD_DIM=%d HEADS_NUM=%d LAYERS_NUM=%d\n", HEAD_DIM, HEADS_NUM,
              LAYERS_NUM);
  std::printf("HIDDEN_DIM=%d MLP_HIDDEN_DIM=%d\n", kHiddenDim, kMlpHiddenDim);
  std::printf("LOGICAL_SEQ_LEN=%d PHYSICAL_RING_LEN=%d\n", LOGICAL_SEQ_LEN,
              PHYSICAL_RING_LEN);
  std::printf("PROJ_TAPS=%d MLP_TAPS=%d ATTN_WINDOW=%d\n", PROJ_TAPS, MLP_TAPS,
              ATTN_WINDOW);
  std::printf("GEN_LEN=%d\n", GEN_LEN);

  const char* meta_path = std::getenv("ACCELSIM_BACKEND_META_PATH");

  const size_t qkv_layer_elems = static_cast<size_t>(kHiddenDim) * kHiddenDim;
  const size_t qkv_total_bytes =
      static_cast<size_t>(LAYERS_NUM) * qkv_layer_elems * sizeof(__half);

  const size_t mlp_up_layer_elems =
      static_cast<size_t>(kMlpHiddenDim) * kHiddenDim;
  const size_t mlp_up_total_bytes =
      static_cast<size_t>(LAYERS_NUM) * mlp_up_layer_elems * sizeof(__half);

  const size_t mlp_down_layer_elems =
      static_cast<size_t>(kHiddenDim) * kMlpHiddenDim;
  const size_t mlp_down_total_bytes =
      static_cast<size_t>(LAYERS_NUM) * mlp_down_layer_elems * sizeof(__half);

  const size_t kv_layer_elems =
      static_cast<size_t>(HEADS_NUM) * PHYSICAL_RING_LEN * HEAD_DIM * 2;
  const size_t kv_total_bytes =
      static_cast<size_t>(LAYERS_NUM) * kv_layer_elems * sizeof(__half);

  __half* d_weights_q = nullptr;
  __half* d_weights_k = nullptr;
  __half* d_weights_v = nullptr;
  __half* d_weights_o = nullptr;
  __half* d_weights_mlp_up = nullptr;
  __half* d_weights_mlp_gate = nullptr;
  __half* d_weights_mlp_down = nullptr;
  __half* d_kv_cache = nullptr;

  CHECK_CUDA(cudaMalloc(&d_weights_q, qkv_total_bytes));
  CHECK_CUDA(cudaMalloc(&d_weights_k, qkv_total_bytes));
  CHECK_CUDA(cudaMalloc(&d_weights_v, qkv_total_bytes));
  CHECK_CUDA(cudaMalloc(&d_weights_o, qkv_total_bytes));
  CHECK_CUDA(cudaMalloc(&d_weights_mlp_up, mlp_up_total_bytes));
  CHECK_CUDA(cudaMalloc(&d_weights_mlp_gate, mlp_up_total_bytes));
  CHECK_CUDA(cudaMalloc(&d_weights_mlp_down, mlp_down_total_bytes));
  CHECK_CUDA(cudaMalloc(&d_kv_cache, kv_total_bytes));

  CHECK_CUDA(cudaMemset(d_weights_q, 0, qkv_total_bytes));
  CHECK_CUDA(cudaMemset(d_weights_k, 0, qkv_total_bytes));
  CHECK_CUDA(cudaMemset(d_weights_v, 0, qkv_total_bytes));
  CHECK_CUDA(cudaMemset(d_weights_o, 0, qkv_total_bytes));
  CHECK_CUDA(cudaMemset(d_weights_mlp_up, 0, mlp_up_total_bytes));
  CHECK_CUDA(cudaMemset(d_weights_mlp_gate, 0, mlp_up_total_bytes));
  CHECK_CUDA(cudaMemset(d_weights_mlp_down, 0, mlp_down_total_bytes));
  CHECK_CUDA(cudaMemset(d_kv_cache, 0, kv_total_bytes));

  const RegionSpec regions[] = {
      {"weights_q", d_weights_q, qkv_total_bytes, "mqsim", "weights_q"},
      {"weights_k", d_weights_k, qkv_total_bytes, "mqsim", "weights_k"},
      {"weights_v", d_weights_v, qkv_total_bytes, "mqsim", "weights_v"},
      {"weights_o", d_weights_o, qkv_total_bytes, "mqsim", "weights_o"},
      {"weights_mlp_up", d_weights_mlp_up, mlp_up_total_bytes, "mqsim",
       "weights_mlp_up"},
      {"weights_mlp_gate", d_weights_mlp_gate, mlp_up_total_bytes, "mqsim",
       "weights_mlp_gate"},
      {"weights_mlp_down", d_weights_mlp_down, mlp_down_total_bytes, "mqsim",
       "weights_mlp_down"},
      {"kv_cache", d_kv_cache, kv_total_bytes, "ramulator", "kv_cache"},
  };
  maybe_write_backend_metadata(meta_path, regions,
                               sizeof(regions) / sizeof(regions[0]));
  if (meta_path != nullptr && meta_path[0] != '\0') {
    std::printf("backend_meta=%s\n", meta_path);
  }

  const dim3 grid(1, 1, 1);
  const dim3 block(kHiddenDim, 1, 1);
  const float sm_scale = 1.0f / std::sqrt(static_cast<float>(HEAD_DIM));

  CHECK_CUDA(cudaFree(0));
  CHECK_CUDA(cudaDeviceSynchronize());

  CHECK_CUDA(cudaProfilerStart());
  for (int step = 0; step < GEN_LEN; ++step) {
    for (int layer = 0; layer < LAYERS_NUM; ++layer) {
      const __half* layer_weights_q =
          d_weights_q + static_cast<size_t>(layer) * qkv_layer_elems;
      const __half* layer_weights_k =
          d_weights_k + static_cast<size_t>(layer) * qkv_layer_elems;
      const __half* layer_weights_v =
          d_weights_v + static_cast<size_t>(layer) * qkv_layer_elems;
      const __half* layer_weights_o =
          d_weights_o + static_cast<size_t>(layer) * qkv_layer_elems;
      const __half* layer_weights_mlp_up =
          d_weights_mlp_up + static_cast<size_t>(layer) * mlp_up_layer_elems;
      const __half* layer_weights_mlp_gate =
          d_weights_mlp_gate + static_cast<size_t>(layer) * mlp_up_layer_elems;
      const __half* layer_weights_mlp_down = d_weights_mlp_down +
                                             static_cast<size_t>(layer) *
                                                 mlp_down_layer_elems;
      __half* layer_kv = d_kv_cache + static_cast<size_t>(layer) * kv_layer_elems;

      decoder_block_kernel<<<grid, block>>>(
          layer_weights_q, layer_weights_k, layer_weights_v, layer_weights_o,
          layer_weights_mlp_up, layer_weights_mlp_gate, layer_weights_mlp_down,
          layer_kv, layer, step, sm_scale);
      CHECK_CUDA(cudaGetLastError());
    }
  }
  CHECK_CUDA(cudaProfilerStop());
  CHECK_CUDA(cudaDeviceSynchronize());

  CHECK_CUDA(cudaFree(d_weights_q));
  CHECK_CUDA(cudaFree(d_weights_k));
  CHECK_CUDA(cudaFree(d_weights_v));
  CHECK_CUDA(cudaFree(d_weights_o));
  CHECK_CUDA(cudaFree(d_weights_mlp_up));
  CHECK_CUDA(cudaFree(d_weights_mlp_gate));
  CHECK_CUDA(cudaFree(d_weights_mlp_down));
  CHECK_CUDA(cudaFree(d_kv_cache));

  std::printf("Done.\n");
  return 0;
}
