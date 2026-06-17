#include "unity.h"
#include "comm_route.h"
#include <string.h>

static uint8_t sent_buf[512];
static uint16_t sent_len;
static int send_count;

static int mock_send(void *ctx, uint8_t next_hop, const uint8_t *data, uint16_t len) {
    (void)ctx; (void)next_hop;
    if (len <= sizeof(sent_buf)) memcpy(sent_buf, data, len);
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

/* Scenario: LEN 超过帧实际大小 - 丢弃帧 */
void test_rx_len_overflow_drops_frame(void) {
    setup_instance(0x02, 64, 0);
    cr_set_recv_callback(&inst, mock_recv, NULL);
    recv_count = 0;

    /* 帧总长 10 字节，但 LEN 声称 100 */
    uint8_t frame[10] = {0x02, 0x01, 0x00, 0x00, 0x03, 100, 'A', 'B', 'C', 'D'};
    cr_feed_frame(&inst, frame, 10);

    TEST_ASSERT_EQUAL_INT(0, recv_count);  /* 应丢弃，不回调 */
}

/* Scenario: LEN 超过帧实际大小 - 广播帧也丢弃 */
void test_rx_len_overflow_broadcast_drops(void) {
    setup_instance(0x02, 64, 0);
    cr_set_recv_callback(&inst, mock_recv, NULL);
    recv_count = 0;

    /* 广播帧总长 8 字节，但 LEN 声称 50 */
    uint8_t frame[8] = {0xFF, 0x01, 0x40, 0x00, 0x02, 50, 'X', 'Y'};
    cr_feed_frame(&inst, frame, 8);

    TEST_ASSERT_EQUAL_INT(0, recv_count);
}

/* Scenario: 长数据拆帧 - 每帧各自填充 LEN */
void test_tx_multiframe_each_fills_len(void) {
    /* MTU=16, 帧头=6, 单帧最大载荷=10 */
    routes[0] = (cr_route_entry_t){.dest = 0x03, .next_hop = 0x02};
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 16, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = routes, .route_count = 1,
    };
    cr_init(&inst, &cfg, buffer, sizeof(buffer));

    static uint8_t frames[3][16];
    static uint16_t frame_lens[3];
    static int frame_idx;
    frame_idx = 0;

    int multi_send(void *ctx, uint8_t next_hop, const uint8_t *data, uint16_t len) {
        (void)ctx; (void)next_hop;
        if (frame_idx < 3) {
            memcpy(frames[frame_idx], data, len);
            frame_lens[frame_idx] = len;
            frame_idx++;
        }
        return 0;
    }

    cr_hal_t mhal = { .send = multi_send, .get_tick_ms = mock_get_tick, .hw_ctx = NULL };
    cr_set_hal(&inst, &mhal);

    uint8_t data[25];
    memset(data, 0xCC, 25);
    cr_send(&inst, 0x03, 0, data, 25, NULL, NULL);

    /* 发 3 帧 */
    mock_tick = 0;  cr_poll(&inst);
    mock_tick = 1;  cr_poll(&inst);
    mock_tick = 2;  cr_poll(&inst);

    TEST_ASSERT_EQUAL_INT(3, frame_idx);
    TEST_ASSERT_EQUAL_UINT8(10, frames[0][5]);  /* LEN=10 */
    TEST_ASSERT_EQUAL_UINT8(10, frames[1][5]);  /* LEN=10 */
    TEST_ASSERT_EQUAL_UINT8(5,  frames[2][5]);  /* LEN=5 */
}

/* Scenario: 长数据组装 - 按各帧 LEN 累计 */
void test_rx_multiframe_assembly_uses_len(void) {
    setup_instance(0x02, 64, 0);
    cr_set_recv_callback(&inst, mock_recv, NULL);
    recv_count = 0;
    recv_len = 0;

    /* 3 帧分片：LEN=10, LEN=10, LEN=5 */
    uint8_t f1[16] = {0x02, 0x01, 0x20, 0x00, 0x03, 10};  /* CTL: FRAG=1 */
    memset(&f1[6], 'A', 10);
    uint8_t f2[16] = {0x02, 0x01, 0x20, 0x01, 0x03, 10};  /* FRAG, SEQ=1 */
    memset(&f2[6], 'B', 10);
    uint8_t f3[11] = {0x02, 0x01, 0x30, 0x02, 0x03, 5};   /* FRAG+LAST, SEQ=2 */
    memset(&f3[6], 'C', 5);

    cr_feed_frame(&inst, f1, sizeof(f1));
    cr_feed_frame(&inst, f2, sizeof(f2));
    cr_feed_frame(&inst, f3, sizeof(f3));

    TEST_ASSERT_EQUAL_INT(1, recv_count);
    TEST_ASSERT_EQUAL_UINT16(25, recv_len);
}

