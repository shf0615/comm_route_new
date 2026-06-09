#include "unity.h"
#include "comm_route.h"
#include <string.h>

/* ===== Mock HAL ===== */
static uint8_t cov_sent_buf[512];
static uint16_t cov_sent_len;
static int cov_send_count;
static uint8_t cov_sent_next_hop;
static int cov_hal_send_retval;

static int cov_mock_send(void *ctx, uint8_t next_hop, const uint8_t *data, uint16_t len) {
    (void)ctx;
    cov_sent_next_hop = next_hop;
    if (len <= sizeof(cov_sent_buf))
        memcpy(cov_sent_buf, data, len);
    cov_sent_len = len;
    cov_send_count++;
    return cov_hal_send_retval;
}

static uint32_t cov_mock_tick = 0;
static uint32_t cov_mock_get_tick(void) { return cov_mock_tick; }

/* Multi-frame send history */
static uint8_t cov_send_history[32][512];
static uint16_t cov_send_history_len[32];
static int cov_send_history_count;

static int cov_mock_send_history(void *ctx, uint8_t next_hop, const uint8_t *data, uint16_t len) {
    (void)ctx; (void)next_hop;
    if (cov_send_history_count < 32 && len <= 512) {
        memcpy(cov_send_history[cov_send_history_count], data, len);
        cov_send_history_len[cov_send_history_count] = len;
    }
    cov_send_history_count++;
    return cov_hal_send_retval;
}

/* Recv callback */
static uint8_t cov_recv_src;
static uint8_t cov_recv_biz_id;
static uint8_t cov_recv_data[512];
static uint16_t cov_recv_len;
static int cov_recv_called;

static void cov_mock_recv_cb(cr_instance_t *inst, uint8_t src, uint8_t biz_id,
                             const uint8_t *data, uint16_t len, void *ctx) {
    (void)inst; (void)ctx;
    cov_recv_src = src;
    cov_recv_biz_id = biz_id;
    if (len <= sizeof(cov_recv_data))
        memcpy(cov_recv_data, data, len);
    cov_recv_len = len;
    cov_recv_called++;
}

/* Complete callback */
static uint8_t cov_complete_status;
static int cov_complete_called;
static void cov_on_complete(uint8_t status, void *ctx) {
    (void)ctx;
    cov_complete_status = status;
    cov_complete_called++;
}

/* ===== Helper ===== */
static cr_instance_t cov_inst;
static uint8_t cov_buffer[8192];
static cr_hal_t cov_hal;

static void cov_reset(void) {
    cov_mock_tick = 0;
    cov_send_count = 0;
    cov_sent_len = 0;
    cov_recv_called = 0;
    cov_recv_len = 0;
    cov_complete_called = 0;
    cov_send_history_count = 0;
    cov_hal_send_retval = 0;
    memset(cov_sent_buf, 0, sizeof(cov_sent_buf));
}

static void cov_setup(const cr_config_t *cfg) {
    cov_reset();
    cr_init(&cov_inst, cfg, cov_buffer, sizeof(cov_buffer));
    cov_hal.send = cov_mock_send;
    cov_hal.get_tick_ms = cov_mock_get_tick;
    cov_hal.hw_ctx = NULL;
    cr_set_hal(&cov_inst, &cov_hal);
    cr_set_recv_callback(&cov_inst, cov_mock_recv_cb, NULL);
}

/* ================================================================
 * 边界情况
 * ================================================================ */

/* [额外-边界] MTU=1 时的拆帧 */
void test_boundary_mtu_one_segmentation(void) {
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 6, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cov_reset();
    cr_init(&cov_inst, &cfg, cov_buffer, sizeof(cov_buffer));
    cov_hal.send = cov_mock_send_history;
    cov_hal.get_tick_ms = cov_mock_get_tick;
    cov_hal.hw_ctx = NULL;
    cr_set_hal(&cov_inst, &cov_hal);

    /* 5 bytes data → 5 frames with MTU=1 */
    uint8_t data[] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5};
    cr_send(&cov_inst, 0x02, 0, data, 5, cov_on_complete, NULL);

    for (int i = 0; i < 5; i++) {
        cr_poll(&cov_inst);
    }

    TEST_ASSERT_EQUAL_INT(5, cov_send_history_count);
    /* Each frame: 5-byte header + 1-byte payload = 6 bytes */
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_UINT16(6, cov_send_history_len[i]);
    }
    /* First 4: FRAG=1, LAST=0; Last: FRAG=1, LAST=1 */
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_BITS(0x20, 0x20, cov_send_history[i][2]); /* FRAG */
        TEST_ASSERT_BITS(0x10, 0x00, cov_send_history[i][2]); /* not LAST */
    }
    TEST_ASSERT_BITS(0x30, 0x30, cov_send_history[4][2]); /* FRAG+LAST */

    /* Verify payload content byte by byte */
    for (int i = 0; i < 5; i++) {
        TEST_ASSERT_EQUAL_UINT8(data[i], cov_send_history[i][5]);
    }

    /* Complete callback invoked */
    TEST_ASSERT_EQUAL_INT(1, cov_complete_called);
    TEST_ASSERT_EQUAL_UINT8(0, cov_complete_status);
}

