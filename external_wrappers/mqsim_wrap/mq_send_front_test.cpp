#include "mqsim_wrap.h"

#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
  const char* xml_path = (argc > 1) ? argv[1] : "mqsim.xml";

  mq_create_params_t params = {};
  // Keep these zero to use XML defaults.
  params.page_size_bytes = 4096;

  void* h = mq_create2(xml_path, &params);
  if (!h) {
    std::fprintf(stderr, "mq_create2 failed, xml=%s\n", xml_path);
    return 1;
  }

  const uint64_t addr = 4096ULL * 100;  // fixed LPN
  const uint32_t size = 4096;
  const uint32_t part_id = 0;
  const int source_id = 0;

  // A group: stream_type=1 (SLC) write then read.
  if (!mq_send(h, part_id, addr, size, 1, source_id, 1, (void*)0x1111)) {
    std::fprintf(stderr, "mq_send A-write failed\n");
  }
  mq_tick_to(h, 1000);

  if (!mq_send(h, part_id, addr, size, 0, source_id, 1, (void*)0x2222)) {
    std::fprintf(stderr, "mq_send A-read failed\n");
  }
  mq_tick_to(h, 2000);

  // B group: stream_type=2 (MLC) write then read, same LPN.
  if (!mq_send(h, part_id, addr, size, 1, source_id, 2, (void*)0x3333)) {
    std::fprintf(stderr, "mq_send B-write failed\n");
  }
  mq_tick_to(h, 3000);

  if (!mq_send(h, part_id, addr, size, 0, source_id, 2, (void*)0x4444)) {
    std::fprintf(stderr, "mq_send B-read failed\n");
  }
  mq_tick_to(h, 4000);

  mq_completion c;
  while (mq_poll(h, &c)) {
    std::printf("complete user_ptr=%p t=%llu\n", c.user_ptr,
                (unsigned long long)c.finish_time_ns);
  }

  mq_destroy(h);
  return 0;
}