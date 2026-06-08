#include "unity.h"
#include "comm_route.h"
#include <string.h>

/* ===== Mock HAL ===== */
static uint8_t ex_sent_buf[256];
static uint16_t ex_sent_len;
static int ex_send_count;
static uint8_t ex_sent_next_hop;

static int ex_mock_send(void *ctx, uint8_t next_hop, const uint8_t *data, uint16_t len) {
    (void)ctx;
    ex_sent_next_hop = next_hop;
    if (len <= sizeof(ex_sent_buf))
        memcpy(ex_sent_buf, data, len);
    ex_sent_len = len;
    ex_send_count++;
    return 0;
}

static uint32_t ex_mock_tick = 0;
static uint32_t ex_mock_get_tick(void) { return ex_mock_tick; }

/* Multi-frame send history */
static uint8_t ex_send_history[16][256];
static uint16_t ex_send_history_len[16];
static int ex_send_history_count;

static int ex_mock_send_history(void *ctx, uint8_t next_hop, const uint8_t *data, uint16_t len) {
    (void)ctx; (void)next_hop;
    if (ex_send_history_count < 16 && len <= 256) {
        memcpy(ex_send_history[ex_send_history_count], data, len);
        ex_send_history_len[ex_send_history_count] = len;
    }
    ex_send_history_count++;
    return 0;
}

/* Recv callback */
static uint8_t ex_recv_src;
static uint8_t ex_recv_biz_id;
static uint8_t ex_recv_data[512];
static uint16_t ex_recv_len;
static int ex_recv_called;

static void ex_mock_recv_cb(cr_instance_t *inst, uint8_t src, uint8_t biz_id,
                            const uint8_t *data, uint16_t len, void *ctx) {
    (void)inst; (void)ctx;
    ex_recv_src = src;
    ex_recv_biz_id = biz_id;
    if (len <= sizeof(ex_recv_data))
        memcpy(ex_recv_data, data, len);
    ex_recv_len = len;
    ex_recv_called++;
}

/* Complete callback */
static uint8_t ex_complete_status;
static int ex_complete_called;
static void ex_on_complete(uint8_t status, void *ctx) {
    (void)ctx;
    ex_complete_status = status;
    ex_complete_called++;
}

/* ===== Helper: setup instance ===== */
static cr_instance_t ex_inst;
static uint8_t ex_buffer[4096];
static cr_hal_t ex_hal;

static void ex_setup(uint8_t addr, uint16_t mtu, uint8_t ack_enabled,
                     cr_ack_mode_t ack_mode,
                     const cr_route_entry_t *routes, uint8_t route_count) {
    cr_config_t cfg = {
        .local_addr = addr, .mtu = mtu, .frame_interval_ms = 0,
        .max_retries = 3, .ack_timeout_ms = 100,
        .ack_enabled = ack_enabled, .ack_mode = ack_mode,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .tx_buf_per_slot = 256, .bcast_queue_depth = 4,
        .route_table = routes, .route_count = route_count,
    };
    cr_init(&ex_inst, &cfg, ex_buffer, sizeof(ex_buffer));
    ex_hal.send = ex_mock_send;
    ex_hal.get_tick_ms = ex_mock_get_tick;
    ex_hal.hw_ctx = NULL;
    cr_set_hal(&ex_inst, &ex_hal);
    cr_set_recv_callback(&ex_inst, ex_mock_recv_cb, NULL);
    ex_mock_tick = 0;
    ex_send_count = 0;
    ex_sent_len = 0;
    ex_recv_called = 0;
    ex_recv_len = 0;
    ex_complete_called = 0;
    ex_send_history_count = 0;
}

/* ==================== 边界情况 ==================== */

void test_send_zero_length_data(void) {
    ex_setup(0x01, 64, 0, CR_ACK_MODE_REPLY, NULL, 0);

    int ret = cr_send(&ex_inst, 0x02, 1, NULL, 0, ex_on_complete, NULL);
    TEST_ASSERT_EQUAL_INT(0, ret);

    cr_poll(&ex_inst);

    TEST_ASSERT_EQUAL_INT(1, ex_send_count);
    /* Frame: 5-byte header + 0 payload */
    TEST_ASSERT_EQUAL_UINT16(5, ex_sent_len);
    /* Complete should be called (ack disabled, fire-and-forget) */
    TEST_ASSERT_EQUAL_INT(1, ex_complete_called);
    TEST_ASSERT_EQUAL_UINT8(0, ex_complete_status);
}