/* [额外-边界] tx_queue_depth=1 的边界 */
void test_boundary_tx_queue_depth_one(void) {
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 1,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cov_setup(&cfg);

    uint8_t data[] = {1, 2, 3};
    /* First send should succeed */
    TEST_ASSERT_EQUAL_INT(0, cr_send(&cov_inst, 0x02, 0, data, 3, NULL, NULL));
    /* Second send should fail - queue full */
    TEST_ASSERT_EQUAL_INT(-1, cr_send(&cov_inst, 0x03, 0, data, 3, NULL, NULL));

    /* After poll (completes first), next send should succeed */
    cr_poll(&cov_inst);
    TEST_ASSERT_EQUAL_INT(0, cr_send(&cov_inst, 0x03, 0, data, 3, NULL, NULL));
}

/* [额外-边界] TTL=0 的广播发送 */
void test_boundary_broadcast_ttl_zero_send(void) {
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 0, /* TTL=0 */
        .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cov_setup(&cfg);

    uint8_t data[] = "TTL0";
    int ret = cr_broadcast(&cov_inst, 0, data, 4);
    TEST_ASSERT_EQUAL_INT(0, ret);

    cr_poll(&cov_inst);

    TEST_ASSERT_EQUAL_INT(1, cov_send_count);
    /* Frame should have TTL=0 in header */
    TEST_ASSERT_EQUAL_UINT8(0x00, cov_sent_buf[4]);
    /* DST = broadcast */
    TEST_ASSERT_EQUAL_UINT8(0xFF, cov_sent_buf[0]);
}

/* [额外-边界] 路由表为空时的行为：发送单播 */
void test_boundary_empty_route_table_send(void) {
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cov_setup(&cfg);

    /* cr_send with no route: route_lookup returns 0xFF (broadcast sentinel) */
    uint8_t data[] = {0xAA};
    int ret = cr_send(&cov_inst, 0x03, 0, data, 1, cov_on_complete, NULL);
    TEST_ASSERT_EQUAL_INT(0, ret);

    cr_poll(&cov_inst);

    /* Should still send (next_hop resolves to 0xFF) — hal.send is called */
    TEST_ASSERT_EQUAL_INT(1, cov_send_count);
    /* next_hop passed to HAL send = 0xFF (broadcast sentinel) */
    TEST_ASSERT_EQUAL_UINT8(0xFF, cov_sent_next_hop);
}

