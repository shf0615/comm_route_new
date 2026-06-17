#include "comm_route.h"
#include <stdio.h>
#include <string.h>

/* 记录发送的所有帧 */
#define MAX_FRAMES 512
static uint8_t frames[MAX_FRAMES][64];
static uint16_t frame_lens[MAX_FRAMES];
static int frame_count;

static int mock_send(void *ctx, uint8_t next_hop, const uint8_t *data, uint16_t len) {
    (void)ctx; (void)next_hop;
    if (frame_count < MAX_FRAMES) {
        memcpy(frames[frame_count], data, len);
        frame_lens[frame_count] = len;
        frame_count++;
    }
    return 0;
}

static uint32_t tick = 0;
static uint32_t get_tick(void) { return tick; }

/* 接收回调 */
static uint8_t rx_buf[4096];
static uint16_t rx_len;
static int rx_called;
static uint8_t rx_src_list[16];
static uint16_t rx_len_list[16];

static void on_recv(cr_instance_t *inst, uint8_t src, uint8_t biz_id,
                    const uint8_t *data, uint16_t len, void *ctx) {
    (void)inst; (void)biz_id; (void)ctx;
    if (rx_called < 16) {
        rx_src_list[rx_called] = src;
        rx_len_list[rx_called] = len;
        /* 验证数据 */
        int errors = 0;
        uint8_t seed = (uint8_t)(rx_called * 7);  /* 每包不同种子 */
        for (uint16_t i = 0; i < len; i++) {
            if (data[i] != (uint8_t)((i + seed) & 0xFF)) {
                errors++;
            }
        }
        if (errors > 0) {
            printf("  包 %d: 数据校验失败 (%d 字节错误)\n", rx_called, errors);
        }
    }
    rx_called++;
}

static void on_done(uint8_t status, void *ctx) {
    int *done_count = (int *)ctx;
    (*done_count)++;
    if (status != 0) {
        printf("  发送失败! status=%d\n", status);
    }
}

int main(void) {
    printf("=== 连续 10 包 × 2048 字节 拆帧+组包验证 ===\n\n");

    /* TX 实例 */
    static uint8_t tx_buffer[32768];
    cr_route_entry_t routes[] = { {.dest = 0x02, .next_hop = 0x02} };
    cr_config_t tx_cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 16,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 5000, .rx_buf_per_slot = 2560,
        .pool_size = 200, .bcast_queue_depth = 4,
        .route_table = routes, .route_count = 1,
    };
    cr_instance_t tx_inst;
    int ret = cr_init(&tx_inst, &tx_cfg, tx_buffer, sizeof(tx_buffer));
    if (ret != 0) { printf("FAIL: TX cr_init = %d\n", ret); return 1; }
    cr_hal_t tx_hal = { .send = mock_send, .get_tick_ms = get_tick, .hw_ctx = NULL };
    cr_set_hal(&tx_inst, &tx_hal);

    /* RX 实例 */
    static uint8_t rx_buffer[32768];
    cr_config_t rx_cfg = {
        .local_addr = 0x02, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 5000, .rx_buf_per_slot = 2560,
        .pool_size = 200, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cr_instance_t rx_inst;
    ret = cr_init(&rx_inst, &rx_cfg, rx_buffer, sizeof(rx_buffer));
    if (ret != 0) { printf("FAIL: RX cr_init = %d\n", ret); return 1; }
    cr_hal_t rx_hal = { .send = mock_send, .get_tick_ms = get_tick, .hw_ctx = NULL };
    cr_set_hal(&rx_inst, &rx_hal);
    cr_set_recv_callback(&rx_inst, on_recv, NULL);

    /* 逐包发送+接收 */
    int total_frames_sent = 0;
    int done_count = 0;

    for (int pkt = 0; pkt < 10; pkt++) {
        /* 构造 2048 字节数据，每包种子不同 */
        uint8_t data[2048];
        uint8_t seed = (uint8_t)(pkt * 7);
        for (int i = 0; i < 2048; i++) data[i] = (uint8_t)((i + seed) & 0xFF);

        /* 提交发送 */
        frame_count = 0;
        ret = cr_send(&tx_inst, 0x02, 0, data, 2048, on_done, &done_count);
        if (ret != 0) {
            printf("FAIL: 包 %d cr_send = %d\n", pkt, ret);
            return 1;
        }

        /* 驱动 TX 发完所有帧 */
        for (int t = 0; t < 400; t++) {
            tick++;
            cr_poll(&tx_inst);
        }

        printf("  包 %d: 发送 %d 帧", pkt, frame_count);
        total_frames_sent += frame_count;

        /* 喂给 RX */
        for (int i = 0; i < frame_count; i++) {
            cr_feed_frame(&rx_inst, frames[i], frame_lens[i]);
        }
        printf(", RX 已收 %d 包\n", rx_called);
    }

    printf("\n--- 汇总 ---\n");
    printf("  发送包数: 10\n");
    printf("  总帧数: %d (预期 360 = 36帧×10包)\n", total_frames_sent);
    printf("  RX 回调次数: %d (预期 10)\n", rx_called);
    printf("  TX 完成回调: %d (预期 10)\n", done_count);

    int pass = 1;
    if (total_frames_sent != 360) { printf("  FAIL: 总帧数错误\n"); pass = 0; }
    if (rx_called != 10) { printf("  FAIL: RX 回调次数错误\n"); pass = 0; }
    if (done_count != 10) { printf("  FAIL: TX 完成回调次数错误\n"); pass = 0; }

    /* 验证每包接收长度 */
    for (int i = 0; i < 10 && i < rx_called; i++) {
        if (rx_len_list[i] != 2048) {
            printf("  FAIL: 包 %d 接收长度=%d (预期 2048)\n", i, rx_len_list[i]);
            pass = 0;
        }
    }

    if (pass) {
        printf("\n=== 连续 10 包 × 2048 字节 全部通过 ✓ ===\n");
    } else {
        printf("\n=== 测试失败 ===\n");
    }
    return pass ? 0 : 1;
}