void test_broadcast_zero_length(void) {
    ex_setup(0x01, 64, 0, CR_ACK_MODE_REPLY, NULL, 0);

    int ret = cr_broadcast(&ex_inst, 2, NULL, 0);
    TEST_ASSERT_EQUAL_INT(0, ret);

    cr_poll(&ex_inst);

    TEST_ASSERT_EQUAL_INT(1, ex_send_count);
    /* Broadcast frame: 5-byte header + 0 payload */
    TEST_ASSERT_EQUAL_UINT16(5, ex_sent_len);
    /* CTL: broadcast bit set */
    TEST_ASSERT_BITS(0x40, 0x40, ex_sent_buf[2]);
}

void test_send_exactly_mtu(void) {
    ex_setup(0x01, 8, 0, CR_ACK_MODE_REPLY, NULL, 0);
    ex_hal.send = ex_mock_send_history;
    cr_set_hal(&ex_inst, &ex_hal);

    uint8_t data[8] = {1,2,3,4,5,6,7,8};
    cr_send(&ex_inst, 0x02, 0, data, 8, NULL, NULL);
    cr_poll(&ex_inst);

    /* Exactly MTU → single frame, no fragmentation */
    TEST_ASSERT_EQUAL_INT(1, ex_send_history_count);
    TEST_ASSERT_EQUAL_UINT16(5 + 8, ex_send_history_len[0]);
    /* CTL: no FRAG bit */
    TEST_ASSERT_BITS(0x20, 0x00, ex_send_history[0][2]);
}

void test_send_mtu_plus_one(void) {
    ex_setup(0x01, 8, 0, CR_ACK_MODE_REPLY, NULL, 0);
    ex_hal.send = ex_mock_send_history;
    cr_set_hal(&ex_inst, &ex_hal);

    uint8_t data[9] = {1,2,3,4,5,6,7,8,9};
    cr_send(&ex_inst, 0x02, 0, data, 9, NULL, NULL);
    cr_poll(&ex_inst);
    cr_poll(&ex_inst);

    /* MTU+1 → 2 frames */
    TEST_ASSERT_EQUAL_INT(2, ex_send_history_count);
    /* Frame 1: 5 header + 8 payload */
    TEST_ASSERT_EQUAL_UINT16(13, ex_send_history_len[0]);
    /* Frame 2: 5 header + 1 payload (last frag) */
    TEST_ASSERT_EQUAL_UINT16(6, ex_send_history_len[1]);
    /* Frame 1: FRAG=1, LAST=0 */
    TEST_ASSERT_BITS(0x20, 0x20, ex_send_history[0][2]);
    TEST_ASSERT_BITS(0x10, 0x00, ex_send_history[0][2]);
    /* Frame 2: FRAG=1, LAST=1 */
    TEST_ASSERT_BITS(0x30, 0x30, ex_send_history[1][2]);
}

void test_seq_counter_wraps(void) {
    ex_setup(0x01, 64, 0, CR_ACK_MODE_REPLY, NULL, 0);
    ex_hal.send = ex_mock_send_history;
    cr_set_hal(&ex_inst, &ex_hal);

    /* Send 256 times to wrap seq_counter from 0 back to 0 */
    uint8_t data = 0xAA;
    for (int i = 0; i < 256; i++) {
        /* Need to drain queue each time since depth=4 */
        cr_send(&ex_inst, 0x02, 0, &data, 1, NULL, NULL);
        cr_poll(&ex_inst);
    }

    /* The 256th send used seq=255. Next send should use seq=0 */
    ex_send_history_count = 0;
    cr_send(&ex_inst, 0x02, 0, &data, 1, NULL, NULL);
    cr_poll(&ex_inst);
    /* SEQ field is at offset 3 */
    TEST_ASSERT_EQUAL_UINT8(0, ex_send_history[0][3]);
}

/* ==================== 异常路径 ==================== */

void test_feed_frame_too_short(void) {
    ex_setup(0x02, 64, 0, CR_ACK_MODE_REPLY, NULL, 0);

    /* Frame shorter than 5 bytes should be silently ignored */
    uint8_t short_frame[] = {0x02, 0x01, 0x00, 0x00}; /* 4 bytes */
    cr_feed_frame(&ex_inst, short_frame, 4);

    TEST_ASSERT_EQUAL_INT(0, ex_recv_called);
    TEST_ASSERT_EQUAL_INT(0, ex_send_count);
}