/* ===== 额外测试：边界 + 异常 + 安全 + 性能 + 集成 + 端到端 ===== */

/* [额外-边界] LEN=255（最大值） */
void test_len_field_max_255(void) {
    /* MTU 需要足够大：header(6) + 255 = 261, pool 减小以适配 buffer */
    static uint8_t big_buffer[16384];
    routes[0] = (cr_route_entry_t){.dest = 0x03, .next_hop = 0x02};
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 261, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 8, .bcast_queue_depth = 4,
        .route_table = routes, .route_count = 1,
    };
    cr_init(&inst, &cfg, big_buffer, sizeof(big_buffer));
    hal = (cr_hal_t){ .send = mock_send, .get_tick_ms = mock_get_tick, .hw_ctx = NULL };
    cr_set_hal(&inst, &hal);
    send_count = 0;

    uint8_t payload[255];
    memset(payload, 0xAB, 255);
    cr_send(&inst, 0x03, 0, payload, 255, NULL, NULL);
    cr_poll(&inst);

    TEST_ASSERT_EQUAL_INT(1, send_count);
    /* frame[5] = LEN = 255 */
    TEST_ASSERT_EQUAL_UINT8(255, sent_buf[5]);
    /* 总长 = header(6) + 255 = 261 */
    TEST_ASSERT_EQUAL_UINT16(261, sent_len);
}

/* [额外-边界] LEN 恰好等于帧剩余空间（header+LEN == 帧总长） */
void test_len_field_exactly_fills_frame(void) {
    /* MTU=16, 帧头=6, 最大payload=10 */
    setup_instance(0x01, 16, 0);
    uint8_t payload[10];
    memset(payload, 0xDD, 10);
    cr_send(&inst, 0x03, 0, payload, 10, NULL, NULL);
    cr_poll(&inst);

    TEST_ASSERT_EQUAL_INT(1, send_count);
    /* LEN=10, 帧总长=16=MTU */
    TEST_ASSERT_EQUAL_UINT8(10, sent_buf[5]);
    TEST_ASSERT_EQUAL_UINT16(16, sent_len);
    /* payload 完整 */
    uint8_t expected[10];
    memset(expected, 0xDD, 10);
    TEST_ASSERT_EQUAL_MEMORY(expected, &sent_buf[6], 10);
}

