// CUDA microbenchmark: generate lots of global-memory traffic while keeping the
// traced instruction stream very small.
//
// The kernel is intentionally simple: each thread issues a small fixed number
// of vectorized global loads/stores to distinct locations.

#include <cuda_profiler_api.h>
#include <cuda_runtime.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static void check(cudaError_t status, const char *what) {
  if (status == cudaSuccess) return;
  std::fprintf(stderr, "CUDA error (%s): %s\n", what, cudaGetErrorString(status));
  std::exit(1);
}

__device__ __forceinline__ std::uint32_t hbf_hash32(std::uint32_t x) {
  // 32-bit mix (variant of splitmix32/murmur finalizer).
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

enum class hbf_mode_t : int { kReadOnly = 0, kReadWrite = 1 };
enum class hbf_pattern_t : int { kGrouped = 0, kWarpScatterPages = 1 };

constexpr std::uint32_t kHbfPageBytes = 16384u;
// Use 32B sectors to reduce over-fetching with the sectorized L2 in
// GPGPU-Sim, while still letting HBF treat each miss as a full-page transfer.
constexpr std::uint32_t kHbfSectorBytes = 32u;
constexpr std::uint32_t kHbfElemBytes = (std::uint32_t)sizeof(std::uint32_t);
constexpr std::uint32_t kHbfPageElems = kHbfPageBytes / kHbfElemBytes;    // 4096
constexpr std::uint32_t kHbfSectorElems = kHbfSectorBytes / kHbfElemBytes; // 8
constexpr std::uint32_t kHbfSectorsPerPage = kHbfPageBytes / kHbfSectorBytes; // 512
constexpr int kHbfMaxOps = 32;

__device__ __forceinline__ std::uint32_t hbf_compute_idx(
    std::uint32_t warp_i, std::uint32_t group, std::uint32_t lane_in_group,
    std::uint32_t pages,
    std::uint32_t page_mask) {
  std::uint32_t h = hbf_hash32(warp_i ^ (group * 0x9e3779b9u));
  std::uint32_t page_base = h & page_mask;
  std::uint32_t stride = pages >> 2;
  stride = stride ? stride : 1u;
  std::uint32_t page = (page_base + group * stride) & page_mask;
  std::uint32_t sector = (h >> 16) & (kHbfSectorsPerPage - 1u);
  return page * kHbfPageElems + sector * kHbfSectorElems + lane_in_group;
}

__global__ void hbf_stream_grouped(std::uint32_t *dst, const std::uint32_t *src,
                                   std::uint32_t mask,
                                   int ops, hbf_mode_t mode) {
  std::uint32_t tid =
      (std::uint32_t)(blockIdx.x * blockDim.x + threadIdx.x);

  // Generate a coalesced, *page-spread* access pattern suitable for an HBF
  // model where each off-chip miss is treated as a full 16KB page operation:
  //
  // - Split the warp into 4 groups of 8 lanes. Each group issues a contiguous
  //   8*16B = 128B access (1 cache line).
  // - Each group targets a *different* 16KB page, so the 4 cache lines in a
  //   warp map to 4 different pages. This is important for exercising internal
  //   die/plane parallelism in the HBF controller; otherwise, many requests
  //   collapse onto the same page/plane and tREAD serialization dominates.
  std::uint32_t lane = tid & 31u;
  std::uint32_t warp = tid >> 5;
  std::uint32_t warps = (std::uint32_t)(gridDim.x * blockDim.x) >> 5;

  std::uint32_t pages = (mask + 1u) / kHbfPageElems;
  pages = pages ? pages : 1u;
  const std::uint32_t page_mask = pages - 1u;
  const std::uint32_t lane_in_group = lane & 7u;
  const std::uint32_t group = lane >> 3;  // 0..3

  // Batch independent loads first to create high memory-level parallelism (MLP)
  // per warp, then consume the data.
  //
  // IMPORTANT: keep loaded values in distinct SSA temporaries so the compiler
  // does not reuse the same destination register across loads (which would
  // serialize them via WAW hazards in the simulator scoreboard).
  ops = (ops < 1) ? 1 : ((ops > kHbfMaxOps) ? kHbfMaxOps : ops);

  std::uint32_t vals[kHbfMaxOps];
#pragma unroll
  for (int i = 0; i < kHbfMaxOps; ++i) {
    if (i < ops) {
      vals[i] =
          src[hbf_compute_idx(warp + (std::uint32_t)i * warps, group,
                              lane_in_group, pages, page_mask)];
    } else {
      vals[i] = 0u;
    }
  }

  std::uint32_t acc = 0u;
#pragma unroll
  for (int i = 0; i < kHbfMaxOps; ++i) {
    if (i < ops) acc ^= vals[i];
  }

  if (mode == hbf_mode_t::kReadWrite) {
#pragma unroll
    for (int i = 0; i < kHbfMaxOps; ++i) {
      if (i < ops) {
        dst[hbf_compute_idx(warp + (std::uint32_t)i * warps, group,
                            lane_in_group, pages, page_mask)] = vals[i] + 1u;
      }
    }
  }

  if (tid == 0) dst[0] = acc;
}

__global__ void hbf_stream_warp_scatter_pages(std::uint32_t *dst,
                                             const std::uint32_t *src,
                                             std::uint32_t mask,
                                             int ops, hbf_mode_t mode) {
  std::uint32_t tid =
      (std::uint32_t)(blockIdx.x * blockDim.x + threadIdx.x);
  std::uint32_t lane = tid & 31u;
  std::uint32_t warp = tid >> 5;
  std::uint32_t warps = (std::uint32_t)(gridDim.x * blockDim.x) >> 5;

  std::uint32_t pages = (mask + 1u) / kHbfPageElems;
  pages = pages ? pages : 1u;
  const std::uint32_t page_mask = pages - 1u;

  ops = (ops < 1) ? 1 : ((ops > kHbfMaxOps) ? kHbfMaxOps : ops);

  // Max-parallel pattern: each lane touches a different 16KB page per
  // instruction, producing up to 32 independent cache-line misses per warp.
  // This is intentionally *not* coalesced; it exists to create enough MLP to
  // saturate the HBF link under long tREAD.
  std::uint32_t vals[kHbfMaxOps];
#pragma unroll
  for (int i = 0; i < kHbfMaxOps; ++i) {
    if (i < ops) {
      const std::uint32_t id =
          (warp + (std::uint32_t)i * warps) * 32u + lane;
      const std::uint32_t page = hbf_hash32(id) & page_mask;
      // Use a sector index derived from the hashed id so repeated pages (wrap)
      // still map to different cache lines.
      const std::uint32_t sector =
          (hbf_hash32(id ^ 0xa5a5a5a5u) & (kHbfSectorsPerPage - 1u));

      const std::uint32_t idx =
          page * kHbfPageElems + sector * kHbfSectorElems;
      vals[i] = src[idx];
    } else {
      vals[i] = 0u;
    }
  }

  std::uint32_t acc = 0u;
#pragma unroll
  for (int i = 0; i < kHbfMaxOps; ++i) {
    if (i < ops) acc ^= vals[i];
  }

  if (mode == hbf_mode_t::kReadWrite) {
#pragma unroll
    for (int i = 0; i < kHbfMaxOps; ++i) {
      if (i < ops) {
        const std::uint32_t id =
            (warp + (std::uint32_t)i * warps) * 32u + lane;
        const std::uint32_t page = hbf_hash32(id) & page_mask;
        const std::uint32_t sector =
            (hbf_hash32(id ^ 0xa5a5a5a5u) & (kHbfSectorsPerPage - 1u));
        const std::uint32_t idx =
            page * kHbfPageElems + sector * kHbfSectorElems;
        dst[idx] = vals[i] + 1u;
      }
    }
  }

  if (tid == 0) dst[0] = acc;
}

static int read_int_arg(int argc, char **argv, const char *name, int def) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], name) == 0) return std::atoi(argv[i + 1]);
  }
  return def;
}