void test_feed_frame_exactly_header(void) {
    ex_setup(0x02, 64, 0, CR_ACK_MODE_REPLY, NULL, 0);

    /* Frame exactly 5 bytes (header only, 0 payload) — should deliver with len=0 */
    uint8_t frame[] = {0x02, 0x01, 0x00, 0x00, 0x03};
    cr_feed_frame(&ex_inst, frame, 5);

    TEST_ASSERT_EQUAL_INT(1, ex_recv_called);
    TEST_ASSERT_EQUAL_UINT8(0x01, ex_recv_src);
    TEST_ASSERT_EQUAL_UINT16(0, ex_recv_len);
}

void test_rx_out_of_order_frame_dropped(void) {
    ex_setup(0x02, 64, 0, CR_ACK_MODE_REPLY, NULL, 0);

    /* Fragment with seq=0 (first, allocates slot) */
    uint8_t f0[] = {0x02, 0x01, 0x20, 0x00, 0x03, 1, 2, 3};
    cr_feed_frame(&ex_inst, f0, sizeof(f0));
    TEST_ASSERT_EQUAL_INT(0, ex_recv_called);

    /* Skip seq=1, send seq=2 (out of order, > expected) → dropped */
    uint8_t f2[] = {0x02, 0x01, 0x30, 0x02, 0x03, 7, 8, 9};
    cr_feed_frame(&ex_inst, f2, sizeof(f2));
    TEST_ASSERT_EQUAL_INT(0, ex_recv_called);

    /* Now send correct seq=1 with LAST → should deliver (only f0 + f1 data) */
    uint8_t f1[] = {0x02, 0x01, 0x30, 0x01, 0x03, 4, 5, 6};
    cr_feed_frame(&ex_inst, f1, sizeof(f1));
    TEST_ASSERT_EQUAL_INT(1, ex_recv_called);
    TEST_ASSERT_EQUAL_UINT16(6, ex_recv_len); /* f0(3) + f1(3) */
}

void test_ack_wrong_seq_ignored(void) {
    cr_route_entry_t routes[] = { {.dest = 0x02, .next_hop = 0x02} };
    ex_setup(0x01, 64, 1, CR_ACK_MODE_REPLY, routes, 1);

    uint8_t data[] = {0xAA};
    cr_send(&ex_inst, 0x02, 0, data, 1, ex_on_complete, NULL);
    cr_poll(&ex_inst); /* sends frame, enters WAIT_ACK */

    /* Construct ACK with wrong SEQ */
    uint8_t ack_frame[] = {0x01, 0x02, 0x80, 0xFF, 0x03}; /* SEQ=0xFF, not 0 */
    cr_feed_frame(&ex_inst, ack_frame, sizeof(ack_frame));

    /* Should NOT complete */
    TEST_ASSERT_EQUAL_INT(0, ex_complete_called);

    /* Now send correct ACK with SEQ=0 */
    uint8_t ack_ok[] = {0x01, 0x02, 0x80, 0x00, 0x03};
    cr_feed_frame(&ex_inst, ack_ok, sizeof(ack_ok));
    TEST_ASSERT_EQUAL_INT(1, ex_complete_called);
    TEST_ASSERT_EQUAL_UINT8(0, ex_complete_status);
}

void test_poll_no_task_no_crash(void) {
    ex_setup(0x01, 64, 0, CR_ACK_MODE_REPLY, NULL, 0);

    /* Poll with empty queue — should not crash or send anything */
    cr_poll(&ex_inst);
    cr_poll(&ex_inst);
    cr_poll(&ex_inst);

    TEST_ASSERT_EQUAL_INT(0, ex_send_count);
}

