#include "unity.h"
#include "comm_route.h"
#include <string.h>
#include <stdlib.h>

void test_create_instance_with_static_buffer(void) {
    uint8_t buffer[4096];
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
        .tx_buf_per_slot = 256,
        .bcast_queue_depth = 4,
        .route_table = NULL,
        .route_count = 0,
    };
    cr_instance_t inst;
    int ret = cr_init(&inst, &cfg, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_INT(0, ret);
}

void test_init_fails_with_insufficient_buffer(void) {
    uint8_t buffer[32];
    cr_config_t cfg = {
        .local_addr = 0x01,
        .mtu = 64,
        .tx_queue_depth = 4,
        .rx_assem_count = 2,
        .dedup_table_size = 16,
        .rx_buf_per_slot = 256,
        .tx_buf_per_slot = 256,
        .bcast_queue_depth = 4,
    };
    cr_instance_t inst;
    int ret = cr_init(&inst, &cfg, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL_INT(-3, ret);
}

static int dummy_send(void *ctx, uint8_t next_hop, const uint8_t *data, uint16_t len) {
    (void)next_hop;
    (void)ctx; (void)data; (void)len; return 0;
}
static uint32_t dummy_tick(void) { return 0; }

void test_multi_instance_isolation(void) {
    uint8_t buf_a[4096], buf_b[2048];
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 8, .tx_queue_depth = 2,
        .rx_assem_count = 1, .dedup_table_size = 8,
        .frame_interval_ms = 0, .max_retries = 3,
        .ack_timeout_ms = 100, .ack_enabled = 0,
        .ack_mode = CR_ACK_MODE_REPLY, .default_ttl = 3,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 128,
        .tx_buf_per_slot = 256, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cr_instance_t inst_a, inst_b;
    cr_config_t cfg_b = cfg;
    cfg_b.local_addr = 0x02;

    TEST_ASSERT_EQUAL_INT(0, cr_init(&inst_a, &cfg, buf_a, sizeof(buf_a)));
    TEST_ASSERT_EQUAL_INT(0, cr_init(&inst_b, &cfg_b, buf_b, sizeof(buf_b)));

    /* Mark end of buf_b */
    buf_b[510] = 0xDE;
    buf_b[511] = 0xAD;

    /* Operate on inst_a */
    cr_hal_t hal_a = { .send = dummy_send, .get_tick_ms = dummy_tick, .hw_ctx = NULL };
    cr_set_hal(&inst_a, &hal_a);
    uint8_t data[] = {1, 2, 3};
    cr_send(&inst_a, 0x03, 0, data, 3, NULL, NULL);
    cr_poll(&inst_a);

    /* B's buffer tail unchanged */
    TEST_ASSERT_EQUAL_UINT8(0xDE, buf_b[510]);
    TEST_ASSERT_EQUAL_UINT8(0xAD, buf_b[511]);
}

void test_calc_buffer_size(void) {
    cr_config_t cfg = {
        .mtu = 64, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_buf_per_slot = 256,
        .tx_buf_per_slot = 256,
        .bcast_queue_depth = 4,
    };
    size_t size = cr_calc_buffer_size(&cfg);
    uint8_t *buf = (uint8_t *)malloc(size);
    cr_instance_t inst;
    TEST_ASSERT_EQUAL_INT(0, cr_init(&inst, &cfg, buf, size));
    TEST_ASSERT_EQUAL_INT(-3, cr_init(&inst, &cfg, buf, size - 1));
    free(buf);
}
