#include "unity.h"
#include "comm_route.h"
#include <string.h>

static uint8_t sent_buf[256];
static uint16_t sent_len;
static int send_count;

static int mock_send(void *ctx, uint8_t next_hop, const uint8_t *data, uint16_t len) {
    (void)ctx; (void)next_hop;
    memcpy(sent_buf, data, len);
    sent_len = len;
    send_count++;
    return 0;
}

static uint32_t mock_tick = 0;
static uint32_t mock_get_tick(void) { return mock_tick; }

/* Helper: 标准初始化 */
static cr_instance_t inst;
static uint8_t buffer[4096];
static cr_hal_t hal;
static cr_route_entry_t routes[1];

static void setup_instance(uint8_t addr, uint16_t mtu, uint8_t ack_enabled) {
    routes[0] = (cr_route_entry_t){.dest = 0x03, .next_hop = 0x02};
    cr_config_t cfg = {
        .local_addr = addr, .mtu = mtu, .frame_interval_ms = 0,
        .ack_enabled = ack_enabled, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = routes, .route_count = 1,
    };
    cr_init(&inst, &cfg, buffer, sizeof(buffer));
    hal = (cr_hal_t){ .send = mock_send, .get_tick_ms = mock_get_tick, .hw_ctx = NULL };
    cr_set_hal(&inst, &hal);
    send_count = 0;
    sent_len = 0;
    mock_tick = 0;
}

/* Scenario: 发送端自动填充 LEN 字段 */
void test_tx_fills_len_field(void) {
    setup_instance(0x01, 64, 0);
    uint8_t payload[] = "HELLO";  /* 5 bytes */
    cr_send(&inst, 0x03, 0, payload, 5, NULL, NULL);
    cr_poll(&inst);
    TEST_ASSERT_EQUAL_INT(1, send_count);
    /* frame[5] = LEN = 5 */
    TEST_ASSERT_EQUAL_UINT8(5, sent_buf[5]);
    /* 帧头大小 = 6, 总长 = 6 + 5 = 11 */
    TEST_ASSERT_EQUAL_UINT16(11, sent_len);
    /* payload 从 offset 6 开始 */
    TEST_ASSERT_EQUAL_MEMORY("HELLO", &sent_buf[6], 5);
}

/* Scenario: ACK 帧 LEN=0 */
void test_ack_frame_len_zero(void) {
    setup_instance(0x02, 64, 1);
    /* 构造一帧数据帧发给 0x02，触发 ACK 回复 */
    /*         DST   SRC   CTL   SEQ   TTL   LEN   PAYLOAD */
    uint8_t frame[] = {0x02, 0x01, 0x00, 0x00, 0x03, 0x02, 'H', 'I'};
    send_count = 0;
    cr_feed_frame(&inst, frame, sizeof(frame));
    /* 应发送 ACK */
    TEST_ASSERT_EQUAL_INT(1, send_count);
    /* ACK 帧 frame[5] = LEN = 0 */
    TEST_ASSERT_EQUAL_UINT8(0, sent_buf[5]);
    /* ACK 帧总长 = 帧头大小 = 6 */
    TEST_ASSERT_EQUAL_UINT16(6, sent_len);
}

/* Scenario: 广播帧携带 LEN */
void test_broadcast_fills_len_field(void) {
    setup_instance(0x01, 64, 0);
    uint8_t payload[] = "HI";  /* 2 bytes */
    cr_broadcast(&inst, 0, payload, 2);
    cr_poll(&inst);
    TEST_ASSERT_EQUAL_INT(1, send_count);
    /* frame[5] = LEN = 2 */
    TEST_ASSERT_EQUAL_UINT8(2, sent_buf[5]);
    /* DST = 0xFF */
    TEST_ASSERT_EQUAL_UINT8(0xFF, sent_buf[0]);
}

/* ===== RX Tests ===== */

static uint8_t recv_payload[256];
static uint16_t recv_len;
static int recv_count;

static void mock_recv(cr_instance_t *i, uint8_t src, uint8_t biz_id,
                      const uint8_t *data, uint16_t len, void *ctx) {
    (void)i; (void)src; (void)biz_id; (void)ctx;
    if (len > 0) memcpy(recv_payload, data, len);
    recv_len = len;
    recv_count++;
}

/* Scenario: 接收端根据 LEN 截取有效数据 */
void test_rx_uses_len_field(void) {
    setup_instance(0x02, 64, 0);
    cr_set_recv_callback(&inst, mock_recv, NULL);
    recv_count = 0;
    recv_len = 0;

    /* 构造 64 字节帧，LEN=5，后面填充垃圾 */
    uint8_t frame[64];
    memset(frame, 0xAA, sizeof(frame));  /* 填充 */
    frame[0] = 0x02;  /* DST = 本机 */
    frame[1] = 0x01;  /* SRC */
    frame[2] = 0x00;  /* CTL */
    frame[3] = 0x00;  /* SEQ */
    frame[4] = 0x03;  /* TTL */
    frame[5] = 5;     /* LEN = 5 */
    memcpy(&frame[6], "HELLO", 5);

    cr_feed_frame(&inst, frame, 64);  /* 底层传入总长 64 */

    TEST_ASSERT_EQUAL_INT(1, recv_count);
    TEST_ASSERT_EQUAL_UINT16(5, recv_len);  /* 应该是 5，不是 64-6=58 */
    TEST_ASSERT_EQUAL_MEMORY("HELLO", recv_payload, 5);
}

/* Scenario: LEN=0 的数据帧 */
void test_rx_len_zero_data_frame(void) {
    setup_instance(0x02, 64, 0);
    cr_set_recv_callback(&inst, mock_recv, NULL);
    recv_count = 0;
    recv_len = 0xFF;  /* 故意设非零 */

    uint8_t frame[] = {0x02, 0x01, 0x00, 0x00, 0x03, 0x00};  /* LEN=0 */
    cr_feed_frame(&inst, frame, sizeof(frame));

    TEST_ASSERT_EQUAL_INT(1, recv_count);
    TEST_ASSERT_EQUAL_UINT16(0, recv_len);
}
