#include "unity.h"
#include "comm_route.h"
#include <string.h>

static uint8_t sent_buf[64];
static uint16_t sent_len;
static int send_count;

static int mock_send(void *ctx, uint8_t next_hop, const uint8_t *data, uint16_t len) {
    (void)next_hop;
    memcpy(sent_buf, data, len);
    sent_len = len;
    send_count++;
    return 0;
}

static uint32_t mock_tick = 0;
static uint32_t mock_get_tick(void) { return mock_tick; }

void test_broadcast_single_frame(void) {
    uint8_t buffer[4096];
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .tx_buf_per_slot = 256, .bcast_queue_depth = 4,
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
    uint8_t buffer[4096];
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 8, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .tx_buf_per_slot = 256, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cr_instance_t inst;
    cr_init(&inst, &cfg, buffer, sizeof(buffer));

    uint8_t data[16] = {0};
    int ret = cr_broadcast(&inst, 0, data, 16);
    TEST_ASSERT_EQUAL_INT(-2, ret);
}

/* --- 接收广播相关 --- */
static uint8_t recv_src_b;
static uint8_t recv_data_b[64];
static uint16_t recv_len_b;
static int recv_called_b;

static void mock_recv_cb_b(cr_instance_t *i, uint8_t src, uint8_t biz_id,
                           const uint8_t *data, uint16_t len, void *ctx) {
    (void)i; (void)biz_id; (void)ctx;
    recv_src_b = src;
    memcpy(recv_data_b, data, len);
    recv_len_b = len;
    recv_called_b++;
}

static cr_instance_t inst_b;
static uint8_t buf_b[4096];
static cr_hal_t hal_b;
static uint8_t sent_buf_b2[64];
static uint16_t sent_len_b2;
static int send_count_b2;
static uint8_t send_history_b2[10][64];

static int mock_send_b2(void *ctx, uint8_t next_hop, const uint8_t *data, uint16_t len) {
    (void)next_hop;
    (void)ctx;
    memcpy(sent_buf_b2, data, len);
    if (send_count_b2 < 10) {
        memcpy(send_history_b2[send_count_b2], data, len);
    }
    sent_len_b2 = len;
    send_count_b2++;
    return 0;
}

static void setup_bcast_instance(uint8_t addr) {
    cr_config_t cfg = {
        .local_addr = addr, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .tx_buf_per_slot = 256, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cr_init(&inst_b, &cfg, buf_b, sizeof(buf_b));
    hal_b.send = mock_send_b2;
    hal_b.get_tick_ms = mock_get_tick;
    hal_b.hw_ctx = NULL;
    cr_set_hal(&inst_b, &hal_b);
    cr_set_recv_callback(&inst_b, mock_recv_cb_b, NULL);
    recv_called_b = 0;
    send_count_b2 = 0;
    sent_len_b2 = 0;
}

void test_receive_broadcast_and_forward(void) {
    setup_bcast_instance(0x02);

    /* CTL: bit6=1(广播) → 0x40, SEQ=7, TTL=2 */
    uint8_t frame[] = {0xFF, 0x01, 0x40, 0x07, 0x02, 'B'};
    cr_feed_frame(&inst_b, frame, sizeof(frame));

    /* 交给上层 */
    TEST_ASSERT_EQUAL_INT(1, recv_called_b);
    TEST_ASSERT_EQUAL_UINT8(0x01, recv_src_b);
    /* 转发且 TTL 递减 */
    TEST_ASSERT_EQUAL_INT(1, send_count_b2);
    TEST_ASSERT_EQUAL_UINT8(0x01, sent_buf_b2[4]);  /* TTL=2-1=1 */
}

void test_broadcast_ttl_zero_no_forward(void) {
    setup_bcast_instance(0x03);

    /* TTL=0 */
    uint8_t frame[] = {0xFF, 0x01, 0x40, 0x08, 0x00, 'X'};
    cr_feed_frame(&inst_b, frame, sizeof(frame));

    TEST_ASSERT_EQUAL_INT(1, recv_called_b);   /* 仍处理 */
    TEST_ASSERT_EQUAL_INT(0, send_count_b2);   /* 不转发 */
}

void test_broadcast_dedup(void) {
    setup_bcast_instance(0x02);

    /* 第一次：正常处理 */
    uint8_t frame[] = {0xFF, 0x01, 0x40, 0x05, 0x02, 'D'};
    cr_feed_frame(&inst_b, frame, sizeof(frame));
    TEST_ASSERT_EQUAL_INT(1, recv_called_b);

    /* 第二次：重复，丢弃 */
    recv_called_b = 0;
    send_count_b2 = 0;
    cr_feed_frame(&inst_b, frame, sizeof(frame));
    TEST_ASSERT_EQUAL_INT(0, recv_called_b);
    TEST_ASSERT_EQUAL_INT(0, send_count_b2);
}

void test_broadcast_parallel_with_unicast(void) {
    uint8_t buffer[4096];
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 8, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .tx_buf_per_slot = 256, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cr_instance_t inst_p;
    cr_init(&inst_p, &cfg, buffer, sizeof(buffer));
    cr_hal_t hal_p = { .send = mock_send_b2, .get_tick_ms = mock_get_tick, .hw_ctx = NULL };
    cr_set_hal(&inst_p, &hal_p);

    send_count_b2 = 0;
    uint8_t data[20] = {0};
    cr_send(&inst_p, 0x03, 0, data, 20, NULL, NULL);

    cr_poll(&inst_p); /* 单播帧1 */
    cr_poll(&inst_p); /* 单播帧2 */
    TEST_ASSERT_EQUAL_INT(2, send_count_b2);

    /* 提交广播 */
    uint8_t bcast[] = "BC";
    cr_broadcast(&inst_p, 1, bcast, 2);

    cr_poll(&inst_p); /* 应发出广播帧 + 单播帧3 */

    /* 验证广播帧被发出（检查 send 记录中有 DST=0xFF） */
    int found_broadcast = 0;
    int total_after = send_count_b2;
    /* 至少多了一次广播发送 */
    TEST_ASSERT_TRUE(total_after >= 3);

    /* 遍历检查最后几帧 */
    for (int i = 2; i < total_after; i++) {
        if (send_history_b2[i][0] == 0xFF) found_broadcast = 1;
    }
    TEST_ASSERT_EQUAL_INT(1, found_broadcast);
}