void test_feed_frame_no_hal_set(void) {
    /* Init instance but do NOT set HAL */
    cr_config_t cfg = {
        .local_addr = 0x02, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 1, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .tx_buf_per_slot = 256, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cr_instance_t inst_no_hal;
    uint8_t buf[4096];
    cr_init(&inst_no_hal, &cfg, buf, sizeof(buf));
    /* Intentionally NOT calling cr_set_hal */

    ex_recv_called = 0;
    cr_set_recv_callback(&inst_no_hal, ex_mock_recv_cb, NULL);

    /* With ack_enabled=1 and no HAL, cr_feed_frame should return early
     * without crashing (NULL guard). No delivery expected. */
    uint8_t frame[] = {0x02, 0x01, 0x00, 0x00, 0x03, 'X'};
    cr_feed_frame(&inst_no_hal, frame, sizeof(frame));

    TEST_ASSERT_EQUAL_INT(0, ex_recv_called);

    /* Also verify cr_poll doesn't crash with no HAL */
    cr_poll(&inst_no_hal);
}

void test_interrupt_mode_no_timeout(void) {
    cr_route_entry_t routes[] = { {.dest = 0x02, .next_hop = 0x02} };
    ex_setup(0x01, 64, 1, CR_ACK_MODE_INTERRUPT, routes, 1);

    uint8_t data[] = {0xBB};
    cr_send(&ex_inst, 0x02, 0, data, 1, ex_on_complete, NULL);
    cr_poll(&ex_inst); /* sends frame, enters WAIT_ACK */

    /* Advance time way past ack_timeout */
    ex_mock_tick = 5000;
    cr_poll(&ex_inst);

    /* In interrupt mode, timeout does NOT trigger retransmit or failure */
    TEST_ASSERT_EQUAL_INT(0, ex_complete_called);
    TEST_ASSERT_EQUAL_INT(1, ex_send_count); /* only the initial send */

    /* Confirm it completes via notify_send_done */
    cr_notify_send_done(&ex_inst);
    cr_poll(&ex_inst);
    TEST_ASSERT_EQUAL_INT(1, ex_complete_called);
    TEST_ASSERT_EQUAL_UINT8(0, ex_complete_status);
}

/* ==================== 安全/防御 ==================== */

void test_init_null_instance(void) {
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_buf_per_slot = 256,
        .tx_buf_per_slot = 256, .bcast_queue_depth = 4,
    };
    uint8_t buf[4096];
    int ret = cr_init(NULL, &cfg, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-2, ret);
}

void test_init_null_config(void) {
    cr_instance_t inst;
    uint8_t buf[4096];
    int ret = cr_init(&inst, NULL, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-2, ret);
}

void test_init_null_buffer(void) {
    cr_instance_t inst;
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_buf_per_slot = 256,
        .tx_buf_per_slot = 256, .bcast_queue_depth = 4,
    };
    int ret = cr_init(&inst, &cfg, NULL, 1024);
    TEST_ASSERT_EQUAL_INT(-2, ret);
}

void test_broadcast_busy_returns_error(void) {
    /* Use bcast_queue_depth=1 so queue fills after one broadcast */
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .max_retries = 3, .ack_timeout_ms = 100,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .tx_buf_per_slot = 256, .bcast_queue_depth = 1,
        .route_table = NULL, .route_count = 0,
    };
    cr_init(&ex_inst, &cfg, ex_buffer, sizeof(ex_buffer));
    ex_hal.send = ex_mock_send;
    ex_hal.get_tick_ms = ex_mock_get_tick;
    ex_hal.hw_ctx = NULL;
    cr_set_hal(&ex_inst, &ex_hal);
    ex_mock_tick = 0;
    ex_send_count = 0;

    uint8_t data[] = {1, 2, 3};
    /* First broadcast succeeds (queue has 1 slot) */
    int ret = cr_broadcast(&ex_inst, 0, data, 3);
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Second broadcast → queue full, should return -1 */
    ret = cr_broadcast(&ex_inst, 0, data, 3);
    TEST_ASSERT_EQUAL_INT(-1, ret);
}

