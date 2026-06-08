#include "unity.h"
#include "comm_route.h"
#include <string.h>

static uint8_t sent_buf[64];
static uint16_t sent_len;
static int send_count;

static int mock_send(void *ctx, const uint8_t *data, uint16_t len) {
    (void)ctx;
    memcpy(sent_buf, data, len);
    sent_len = len;
    send_count++;
    return 0;
}

static uint32_t mock_tick = 0;
static uint32_t mock_get_tick(void) { return mock_tick; }

static uint8_t complete_status;
static int complete_called;
static void mock_on_complete(uint8_t status, void *ctx) {
    (void)ctx;
    complete_status = status;
    complete_called++;
}

void test_ack_disabled_fire_and_forget(void) {
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

    complete_called = 0;
    uint8_t data[] = {1, 2, 3};
    cr_send(&inst, 0x03, 0, data, 3, mock_on_complete, NULL);
    cr_poll(&inst);

    TEST_ASSERT_EQUAL_INT(1, complete_called);
    TEST_ASSERT_EQUAL_UINT8(0, complete_status);
}

void test_long_data_complete_callback_ack_off(void) {
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
    cr_hal_t hal = { .send = mock_send, .get_tick_ms = mock_get_tick, .hw_ctx = NULL };
    cr_set_hal(&inst, &hal);

    complete_called = 0;
    mock_tick = 0;
    uint8_t data[20] = {0};
    cr_send(&inst, 0x03, 0, data, 20, mock_on_complete, NULL);

    cr_poll(&inst); cr_poll(&inst);
    TEST_ASSERT_EQUAL_INT(0, complete_called);

    cr_poll(&inst); /* 第3帧（最后） */
    TEST_ASSERT_EQUAL_INT(1, complete_called);
    TEST_ASSERT_EQUAL_UINT8(0, complete_status);
}

void test_ack_reply_normal(void) {
    uint8_t buffer[1024];
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 1, .ack_mode = CR_ACK_MODE_REPLY,
        .ack_timeout_ms = 100, .max_retries = 3,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .route_table = NULL, .route_count = 0,
    };
    cr_instance_t inst;
    cr_init(&inst, &cfg, buffer, sizeof(buffer));
    cr_hal_t hal = { .send = mock_send, .get_tick_ms = mock_get_tick, .hw_ctx = NULL };
    cr_set_hal(&inst, &hal);

    complete_called = 0;
    mock_tick = 0;
    uint8_t data[] = {1, 2, 3};
    cr_send(&inst, 0x03, 0, data, 3, mock_on_complete, NULL);
    cr_poll(&inst); /* sends frame, enters WAIT_ACK */

    /* ACK帧: DST=0x01, SRC=0x03, CTL.bit7=1, SEQ=匹配 */
    uint8_t ack_frame[] = {0x01, 0x03, 0x80, sent_buf[3], 0x03};
    cr_feed_frame(&inst, ack_frame, sizeof(ack_frame));

    TEST_ASSERT_EQUAL_INT(1, complete_called);
    TEST_ASSERT_EQUAL_UINT8(0, complete_status);
}

void test_ack_timeout_retransmit(void) {
    uint8_t buffer[1024];
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 1, .ack_mode = CR_ACK_MODE_REPLY,
        .ack_timeout_ms = 100, .max_retries = 3,
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
    mock_tick = 0;
    uint8_t data[] = {1, 2, 3};
    cr_send(&inst, 0x03, 0, data, 3, mock_on_complete, NULL);
    cr_poll(&inst);
    TEST_ASSERT_EQUAL_INT(1, send_count);

    mock_tick = 101;
    cr_poll(&inst);
    TEST_ASSERT_EQUAL_INT(2, send_count);
}

void test_ack_max_retries_fail(void) {
    uint8_t buffer[1024];
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 1, .ack_mode = CR_ACK_MODE_REPLY,
        .ack_timeout_ms = 100, .max_retries = 3,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .route_table = NULL, .route_count = 0,
    };
    cr_instance_t inst;
    cr_init(&inst, &cfg, buffer, sizeof(buffer));
    cr_hal_t hal = { .send = mock_send, .get_tick_ms = mock_get_tick, .hw_ctx = NULL };
    cr_set_hal(&inst, &hal);

    complete_called = 0;
    mock_tick = 0;
    uint8_t data[] = {1, 2, 3};
    cr_send(&inst, 0x03, 0, data, 3, mock_on_complete, NULL);

    mock_tick = 0;   cr_poll(&inst); /* 首发 */
    mock_tick = 101; cr_poll(&inst); /* 重传1 */
    mock_tick = 202; cr_poll(&inst); /* 重传2 */
    mock_tick = 303; cr_poll(&inst); /* 重传3 */
    mock_tick = 404; cr_poll(&inst); /* 判定失败 */

    TEST_ASSERT_EQUAL_INT(1, complete_called);
    TEST_ASSERT_NOT_EQUAL(0, complete_status);
}

void test_receiver_auto_sends_ack(void) {
    uint8_t buffer[1024];
    cr_config_t cfg = {
        .local_addr = 0x03, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 1, .ack_mode = CR_ACK_MODE_REPLY,
        .ack_timeout_ms = 100, .max_retries = 3,
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
    /* 喂入数据帧: DST=0x03, SRC=0x01, CTL=0x00, SEQ=5, TTL=3, payload */
    uint8_t frame[] = {0x03, 0x01, 0x00, 0x05, 0x03, 'H', 'I'};
    cr_feed_frame(&inst, frame, sizeof(frame));

    /* 验证自动发了 ACK */
    TEST_ASSERT_EQUAL_INT(1, send_count);
    TEST_ASSERT_EQUAL_UINT8(0x01, sent_buf[0]);  /* DST=原始SRC */
    TEST_ASSERT_EQUAL_UINT8(0x03, sent_buf[1]);  /* SRC=本机 */
    TEST_ASSERT_BITS(0x80, 0x80, sent_buf[2]);   /* CTL.bit7=1 (ACK) */
    TEST_ASSERT_EQUAL_UINT8(0x05, sent_buf[3]);  /* SEQ匹配 */
}
