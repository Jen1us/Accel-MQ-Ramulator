#ifndef RAMULATOR2_BACKEND_H
#define RAMULATOR2_BACKEND_H

#include <cstdint>
#include <deque>
#include <vector>

#include "mem_backend.h"

class memory_config;
class memory_partition_unit;
class memory_stats_t;
class gpgpu_sim;
class mem_fetch;

// HBM backend backed by Ramulator2 via a small C-ABI wrapper loaded with dlopen.
//
// Design notes:
// - One Ramulator2 instance is shared across all partitions ("shared" mode).
// - Partition 0 drives time advancement + completion polling once per DRAM tick.
// - Completed mem_fetch objects are routed into per-partition return queues.
class ramulator2_backend final : public hbm_backend_ifc {
 public:
  ramulator2_backend(unsigned partition_id, const memory_config *config,
                     memory_stats_t *stats, memory_partition_unit *mp,
                     gpgpu_sim *gpu);
  ~ramulator2_backend() override;

  bool full(bool is_write) const override;
  void push(mem_fetch *data) override;
  void cycle() override;

  mem_fetch *return_queue_pop() override;
  mem_fetch *return_queue_top() override;

  void print(FILE *simFile) const override;
  void visualize() const override;
  void print_stat(FILE *simFile) override;
  void visualizer_print(gzFile visualizer_file) override;

  void dram_log(int task) override;

  void set_dram_power_stats(unsigned &cmd, unsigned &activity, unsigned &nop,
                            unsigned &act, unsigned &pre, unsigned &rd,
                            unsigned &wr, unsigned &wr_WB,
                            unsigned &req) const override;

 private:
  struct shared_state_t;
  static shared_state_t &shared();

  uint64_t now_time_ns() const;

  unsigned m_id;
  const memory_config *m_config;
  memory_stats_t *m_stats;
  memory_partition_unit *m_mp;
  gpgpu_sim *m_gpu;
};

#endif  // RAMULATOR2_BACKEND_H

