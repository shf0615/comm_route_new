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

void test_unicast_send_single_frame(void) {
    uint8_t buffer[1024];
    cr_route_entry_t routes[] = { {.dest = 0x03, .next_hop = 0x02} };
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .route_table = routes, .route_count = 1,
    };
    cr_instance_t inst;
    cr_init(&inst, &cfg, buffer, sizeof(buffer));

    cr_hal_t hal = { .send = mock_send, .get_tick_ms = mock_get_tick, .hw_ctx = NULL };
    cr_set_hal(&inst, &hal);

    send_count = 0;
    sent_len = 0;
    uint8_t payload[] = "HELLO";
    int ret = cr_send(&inst, 0x03, 0, payload, 5, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(0, ret);

    cr_poll(&inst);

    TEST_ASSERT_EQUAL_INT(1, send_count);
    /* 帧头: DST=0x03, SRC=0x01, CTL=0x00, SEQ=0, TTL=3 */
    TEST_ASSERT_EQUAL_UINT8(0x03, sent_buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x01, sent_buf[1]);
    TEST_ASSERT_EQUAL_UINT8(0x00, sent_buf[2]);
    TEST_ASSERT_EQUAL_UINT8(3, sent_buf[4]);
    /* payload */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(payload, &sent_buf[5], 5);
    /* total length = 5 header + 5 payload */
    TEST_ASSERT_EQUAL_UINT16(10, sent_len);
}

void test_send_queue_full(void) {
    uint8_t buffer[1024];
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 2,
        .rx_assem_count = 1, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .route_table = NULL, .route_count = 0,
    };
    cr_instance_t inst;
    cr_init(&inst, &cfg, buffer, sizeof(buffer));
    cr_hal_t hal = { .send = mock_send, .get_tick_ms = mock_get_tick, .hw_ctx = NULL };
    cr_set_hal(&inst, &hal);

    uint8_t data[] = {1, 2, 3};
    TEST_ASSERT_EQUAL_INT(0, cr_send(&inst, 0x02, 0, data, 3, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(0, cr_send(&inst, 0x03, 0, data, 3, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(-1, cr_send(&inst, 0x04, 0, data, 3, NULL, NULL));
}

/* --- 长数据拆帧 --- */
static uint8_t send_history[10][64];
static uint16_t send_history_len[10];
static int send_history_count;

static int mock_send_history(void *ctx, const uint8_t *data, uint16_t len) {
    (void)ctx;
    memcpy(send_history[send_history_count], data, len);
    send_history_len[send_history_count] = len;
    send_history_count++;
    return 0;
}

void test_long_data_segmentation(void) {
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
    cr_hal_t hal = { .send = mock_send_history, .get_tick_ms = mock_get_tick, .hw_ctx = NULL };
    cr_set_hal(&inst, &hal);

    send_history_count = 0;
    uint8_t data[20];
    for (int i = 0; i < 20; i++) data[i] = (uint8_t)i;

    cr_send(&inst, 0x03, 0, data, 20, NULL, NULL);

    mock_tick = 0;
    cr_poll(&inst); cr_poll(&inst); cr_poll(&inst);

    TEST_ASSERT_EQUAL_INT(3, send_history_count);
    /* 帧1: 5头 + 8负载 = 13 */
    TEST_ASSERT_EQUAL_UINT16(13, send_history_len[0]);
    /* 帧2: 5 + 8 = 13 */
    TEST_ASSERT_EQUAL_UINT16(13, send_history_len[1]);
    /* 帧3: 5 + 4 = 9 */
    TEST_ASSERT_EQUAL_UINT16(9, send_history_len[2]);

    /* CTL: 帧1 分片=1 末帧=0 */
    TEST_ASSERT_BITS(0x20, 0x20, send_history[0][2]);
    TEST_ASSERT_BITS(0x10, 0x00, send_history[0][2]);
    /* CTL: 帧3 分片=1 末帧=1 */
    TEST_ASSERT_BITS(0x30, 0x30, send_history[2][2]);
}

void test_frame_interval_pacing(void) {
    uint8_t buffer[1024];
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 8, .frame_interval_ms = 10,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .route_table = NULL, .route_count = 0,
    };
    cr_instance_t inst;
    cr_init(&inst, &cfg, buffer, sizeof(buffer));
    cr_hal_t hal = { .send = mock_send_history, .get_tick_ms = mock_get_tick, .hw_ctx = NULL };
    cr_set_hal(&inst, &hal);

    send_history_count = 0;
    uint8_t data[20] = {0};
    cr_send(&inst, 0x03, 0, data, 20, NULL, NULL);

    mock_tick = 0;
    cr_poll(&inst);
    TEST_ASSERT_EQUAL_INT(1, send_history_count);

    mock_tick = 5; /* 才过5ms */
    cr_poll(&inst);
    TEST_ASSERT_EQUAL_INT(1, send_history_count); /* 未发送 */

    mock_tick = 10; /* 够10ms */
    cr_poll(&inst);
    TEST_ASSERT_EQUAL_INT(2, send_history_count); /* 第2帧 */
}
