#include <cstdint>

#include <deque>
#include <cstdio>
#include <exception>
#include <functional>

#include "base/config.h"
#include "base/request.h"
#include "frontend/frontend.h"
#include "memory_system/memory_system.h"

namespace {

struct ram2_completion_t {
  void* user_ptr;
  uint64_t finish_time_ns;
};

struct ram2_pending_req_t {
  uint64_t addr;
  int req_type_id;
  int source_id;
  void* user_ptr;
};

struct ram2_handle_t {
  Ramulator::IFrontEnd* frontend = nullptr;
  Ramulator::IMemorySystem* mem = nullptr;
  double tck_ns = 1.0;
  uint64_t now_ticks = 0;
  uint64_t now_ns = 0;
  size_t max_pending = 1u << 20;  // safety cap to avoid unbounded growth
  std::deque<ram2_pending_req_t> pending;
  std::deque<ram2_completion_t> completions;
};

static void cleanup_partial_handle(ram2_handle_t* h) {
  if (!h) return;
  delete h->frontend;
  delete h->mem;
  delete h;
}

}  // namespace

extern "C" {

// Mirror the internal completion struct for the C ABI.
typedef struct ram2_completion {
  void* user_ptr;
  uint64_t finish_time_ns;
} ram2_completion;

void* ram2_create(const char* yaml_config_path) {
  auto* h = new ram2_handle_t;
  const char* stage = "start";
  try {
    stage = "parse_config";
    YAML::Node config =
        Ramulator::Config::parse_config_file(yaml_config_path, {});
    stage = "create_frontend";
    h->frontend = Ramulator::Factory::create_frontend(config);
    stage = "create_memory_system";
    h->mem = Ramulator::Factory::create_memory_system(config);

    stage = "connect_frontend_mem";
    h->frontend->connect_memory_system(h->mem);
    h->mem->connect_frontend(h->frontend);

    stage = "get_tCK";
    h->tck_ns = static_cast<double>(h->mem->get_tCK());
    if (h->tck_ns <= 0.0) h->tck_ns = 1.0;

    h->now_ticks = 0;
    h->now_ns = 0;
    return h;
  } catch (const std::exception& e) {
    // Don't let C++ exceptions cross the C ABI boundary; surface errors as NULL.
    fprintf(stderr, "ram2_create failed at %s: %s\n", stage, e.what());
  } catch (...) {
    fprintf(stderr, "ram2_create failed at %s: unknown exception\n", stage);
  }

  cleanup_partial_handle(h);
  return nullptr;
}

void ram2_destroy(void* handle) {
  auto* h = static_cast<ram2_handle_t*>(handle);
  if (!h) return;

  if (h->frontend) h->frontend->finalize();
  if (h->mem) h->mem->finalize();

  delete h->frontend;
  delete h->mem;
  delete h;
}

int ram2_send(void* handle, uint64_t addr, int is_write, int source_id,
              void* user_ptr) {
  auto* h = static_cast<ram2_handle_t*>(handle);
  if (!h) return 0;

  const int req_type_id = is_write ? Ramulator::Request::Type::Write
                                   : Ramulator::Request::Type::Read;

  if (h->pending.size() >= h->max_pending) return 0;
  h->pending.push_back({addr, req_type_id, source_id, user_ptr});
  return 1;
}

// Advance the Ramulator2 memory system to (approximately) time_ns.
//
// Note: We interpret get_tCK() as ns per memory tick. The wrapper advances by
// integer ticks and clamps the internal now_ns to time_ns (i.e., the wrapper
// acts as a tick-to driver).
void ram2_tick_to(void* handle, uint64_t time_ns) {
  auto* h = static_cast<ram2_handle_t*>(handle);
  if (!h || !h->frontend || !h->mem) return;

  if (time_ns <= h->now_ns) return;

  // Compute target ticks using the last known tCK.
  uint64_t target_ticks =
      static_cast<uint64_t>(static_cast<double>(time_ns) / h->tck_ns);
  if (target_ticks < h->now_ticks) target_ticks = h->now_ticks;

  const uint64_t delta = target_ticks - h->now_ticks;
  for (uint64_t i = 0; i < delta; ++i) {
    const uint64_t cur_tick = h->now_ticks + i;
    // Try to inject as many pending requests as the memory system will accept.
    while (!h->pending.empty()) {
      const auto preq = h->pending.front();
      // Ramulator2's GenericDRAMController only invokes callbacks for *reads*.
      // For writes, GEM5 wrapper returns the ack immediately after the request
      // is accepted. Keep the same posted-write semantics here so accel-sim
      // doesn't stall forever on stores.
      std::function<void(Ramulator::Request&)> cb;
      if (preq.req_type_id == Ramulator::Request::Type::Read) {
        cb = [h, user_ptr = preq.user_ptr](Ramulator::Request& req) {
          const uint64_t finish_ns = static_cast<uint64_t>(
              static_cast<double>(req.depart) * h->tck_ns);
          h->completions.push_back({user_ptr, finish_ns});
        };
      }

      bool ok = h->frontend->receive_external_requests(preq.req_type_id,
                                                       preq.addr,
                                                       preq.source_id, cb);
      if (!ok) break;

      if (preq.req_type_id == Ramulator::Request::Type::Write) {
        // Ack a posted write on the next controller tick.
        const uint64_t depart_tick = cur_tick + 1;
        const uint64_t finish_ns = static_cast<uint64_t>(
            static_cast<double>(depart_tick) * h->tck_ns);
        h->completions.push_back({preq.user_ptr, finish_ns});
      }

      h->pending.pop_front();
    }

    h->mem->tick();
  }

  h->now_ticks = target_ticks;
  h->now_ns = time_ns;
}

int ram2_poll(void* handle, ram2_completion* out) {
  auto* h = static_cast<ram2_handle_t*>(handle);
  if (!h || !out) return 0;
  if (h->completions.empty()) return 0;

  auto c = h->completions.front();
  h->completions.pop_front();
  out->user_ptr = c.user_ptr;
  out->finish_time_ns = c.finish_time_ns;
  return 1;
}

}  // extern C