/* [额外-边界] rx_buf_per_slot 刚好够装完整长数据（边界）*/
void test_boundary_rx_buf_exactly_fits(void) {
    /* rx_buf_per_slot=16, receive data exactly 16 bytes (8+8, 2 frags) */
    cr_config_t cfg = {
        .local_addr = 0x02, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 16,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cov_setup(&cfg);

    /* Frag 0: 8 bytes payload */
    uint8_t f0[] = {0x02, 0x01, 0x20, 0x00, 0x03, 1,2,3,4,5,6,7,8};
    cr_feed_frame(&cov_inst, f0, sizeof(f0));
    TEST_ASSERT_EQUAL_INT(0, cov_recv_called);

    /* Frag 1 (LAST): 8 bytes payload → total=16, exactly fits rx_buf_per_slot */
    uint8_t f1[] = {0x02, 0x01, 0x30, 0x01, 0x03, 9,10,11,12,13,14,15,16};
    cr_feed_frame(&cov_inst, f1, sizeof(f1));
    TEST_ASSERT_EQUAL_INT(1, cov_recv_called);
    TEST_ASSERT_EQUAL_UINT16(16, cov_recv_len);
}

/* [额外-边界] rx_buf_per_slot 刚好不够装完整长数据（超1字节） */
void test_boundary_rx_buf_one_byte_short(void) {
    /* rx_buf_per_slot=15, receive data 16 bytes (8+8) → overflow at frag 1 */
    cr_config_t cfg = {
        .local_addr = 0x02, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 15,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cov_setup(&cfg);

    uint8_t f0[] = {0x02, 0x01, 0x20, 0x00, 0x03, 1,2,3,4,5,6,7,8};
    cr_feed_frame(&cov_inst, f0, sizeof(f0));
    TEST_ASSERT_EQUAL_INT(0, cov_recv_called);

    /* Frag 1: 8 bytes → total would be 16 > 15 → overflow, slot dropped */
    uint8_t f1[] = {0x02, 0x01, 0x30, 0x01, 0x03, 9,10,11,12,13,14,15,16};
    cr_feed_frame(&cov_inst, f1, sizeof(f1));
    TEST_ASSERT_EQUAL_INT(0, cov_recv_called);
}

/* ================================================================
 * 异常路径
 * ================================================================ */

/* [额外-异常] HAL send 返回非0（失败）时的行为 */
void test_error_hal_send_fail(void) {
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cov_setup(&cfg);
    cov_hal_send_retval = -1; /* HAL returns error */

    uint8_t data[] = {1, 2, 3};
    int ret = cr_send(&cov_inst, 0x02, 0, data, 3, cov_on_complete, NULL);
    TEST_ASSERT_EQUAL_INT(0, ret); /* send enqueues, doesn't check HAL yet */

    /* Poll should call HAL send (which returns -1) — no crash */
    cr_poll(&cov_inst);

    /* The send was attempted */
    TEST_ASSERT_EQUAL_INT(1, cov_send_count);
    /* With ack_disabled, complete callback should still fire
     * (library doesn't check hal.send return value for completion decision) */
    TEST_ASSERT_EQUAL_INT(1, cov_complete_called);
}

/* [额外-异常] cr_feed_frame 传入 NULL data */
void test_error_feed_frame_null_data(void) {
    cr_config_t cfg = {
        .local_addr = 0x02, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cov_setup(&cfg);

    /* Should not crash */
    cr_feed_frame(&cov_inst, NULL, 10);
    TEST_ASSERT_EQUAL_INT(0, cov_recv_called);
    TEST_ASSERT_EQUAL_INT(0, cov_send_count);
}

/* [额外-异常] cr_feed_frame 传入 NULL instance */
void test_error_feed_frame_null_instance(void) {
    uint8_t frame[] = {0x02, 0x01, 0x00, 0x00, 0x03, 'X'};
    /* Should not crash */
    cr_feed_frame(NULL, frame, sizeof(frame));
    /* No assertion needed — just verify no crash */
    TEST_ASSERT_TRUE(1);
}

/* [额外-异常] cr_poll 在 HAL 未设置时的行为 */
void test_error_poll_no_hal(void) {
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cr_instance_t inst;
    uint8_t buf[4096];
    cr_init(&inst, &cfg, buf, sizeof(buf));
    /* NOT calling cr_set_hal */

    /* Enqueue a send */
    uint8_t data[] = {1, 2, 3};
    cr_send(&inst, 0x02, 0, data, 3, NULL, NULL);

    /* Poll should not crash */
    cr_poll(&inst);
    cr_poll(&inst);
    TEST_ASSERT_TRUE(1);
}

/* [额外-异常] 配置 mtu=0 初始化失败 */
void test_error_init_mtu_zero(void) {
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 0, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cr_instance_t inst;
    uint8_t buf[4096];
    int ret = cr_init(&inst, &cfg, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-2, ret);
}

/* [额外-异常] 配置 tx_queue_depth=0 初始化失败 */
void test_error_init_tx_queue_depth_zero(void) {
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 0,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cr_instance_t inst;
    uint8_t buf[4096];
    int ret = cr_init(&inst, &cfg, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-2, ret);
}

/* [额外-异常] 配置 bcast_queue_depth=0 初始化失败 */
void test_error_init_bcast_queue_depth_zero(void) {
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 0,
        .route_table = NULL, .route_count = 0,
    };
    cr_instance_t inst;
    uint8_t buf[4096];
    int ret = cr_init(&inst, &cfg, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-2, ret);
}

/* [额外-异常] 配置 dedup_table_size=0 初始化失败 */
void test_error_init_dedup_table_size_zero(void) {
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 0,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cr_instance_t inst;
    uint8_t buf[4096];
    int ret = cr_init(&inst, &cfg, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(-2, ret);
}

/* ================================================================
 * 安全/防御
 * ================================================================ */

/* [额外-安全] 超大 len 值（接近 UINT16_MAX）cr_send */
void test_safety_send_huge_len(void) {
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cov_setup(&cfg);

    /* len=65535, needs many blocks → should return CR_ERR_POOL_FULL (-4) */
    uint8_t data = 0xAA;
    int ret = cr_send(&cov_inst, 0x02, 0, &data, 65535, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(-4, ret);  /* CR_ERR_POOL_FULL */
}

/* [额外-安全] 超大 len 值 cr_broadcast */
void test_safety_broadcast_huge_len(void) {
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cov_setup(&cfg);

    /* len=65535, exceeds mtu=64 → should return -2 */
    uint8_t data = 0xBB;
    int ret = cr_broadcast(&cov_inst, 0, &data, 65535);
    TEST_ASSERT_EQUAL_INT(-2, ret);
}

/* [额外-安全] 伪造帧 CTL 字段无效组合（ACK+BROADCAST+FRAG all set） */
void test_safety_forged_frame_invalid_ctl(void) {
    cr_config_t cfg = {
        .local_addr = 0x02, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 1, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cov_setup(&cfg);

    /* CTL = 0xF0 (ACK+BROADCAST+FRAG+LAST all set) — should not crash */
    uint8_t frame[] = {0x02, 0x01, 0xF0, 0x00, 0x03, 'X', 'Y'};
    cr_feed_frame(&cov_inst, frame, sizeof(frame));

    /* Since ACK bit is set, it'll be treated as ACK frame → no recv callback */
    TEST_ASSERT_EQUAL_INT(0, cov_recv_called);
}

/* [额外-安全] 伪造帧 CTL = 0x00 但 DST=broadcast address */
void test_safety_forged_unicast_to_broadcast_addr(void) {
    cr_config_t cfg = {
        .local_addr = 0x02, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cov_setup(&cfg);

    /* DST=0xFF but CTL doesn't have broadcast bit → cr_handle_broadcast_frame path */
    uint8_t frame[] = {0xFF, 0x01, 0x00, 0x05, 0x02, 'Z'};
    cr_feed_frame(&cov_inst, frame, sizeof(frame));

    /* Should be handled as broadcast (checked by DST==0xFF) but CTL has no bcast bit
     * The code dispatches on DST field, so this goes through broadcast handler.
     * Should be delivered (not our own broadcast) and forwarded if TTL>0 */
    TEST_ASSERT_EQUAL_INT(1, cov_recv_called);
}

/* [额外-安全] 接收帧 payload 长度超过 MTU（转发场景） */
void test_safety_rx_frame_payload_exceeds_mtu(void) {
    cr_route_entry_t routes[] = { {.dest = 0x03, .next_hop = 0x03} };
    cr_config_t cfg = {
        .local_addr = 0x02, .mtu = 13, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = routes, .route_count = 1,
    };
    cov_setup(&cfg);

    /* Frame with 20-byte payload (MTU=8), target is 0x03 → forward path */
    uint8_t frame[25] = {0x03, 0x01, 0x00, 0x00, 0x03};
    memset(&frame[5], 0xAA, 20);
    cr_feed_frame(&cov_inst, frame, 25);

    /* The forward function doesn't check payload vs MTU - it just forwards raw */
    TEST_ASSERT_EQUAL_INT(1, cov_send_count);
    TEST_ASSERT_EQUAL_UINT16(25, cov_sent_len);
}

/* [额外-安全] 接收广播帧 payload 超过 MTU → 不转发 */
void test_safety_rx_broadcast_oversized_no_forward(void) {
    cr_config_t cfg = {
        .local_addr = 0x02, .mtu = 9, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cov_setup(&cfg);

    /* Broadcast frame with 10-byte payload (MTU=4): 5 header + 10 payload = 15 */
    /* max_frame = 5 + 4 = 9, frame len = 15 > 9 → should NOT forward */
    uint8_t frame[15] = {0xFF, 0x01, 0x40, 0x00, 0x02}; /* TTL=2 */
    memset(&frame[5], 0xBB, 10);
    cr_feed_frame(&cov_inst, frame, 15);

    /* Should still deliver to user */
    TEST_ASSERT_EQUAL_INT(1, cov_recv_called);
    /* Should NOT forward (oversized) */
    TEST_ASSERT_EQUAL_INT(0, cov_send_count);
}

/* ================================================================
 * 性能/压力
 * ================================================================ */

/* [额外-压力] 快速连续提交多个发送任务直到队列满 */
void test_stress_fill_tx_queue(void) {
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cov_setup(&cfg);
    cov_hal.send = cov_mock_send_history;
    cr_set_hal(&cov_inst, &cov_hal);
    cov_send_history_count = 0;

    uint8_t data[] = {0x11, 0x22};
    /* Fill all 4 slots */
    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT(0, cr_send(&cov_inst, (uint8_t)(0x02 + i), 0, data, 2, NULL, NULL));
    }
    /* 5th should fail */
    TEST_ASSERT_EQUAL_INT(-1, cr_send(&cov_inst, 0x06, 0, data, 2, NULL, NULL));

    /* Poll all 4 — should send 4 frames */
    for (int i = 0; i < 4; i++) {
        cr_poll(&cov_inst);
    }
    TEST_ASSERT_EQUAL_INT(4, cov_send_history_count);

    /* Verify each was addressed differently */
    TEST_ASSERT_EQUAL_UINT8(0x02, cov_send_history[0][0]);
    TEST_ASSERT_EQUAL_UINT8(0x03, cov_send_history[1][0]);
    TEST_ASSERT_EQUAL_UINT8(0x04, cov_send_history[2][0]);
    TEST_ASSERT_EQUAL_UINT8(0x05, cov_send_history[3][0]);
}

/* [额外-压力] 多个 RX 组装槽同时活跃 */
void test_stress_multiple_rx_slots_active(void) {
    cr_config_t cfg = {
        .local_addr = 0x02, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 4, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 5000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cov_setup(&cfg);

    /* Start assembly from 4 different sources simultaneously */
    for (uint8_t i = 0; i < 4; i++) {
        uint8_t f[] = {0x02, (uint8_t)(0x10 + i), 0x20, 0x00, 0x03, (uint8_t)(i * 10)};
        cr_feed_frame(&cov_inst, f, sizeof(f));
    }
    TEST_ASSERT_EQUAL_INT(0, cov_recv_called); /* all pending */

    /* Complete all 4 with their second (last) fragments */
    for (uint8_t i = 0; i < 4; i++) {
        uint8_t f[] = {0x02, (uint8_t)(0x10 + i), 0x30, 0x01, 0x03, (uint8_t)(i * 10 + 1)};
        cr_feed_frame(&cov_inst, f, sizeof(f));
    }
    TEST_ASSERT_EQUAL_INT(4, cov_recv_called);
}

/* ================================================================
 * 端到端场景
 * ================================================================ */

/* Multi-node infrastructure for E2E tests */
static uint8_t e2e_buf_a[256], e2e_buf_b[256], e2e_buf_c[256];
static uint16_t e2e_len_a, e2e_len_b, e2e_len_c;
static int e2e_count_a, e2e_count_b, e2e_count_c;
static uint8_t e2e_history_b[16][256];
static uint16_t e2e_history_b_len[16];
static int e2e_history_b_count;

static int e2e_send_a(void *ctx, uint8_t nh, const uint8_t *d, uint16_t l) {
    (void)ctx; (void)nh; memcpy(e2e_buf_a, d, l); e2e_len_a = l; e2e_count_a++; return 0;
}
static int e2e_send_b(void *ctx, uint8_t nh, const uint8_t *d, uint16_t l) {
    (void)ctx; (void)nh;
    memcpy(e2e_buf_b, d, l); e2e_len_b = l;
    if (e2e_history_b_count < 16) {
        memcpy(e2e_history_b[e2e_history_b_count], d, l);
        e2e_history_b_len[e2e_history_b_count] = l;
    }
    e2e_history_b_count++;
    e2e_count_b++; return 0;
}
static int e2e_send_c(void *ctx, uint8_t nh, const uint8_t *d, uint16_t l) {
    (void)ctx; (void)nh; memcpy(e2e_buf_c, d, l); e2e_len_c = l; e2e_count_c++; return 0;
}

static uint32_t e2e_tick = 0;
static uint32_t e2e_get_tick(void) { return e2e_tick; }

/* [额外-端到端] 完整的三节点链式传输 A→B→C 含 ACK 回传 (长数据) */
void test_e2e_three_node_chain_long_data(void) {
    e2e_tick = 0;
    e2e_count_a = 0; e2e_count_b = 0; e2e_count_c = 0;
    e2e_history_b_count = 0;

    /* Node A (0x01): sender */
    uint8_t bufA[8192];
    cr_route_entry_t rA[] = { {.dest = 0x03, .next_hop = 0x02} };
    cr_config_t cfgA = { .local_addr = 0x01, .mtu = 13, .frame_interval_ms = 0,
        .max_retries = 3, .ack_timeout_ms = 100, .ack_enabled = 1,
        .ack_mode = CR_ACK_MODE_REPLY, .default_ttl = 3,
        .tx_queue_depth = 4, .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = rA, .route_count = 1 };
    cr_instance_t instA;
    cr_init(&instA, &cfgA, bufA, sizeof(bufA));
    cr_hal_t halA = { .send = e2e_send_a, .get_tick_ms = e2e_get_tick, .hw_ctx = NULL };
    cr_set_hal(&instA, &halA);

    /* Node B (0x02): relay */
    uint8_t bufB[8192];
    cr_route_entry_t rB[] = { {.dest = 0x01, .next_hop = 0x01}, {.dest = 0x03, .next_hop = 0x03} };
    cr_config_t cfgB = { .local_addr = 0x02, .mtu = 13, .frame_interval_ms = 0,
        .max_retries = 3, .ack_timeout_ms = 100, .ack_enabled = 1,
        .ack_mode = CR_ACK_MODE_REPLY, .default_ttl = 3,
        .tx_queue_depth = 4, .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = rB, .route_count = 2 };
    cr_instance_t instB;
    cr_init(&instB, &cfgB, bufB, sizeof(bufB));
    cr_hal_t halB = { .send = e2e_send_b, .get_tick_ms = e2e_get_tick, .hw_ctx = NULL };
    cr_set_hal(&instB, &halB);

    /* Node C (0x03): receiver */
    uint8_t bufC[8192];
    cr_route_entry_t rC[] = { {.dest = 0x01, .next_hop = 0x02} };
    cr_config_t cfgC = { .local_addr = 0x03, .mtu = 13, .frame_interval_ms = 0,
        .max_retries = 3, .ack_timeout_ms = 100, .ack_enabled = 1,
        .ack_mode = CR_ACK_MODE_REPLY, .default_ttl = 3,
        .tx_queue_depth = 4, .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = rC, .route_count = 1 };
    cr_instance_t instC;
    cr_init(&instC, &cfgC, bufC, sizeof(bufC));
    cr_hal_t halC = { .send = e2e_send_c, .get_tick_ms = e2e_get_tick, .hw_ctx = NULL };
    cr_set_hal(&instC, &halC);

    /* C registers recv callback */
    cov_recv_called = 0;
    cov_recv_len = 0;
    cr_set_recv_callback(&instC, cov_mock_recv_cb, NULL);

    /* A sends 20 bytes to C → fragments into 3 frames (8+8+4) */
    cov_complete_called = 0;
    uint8_t data[20];
    for (int i = 0; i < 20; i++) data[i] = (uint8_t)(i + 1);
    cr_send(&instA, 0x03, 5, data, 20, cov_on_complete, NULL);

    /* Process 3 frames: for each, A sends → B forwards → C receives + ACKs → B forwards ACK → A receives ACK */
    for (int frame_idx = 0; frame_idx < 3; frame_idx++) {
        /* A sends frame */
        cr_poll(&instA);
        TEST_ASSERT_TRUE(e2e_len_a > 0);

        /* B receives from A, forwards to C */
        e2e_count_b = 0;
        cr_feed_frame(&instB, e2e_buf_a, e2e_len_a);
        TEST_ASSERT_EQUAL_INT(1, e2e_count_b);

        /* C receives from B, auto-ACK to A (through B) */
        e2e_count_c = 0;
        cr_feed_frame(&instC, e2e_buf_b, e2e_len_b);
        TEST_ASSERT_EQUAL_INT(1, e2e_count_c); /* ACK sent */
        TEST_ASSERT_EQUAL_UINT8(0x01, e2e_buf_c[0]); /* ACK DST=A */
        TEST_ASSERT_BITS(0x80, 0x80, e2e_buf_c[2]); /* ACK bit */

        /* B receives ACK from C, forwards to A */
        e2e_count_b = 0;
        cr_feed_frame(&instB, e2e_buf_c, e2e_len_c);
        TEST_ASSERT_EQUAL_INT(1, e2e_count_b);

        /* A receives ACK */
        cr_feed_frame(&instA, e2e_buf_b, e2e_len_b);
    }

    /* Verify A got complete success */
    TEST_ASSERT_EQUAL_INT(1, cov_complete_called);
    TEST_ASSERT_EQUAL_UINT8(0, cov_complete_status);

    /* Verify C received complete data */
    TEST_ASSERT_EQUAL_INT(1, cov_recv_called);
    TEST_ASSERT_EQUAL_UINT16(20, cov_recv_len);
    uint8_t expected[20];
    for (int i = 0; i < 20; i++) expected[i] = (uint8_t)(i + 1);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, cov_recv_data, 20);
}

/* Node B recv callback for broadcast flood test */
static int e2e_b_recv_called;
static void e2e_b_recv_cb(cr_instance_t *i, uint8_t s, uint8_t bid,
                          const uint8_t *d, uint16_t l, void *c) {
    (void)i;(void)s;(void)bid;(void)d;(void)l;(void)c;
    e2e_b_recv_called++;
}

/* [额外-端到端] 广播泛洪完整流程（发送→转发→去重） */
void test_e2e_broadcast_flood_and_dedup(void) {
    e2e_tick = 0;
    e2e_count_a = 0; e2e_count_b = 0; e2e_count_c = 0;

    /* A(0x01), B(0x02), C(0x03) — linear topology for broadcast flooding
     * A sends broadcast → B receives+forwards → C receives
     * If C's forwarded frame loops back to B → dedup prevents re-delivery */

    /* Node A (0x01): broadcaster */
    uint8_t bufA[4096];
    cr_config_t cfgA = { .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY, .default_ttl = 2,
        .tx_queue_depth = 4, .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0 };
    cr_instance_t instA;
    cr_init(&instA, &cfgA, bufA, sizeof(bufA));
    cr_hal_t halA = { .send = e2e_send_a, .get_tick_ms = e2e_get_tick, .hw_ctx = NULL };
    cr_set_hal(&instA, &halA);

    /* Node B (0x02): receiver + relay */
    uint8_t bufB[4096];
    cr_config_t cfgB = { .local_addr = 0x02, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY, .default_ttl = 2,
        .tx_queue_depth = 4, .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0 };
    cr_instance_t instB;
    cr_init(&instB, &cfgB, bufB, sizeof(bufB));
    cr_hal_t halB = { .send = e2e_send_b, .get_tick_ms = e2e_get_tick, .hw_ctx = NULL };
    cr_set_hal(&instB, &halB);

    e2e_b_recv_called = 0;
    cr_set_recv_callback(&instB, e2e_b_recv_cb, NULL);

    /* Node C (0x03): receiver */
    uint8_t bufC[4096];
    cr_config_t cfgC = { .local_addr = 0x03, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY, .default_ttl = 2,
        .tx_queue_depth = 4, .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0 };
    cr_instance_t instC;
    cr_init(&instC, &cfgC, bufC, sizeof(bufC));
    cr_hal_t halC = { .send = e2e_send_c, .get_tick_ms = e2e_get_tick, .hw_ctx = NULL };
    cr_set_hal(&instC, &halC);
    cov_recv_called = 0;
    cr_set_recv_callback(&instC, cov_mock_recv_cb, NULL);

    /* A sends broadcast */
    uint8_t bcast_data[] = "FLOOD";
    cr_broadcast(&instA, 7, bcast_data, 5);
    cr_poll(&instA);

    /* Verify A sent broadcast frame */
    TEST_ASSERT_EQUAL_UINT8(0xFF, e2e_buf_a[0]); /* DST=broadcast */
    TEST_ASSERT_EQUAL_UINT8(0x01, e2e_buf_a[1]); /* SRC=A */
    TEST_ASSERT_EQUAL_UINT8(2, e2e_buf_a[4]);    /* TTL=2 */

    /* B receives broadcast from A → delivers + forwards with TTL-1 */
    e2e_count_b = 0;
    cr_feed_frame(&instB, e2e_buf_a, e2e_len_a);
    TEST_ASSERT_EQUAL_INT(1, e2e_b_recv_called);
    TEST_ASSERT_EQUAL_INT(1, e2e_count_b); /* forwarded */
    TEST_ASSERT_EQUAL_UINT8(1, e2e_buf_b[4]); /* TTL=1 */

    /* C receives forwarded broadcast from B → delivers + forwards with TTL-1 */
    e2e_count_c = 0;
    cr_feed_frame(&instC, e2e_buf_b, e2e_len_b);
    TEST_ASSERT_EQUAL_INT(1, cov_recv_called);
    TEST_ASSERT_EQUAL_INT(1, e2e_count_c); /* forwarded with TTL=0 */
    TEST_ASSERT_EQUAL_UINT8(0, e2e_buf_c[4]); /* TTL=0 */

    /* Now C's forwarded frame reaches B again — B should dedup */
    e2e_b_recv_called = 0;
    e2e_count_b = 0;
    cr_feed_frame(&instB, e2e_buf_c, e2e_len_c);
    TEST_ASSERT_EQUAL_INT(0, e2e_b_recv_called); /* dedup! */
    TEST_ASSERT_EQUAL_INT(0, e2e_count_b);   /* not forwarded */
}

/* [额外-压力] SEQ 回卷后 ACK 仍能正确匹配 */
void test_stress_seq_wrap_with_ack(void) {
    cr_route_entry_t routes[] = { {.dest = 0x02, .next_hop = 0x02} };
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .max_retries = 3, .ack_timeout_ms = 100,
        .ack_enabled = 1, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = routes, .route_count = 1,
    };
    cov_setup(&cfg);

    /* Send 255 frames to get SEQ to 255 */
    uint8_t data = 0xAA;
    for (int i = 0; i < 255; i++) {
        cov_send_count = 0;
        cr_send(&cov_inst, 0x02, 0, &data, 1, NULL, NULL);
        cr_poll(&cov_inst);
        /* ACK it */
        uint8_t ack[] = {0x01, 0x02, 0x80, cov_sent_buf[3], 0x03};
        cr_feed_frame(&cov_inst, ack, sizeof(ack));
    }

    /* Next send should have SEQ=255 */
    cov_send_count = 0;
    cov_complete_called = 0;
    cr_send(&cov_inst, 0x02, 0, &data, 1, cov_on_complete, NULL);
    cr_poll(&cov_inst);
    TEST_ASSERT_EQUAL_UINT8(255, cov_sent_buf[3]);

    /* ACK with SEQ=255 */
    uint8_t ack255[] = {0x01, 0x02, 0x80, 255, 0x03};
    cr_feed_frame(&cov_inst, ack255, sizeof(ack255));
    TEST_ASSERT_EQUAL_INT(1, cov_complete_called);
    TEST_ASSERT_EQUAL_UINT8(0, cov_complete_status);

    /* Next send wraps to SEQ=0 */
    cov_send_count = 0;
    cov_complete_called = 0;
    cr_send(&cov_inst, 0x02, 0, &data, 1, cov_on_complete, NULL);
    cr_poll(&cov_inst);
    TEST_ASSERT_EQUAL_UINT8(0, cov_sent_buf[3]);

    /* ACK with SEQ=0 */
    uint8_t ack0[] = {0x01, 0x02, 0x80, 0, 0x03};
    cr_feed_frame(&cov_inst, ack0, sizeof(ack0));
    TEST_ASSERT_EQUAL_INT(1, cov_complete_called);
    TEST_ASSERT_EQUAL_UINT8(0, cov_complete_status);
}

/* [额外-边界] cr_calc_buffer_size with mtu=0 returns 0 */
void test_boundary_calc_buffer_size_mtu_zero(void) {
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 0, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_buf_per_slot = 256, .pool_size = 32, .bcast_queue_depth = 4,
    };
    size_t size = cr_calc_buffer_size(&cfg);
    TEST_ASSERT_EQUAL_INT(0, size);
}

/* [额外-边界] cr_calc_buffer_size with NULL config */
void test_boundary_calc_buffer_size_null(void) {
    size_t size = cr_calc_buffer_size(NULL);
    TEST_ASSERT_EQUAL_INT(0, size);
}

/* [额外-异常] 长数据 ACK 中一帧失败导致整个长数据失败 */
void test_error_long_data_ack_fail_whole_task(void) {
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 13, .frame_interval_ms = 0,
        .max_retries = 1, .ack_timeout_ms = 50,
        .ack_enabled = 1, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cov_setup(&cfg);

    /* 20 bytes → 3 frames with MTU=8 */
    uint8_t data[20] = {0};
    cov_complete_called = 0;
    cr_send(&cov_inst, 0x03, 0, data, 20, cov_on_complete, NULL);

    /* First frame: send + ACK OK */
    cov_mock_tick = 0;
    cr_poll(&cov_inst);
    uint8_t ack1[] = {0x01, 0x03, 0x80, cov_sent_buf[3], 0x03};
    cr_feed_frame(&cov_inst, ack1, sizeof(ack1));

    /* Second frame: send but no ACK → timeout + retransmit 1 → still no ACK → fail */
    cr_poll(&cov_inst); /* sends frame 2 */
    cov_mock_tick = 51; /* timeout */
    cr_poll(&cov_inst); /* retransmit 1 */
    cov_mock_tick = 102; /* timeout again */
    cr_poll(&cov_inst); /* max retries reached → fail */

    TEST_ASSERT_EQUAL_INT(1, cov_complete_called);
    TEST_ASSERT_NOT_EQUAL(0, cov_complete_status); /* FAIL */
}

/* [额外-边界] 单帧 ACK 模式下 biz_id 正确传递 */
void test_boundary_biz_id_propagation(void) {
    cr_config_t cfg = {
        .local_addr = 0x02, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cov_setup(&cfg);

    /* CTL lower 4 bits = biz_id = 0x0A */
    uint8_t frame[] = {0x02, 0x01, 0x0A, 0x00, 0x03, 'B', 'I', 'Z'};
    cr_feed_frame(&cov_inst, frame, sizeof(frame));

    TEST_ASSERT_EQUAL_INT(1, cov_recv_called);
    TEST_ASSERT_EQUAL_UINT8(0x0A, cov_recv_biz_id);
}

/* [额外-安全] cr_send 传入 data=NULL len>0 → 参数错误 */
void test_safety_send_null_data_nonzero_len(void) {
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cov_setup(&cfg);

    int ret = cr_send(&cov_inst, 0x02, 0, NULL, 10, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(-2, ret);
}

/* [额外-安全] cr_broadcast 传入 data=NULL len>0 → 参数错误 */
void test_safety_broadcast_null_data_nonzero_len(void) {
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cov_setup(&cfg);

    int ret = cr_broadcast(&cov_inst, 0, NULL, 10);
    TEST_ASSERT_EQUAL_INT(-2, ret);
}

/* [额外-安全] cr_send 传入 NULL instance */
void test_safety_send_null_instance(void) {
    uint8_t data[] = {1};
    int ret = cr_send(NULL, 0x02, 0, data, 1, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(-2, ret);
}

/* [额外-安全] cr_broadcast 传入 NULL instance */
void test_safety_broadcast_null_instance(void) {
    uint8_t data[] = {1};
    int ret = cr_broadcast(NULL, 0, data, 1);
    TEST_ASSERT_EQUAL_INT(-2, ret);
}

/* [额外-异常] cr_poll 传入 NULL instance */
void test_error_poll_null_instance(void) {
    /* Should not crash */
    cr_poll(NULL);
    TEST_ASSERT_TRUE(1);
}

/* [额外-异常] cr_notify_send_done 传入 NULL instance */
void test_error_notify_send_done_null(void) {
    cr_notify_send_done(NULL);
    TEST_ASSERT_TRUE(1);
}

/* ================================================================
 * 补充覆盖: feature 规格中明确要求的场景
 * ================================================================ */

/* [Scenario补充] 单播转发时 TTL 不递减
 * feature "中间节点透传转发" 明确要求：将该帧原样转发到下一跳
 * 即 TTL 在单播转发中不应改变 */
void test_unicast_forward_ttl_not_decremented(void) {
    cr_route_entry_t routes[] = { {.dest = 0x03, .next_hop = 0x03} };
    cr_config_t cfg = {
        .local_addr = 0x02, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = routes, .route_count = 1,
    };
    cov_setup(&cfg);

    /* 单播帧: DST=0x03, SRC=0x01, CTL=0x00, SEQ=0, TTL=3 */
    uint8_t frame[] = {0x03, 0x01, 0x00, 0x00, 0x03, 'D', 'A', 'T', 'A'};
    cr_feed_frame(&cov_inst, frame, sizeof(frame));

    /* 应转发 */
    TEST_ASSERT_EQUAL_INT(1, cov_send_count);
    /* 验证 TTL 未递减: 原始 TTL=3, 转发后仍为 3 */
    TEST_ASSERT_EQUAL_UINT8(0x03, cov_sent_buf[4]);
    /* 验证帧内容完全原样转发 */
    TEST_ASSERT_EQUAL_UINT8_ARRAY(frame, cov_sent_buf, sizeof(frame));
}

/* [Scenario补充] ACK 模式为 INTERRUPT 时，连续多帧长数据的完整流程
 * 验证在 INTERRUPT 模式下，长数据的每一帧都通过 cr_notify_send_done 确认，
 * 最终触发完成回调 */
void test_interrupt_mode_long_data_multi_frame(void) {
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 13, .frame_interval_ms = 0,
        .max_retries = 3, .ack_timeout_ms = 100,
        .ack_enabled = 1, .ack_mode = CR_ACK_MODE_INTERRUPT,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cov_setup(&cfg);
    cov_hal.send = cov_mock_send_history;
    cr_set_hal(&cov_inst, &cov_hal);

    /* 20 bytes → 3 frames with MTU=8 (8+8+4) */
    uint8_t data[20];
    for (int i = 0; i < 20; i++) data[i] = (uint8_t)(i + 0x10);
    cov_complete_called = 0;
    cr_send(&cov_inst, 0x03, 0, data, 20, cov_on_complete, NULL);

    /* 逐帧发送 + 中断确认 */
    for (int i = 0; i < 3; i++) {
        cr_poll(&cov_inst); /* 发送帧 i */
        TEST_ASSERT_EQUAL_INT(i + 1, cov_send_history_count);

        /* 模拟中断确认 */
        cr_notify_send_done(&cov_inst);
        cr_poll(&cov_inst); /* 处理确认 */
    }

    /* 验证完成回调：3 帧全部确认后触发 */
    TEST_ASSERT_EQUAL_INT(1, cov_complete_called);
    TEST_ASSERT_EQUAL_UINT8(0, cov_complete_status);

    /* 验证 3 帧确实发出 */
    TEST_ASSERT_EQUAL_INT(3, cov_send_history_count);
    /* 帧1: 5头 + 8负载 */
    TEST_ASSERT_EQUAL_UINT16(13, cov_send_history_len[0]);
    /* 帧2: 5头 + 8负载 */
    TEST_ASSERT_EQUAL_UINT16(13, cov_send_history_len[1]);
    /* 帧3: 5头 + 4负载 */
    TEST_ASSERT_EQUAL_UINT16(9, cov_send_history_len[2]);
}
