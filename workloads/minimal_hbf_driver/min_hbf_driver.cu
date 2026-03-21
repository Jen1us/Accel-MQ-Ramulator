// Minimal CUDA app for NVBit→Accel-Sim trace-driven workflow smoke testing.
// Intentionally tiny to avoid huge traces / OOM-kills.

#include <cuda_profiler_api.h>
#include <cuda_runtime.h>

#include <cstdio>
#include <cstdlib>

__global__ void touch_global(const float* __restrict__ in,
                             float* __restrict__ out) {
  // Intentionally minimal: 1 thread, a small number of distinct global memory
  // ops so we can observe both HBF/HBM behavior without huge traces.
  float acc = 0.0f;
  constexpr int kStrideElems = 64;  // 256B stride (helps spread partitions)
  constexpr int kOps = 16;

#pragma unroll
  for (int i = 0; i < kOps; ++i) {
    float v = in[i * kStrideElems];
    out[i * kStrideElems] = v + 1.0f;
    acc += v;
  }
  out[0] = acc;
}

static void check(cudaError_t status, const char* what) {
  if (status == cudaSuccess) return;
  std::fprintf(stderr, "CUDA error (%s): %s\n", what, cudaGetErrorString(status));
  std::exit(1);
}

int main() {
  constexpr int kThreads = 1;
  constexpr int kStrideElems = 64;
  constexpr int kOps = 16;
  constexpr int kElems = kStrideElems * kOps;

  float* in_d = nullptr;
  float* out_d = nullptr;
  float in_h[kElems];
  float out_h[kThreads];
  for (int i = 0; i < kElems; ++i) in_h[i] = static_cast<float>(i);

  check(cudaMalloc(&in_d, kElems * sizeof(float)), "cudaMalloc(in_d)");
  check(cudaMalloc(&out_d, kElems * sizeof(float)), "cudaMalloc(out_d)");
  check(cudaMemcpy(in_d, in_h, kElems * sizeof(float), cudaMemcpyHostToDevice),
        "cudaMemcpy(HtoD)");

  check(cudaDeviceSynchronize(), "cudaDeviceSynchronize(pre)");

  // For NVBit tracer: with ACTIVE_FROM_START=0, only trace between these calls.
  check(cudaProfilerStart(), "cudaProfilerStart");
  touch_global<<<1, kThreads>>>(in_d, out_d);
  check(cudaGetLastError(), "kernel launch");
  check(cudaDeviceSynchronize(), "cudaDeviceSynchronize(kernel)");
  check(cudaProfilerStop(), "cudaProfilerStop");

  check(cudaMemcpy(out_h, out_d, kThreads * sizeof(float), cudaMemcpyDeviceToHost),
        "cudaMemcpy(DtoH)");

  std::printf("out[0]=%f\n", out_h[0]);

  cudaFree(in_d);
  cudaFree(out_d);
  return 0;
}
