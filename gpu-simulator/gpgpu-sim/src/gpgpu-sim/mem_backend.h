// Minimal abstract interfaces for memory backends used by memory_partition_unit.
//
// We keep the "dram_t/hbf_t" style interface surface (push/full/cycle/returnq)
// so L2/ICNT/warp-side code stays unchanged, while allowing external backends
// (e.g., Ramulator2, MQSim) to plug in.

#ifndef MEM_BACKEND_H
#define MEM_BACKEND_H

#include <stdio.h>
#include <zlib.h>

class mem_fetch;

class hbm_backend_ifc {
 public:
  virtual ~hbm_backend_ifc() = default;

  virtual bool full(bool is_write) const = 0;
  virtual void push(mem_fetch *data) = 0;
  virtual void cycle() = 0;

  virtual mem_fetch *return_queue_pop() = 0;
  virtual mem_fetch *return_queue_top() = 0;

  virtual void print(FILE *simFile) const = 0;
  virtual void visualize() const = 0;
  virtual void print_stat(FILE *simFile) = 0;
  virtual void visualizer_print(gzFile visualizer_file) = 0;

  virtual void dram_log(int task) = 0;

  // Power model interface (same as dram_t).
  virtual void set_dram_power_stats(unsigned &cmd, unsigned &activity,
                                    unsigned &nop, unsigned &act,
                                    unsigned &pre, unsigned &rd,
                                    unsigned &wr, unsigned &wr_WB,
                                    unsigned &req) const = 0;
};

class hbf_backend_ifc {
 public:
  virtual ~hbf_backend_ifc() = default;

  virtual bool full(bool is_write) const = 0;
  virtual void push(mem_fetch *data) = 0;
  virtual void cycle() = 0;

  virtual mem_fetch *return_queue_pop() = 0;
  virtual mem_fetch *return_queue_top() = 0;

  virtual void print(FILE *simFile) const = 0;
  virtual void visualize() const = 0;
  virtual void print_stat(FILE *simFile) = 0;
  virtual void visualizer_print(gzFile visualizer_file) = 0;

  // Power model interface (same as hbf_t).
  virtual void set_hbf_power_stats(unsigned &cmd, unsigned &activity,
                                   unsigned &nop, unsigned &act,
                                   unsigned &pre, unsigned &rd,
                                   unsigned &wr, unsigned &wr_WB,
                                   unsigned &req) const = 0;
};

#endif  // MEM_BACKEND_H