/* [额外-异常] LEN=0 但帧中有多余数据（应忽略多余数据） */
void test_rx_len_zero_ignores_trailing_data(void) {
    setup_instance(0x02, 64, 0);
    cr_set_recv_callback(&inst, mock_recv, NULL);
    recv_count = 0;
    recv_len = 0xFF;

    /* LEN=0 但帧后面有多余垃圾数据 */
    uint8_t frame[20] = {0x02, 0x01, 0x00, 0x00, 0x03, 0x00,
                         'G', 'A', 'R', 'B', 'A', 'G', 'E', '!',
                         0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
    cr_feed_frame(&inst, frame, sizeof(frame));

    /* 应回调，但交付数据长度为 0（忽略多余数据） */
    TEST_ASSERT_EQUAL_INT(1, recv_count);
    TEST_ASSERT_EQUAL_UINT16(0, recv_len);
}

/* [额外-异常] 广播帧 LEN 超过实际大小 */
void test_rx_broadcast_len_exceeds_actual_drops(void) {
    setup_instance(0x02, 64, 0);
    cr_set_recv_callback(&inst, mock_recv, NULL);
    recv_count = 0;

    /* 广播帧总长=9, LEN声称20（header(6)+20=26 > 9） */
    uint8_t frame[9] = {0xFF, 0x01, 0x40, 0x00, 0x02, 20, 'A', 'B', 'C'};
    cr_feed_frame(&inst, frame, sizeof(frame));

    /* 应丢弃，不回调 */
    TEST_ASSERT_EQUAL_INT(0, recv_count);
}

/* [额外-安全] 伪造帧 LEN 远大于 MTU（不能缓冲区越界） */
void test_security_forged_len_far_exceeds_mtu(void) {
    setup_instance(0x02, 64, 0);
    cr_set_recv_callback(&inst, mock_recv, NULL);
    recv_count = 0;

    /* 帧总长=10, LEN=255（远超 MTU=64） */
    uint8_t frame[10] = {0x02, 0x01, 0x00, 0x00, 0x03, 255, 'X', 'Y', 'Z', 'W'};
    cr_feed_frame(&inst, frame, 10);

    /* 必须丢弃，不能越界读取 */
    TEST_ASSERT_EQUAL_INT(0, recv_count);

    /* 再测试 LEN=254 但帧只有 header */
    uint8_t frame2[6] = {0x02, 0x01, 0x00, 0x00, 0x03, 254};
    cr_feed_frame(&inst, frame2, 6);
    TEST_ASSERT_EQUAL_INT(0, recv_count);
}

/* [额外-性能] 连续发送 100 帧，每帧 LEN 正确填充 */
void test_perf_100_frames_len_correct(void) {
    routes[0] = (cr_route_entry_t){.dest = 0x03, .next_hop = 0x02};
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 8,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = routes, .route_count = 1,
    };
    cr_init(&inst, &cfg, buffer, sizeof(buffer));

    static uint8_t perf_last_len;
    static int perf_send_count;
    static int perf_len_errors;
    perf_send_count = 0;
    perf_len_errors = 0;

    int perf_send(void *ctx, uint8_t next_hop, const uint8_t *data, uint16_t len) {
        (void)ctx; (void)next_hop;
        perf_send_count++;
        uint8_t expected_payload_len = (uint8_t)(len - 6);
        if (data[5] != expected_payload_len) {
            perf_len_errors++;
        }
        perf_last_len = data[5];
        return 0;
    }

    cr_hal_t phal = { .send = perf_send, .get_tick_ms = mock_get_tick, .hw_ctx = NULL };
    cr_set_hal(&inst, &phal);

    uint8_t payload[10];
    memset(payload, 0xEE, 10);

    for (int i = 0; i < 100; i++) {
        cr_send(&inst, 0x03, 0, payload, 10, NULL, NULL);
        mock_tick = (uint32_t)i;
        cr_poll(&inst);
    }

    TEST_ASSERT_EQUAL_INT(100, perf_send_count);
    TEST_ASSERT_EQUAL_INT(0, perf_len_errors);
    TEST_ASSERT_EQUAL_UINT8(10, perf_last_len);
}

/* [额外-集成] TX→RX 端到端：发送端填 LEN，接收端正确截取 */
void test_e2e_tx_rx_len_roundtrip(void) {
    /* TX 端发送 */
    setup_instance(0x01, 64, 0);
    uint8_t payload[] = "INTEGRATION";  /* 11 bytes */
    cr_send(&inst, 0x03, 0, payload, 11, NULL, NULL);
    cr_poll(&inst);
    TEST_ASSERT_EQUAL_INT(1, send_count);
    TEST_ASSERT_EQUAL_UINT8(11, sent_buf[5]);

    /* 模拟底层传输：固定长度 64 字节（padding 补齐） */
    uint8_t wire_frame[64];
    memset(wire_frame, 0x00, sizeof(wire_frame));
    memcpy(wire_frame, sent_buf, sent_len);
    /* 改 DST 为接收端地址（路由时 DST 是最终目的地） */
    /* sent_buf[0] 已经是 0x03（目标地址），接收端地址配置为 0x03 */

    /* RX 端接收 */
    cr_instance_t rx_inst;
    uint8_t rx_buffer[4096];
    cr_route_entry_t rx_routes[1] = {{.dest = 0x01, .next_hop = 0x01}};
    cr_config_t rx_cfg = {
        .local_addr = 0x03, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = rx_routes, .route_count = 1,
    };
    cr_init(&rx_inst, &rx_cfg, rx_buffer, sizeof(rx_buffer));
    cr_hal_t rx_hal = { .send = mock_send, .get_tick_ms = mock_get_tick, .hw_ctx = NULL };
    cr_set_hal(&rx_inst, &rx_hal);

    recv_count = 0;
    recv_len = 0;
    cr_set_recv_callback(&rx_inst, mock_recv, NULL);

    /* 底层传入完整 64 字节帧 */
    cr_feed_frame(&rx_inst, wire_frame, 64);

    /* 接收端应按 LEN=11 截取 */
    TEST_ASSERT_EQUAL_INT(1, recv_count);
    TEST_ASSERT_EQUAL_UINT16(11, recv_len);
    TEST_ASSERT_EQUAL_MEMORY("INTEGRATION", recv_payload, 11);
}