int main(int argc, char **argv) {
  // Defaults chosen to keep traces manageable but still create enough off-chip
  // traffic to observe link saturation / queueing.
  const int blocks = read_int_arg(argc, argv, "--blocks", 1024);
  const int threads = read_int_arg(argc, argv, "--threads", 256);
  const int ops = read_int_arg(argc, argv, "--iters", 8);
  const int log2_elems = read_int_arg(argc, argv, "--log2-elems", 22);
  const int mode_i = read_int_arg(argc, argv, "--mode", 0);  // 0=read,1=rw
  const int pattern_i = read_int_arg(argc, argv, "--pattern", 0);  // 0/1
  const hbf_mode_t mode =
      (mode_i == 0) ? hbf_mode_t::kReadOnly : hbf_mode_t::kReadWrite;
  const hbf_pattern_t pattern = (pattern_i == 1)
                                    ? hbf_pattern_t::kWarpScatterPages
                                    : hbf_pattern_t::kGrouped;

  if (blocks <= 0 || threads <= 0 || ops <= 0 || ops > kHbfMaxOps ||
      log2_elems < 10 || log2_elems > 30) {
    std::fprintf(stderr,
                 "Usage: %s [--blocks N] [--threads N] [--iters 1..32] "
                 "[--log2-elems 10..30] [--mode 0|1] [--pattern 0|1]\n",
                 argv[0]);
    return 2;
  }

  const std::uint32_t elems = 1u << log2_elems;
  const std::uint32_t mask = elems - 1u;
  const size_t bytes = (size_t)elems * sizeof(std::uint32_t);

  std::uint32_t *src_d = nullptr;
  std::uint32_t *dst_d = nullptr;
  check(cudaMalloc(&src_d, bytes), "cudaMalloc(src_d)");
  check(cudaMalloc(&dst_d, bytes), "cudaMalloc(dst_d)");
  check(cudaMemset(dst_d, 0, bytes), "cudaMemset(dst_d)");

  check(cudaDeviceSynchronize(), "cudaDeviceSynchronize(pre)");
  check(cudaProfilerStart(), "cudaProfilerStart");
  if (pattern == hbf_pattern_t::kWarpScatterPages) {
    hbf_stream_warp_scatter_pages<<<blocks, threads>>>(dst_d, src_d, mask, ops,
                                                       mode);
  } else {
    hbf_stream_grouped<<<blocks, threads>>>(dst_d, src_d, mask, ops, mode);
  }
  check(cudaGetLastError(), "kernel launch");
  check(cudaDeviceSynchronize(), "cudaDeviceSynchronize(kernel)");
  check(cudaProfilerStop(), "cudaProfilerStop");

  std::uint32_t out0 = 0u;
  check(cudaMemcpy(&out0, dst_d, sizeof(out0), cudaMemcpyDeviceToHost),
        "cudaMemcpy(DtoH)");
  std::printf("out[0]=%u\n", out0);

  cudaFree(src_d);
  cudaFree(dst_d);
  return 0;
}
