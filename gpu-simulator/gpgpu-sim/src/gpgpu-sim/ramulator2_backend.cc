#include "ramulator2_backend.h"

#include <dlfcn.h>
#include <inttypes.h>

#include <cassert>
#include <cstdio>
#include <cstring>

#include "gpu-sim.h"
#include "l2cache.h"
#include "mem_fetch.h"
#include "mem_latency_stat.h"

namespace {

// C-ABI mirror of the wrapper's completion struct.
struct ram2_completion_t {
  void *user_ptr;
  uint64_t finish_time_ns;
};

}  // namespace

struct ramulator2_backend::shared_state_t {
  bool enabled = false;
  bool initialized = false;

  void *dl = nullptr;
  void *h = nullptr;

  using create_f = void *(*)(const char *);
  using destroy_f = void (*)(void *);
  using send_f = int (*)(void *, uint64_t, int, int, void *);
  using tick_to_f = void (*)(void *, uint64_t);
  using poll_f = int (*)(void *, ram2_completion_t *);

  create_f create = nullptr;
  destroy_f destroy = nullptr;
  send_f send = nullptr;
  tick_to_f tick_to = nullptr;
  poll_f poll = nullptr;

  uint64_t last_tick_ns = 0;

  // Per-partition return queues, and book-keeping.
  std::vector<std::deque<mem_fetch *>> returnq;
  std::vector<memory_partition_unit *> mp_by_part;
  std::vector<unsigned> outstanding;

  unsigned max_outstanding_per_part = 32768;

  // Simple counters for power/stats (optional).
  std::vector<unsigned long long> n_req;
  std::vector<unsigned long long> n_rd;
  std::vector<unsigned long long> n_wr;

  void ensure_vectors(unsigned n_mem) {
    if (returnq.size() == n_mem) return;
    returnq.assign(n_mem, {});
    mp_by_part.assign(n_mem, nullptr);
    outstanding.assign(n_mem, 0);
    n_req.assign(n_mem, 0);
    n_rd.assign(n_mem, 0);
    n_wr.assign(n_mem, 0);
  }
};

ramulator2_backend::shared_state_t &ramulator2_backend::shared() {
  static shared_state_t st;
  return st;
}

static void *load_sym(void *dl, const char *name) {
  void *p = dlsym(dl, name);
  if (!p) {
    fprintf(stderr, "Ramulator2 backend: dlsym(%s) failed: %s\n", name,
            dlerror());
  }
  return p;
}

ramulator2_backend::ramulator2_backend(unsigned partition_id,
                                       const memory_config *config,
                                       memory_stats_t *stats,
                                       memory_partition_unit *mp,
                                       gpgpu_sim *gpu)
    : m_id(partition_id), m_config(config), m_stats(stats), m_mp(mp), m_gpu(gpu) {
  auto &st = shared();
  st.enabled = config->hbm_use_ramulator2;
  if (!st.enabled) return;

  st.ensure_vectors(config->m_n_mem);
  st.mp_by_part[m_id] = mp;

  if (config->hbm_ramulator2_max_outstanding)
    st.max_outstanding_per_part = config->hbm_ramulator2_max_outstanding;

  if (st.initialized) return;

  const char *so_path = config->hbm_ramulator2_wrapper
                            ? config->hbm_ramulator2_wrapper
                            : "libramulator2_wrap.so";
  const char *cfg_path = config->hbm_ramulator2_config;
  if (!cfg_path || cfg_path[0] == '\0') {
    fprintf(stderr,
            "Ramulator2 backend enabled but -hbm_ramulator2_config not set\n");
    return;
  }

  st.dl = dlopen(so_path, RTLD_NOW | RTLD_LOCAL);
  if (!st.dl) {
    fprintf(stderr, "Ramulator2 backend: dlopen(%s) failed: %s\n", so_path,
            dlerror());
    return;
  }

  st.create = reinterpret_cast<shared_state_t::create_f>(
      load_sym(st.dl, "ram2_create"));
  st.destroy = reinterpret_cast<shared_state_t::destroy_f>(
      load_sym(st.dl, "ram2_destroy"));
  st.send = reinterpret_cast<shared_state_t::send_f>(load_sym(st.dl, "ram2_send"));
  st.tick_to = reinterpret_cast<shared_state_t::tick_to_f>(
      load_sym(st.dl, "ram2_tick_to"));
  st.poll = reinterpret_cast<shared_state_t::poll_f>(load_sym(st.dl, "ram2_poll"));

  if (!st.create || !st.destroy || !st.send || !st.tick_to || !st.poll) {
    fprintf(stderr, "Ramulator2 backend: missing symbols, disabling\n");
    return;
  }

  st.h = st.create(cfg_path);
  if (!st.h) {
    fprintf(stderr, "Ramulator2 backend: ram2_create failed\n");
    return;
  }

  st.last_tick_ns = 0;
  st.initialized = true;
  fprintf(stdout, "Ramulator2 backend: enabled (shared), so=%s cfg=%s\n",
          so_path, cfg_path);
}

ramulator2_backend::~ramulator2_backend() {
  // Shared handle is intentionally kept alive for the whole process lifetime.
}

uint64_t ramulator2_backend::now_time_ns() const {
  // memory_partition_unit::dram_cycle() is called on DRAM clock edges.
  // gpgpu_sim stores the time of the *next* edge, so subtract one period.
  const double t_s = m_gpu->get_dram_time() - m_gpu->get_dram_period();
  const double t_ns = t_s * 1e9;
  if (t_ns <= 0.0) return 0;
  return static_cast<uint64_t>(t_ns);
}