/* [补充-边界] 多帧组装中某帧 LEN 溢出 - 该帧被丢弃导致组装超时 */
void test_rx_multiframe_fragment_len_overflow(void) {
    setup_instance(0x02, 64, 0);
    cr_set_recv_callback(&inst, mock_recv, NULL);
    recv_count = 0;
    recv_len = 0;

    /* 第1帧正常：LEN=5 */
    uint8_t f1[11] = {0x02, 0x01, 0x20, 0x00, 0x03, 5, 'A', 'B', 'C', 'D', 'E'};
    cr_feed_frame(&inst, f1, sizeof(f1));

    /* 第2帧 LEN 溢出：帧总长=8 但 LEN 声称 50 */
    uint8_t f2[8] = {0x02, 0x01, 0x20, 0x01, 0x03, 50, 'X', 'Y'};
    cr_feed_frame(&inst, f2, sizeof(f2));

    /* 第2帧应被丢弃（LEN 校验失败），组装不完整，不应回调 */
    TEST_ASSERT_EQUAL_INT(0, recv_count);

    /* 超时后组装槽位应被清理（不导致内存泄漏） */
    mock_tick = 2000;  /* 超过 rx_assem_timeout_ms=1000 */
    cr_poll(&inst);
    TEST_ASSERT_EQUAL_INT(0, recv_count);
}

/* [补充-安全] 转发路径中 LEN 无效不导致崩溃 */
void test_forward_invalid_len_no_crash(void) {
    /* Node2(0x02) 转发一个 LEN 无效的帧到 Node3(0x03) */
    cr_route_entry_t fwd_routes[1] = {{.dest = 0x03, .next_hop = 0x03}};
    cr_config_t cfg = {
        .local_addr = 0x02, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = fwd_routes, .route_count = 1,
    };
    cr_init(&inst, &cfg, buffer, sizeof(buffer));
    hal = (cr_hal_t){ .send = mock_send, .get_tick_ms = mock_get_tick, .hw_ctx = NULL };
    cr_set_hal(&inst, &hal);
    send_count = 0;

    /* 帧 DST=0x03（非本机），LEN=200 远超帧实际大小 10 */
    uint8_t frame[10] = {0x03, 0x01, 0x00, 0x00, 0x03, 200, 'A', 'B', 'C', 'D'};
    cr_feed_frame(&inst, frame, sizeof(frame));

    /* 中间节点应透传（不校验 LEN），不崩溃 */
    TEST_ASSERT_EQUAL_INT(1, send_count);
    /* 帧内容完整转发 */
    TEST_ASSERT_EQUAL_UINT8(200, sent_buf[5]);
    TEST_ASSERT_EQUAL_UINT16(10, sent_len);
}

/* [补充-安全] 伪造 ACK 帧 LEN 溢出应被丢弃 */
void test_rx_forged_ack_len_overflow_drops(void) {
    setup_instance(0x02, 64, 1);
    cr_set_recv_callback(&inst, mock_recv, NULL);
    recv_count = 0;
    send_count = 0;

    /* 构造一个 ACK 帧，但 LEN 声称 50，帧总长只有 7 */
    uint8_t frame[7] = {0x02, 0x01, 0x80, 0x00, 0x03, 50, 'X'};
    cr_feed_frame(&inst, frame, sizeof(frame));

    /* LEN 校验失败，应丢弃，不处理 ACK 逻辑 */
    TEST_ASSERT_EQUAL_INT(0, recv_count);
    /* 也不应触发任何发送 */
    TEST_ASSERT_EQUAL_INT(0, send_count);
}