void test_rx_buffer_overflow_drops_slot(void) {
    /* Setup with very small rx_buf_per_slot */
    cr_config_t cfg = {
        .local_addr = 0x02, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 10, /* only 10 bytes! */
        .tx_buf_per_slot = 256, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cr_instance_t inst;
    uint8_t buf[4096];
    cr_init(&inst, &cfg, buf, sizeof(buf));
    cr_hal_t hal = { .send = ex_mock_send, .get_tick_ms = ex_mock_get_tick, .hw_ctx = NULL };
    cr_set_hal(&inst, &hal);
    cr_set_recv_callback(&inst, ex_mock_recv_cb, NULL);
    ex_recv_called = 0;

    /* First frag: 8 bytes payload → fits in 10 */
    uint8_t f0[] = {0x02, 0x01, 0x20, 0x00, 0x03, 1,2,3,4,5,6,7,8};
    cr_feed_frame(&inst, f0, sizeof(f0));
    TEST_ASSERT_EQUAL_INT(0, ex_recv_called);

    /* Second frag: 8 bytes → total would be 16, exceeds 10 → slot dropped */
    uint8_t f1[] = {0x02, 0x01, 0x30, 0x01, 0x03, 9,10,11,12,13,14,15,16};
    cr_feed_frame(&inst, f1, sizeof(f1));

    /* Should NOT deliver (slot was discarded due to overflow) */
    TEST_ASSERT_EQUAL_INT(0, ex_recv_called);
}

/* ==================== 集成/端到端 ==================== */

/* Multi-node mock infrastructure */
static uint8_t node_a_buf[256], node_b_buf[256], node_c_buf[256];
static uint16_t node_a_len, node_b_len, node_c_len;
static int node_a_send_count, node_b_send_count, node_c_send_count;

static int node_a_send(void *ctx, uint8_t nh, const uint8_t *d, uint16_t l) {
    (void)ctx; (void)nh; memcpy(node_a_buf, d, l); node_a_len = l; node_a_send_count++; return 0;
}
static int node_b_send(void *ctx, uint8_t nh, const uint8_t *d, uint16_t l) {
    (void)ctx; (void)nh; memcpy(node_b_buf, d, l); node_b_len = l; node_b_send_count++; return 0;
}
static int node_c_send(void *ctx, uint8_t nh, const uint8_t *d, uint16_t l) {
    (void)ctx; (void)nh; memcpy(node_c_buf, d, l); node_c_len = l; node_c_send_count++; return 0;
}

static uint32_t node_tick = 0;
static uint32_t node_get_tick(void) { return node_tick; }

static int node_c_recv_called;
static void node_c_recv_cb(cr_instance_t *inst, uint8_t src, uint8_t biz_id,
                           const uint8_t *data, uint16_t len, void *ctx) {
    (void)inst; (void)src; (void)biz_id; (void)data; (void)len; (void)ctx;
    node_c_recv_called++;
}

void test_multihop_intermediate_no_consume_ack(void) {
    /* A(0x01) → B(0x02) → C(0x03): B should forward ACK from C to A */
    node_tick = 0;
    node_a_send_count = 0; node_b_send_count = 0; node_c_send_count = 0;

    /* Node A */
    uint8_t bufA[4096];
    cr_route_entry_t rA[] = { {.dest = 0x03, .next_hop = 0x02} };
    cr_config_t cfgA = { .local_addr = 0x01, .mtu = 32, .frame_interval_ms = 0,
        .max_retries = 3, .ack_timeout_ms = 100, .ack_enabled = 1,
        .ack_mode = CR_ACK_MODE_REPLY, .default_ttl = 3,
        .tx_queue_depth = 4, .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .tx_buf_per_slot = 256, .bcast_queue_depth = 4,
        .route_table = rA, .route_count = 1 };
    cr_instance_t instA;
    cr_init(&instA, &cfgA, bufA, sizeof(bufA));
    cr_hal_t halA = { .send = node_a_send, .get_tick_ms = node_get_tick, .hw_ctx = NULL };
    cr_set_hal(&instA, &halA);

    /* Node B (intermediate) */
    uint8_t bufB[4096];
    cr_route_entry_t rB[] = { {.dest = 0x01, .next_hop = 0x01}, {.dest = 0x03, .next_hop = 0x03} };
    cr_config_t cfgB = { .local_addr = 0x02, .mtu = 32, .frame_interval_ms = 0,
        .max_retries = 3, .ack_timeout_ms = 100, .ack_enabled = 1,
        .ack_mode = CR_ACK_MODE_REPLY, .default_ttl = 3,
        .tx_queue_depth = 4, .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .tx_buf_per_slot = 256, .bcast_queue_depth = 4,
        .route_table = rB, .route_count = 2 };
    cr_instance_t instB;
    cr_init(&instB, &cfgB, bufB, sizeof(bufB));
    cr_hal_t halB = { .send = node_b_send, .get_tick_ms = node_get_tick, .hw_ctx = NULL };
    cr_set_hal(&instB, &halB);

    /* Node C (receiver) */
    uint8_t bufC[4096];
    cr_route_entry_t rC[] = { {.dest = 0x01, .next_hop = 0x02} };
    cr_config_t cfgC = { .local_addr = 0x03, .mtu = 32, .frame_interval_ms = 0,
        .max_retries = 3, .ack_timeout_ms = 100, .ack_enabled = 1,
        .ack_mode = CR_ACK_MODE_REPLY, .default_ttl = 3,
        .tx_queue_depth = 4, .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .tx_buf_per_slot = 256, .bcast_queue_depth = 4,
        .route_table = rC, .route_count = 1 };
    cr_instance_t instC;
    cr_init(&instC, &cfgC, bufC, sizeof(bufC));
    cr_hal_t halC = { .send = node_c_send, .get_tick_ms = node_get_tick, .hw_ctx = NULL };
    cr_set_hal(&instC, &halC);

    /* A sends to C */
    ex_complete_called = 0;
    uint8_t data[] = {0xDE, 0xAD};
    cr_send(&instA, 0x03, 0, data, 2, ex_on_complete, NULL);
    cr_poll(&instA); /* A → node_a_buf */

    /* B receives data frame (DST=0x03 != 0x02) → forwards */
    node_b_send_count = 0;
    cr_feed_frame(&instB, node_a_buf, node_a_len);
    TEST_ASSERT_EQUAL_INT(1, node_b_send_count); /* B forwarded data */

    /* C receives data → sends ACK back to A (DST=0x01) */
    node_c_send_count = 0;
    cr_feed_frame(&instC, node_b_buf, node_b_len);
    TEST_ASSERT_EQUAL_INT(1, node_c_send_count);
    /* Verify it's an ACK with DST=0x01 */
    TEST_ASSERT_EQUAL_UINT8(0x01, node_c_buf[0]);
    TEST_ASSERT_BITS(0x80, 0x80, node_c_buf[2]);

    /* B receives ACK (DST=0x01 != 0x02) → should forward, NOT consume */
    node_b_send_count = 0;
    cr_feed_frame(&instB, node_c_buf, node_c_len);
    TEST_ASSERT_EQUAL_INT(1, node_b_send_count); /* B forwarded ACK */

    /* B's tx_queue should still be empty (no task consumed by B) */
    /* Verify: B didn't trigger any on_complete since it has no active task */

    /* A receives forwarded ACK → completes */
    cr_feed_frame(&instA, node_b_buf, node_b_len);
    TEST_ASSERT_EQUAL_INT(1, ex_complete_called);
    TEST_ASSERT_EQUAL_UINT8(0, ex_complete_status);
}

void test_star_topology_routing(void) {
    /* Star: Center=0x01, Endpoints=0x02,0x03,0x04
     * Center routes all endpoints directly. Send from 0x02 to 0x04 via center. */
    node_tick = 0;

    /* Center (0x01) */
    uint8_t bufCenter[4096];
    cr_route_entry_t rCenter[] = {
        {.dest = 0x02, .next_hop = 0x02},
        {.dest = 0x03, .next_hop = 0x03},
        {.dest = 0x04, .next_hop = 0x04},
    };
    cr_config_t cfgCenter = { .local_addr = 0x01, .mtu = 32, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY, .default_ttl = 3,
        .tx_queue_depth = 4, .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .tx_buf_per_slot = 256, .bcast_queue_depth = 4,
        .route_table = rCenter, .route_count = 3 };
    cr_instance_t instCenter;
    cr_init(&instCenter, &cfgCenter, bufCenter, sizeof(bufCenter));
    cr_hal_t halCenter = { .send = node_b_send, .get_tick_ms = node_get_tick, .hw_ctx = NULL };
    cr_set_hal(&instCenter, &halCenter);

    /* Endpoint 0x02 sends data to 0x04 */
    uint8_t bufE2[4096];
    cr_route_entry_t rE2[] = { {.dest = 0x04, .next_hop = 0x01} }; /* via center */
    cr_config_t cfgE2 = { .local_addr = 0x02, .mtu = 32, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY, .default_ttl = 3,
        .tx_queue_depth = 4, .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .tx_buf_per_slot = 256, .bcast_queue_depth = 4,
        .route_table = rE2, .route_count = 1 };
    cr_instance_t instE2;
    cr_init(&instE2, &cfgE2, bufE2, sizeof(bufE2));
    cr_hal_t halE2 = { .send = node_a_send, .get_tick_ms = node_get_tick, .hw_ctx = NULL };
    cr_set_hal(&instE2, &halE2);

    /* Endpoint 0x04 receives */
    uint8_t bufE4[4096];
    cr_route_entry_t rE4[] = { {.dest = 0x02, .next_hop = 0x01} };
    cr_config_t cfgE4 = { .local_addr = 0x04, .mtu = 32, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY, .default_ttl = 3,
        .tx_queue_depth = 4, .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .tx_buf_per_slot = 256, .bcast_queue_depth = 4,
        .route_table = rE4, .route_count = 1 };
    cr_instance_t instE4;
    cr_init(&instE4, &cfgE4, bufE4, sizeof(bufE4));
    cr_hal_t halE4 = { .send = node_c_send, .get_tick_ms = node_get_tick, .hw_ctx = NULL };
    cr_set_hal(&instE4, &halE4);
    node_c_recv_called = 0;
    cr_set_recv_callback(&instE4, node_c_recv_cb, NULL);

    /* E2 sends to E4 */
    uint8_t payload[] = {0xCA, 0xFE};
    cr_send(&instE2, 0x04, 5, payload, 2, NULL, NULL);
    cr_poll(&instE2); /* E2 → node_a_buf */

    /* Verify frame: DST=0x04, SRC=0x02 */
    TEST_ASSERT_EQUAL_UINT8(0x04, node_a_buf[0]);
    TEST_ASSERT_EQUAL_UINT8(0x02, node_a_buf[1]);

    /* Center receives (DST=0x04 != 0x01) → forwards to next_hop=0x04 */
    node_b_send_count = 0;
    cr_feed_frame(&instCenter, node_a_buf, node_a_len);
    TEST_ASSERT_EQUAL_INT(1, node_b_send_count);

    /* E4 receives → should deliver */
    cr_feed_frame(&instE4, node_b_buf, node_b_len);
    TEST_ASSERT_EQUAL_INT(1, node_c_recv_called);
}

/* ==================== 压力 ==================== */

void test_dedup_table_ring_overwrite(void) {
    /* dedup_table_size = 4 (small). Fill 4 entries, then the 5th should overwrite [0]. */
    cr_config_t cfg = {
        .local_addr = 0x02, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 4,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .tx_buf_per_slot = 256, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cr_instance_t inst;
    uint8_t buf[4096];
    cr_init(&inst, &cfg, buf, sizeof(buf));
    cr_hal_t hal = { .send = ex_mock_send, .get_tick_ms = ex_mock_get_tick, .hw_ctx = NULL };
    cr_set_hal(&inst, &hal);
    ex_recv_called = 0;
    cr_set_recv_callback(&inst, ex_mock_recv_cb, NULL);

    /* Send 4 different broadcast frames (fills dedup table) */
    for (uint8_t i = 0; i < 4; i++) {
        /* DST=0xFF(broadcast), SRC=0x10+i, CTL=0x40(bcast), SEQ=i, TTL=0(no fwd) */
        uint8_t frame[] = {0xFF, (uint8_t)(0x10 + i), 0x40, i, 0x00, 0xAA};
        cr_feed_frame(&inst, frame, sizeof(frame));
    }
    TEST_ASSERT_EQUAL_INT(4, ex_recv_called);

    /* Re-send first broadcast (src=0x10, seq=0) → should be deduped */
    ex_recv_called = 0;
    uint8_t dup_frame[] = {0xFF, 0x10, 0x40, 0x00, 0x00, 0xAA};
    cr_feed_frame(&inst, dup_frame, sizeof(dup_frame));
    TEST_ASSERT_EQUAL_INT(0, ex_recv_called); /* still deduped */

    /* Send a 5th unique broadcast (overwrites slot[0] which held src=0x10,seq=0) */
    uint8_t frame5[] = {0xFF, 0x20, 0x40, 0x00, 0x00, 0xBB};
    cr_feed_frame(&inst, frame5, sizeof(frame5));
    TEST_ASSERT_EQUAL_INT(1, ex_recv_called); /* new, delivered */

    /* Now the original (src=0x10, seq=0) was overwritten. Re-send it → delivered again */
    ex_recv_called = 0;
    cr_feed_frame(&inst, dup_frame, sizeof(dup_frame));
    TEST_ASSERT_EQUAL_INT(1, ex_recv_called); /* no longer in dedup table! */
}

/* ==================== 深拷贝 & 广播队列 ==================== */

void test_send_deep_copy(void) {
    ex_setup(0x01, 64, 0, CR_ACK_MODE_REPLY, NULL, 0);
    ex_hal.send = ex_mock_send_history;
    cr_set_hal(&ex_inst, &ex_hal);
    ex_send_history_count = 0;

    uint8_t data[] = "ORIGINAL";
    int ret = cr_send(&ex_inst, 0x02, 0, data, 8, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Modify data after cr_send — deep copy should protect */
    memcpy(data, "MODIFIED", 8);

    cr_poll(&ex_inst);

    TEST_ASSERT_EQUAL_INT(1, ex_send_history_count);
    /* Payload starts at offset 5 (after 5-byte header) */
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)"ORIGINAL", &ex_send_history[0][5], 8);
}

void test_broadcast_deep_copy(void) {
    ex_setup(0x01, 64, 0, CR_ACK_MODE_REPLY, NULL, 0);
    ex_hal.send = ex_mock_send_history;
    cr_set_hal(&ex_inst, &ex_hal);
    ex_send_history_count = 0;

    uint8_t data[] = "BCAST1";
    int ret = cr_broadcast(&ex_inst, 0, data, 6);
    TEST_ASSERT_EQUAL_INT(0, ret);

    /* Modify data after cr_broadcast */
    memcpy(data, "XXXXXX", 6);

    cr_poll(&ex_inst);

    TEST_ASSERT_EQUAL_INT(1, ex_send_history_count);
    /* Payload starts at offset 5 */
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)"BCAST1", &ex_send_history[0][5], 6);
}

