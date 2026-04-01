#ifndef HBF_H
#define HBF_H

#include "../abstract_hardware_model.h"

#include <stdio.h>
#include <zlib.h>

#include <cstdint>
#include <cmath>

#include <deque>
#include <queue>
#include <vector>

#include "mem_backend.h"
class mem_fetch;
class memory_config;
class memory_partition_unit;
class memory_stats_t;
class gpgpu_sim;

// HBF (High Bandwidth Flash) controller (Phase 1 timing model).
//
// NOTE: This is intentionally independent from the existing DRAM controller
// implementation. It provides a DRAM-like interface surface
// (push/full/cycle/return queue) so the simulator can route tagged requests to
// an HBF backend without reusing DRAM module code.
//
// Phase 1 behavior (performance-oriented, not a detailed Flash model):
// - Reads complete after tREAD + subpage transfer time.
// - Writes are posted: ACK after buffering (small fixed latency), and buffer
//   space is reclaimed after tPROG.
class hbf_t : public hbf_backend_ifc {
 public:
  hbf_t(unsigned partition_id, const memory_config *config,
        class memory_stats_t *stats, class memory_partition_unit *mp,
        class gpgpu_sim *gpu);

  bool full(bool is_write) const;
  bool returnq_full() const;
  unsigned que_length() const;
  unsigned queue_limit() const;

  void push(class mem_fetch *data);
  void cycle();

  class mem_fetch *return_queue_pop();
  class mem_fetch *return_queue_top();

  void print(FILE *simFile) const;
  void visualize() const;
  void print_stat(FILE *simFile);
  void visualizer_print(gzFile visualizer_file);

  void set_hbf_power_stats(unsigned &cmd, unsigned &activity, unsigned &nop,
                           unsigned &act, unsigned &pre, unsigned &rd,
                           unsigned &wr, unsigned &wr_WB, unsigned &req) const;

 private:
  struct completion_t {
    unsigned long long ready_cycle;
    class mem_fetch *mf;
    bool counts_towards_outstanding;
  };

  struct completion_compare_t {
    bool operator()(const completion_t &a, const completion_t &b) const {
      return a.ready_cycle > b.ready_cycle;  // min-heap by ready_cycle
    }
  };

  struct write_drain_t {
    unsigned long long ready_cycle;
    unsigned entries;
  };

  struct write_drain_compare_t {
    bool operator()(const write_drain_t &a, const write_drain_t &b) const {
      return a.ready_cycle > b.ready_cycle;  // min-heap by ready_cycle
    }
  };

  unsigned long long now() const;
  unsigned long long reserve_link_transfer(unsigned xfer_bytes,
                                           unsigned long long earliest_start);
  unsigned map_to_plane(const class mem_fetch *mf) const;
  unsigned long long reserve_plane(unsigned plane, unsigned latency_cycles,
                                   unsigned long long earliest_start);
  void enqueue_completion(class mem_fetch *mf, unsigned long long ready_cycle,
                          bool counts_towards_outstanding);
  void complete_mem_fetch(class mem_fetch *mf, unsigned long long cycle);

  unsigned m_id;
  const memory_config *m_config;
  class memory_stats_t *m_stats;
  class memory_partition_unit *m_memory_partition_unit;
 class gpgpu_sim *m_gpu;

  // Simplified link model: one shared transfer resource per memory partition.
  // Transfer time is governed by hbf_channel_bytes_per_cycle, and transfers are
  // serialized by reserving a [start, end) window in global cycles.
  //
  // To avoid systematic under-estimation of throughput from integer-cycle
  // rounding, the link scheduler tracks fractional-cycle timing (bytes/bw).
  double m_next_link_time;
  bool m_link_seen;
  double m_link_first_start;
  double m_link_last_end;
  unsigned long long m_link_bytes;
  double m_link_busy_time;

  // Internal parallelism model (Phase 1.5): die/plane-level serialization.
  //
  // Each HBF partition models a set of independent "planes" that can execute
  // Flash operations in parallel. A given page maps to (die, plane); operations
  // targeting the same plane are serialized. This is intentionally simple and
  // ignores FTL/GC/wear-leveling: it exists to capture the performance impact
  // of limited internal parallelism under long tREAD/tPROG latencies.
  unsigned m_dies_per_channel;
  unsigned m_planes_per_die;
  unsigned m_internal_planes;
  // Cached mapping parameters (fast path for power-of-two geometry).
  unsigned m_page_bytes;
  bool m_page_bytes_is_pow2;
  unsigned m_page_bytes_shift;
  bool m_dies_is_pow2;
  unsigned m_dies_shift;
  bool m_planes_is_pow2;
  unsigned m_planes_shift;
  std::vector<unsigned long long> m_plane_next_available_cycle;
  bool m_internal_seen;
  unsigned long long m_internal_first_start;
  unsigned long long m_internal_last_end;
  unsigned long long m_internal_busy_cycles;

  std::priority_queue<completion_t, std::vector<completion_t>,
                      completion_compare_t>
      m_completions;
  std::priority_queue<write_drain_t, std::vector<write_drain_t>,
                      write_drain_compare_t>
      m_write_drains;
  std::deque<class mem_fetch *> m_returnq;

  unsigned m_outstanding_reads;
  unsigned m_write_buffer_occupancy;

  unsigned long long n_req;
  unsigned long long n_rd;
  unsigned long long n_wr;
};

#endif  // HBF_H
