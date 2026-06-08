#include "unity.h"
#include "comm_route.h"
#include <string.h>

void test_create_instance_with_static_buffer(void) {
    uint8_t buffer[1024];
    cr_config_t cfg = {
        .local_addr = 0x01,
        .mtu = 64,
        .frame_interval_ms = 5,
        .max_retries = 3,
        .ack_timeout_ms = 100,
        .ack_enabled = 1,
        .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3,
        .tx_queue_depth = 4,
        .rx_assem_count = 2,
        .dedup_table_size = 16,
        .rx_assem_timeout_ms = 1000,
        .rx_buf_per_slot = 256,
        .route_table = NULL,
        .route_count = 0,
    };
    cr_instance_t inst;
    int ret = cr_init(&inst, &cfg, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_INT(0, ret);
}
