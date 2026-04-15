#include "mqsim_wrap.h"
#include <cstdio>
#include <cstdlib>
// 1) 进入目录并重编 wrapper（确保 AMU 改动进 .so）
// cd "/HSC/accel_HSC/Accel-MQ-Ramulator/external_wrappers/mqsim_wrap"
// cmake --build build -- -j4
// g++ -std=c++17 mq_send_front_test.cpp -I. -Lbuild -lmqsim_wrap -Wl,-rpath,"$PWD/build" -o build/mq_send_front_test
// 2) 打开调试开关并运行测试
// ACCEL_HBF_STREAM_DEBUG=1 ./build/mq_send_front_test ../../MQSim/ssdconfig.xml

int main(int argc, char** argv) {
  const char* xml_path = (argc > 1) ? argv[1] : "mqsim.xml";

  mq_create_params_t params = {};
  params.page_size_bytes = 4096;

  void* h = mq_create2(xml_path, &params);
  if (!h) {
    std::fprintf(stderr, "mq_create2 failed, xml=%s\n", xml_path);
    return 1;
  }

  const uint32_t size = 4096;
  const uint32_t part_id = 0;
  const int source_id = 0;
  uint64_t current_time_ns = 0;

  std::printf("\n=== [TEST STAGE 1] Injecting SLC (stream_type=0) Writes ===\n");
  // 注入 4 个连续的 SLC 写请求 (LPA: 100~103)
  for (int i = 0; i < 5; i++) {
    uint64_t addr = 4096ULL * (100 + i);
    mq_send(h, part_id, addr, size, 1, source_id, 0, (void*)(0x1110ULL + i));
  }
  // 推进 2,000,000 ns (2 毫秒)，给足时间让闪存完成映射表拉取和数据写入
  current_time_ns += 2000000;
  mq_tick_to(h, current_time_ns);


  std::printf("\n=== [TEST STAGE 2] Injecting MLC (stream_type=1) Writes ===\n");
  // 注入 4 个连续的 MLC 写请求 (LPA: 200~203)
  for (int i = 0; i < 5; i++) {
    uint64_t addr = 4096ULL * (200 + i);
    mq_send(h, part_id, addr, size, 1, source_id, 1, (void*)(0x3330ULL + i));
  }
  // 再推进 2 毫秒
  current_time_ns += 2000000;
  mq_tick_to(h, current_time_ns);


  std::printf("\n=== [TEST STAGE 3] Injecting Reads ===\n");
  // 验证读请求也能正确路由
  mq_send(h, part_id, 4096ULL * 100, size, 0, source_id, 0, (void*)0x2220); // 读 SLC
  mq_send(h, part_id, 4096ULL * 200, size, 0, source_id, 1, (void*)0x4440); // 读 MLC
  
  // 推进足够长时间确保 Flash Read 完成

  current_time_ns += 200000000;
  mq_tick_to(h, current_time_ns);
  current_time_ns += 2000000;
  mq_tick_to(h, current_time_ns);

  std::printf("\n=== [TEST STAGE 4] Polling Completions ===\n");
  mq_completion c;
  while (mq_poll(h, &c)) {
    std::printf("Complete user_ptr=%p t=%llu ns\n", c.user_ptr,
                (unsigned long long)c.finish_time_ns);
  }

  mq_destroy(h);
  return 0;
}