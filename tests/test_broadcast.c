#include "unity.h"
#include "comm_route.h"
#include <string.h>

static uint8_t sent_buf[64];
static uint16_t sent_len;
static int send_count;

static int mock_send(void *ctx, const uint8_t *data, uint16_t len) {
    memcpy(sent_buf, data, len);
    sent_len = len;
    send_count++;
    return 0;
}

static uint32_t mock_tick = 0;
static uint32_t mock_get_tick(void) { return mock_tick; }

void test_broadcast_single_frame(void) {
    uint8_t buffer[1024];
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .route_table = NULL, .route_count = 0,
    };
    cr_instance_t inst;
    cr_init(&inst, &cfg, buffer, sizeof(buffer));
    cr_hal_t hal = { .send = mock_send, .get_tick_ms = mock_get_tick, .hw_ctx = NULL };
    cr_set_hal(&inst, &hal);

    send_count = 0;
    sent_len = 0;
    uint8_t payload[] = "BCAST";
    int ret = cr_broadcast(&inst, 0, payload, 5);
    TEST_ASSERT_EQUAL_INT(0, ret);

    cr_poll(&inst);

    TEST_ASSERT_EQUAL_INT(1, send_count);
    TEST_ASSERT_EQUAL_UINT8(0xFF, sent_buf[0]);  /* DST = broadcast */
    TEST_ASSERT_EQUAL_UINT8(0x01, sent_buf[1]);  /* SRC */
    /* CTL: bit6=1(广播) → 0x40 */
    TEST_ASSERT_EQUAL_UINT8(0x40, sent_buf[2]);
    TEST_ASSERT_EQUAL_UINT8(3, sent_buf[4]);     /* TTL=3 */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, &sent_buf[5], 5);
}

void test_broadcast_exceeds_mtu(void) {
    uint8_t buffer[1024];
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 8, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .route_table = NULL, .route_count = 0,
    };
    cr_instance_t inst;
    cr_init(&inst, &cfg, buffer, sizeof(buffer));

    uint8_t data[16] = {0};
    int ret = cr_broadcast(&inst, 0, data, 16);
    TEST_ASSERT_EQUAL_INT(-2, ret);
}
