#include "unity.h"
#include "comm_route.h"
#include <string.h>

/* 三套独立 mock HAL */
static uint8_t sent_a[64], sent_b[64], sent_c[64];
static uint16_t sent_a_len, sent_b_len, sent_c_len;

static int send_a(void *ctx, uint8_t next_hop, const uint8_t *data, uint16_t len) {
    (void)next_hop;
    (void)ctx; memcpy(sent_a, data, len); sent_a_len = len; return 0;
}
static int send_b(void *ctx, uint8_t next_hop, const uint8_t *data, uint16_t len) {
    (void)next_hop;
    (void)ctx; memcpy(sent_b, data, len); sent_b_len = len; return 0;
}
static int send_c(void *ctx, uint8_t next_hop, const uint8_t *data, uint16_t len) {
    (void)next_hop;
    (void)ctx; memcpy(sent_c, data, len); sent_c_len = len; return 0;
}

static uint32_t tick_mh = 0;
static uint32_t get_tick_mh(void) { return tick_mh; }

static uint8_t mh_complete_status;
static int mh_complete_called;
static void on_complete_mh(uint8_t status, void *ctx) {
    (void)ctx; mh_complete_status = status; mh_complete_called++;
}

void test_multihop_ack_routing(void) {
    /* Node A (0x01) */
    uint8_t buf_a[1024];
    cr_route_entry_t routes_a[] = { {.dest = 0x03, .next_hop = 0x02} };
    cr_config_t cfg_a = { .local_addr = 0x01, .mtu = 32, .frame_interval_ms = 0,
        .max_retries = 3, .ack_timeout_ms = 100, .ack_enabled = 1,
        .ack_mode = CR_ACK_MODE_REPLY, .default_ttl = 3,
        .tx_queue_depth = 4, .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .route_table = routes_a, .route_count = 1 };
    cr_instance_t inst_a;
    cr_init(&inst_a, &cfg_a, buf_a, sizeof(buf_a));
    cr_hal_t hal_a = { .send = send_a, .get_tick_ms = get_tick_mh, .hw_ctx = NULL };
    cr_set_hal(&inst_a, &hal_a);

    /* Node B (0x02) */
    uint8_t buf_b[1024];
    cr_route_entry_t routes_b[] = { {.dest = 0x01, .next_hop = 0x01},
                                     {.dest = 0x03, .next_hop = 0x03} };
    cr_config_t cfg_b = { .local_addr = 0x02, .mtu = 32, .frame_interval_ms = 0,
        .max_retries = 3, .ack_timeout_ms = 100, .ack_enabled = 1,
        .ack_mode = CR_ACK_MODE_REPLY, .default_ttl = 3,
        .tx_queue_depth = 4, .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .route_table = routes_b, .route_count = 2 };
    cr_instance_t inst_b;
    cr_init(&inst_b, &cfg_b, buf_b, sizeof(buf_b));
    cr_hal_t hal_b = { .send = send_b, .get_tick_ms = get_tick_mh, .hw_ctx = NULL };
    cr_set_hal(&inst_b, &hal_b);

    /* Node C (0x03) */
    uint8_t buf_c[1024];
    cr_route_entry_t routes_c[] = { {.dest = 0x01, .next_hop = 0x02} };
    cr_config_t cfg_c = { .local_addr = 0x03, .mtu = 32, .frame_interval_ms = 0,
        .max_retries = 3, .ack_timeout_ms = 100, .ack_enabled = 1,
        .ack_mode = CR_ACK_MODE_REPLY, .default_ttl = 3,
        .tx_queue_depth = 4, .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .route_table = routes_c, .route_count = 1 };
    cr_instance_t inst_c;
    cr_init(&inst_c, &cfg_c, buf_c, sizeof(buf_c));
    cr_hal_t hal_c = { .send = send_c, .get_tick_ms = get_tick_mh, .hw_ctx = NULL };
    cr_set_hal(&inst_c, &hal_c);

    /* A sends data to C */
    mh_complete_called = 0;
    uint8_t data[] = {0xAA, 0xBB, 0xCC};
    cr_send(&inst_a, 0x03, 0, data, 3, on_complete_mh, NULL);
    cr_poll(&inst_a); /* A sends frame → sent_a */

    /* B receives, DST=0x03≠0x02, forwards */
    sent_b_len = 0;
    cr_feed_frame(&inst_b, sent_a, sent_a_len);
    TEST_ASSERT_TRUE(sent_b_len > 0); /* B forwarded */

    /* C receives, DST=0x03==自己, auto-ACK */
    sent_c_len = 0;
    cr_feed_frame(&inst_c, sent_b, sent_b_len);
    TEST_ASSERT_TRUE(sent_c_len > 0); /* C sent ACK */
    TEST_ASSERT_EQUAL_UINT8(0x01, sent_c[0]); /* ACK DST=A */
    TEST_ASSERT_BITS(0x80, 0x80, sent_c[2]);  /* CTL.bit7=1 */

    /* B receives ACK, DST=0x01≠0x02, forwards */
    sent_b_len = 0;
    cr_feed_frame(&inst_b, sent_c, sent_c_len);
    TEST_ASSERT_TRUE(sent_b_len > 0); /* B forwarded ACK */

    /* A receives ACK */
    cr_feed_frame(&inst_a, sent_b, sent_b_len);
    TEST_ASSERT_EQUAL_INT(1, mh_complete_called);
    TEST_ASSERT_EQUAL_UINT8(0, mh_complete_status);
}
