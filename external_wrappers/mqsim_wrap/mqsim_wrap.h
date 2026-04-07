#ifndef MQSIM_WRAP_H
#define MQSIM_WRAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mq_create_params_t {
  uint32_t channel_count;
  uint32_t chip_no_per_channel;
  uint32_t die_no_per_chip;
  uint32_t plane_no_per_die;
  uint32_t block_no_per_plane;
  uint32_t page_no_per_block;
  uint32_t page_size_bytes;
  uint32_t channel_width_bytes;
  uint32_t two_unit_data_in_time;
  uint32_t two_unit_data_out_time;
  uint64_t t_read;
  uint64_t t_prog;
  uint64_t t_erase;
} mq_create_params_t;

typedef struct mq_completion {
  void* user_ptr;
  uint64_t finish_time_ns;
} mq_completion;

void* mq_create(const char* mqsim_xml_config_path);
void* mq_create2(const char* mqsim_xml_config_path, const mq_create_params_t* params);
void mq_destroy(void* handle);
int mq_send(void* handle, uint32_t part_id, uint64_t part_addr,
            uint32_t size_bytes, int is_write, int source_id,
            int HBF_Stream_input_type, void* user_ptr);
void mq_tick_to(void* handle, uint64_t time_ns);
int mq_poll(void* handle, mq_completion* out);

#ifdef __cplusplus
}
#endif

#endif
