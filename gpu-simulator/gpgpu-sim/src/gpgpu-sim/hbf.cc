#include "hbf.h"

#include <assert.h>

#include <algorithm>
#include <cmath>

#include "gpu-sim.h"
#include "l2cache.h"
#include "mem_fetch.h"

static inline unsigned hbf_hash32(unsigned x) {
  // 32-bit mix (same structure as the CUDA microbench helper).
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

hbf_t::hbf_t(unsigned partition_id, const memory_config *config,
             class memory_stats_t *stats, class memory_partition_unit *mp,
             class gpgpu_sim *gpu)
    : m_id(partition_id),
      m_config(config),
      m_stats(stats),
      m_memory_partition_unit(mp),
      m_gpu(gpu),
      m_next_link_time(0.0),
      m_link_seen(false),
      m_link_first_start(0.0),
      m_link_last_end(0.0),
      m_link_bytes(0),
      m_link_busy_time(0.0),
      m_dies_per_channel(1),
      m_planes_per_die(1),
      m_internal_planes(1),
      m_page_bytes(1),
      m_page_bytes_is_pow2(true),
      m_page_bytes_shift(0),
      m_dies_is_pow2(true),
      m_dies_shift(0),
      m_planes_is_pow2(true),
      m_planes_shift(0),
      m_plane_next_available_cycle(),
      m_internal_seen(false),
      m_internal_first_start(0),
      m_internal_last_end(0),
      m_internal_busy_cycles(0),
      m_outstanding_reads(0),
      m_write_buffer_occupancy(0),
      n_req(0),
      n_rd(0),
      n_wr(0) {
  assert(m_config);
  assert(m_gpu);

  m_dies_per_channel =
      m_config->hbf_dies_per_channel ? m_config->hbf_dies_per_channel : 1;
  m_planes_per_die =
      m_config->hbf_planes_per_die ? m_config->hbf_planes_per_die : 1;

  m_page_bytes = m_config->hbf_page_bytes ? m_config->hbf_page_bytes : 1;
  m_page_bytes_is_pow2 =
      (m_page_bytes != 0) && ((m_page_bytes & (m_page_bytes - 1)) == 0);
  m_page_bytes_shift = m_page_bytes_is_pow2 ? (unsigned)__builtin_ctz(m_page_bytes) : 0;

  m_dies_is_pow2 =
      (m_dies_per_channel != 0) &&
      ((m_dies_per_channel & (m_dies_per_channel - 1)) == 0);
  m_dies_shift = m_dies_is_pow2 ? (unsigned)__builtin_ctz(m_dies_per_channel) : 0;

  m_planes_is_pow2 =
      (m_planes_per_die != 0) &&
      ((m_planes_per_die & (m_planes_per_die - 1)) == 0);
  m_planes_shift = m_planes_is_pow2 ? (unsigned)__builtin_ctz(m_planes_per_die) : 0;

  const unsigned long long planes =
      (unsigned long long)m_dies_per_channel * (unsigned long long)m_planes_per_die;
  // Avoid zero-sized vectors; also guard against overflow into 0.
  m_internal_planes = planes ? (unsigned)planes : 1;
  m_plane_next_available_cycle.assign(m_internal_planes, 0ULL);
}

unsigned long long hbf_t::now() const {
  return m_gpu->gpu_sim_cycle + m_gpu->gpu_tot_sim_cycle;
}

unsigned hbf_t::queue_limit() const { return m_config->hbf_max_outstanding; }

bool hbf_t::full(bool is_write) const {
  if (is_write) {
    return m_write_buffer_occupancy >= m_config->hbf_write_buffer_entries;
  }
  const unsigned max_outstanding = m_config->hbf_max_outstanding;
  if (max_outstanding == 0) return false;
  return m_outstanding_reads >= max_outstanding;
}

bool hbf_t::returnq_full() const {
  const unsigned limit = m_config->hbf_dram_return_queue_size;
  if (limit == 0) return false;
  return m_returnq.size() >= limit;
}

unsigned hbf_t::que_length() const {
  return m_outstanding_reads + m_write_buffer_occupancy;
}

unsigned long long hbf_t::reserve_link_transfer(
    unsigned xfer_bytes, unsigned long long earliest_start) {
  const double bw = m_config->hbf_channel_bytes_per_cycle
                        ? (double)m_config->hbf_channel_bytes_per_cycle
                        : 1.0;
  const double bytes = xfer_bytes ? (double)xfer_bytes : 1.0;

  const double earliest = (double)earliest_start;
  const double start = std::max(earliest, m_next_link_time);
  const double end = start + (bytes / bw);
  m_next_link_time = end;

  if (!m_link_seen) {
    m_link_seen = true;
    m_link_first_start = start;
    m_link_last_end = end;
  } else {
    m_link_first_start = std::min(m_link_first_start, start);
    m_link_last_end = std::max(m_link_last_end, end);
  }
  m_link_bytes += (unsigned long long)bytes;
  m_link_busy_time += (bytes / bw);

  // Completion is an integer cycle boundary in the simulator. We conservatively
  // round up.
  return (unsigned long long)std::ceil(end);
}

unsigned hbf_t::map_to_plane(const mem_fetch *mf) const {
  // Use the request's global physical address to compute the Flash page index.
  //
  // Important: the existing simulator routes cache-line transactions to
  // partitions using a DRAM-oriented interleaving scheme. If we used the
  // partition-local address here, the "page index" would effectively be
  // compressed (because many global pages are striped across partitions), which
  // would underutilize die/plane parallelism and unrealistically serialize
  // tREAD/tPROG. Using the global address preserves page-level diversity.
  const unsigned long long addr = (unsigned long long)mf->get_addr();
  const unsigned long long page =
      m_page_bytes_is_pow2 ? (addr >> m_page_bytes_shift)
                           : (addr / (unsigned long long)m_page_bytes);

  // Map page -> (die, plane) via a hash to avoid correlations with the DRAM
  // partitioning/interleaving scheme. This acts as a simple stand-in for an
  // FTL remapping layer while keeping the invariant that all cache lines within
  // the same Flash page serialize on the same internal plane.
  const unsigned page32 =
      (unsigned)((page & 0xffffffffULL) ^ ((page >> 32) & 0xffffffffULL));
  const unsigned h = hbf_hash32(page32);
  if (m_internal_planes <= 1) return 0;
  if ((m_internal_planes & (m_internal_planes - 1)) == 0) {
    return h & (m_internal_planes - 1);
  }
  return h % m_internal_planes;
}

unsigned long long hbf_t::reserve_plane(unsigned plane, unsigned latency_cycles,
                                       unsigned long long earliest_start) {
  if (m_internal_planes == 0) return earliest_start + latency_cycles;
  const unsigned idx = plane % m_internal_planes;
  unsigned long long start = earliest_start;
  if (m_plane_next_available_cycle[idx] > start)
    start = m_plane_next_available_cycle[idx];
  const unsigned long long end = start + (unsigned long long)latency_cycles;
  m_plane_next_available_cycle[idx] = end;

  if (!m_internal_seen) {
    m_internal_seen = true;
    m_internal_first_start = start;
    m_internal_last_end = end;
  } else {
    m_internal_first_start = std::min(m_internal_first_start, start);
    m_internal_last_end = std::max(m_internal_last_end, end);
  }
  m_internal_busy_cycles += (unsigned long long)latency_cycles;
  return end;
}

void hbf_t::enqueue_completion(mem_fetch *mf, unsigned long long ready_cycle,
                               bool counts_towards_outstanding) {
  completion_t c;
  c.ready_cycle = ready_cycle;
  c.mf = mf;
  c.counts_towards_outstanding = counts_towards_outstanding;
  m_completions.push(c);
}

void hbf_t::complete_mem_fetch(mem_fetch *data, unsigned long long cycle) {
  data->set_status(IN_PARTITION_MC_RETURNQ, cycle);
  if (data->get_access_type() != L1_WRBK_ACC &&
      data->get_access_type() != L2_WRBK_ACC) {
    data->set_reply();
    m_returnq.push_back(data);
  } else {
    m_memory_partition_unit->set_done(data);
    delete data;
  }
}

void hbf_t::push(class mem_fetch *data) {
  assert(data);

  n_req++;
  if (data->is_write()) {
    n_wr++;
  } else {
    n_rd++;
  }

  const unsigned long long cur = now();
  const unsigned xfer_bytes =
      m_config->hbf_subpage_bytes ? m_config->hbf_subpage_bytes : 1;

  // Posted write: ACK when buffered, and asynchronously drain the buffer.
  if (data->is_write()) {
    if (m_write_buffer_occupancy >= m_config->hbf_write_buffer_entries) return;
    m_write_buffer_occupancy++;

    // Model link transfer as a serialized resource. The write ACK is returned
    // after the write data is transferred into the controller plus a small ACK
    // latency (posted semantics).
    const unsigned long long xfer_done =
        reserve_link_transfer(xfer_bytes, cur);
    const unsigned ack_lat = m_config->hbf_posted_write_ack_latency;
    enqueue_completion(data, xfer_done + ack_lat, false);

    // Drain model (Phase 1): assume each buffered write consumes one entry and
    // is flushed after tPROG. With internal parallelism enabled, the program
    // stage is serialized per (die,plane).
    // This does not yet model page coalescing/FTL/GC.
    const unsigned plane = map_to_plane(data);
    const unsigned long long prog_done =
        reserve_plane(plane, m_config->hbf_t_prog, xfer_done);
    write_drain_t d;
    d.ready_cycle = prog_done;
    d.entries = 1;
    m_write_drains.push(d);
    return;
  }

  // Read: occupy one outstanding slot until completion.
  if (full(false)) return;
  m_outstanding_reads++;

  // Read completes after internal tREAD (serialized per plane), then data
  // transfers over the serialized link.
  const unsigned plane = map_to_plane(data);
  const unsigned long long internal_ready =
      reserve_plane(plane, m_config->hbf_t_read, cur);
  const unsigned long long xfer_done =
      reserve_link_transfer(xfer_bytes, internal_ready);
  enqueue_completion(data, xfer_done, true);
}

void hbf_t::cycle() {
  const unsigned long long cur = now();

  // Drain completed background write flushes (free buffer space).
  while (!m_write_drains.empty() && m_write_drains.top().ready_cycle <= cur) {
    write_drain_t d = m_write_drains.top();
    m_write_drains.pop();
    if (d.entries >= m_write_buffer_occupancy)
      m_write_buffer_occupancy = 0;
    else
      m_write_buffer_occupancy -= d.entries;
  }

  // Move ready completions into the return queue (subject to returnq size).
  while (!m_completions.empty() && m_completions.top().ready_cycle <= cur &&
         !returnq_full()) {
    completion_t c = m_completions.top();
    m_completions.pop();
    if (c.counts_towards_outstanding && m_outstanding_reads > 0) {
      m_outstanding_reads--;
    }
    complete_mem_fetch(c.mf, cur);
  }
}

class mem_fetch *hbf_t::return_queue_top() {
  if (m_returnq.empty()) return NULL;
  return m_returnq.front();
}

class mem_fetch *hbf_t::return_queue_pop() {
  if (m_returnq.empty()) return NULL;
  class mem_fetch *mf = m_returnq.front();
  m_returnq.pop_front();
  return mf;
}

void hbf_t::print(FILE *simFile) const {
  const double cfg_bw = m_config->hbf_channel_bytes_per_cycle
                            ? (double)m_config->hbf_channel_bytes_per_cycle
                            : 0.0;

  double achieved_bytes_per_cycle = 0.0;
  double util = 0.0;
  double span = 0.0;
  if (m_link_seen && (m_link_last_end > m_link_first_start)) {
    span = m_link_last_end - m_link_first_start;
    achieved_bytes_per_cycle = (double)m_link_bytes / span;
    util = m_link_busy_time / span;
  }

  double internal_util = 0.0;
  unsigned long long internal_span = 0;
  if (m_internal_seen && (m_internal_last_end > m_internal_first_start) &&
      m_internal_planes > 0) {
    internal_span = m_internal_last_end - m_internal_first_start;
    internal_util =
        (double)m_internal_busy_cycles /
        ((double)m_internal_planes * (double)internal_span);
  }

  fprintf(
      simFile,
      "HBF[%u]: out_rd=%u wb=%u pending_comp=%zu returnq=%zu n_req=%llu "
      "n_rd=%llu n_wr=%llu link_bytes=%llu link_cfg_B/cycle=%.3f "
      "link_busy_time_cycles=%.3f link_achieved_B/cycle=%.3f link_util=%.3f "
      "internal_planes=%u internal_busy_cycles=%llu internal_span=%llu "
      "internal_util=%.3f\n",
      m_id, m_outstanding_reads, m_write_buffer_occupancy, m_completions.size(),
      m_returnq.size(), n_req, n_rd, n_wr, m_link_bytes, cfg_bw,
      m_link_busy_time, achieved_bytes_per_cycle, util, m_internal_planes,
      m_internal_busy_cycles, internal_span, internal_util);
  fprintf(simFile, "HBF[%u]: link window [%.3f, %.3f) span=%.3f cycles\n",
          m_id, m_link_first_start, m_link_last_end, span);
}

void hbf_t::visualize() const {}

void hbf_t::print_stat(FILE *simFile) { print(simFile); }

void hbf_t::visualizer_print(gzFile /*visualizer_file*/) {}

void hbf_t::set_hbf_power_stats(unsigned &cmd, unsigned &activity,
                                unsigned &nop, unsigned &act, unsigned &pre,
                                unsigned &rd, unsigned &wr, unsigned &wr_WB,
                                unsigned &req) const {
  // Phase 1: model HBF power using the same interface as DRAM, so the existing
  // AccelWattch memory-controller power path can be reused. We only report
  // request counts (reads/writes); command-level DRAM counters are not modeled.
  cmd = 0;
  activity = 0;
  nop = 0;
  act = 0;
  pre = 0;
  rd = (unsigned)n_rd;
  wr = (unsigned)n_wr;
  wr_WB = 0;
  req = (unsigned)n_req;
}
