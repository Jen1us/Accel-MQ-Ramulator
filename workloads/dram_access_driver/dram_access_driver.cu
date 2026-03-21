// Simple CUDA app to force (mostly) DRAM-backed global memory accesses while
// keeping NVBit traces small.

#include <cuda_profiler_api.h>
#include <cuda_runtime.h>

#include <cstdio>
#include <cstdlib>

__global__ void dram_touch(const float* __restrict__ in,
                           float* __restrict__ out,
                           int n) {
  int tid = blockIdx.x * blockDim.x + threadIdx.x;

  // Use a power-of-two array size and bitmask indices to keep address
  // calculations cheap and deterministic.
  int mask = n - 1;

  // Spread accesses far apart to avoid cache line reuse.
  constexpr int kOps = 4;
  constexpr int kStrideElems = 4096;  // 16KB stride per thread
  constexpr int kStepElems = 65536;   // 256KB step between ops

  int base = (tid * kStrideElems) & mask;

  float acc = 0.0f;
#pragma unroll
  for (int i = 0; i < kOps; ++i) {
    int idx = (base + i * kStepElems) & mask;
    float v = in[idx];
    out[idx] = v + 1.0f;
    acc += v;
  }

  // Prevent the compiler from optimizing the whole thing away.
  if (tid == 0) out[0] = acc;
}

static void check(cudaError_t status, const char* what) {
  if (status == cudaSuccess) return;
  std::fprintf(stderr, "CUDA error (%s): %s\n", what, cudaGetErrorString(status));
  std::exit(1);
}

int main() {
  // 4M floats = 16MB; large enough to exceed typical L2 sizes.
  constexpr int kElems = 1 << 22;
  constexpr int kThreads = 256;
  constexpr int kBlocks = 1;

  float* in_d = nullptr;
  float* out_d = nullptr;
  check(cudaMalloc(&in_d, kElems * sizeof(float)), "cudaMalloc(in_d)");
  check(cudaMalloc(&out_d, kElems * sizeof(float)), "cudaMalloc(out_d)");
  check(cudaMemset(out_d, 0, kElems * sizeof(float)), "cudaMemset(out_d)");

  // For NVBit tracer: with ACTIVE_FROM_START=0, only trace between these calls.
  check(cudaDeviceSynchronize(), "cudaDeviceSynchronize(pre)");
  check(cudaProfilerStart(), "cudaProfilerStart");
  dram_touch<<<kBlocks, kThreads>>>(in_d, out_d, kElems);
  check(cudaGetLastError(), "kernel launch");
  check(cudaDeviceSynchronize(), "cudaDeviceSynchronize(kernel)");
  check(cudaProfilerStop(), "cudaProfilerStop");

  float out0 = 0.0f;
  check(cudaMemcpy(&out0, out_d, sizeof(float), cudaMemcpyDeviceToHost),
        "cudaMemcpy(DtoH)");
  std::printf("out[0]=%f\n", out0);

  cudaFree(in_d);
  cudaFree(out_d);
  return 0;
}

