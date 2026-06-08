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

static uint8_t recv_src;
static uint8_t recv_biz_id;
static uint8_t recv_data[64];
static uint16_t recv_len;
static int recv_called;

static void mock_recv_cb(cr_instance_t *inst, uint8_t src, uint8_t biz_id,
                         const uint8_t *data, uint16_t len, void *ctx) {
    (void)inst; (void)ctx;
    recv_src = src;
    recv_biz_id = biz_id;
    memcpy(recv_data, data, len);
    recv_len = len;
    recv_called++;
}

static cr_instance_t inst;
static uint8_t buffer[1024];
static cr_hal_t hal;

static void setup_instance(uint8_t addr, const cr_route_entry_t *routes, uint8_t route_count) {
    cr_config_t cfg = {
        .local_addr = addr, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .route_table = routes, .route_count = route_count,
    };
    cr_init(&inst, &cfg, buffer, sizeof(buffer));
    hal.send = mock_send;
    hal.get_tick_ms = mock_get_tick;
    hal.hw_ctx = NULL;
    cr_set_hal(&inst, &hal);
    cr_set_recv_callback(&inst, mock_recv_cb, NULL);
    recv_called = 0;
    send_count = 0;
    sent_len = 0;
}

void test_receive_unicast_single_frame(void) {
    setup_instance(0x02, NULL, 0);

    /* 帧: DST=0x02, SRC=0x01, CTL=0x00, SEQ=0, TTL=3, payload="HI" */
    uint8_t frame[] = {0x02, 0x01, 0x00, 0x00, 0x03, 'H', 'I'};
    cr_feed_frame(&inst, frame, sizeof(frame));

    TEST_ASSERT_EQUAL_INT(1, recv_called);
    TEST_ASSERT_EQUAL_UINT8(0x01, recv_src);
    TEST_ASSERT_EQUAL_UINT16(2, recv_len);
    TEST_ASSERT_EQUAL_UINT8('H', recv_data[0]);
    TEST_ASSERT_EQUAL_UINT8('I', recv_data[1]);
    /* 不应转发 */
    TEST_ASSERT_EQUAL_INT(0, send_count);
}

void test_forward_frame_to_next_hop(void) {
    cr_route_entry_t routes[] = { {.dest = 0x03, .next_hop = 0x03} };
    setup_instance(0x02, routes, 1);

    /* 帧: DST=0x03, SRC=0x01 → 不是本机，查路由转发 */
    uint8_t frame[] = {0x03, 0x01, 0x00, 0x00, 0x03, 'D', 'A', 'T', 'A'};
    cr_feed_frame(&inst, frame, sizeof(frame));

    /* 应转发，恰好调用一次 send */
    TEST_ASSERT_EQUAL_INT(1, send_count);
    TEST_ASSERT_EQUAL_UINT16(sizeof(frame), sent_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(frame, sent_buf, sizeof(frame));
    /* 不应触发接收回调 */
    TEST_ASSERT_EQUAL_INT(0, recv_called);
}

void test_drop_frame_no_route(void) {
    cr_route_entry_t routes[] = { {.dest = 0x03, .next_hop = 0x03} };
    setup_instance(0x02, routes, 1);

    /* DST=0x05 无路由 */
    uint8_t frame[] = {0x05, 0x01, 0x00, 0x00, 0x03, 'X'};
    cr_feed_frame(&inst, frame, sizeof(frame));

    TEST_ASSERT_EQUAL_INT(0, send_count);
    TEST_ASSERT_EQUAL_INT(0, recv_called);
}