bool ramulator2_backend::full(bool /*is_write*/) const {
  auto &st = shared();
  if (!st.enabled || !st.initialized) return false;
  if (m_id >= st.outstanding.size()) return false;
  return st.outstanding[m_id] >= st.max_outstanding_per_part;
}

void ramulator2_backend::push(mem_fetch *data) {
  auto &st = shared();
  assert(st.enabled && st.initialized);

  if (full(data->is_write())) {
    // Caller should have checked full(); keep behavior deterministic.
    assert(0 && "ramulator2_backend::push called while full()");
  }

  const uint64_t addr = static_cast<uint64_t>(data->get_addr());
  const int is_write = data->is_write() ? 1 : 0;
  // Ramulator2's GenericDRAMController keeps per-core vectors sized by
  // frontend->get_num_cores() (GEM5 frontend defaults to 1).  Accel-sim's
  // mem_fetch::get_sid() can be >> 0 (e.g., SM id), which would cause OOB
  // indexing inside Ramulator2 stats.  We don't currently need per-SM stats,
  // so keep a single source id.
  const int source_id = 0;

  int ok = st.send(st.h, addr, is_write, source_id, data);
  if (!ok) {
    // Wrapper-level pending queue overflow is treated as fatal for now.
    fprintf(stderr,
            "Ramulator2 backend: ram2_send failed (pending queue full?)\n");
    assert(0);
  }

  st.outstanding[m_id]++;
  st.n_req[m_id]++;
  if (is_write)
    st.n_wr[m_id]++;
  else
    st.n_rd[m_id]++;

  data->set_status(IN_PARTITION_MC_INTERFACE_QUEUE,
                   m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle);
  m_stats->memlatstat_dram_access(data);
}

void ramulator2_backend::cycle() {
  auto &st = shared();
  if (!st.enabled || !st.initialized) return;

  // Only partition 0 advances time and drains completions once per DRAM tick.
  if (m_id != 0) return;

  const uint64_t t_ns = now_time_ns();
  if (t_ns == st.last_tick_ns) return;
  st.last_tick_ns = t_ns;

  st.tick_to(st.h, t_ns);

  ram2_completion_t c;
  while (st.poll(st.h, &c)) {
    mem_fetch *mf = static_cast<mem_fetch *>(c.user_ptr);
    if (!mf) continue;

    const unsigned pid = mf->get_tlx_addr().chip;
    if (pid < st.outstanding.size() && st.outstanding[pid] > 0) {
      st.outstanding[pid]--;
    }

    if (mf->get_access_type() == L1_WRBK_ACC ||
        mf->get_access_type() == L2_WRBK_ACC) {
      // Match dram_t behavior: writebacks are completed internally and deleted.
      if (pid < st.mp_by_part.size() && st.mp_by_part[pid]) {
        st.mp_by_part[pid]->set_done(mf);
      }
      delete mf;
      continue;
    }

    mf->set_reply();
    if (pid < st.returnq.size()) {
      st.returnq[pid].push_back(mf);
    } else {
      // Should not happen; avoid leaks.
      delete mf;
    }
  }
}

mem_fetch *ramulator2_backend::return_queue_top() {
  auto &st = shared();
  if (!st.enabled || !st.initialized) return nullptr;
  if (m_id >= st.returnq.size()) return nullptr;
  auto &q = st.returnq[m_id];
  return q.empty() ? nullptr : q.front();
}

mem_fetch *ramulator2_backend::return_queue_pop() {
  auto &st = shared();
  if (!st.enabled || !st.initialized) return nullptr;
  if (m_id >= st.returnq.size()) return nullptr;
  auto &q = st.returnq[m_id];
  if (q.empty()) return nullptr;
  mem_fetch *mf = q.front();
  q.pop_front();
  return mf;
}

void ramulator2_backend::print(FILE *simFile) const {
  auto &st = shared();
  fprintf(simFile, "  HBM backend: Ramulator2 (shared)\n");
  if (m_id < st.outstanding.size()) {
    fprintf(simFile, "    outstanding=%u max_outstanding=%u\n",
            st.outstanding[m_id], st.max_outstanding_per_part);
  }
}

void ramulator2_backend::visualize() const {}

void ramulator2_backend::print_stat(FILE *simFile) {
  auto &st = shared();
  if (!st.enabled || !st.initialized) return;
  if (m_id >= st.n_req.size()) return;
  fprintf(simFile,
          "ramulator2_backend[%u]: n_req=%llu n_rd=%llu n_wr=%llu\n", m_id,
          st.n_req[m_id], st.n_rd[m_id], st.n_wr[m_id]);
}

void ramulator2_backend::visualizer_print(gzFile /*visualizer_file*/) {}

void ramulator2_backend::dram_log(int /*task*/) {}

void ramulator2_backend::set_dram_power_stats(
    unsigned &cmd, unsigned &activity, unsigned &nop, unsigned &act,
    unsigned &pre, unsigned &rd, unsigned &wr, unsigned &wr_WB,
    unsigned &req) const {
  // We only have request-level counts in this integration stage.
  // Keep command-level counters at 0 to avoid breaking AccelWattch.
  cmd = activity = nop = act = pre = wr_WB = 0;

  auto &st = shared();
  if (st.enabled && st.initialized && m_id < st.n_req.size()) {
    rd = (unsigned)st.n_rd[m_id];
    wr = (unsigned)st.n_wr[m_id];
    req = (unsigned)st.n_req[m_id];
  } else {
    rd = wr = req = 0;
  }
}