/* [额外-端到端] 多跳转发：中间节点透传帧（含 LEN），目标节点正确解析 */
void test_e2e_multihop_forward_preserves_len(void) {
    /* 拓扑: Node1(0x01) -> Node2(0x02) -> Node3(0x03)
     * Node1 发给 Node3，经 Node2 转发 */

    /* === Node1: 发送端 === */
    cr_route_entry_t routes1[1] = {{.dest = 0x03, .next_hop = 0x02}};
    cr_config_t cfg1 = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = routes1, .route_count = 1,
    };
    cr_instance_t inst1;
    uint8_t buf1[4096];
    cr_init(&inst1, &cfg1, buf1, sizeof(buf1));

    static uint8_t hop1_buf[64];
    static uint16_t hop1_len;
    int hop1_send(void *ctx, uint8_t next_hop, const uint8_t *data, uint16_t len) {
        (void)ctx; (void)next_hop;
        memcpy(hop1_buf, data, len);
        hop1_len = len;
        return 0;
    }
    cr_hal_t hal1 = { .send = hop1_send, .get_tick_ms = mock_get_tick, .hw_ctx = NULL };
    cr_set_hal(&inst1, &hal1);

    uint8_t payload[] = "MULTIHOP";  /* 8 bytes */
    cr_send(&inst1, 0x03, 0, payload, 8, NULL, NULL);
    cr_poll(&inst1);

    /* 验证 Node1 输出帧 LEN=8 */
    TEST_ASSERT_EQUAL_UINT8(8, hop1_buf[5]);
    TEST_ASSERT_EQUAL_UINT8(0x03, hop1_buf[0]);  /* DST = 最终目的地 */

    /* === Node2: 中间转发节点 === */
    cr_route_entry_t routes2[2] = {
        {.dest = 0x01, .next_hop = 0x01},
        {.dest = 0x03, .next_hop = 0x03},
    };
    cr_config_t cfg2 = {
        .local_addr = 0x02, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = routes2, .route_count = 2,
    };
    cr_instance_t inst2;
    uint8_t buf2[4096];
    cr_init(&inst2, &cfg2, buf2, sizeof(buf2));

    static uint8_t hop2_buf[64];
    static uint16_t hop2_len;
    int hop2_send(void *ctx, uint8_t next_hop, const uint8_t *data, uint16_t len) {
        (void)ctx; (void)next_hop;
        memcpy(hop2_buf, data, len);
        hop2_len = len;
        return 0;
    }
    cr_hal_t hal2 = { .send = hop2_send, .get_tick_ms = mock_get_tick, .hw_ctx = NULL };
    cr_set_hal(&inst2, &hal2);

    /* Node2 收到帧（DST=0x03，不是自己），应转发 */
    cr_feed_frame(&inst2, hop1_buf, hop1_len);

    /* 转发后帧应完整保留 LEN 字段 */
    TEST_ASSERT_EQUAL_UINT8(8, hop2_buf[5]);      /* LEN 未被篡改 */
    TEST_ASSERT_EQUAL_UINT16(hop1_len, hop2_len); /* 帧长度不变 */

    /* === Node3: 最终接收端 === */
    cr_route_entry_t routes3[1] = {{.dest = 0x01, .next_hop = 0x02}};
    cr_config_t cfg3 = {
        .local_addr = 0x03, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = routes3, .route_count = 1,
    };
    cr_instance_t inst3;
    uint8_t buf3[4096];
    cr_init(&inst3, &cfg3, buf3, sizeof(buf3));
    cr_hal_t hal3 = { .send = mock_send, .get_tick_ms = mock_get_tick, .hw_ctx = NULL };
    cr_set_hal(&inst3, &hal3);

    recv_count = 0;
    recv_len = 0;
    cr_set_recv_callback(&inst3, mock_recv, NULL);

    /* Node3 收到转发的帧 */
    cr_feed_frame(&inst3, hop2_buf, hop2_len);

    /* Node3 应正确按 LEN=8 截取 payload */
    TEST_ASSERT_EQUAL_INT(1, recv_count);
    TEST_ASSERT_EQUAL_UINT16(8, recv_len);
    TEST_ASSERT_EQUAL_MEMORY("MULTIHOP", recv_payload, 8);
}