void test_broadcast_queue_multiple(void) {
    ex_setup(0x01, 64, 0, CR_ACK_MODE_REPLY, NULL, 0);
    ex_hal.send = ex_mock_send_history;
    cr_set_hal(&ex_inst, &ex_hal);
    ex_send_history_count = 0;

    uint8_t d1[] = "AAA";
    uint8_t d2[] = "BBB";
    uint8_t d3[] = "CCC";

    /* Enqueue 3 broadcasts (bcast_queue_depth=4 in ex_setup) */
    TEST_ASSERT_EQUAL_INT(0, cr_broadcast(&ex_inst, 0, d1, 3));
    TEST_ASSERT_EQUAL_INT(0, cr_broadcast(&ex_inst, 0, d2, 3));
    TEST_ASSERT_EQUAL_INT(0, cr_broadcast(&ex_inst, 0, d3, 3));

    /* Poll 3 times to send all */
    cr_poll(&ex_inst);
    cr_poll(&ex_inst);
    cr_poll(&ex_inst);

    TEST_ASSERT_TRUE(ex_send_history_count >= 3);

    /* Verify FIFO order: payload at offset 5 */
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)"AAA", &ex_send_history[0][5], 3);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)"BBB", &ex_send_history[1][5], 3);
    TEST_ASSERT_EQUAL_UINT8_ARRAY((const uint8_t *)"CCC", &ex_send_history[2][5], 3);
}

void test_broadcast_queue_full(void) {
    /* bcast_queue_depth=2 */
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .max_retries = 3, .ack_timeout_ms = 100,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .tx_buf_per_slot = 256, .bcast_queue_depth = 2,
        .route_table = NULL, .route_count = 0,
    };
    cr_init(&ex_inst, &cfg, ex_buffer, sizeof(ex_buffer));
    ex_hal.send = ex_mock_send;
    ex_hal.get_tick_ms = ex_mock_get_tick;
    ex_hal.hw_ctx = NULL;
    cr_set_hal(&ex_inst, &ex_hal);
    ex_mock_tick = 0;
    ex_send_count = 0;

    uint8_t data[] = {1, 2, 3};
    TEST_ASSERT_EQUAL_INT(0, cr_broadcast(&ex_inst, 0, data, 3));
    TEST_ASSERT_EQUAL_INT(0, cr_broadcast(&ex_inst, 0, data, 3));
    /* Third should fail — queue full */
    TEST_ASSERT_EQUAL_INT(-1, cr_broadcast(&ex_inst, 0, data, 3));
}

void test_send_exceeds_tx_buf(void) {
    /* tx_buf_per_slot=10 */
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .max_retries = 3, .ack_timeout_ms = 100,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .tx_buf_per_slot = 10, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cr_init(&ex_inst, &cfg, ex_buffer, sizeof(ex_buffer));
    ex_hal.send = ex_mock_send;
    ex_hal.get_tick_ms = ex_mock_get_tick;
    ex_hal.hw_ctx = NULL;
    cr_set_hal(&ex_inst, &ex_hal);
    ex_mock_tick = 0;
    ex_send_count = 0;

    uint8_t data[20] = {0};
    int ret = cr_send(&ex_inst, 0x02, 0, data, 20, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(-2, ret);
}
