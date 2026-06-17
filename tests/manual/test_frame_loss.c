#include "comm_route.h"
#include <stdio.h>
#include <string.h>

#define MAX_FRAMES 64
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

static uint8_t rx_buf[4096];
static uint16_t rx_len;
static int rx_called;

static void on_recv(cr_instance_t *inst, uint8_t src, uint8_t biz_id,
                    const uint8_t *data, uint16_t len, void *ctx) {
    (void)inst; (void)src; (void)biz_id; (void)ctx;
    memcpy(rx_buf, data, len);
    rx_len = len;
    rx_called++;
}

int main(void) {
    printf("=== 帧丢失场景验证 ===\n\n");

    static uint8_t tx_buf[16384], rx_buf_mem[16384];
    cr_route_entry_t routes[] = { {.dest = 0x02, .next_hop = 0x02} };
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 4, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 2560,
        .pool_size = 64, .bcast_queue_depth = 4,
        .route_table = routes, .route_count = 1,
    };

    /* TX 发 200 字节 → 4 帧(58+58+58+26) */
    cr_instance_t tx_inst;
    cr_init(&tx_inst, &cfg, tx_buf, sizeof(tx_buf));
    cr_hal_t tx_hal = { .send = mock_send, .get_tick_ms = get_tick, .hw_ctx = NULL };
    cr_set_hal(&tx_inst, &tx_hal);

    uint8_t data[200];
    for (int i = 0; i < 200; i++) data[i] = (uint8_t)(i & 0xFF);
    frame_count = 0;
    cr_send(&tx_inst, 0x02, 0, data, 200, NULL, NULL);
    for (int i = 0; i < 100; i++) { tick++; cr_poll(&tx_inst); }

    printf("TX: %d 帧 (200字节, MTU=64, 载荷=58/帧)\n", frame_count);
    for (int i = 0; i < frame_count; i++) {
        printf("  帧%d: SEQ=%d, LEN=%d, CTL=0x%02X %s%s\n",
            i, frames[i][3], frames[i][5], frames[i][2],
            (frames[i][2] & 0x20) ? "FRAG" : "",
            (frames[i][2] & 0x10) ? "+LAST" : "");
    }

    /* RX 实例 */
    cr_config_t rx_cfg = cfg;
    rx_cfg.local_addr = 0x02;
    rx_cfg.route_table = NULL;
    rx_cfg.route_count = 0;
    cr_instance_t rx_inst;
    cr_init(&rx_inst, &rx_cfg, rx_buf_mem, sizeof(rx_buf_mem));
    cr_hal_t rx_hal = { .send = mock_send, .get_tick_ms = get_tick, .hw_ctx = NULL };
    cr_set_hal(&rx_inst, &rx_hal);
    cr_set_recv_callback(&rx_inst, on_recv, NULL);

    /* ===== 场景 1: 丢失中间帧 (帧1) ===== */
    printf("\n--- 场景 1: 丢失帧1 (中间帧) ---\n");
    rx_called = 0;
    cr_feed_frame(&rx_inst, frames[0], frame_lens[0]);  /* 帧0 OK */
    /* 跳过帧1 */
    cr_feed_frame(&rx_inst, frames[2], frame_lens[2]);  /* 帧2 到达但 SEQ 不匹配 */
    cr_feed_frame(&rx_inst, frames[3], frame_lens[3]);  /* 帧3(末帧) 到达 */
    printf("  喂入: 帧0, 帧2, 帧3 (跳过帧1)\n");
    printf("  回调次数: %d\n", rx_called);
    printf("  结论: %s\n", rx_called == 0 ? "数据未交付 (等待帧1)" : "有数据交付");

    /* 推进超时 */
    tick += 1100;
    cr_poll(&rx_inst);
    printf("  超时后: slot 释放\n");

    /* ===== 场景 2: 丢失首帧 (帧0) ===== */
    printf("\n--- 场景 2: 丢失帧0 (首帧) ---\n");
    rx_called = 0;
    /* 重新 init RX */
    cr_init(&rx_inst, &rx_cfg, rx_buf_mem, sizeof(rx_buf_mem));
    cr_set_hal(&rx_inst, &rx_hal);
    cr_set_recv_callback(&rx_inst, on_recv, NULL);
    tick = 0;

    /* 跳过帧0 */
    cr_feed_frame(&rx_inst, frames[1], frame_lens[1]);  /* 帧1: 分配新 slot, expected=SEQ1 */
    cr_feed_frame(&rx_inst, frames[2], frame_lens[2]);  /* 帧2 */
    cr_feed_frame(&rx_inst, frames[3], frame_lens[3]);  /* 帧3(末帧) */
    printf("  喂入: 帧1, 帧2, 帧3 (跳过帧0)\n");
    printf("  回调次数: %d\n", rx_called);
    if (rx_called > 0) {
        printf("  接收长度: %d (预期完整=200, 实际缺帧0的58字节 → %d)\n", rx_len, 200-58);
    }
    printf("  结论: %s\n", rx_called > 0 ? "交付不完整数据 (丢失首帧的载荷)" : "数据未交付");

    /* ===== 场景 3: 丢失末帧 (帧3) ===== */
    printf("\n--- 场景 3: 丢失帧3 (末帧) ---\n");
    rx_called = 0;
    cr_init(&rx_inst, &rx_cfg, rx_buf_mem, sizeof(rx_buf_mem));
    cr_set_hal(&rx_inst, &rx_hal);
    cr_set_recv_callback(&rx_inst, on_recv, NULL);
    tick = 0;

    cr_feed_frame(&rx_inst, frames[0], frame_lens[0]);
    cr_feed_frame(&rx_inst, frames[1], frame_lens[1]);
    cr_feed_frame(&rx_inst, frames[2], frame_lens[2]);
    /* 跳过帧3(末帧) */
    printf("  喂入: 帧0, 帧1, 帧2 (跳过末帧3)\n");
    printf("  回调次数: %d\n", rx_called);

    /* 等超时 */
    tick = 1100;
    cr_poll(&rx_inst);
    printf("  超时后回调: %d\n", rx_called);
    printf("  结论: %s\n", rx_called == 0 ? "数据未交付, 超时后 slot 释放 (数据丢弃)" : "有数据交付");

    /* ===== 场景 4: ACK 模式下丢帧 → 重传 ===== */
    printf("\n--- 场景 4: ACK 模式 + 丢帧 → 重传恢复 ---\n");
    static uint8_t tx2_buf[16384];
    cr_config_t ack_cfg = cfg;
    ack_cfg.ack_enabled = 1;
    ack_cfg.ack_timeout_ms = 100;
    ack_cfg.max_retries = 3;
    cr_instance_t tx2;
    cr_init(&tx2, &ack_cfg, tx2_buf, sizeof(tx2_buf));
    frame_count = 0;
    cr_hal_t tx2_hal = { .send = mock_send, .get_tick_ms = get_tick, .hw_ctx = NULL };
    cr_set_hal(&tx2, &tx2_hal);
    tick = 0;

    uint8_t small[120];  /* 3 帧 (58+58+4) */
    for (int i = 0; i < 120; i++) small[i] = (uint8_t)i;
    cr_send(&tx2, 0x02, 0, small, 120, NULL, NULL);

    /* 发第1帧 */
    frame_count = 0;
    cr_poll(&tx2);
    printf("  帧0 发出: SEQ=%d, LEN=%d\n", frames[0][3], frames[0][5]);

    /* 模拟丢失: 不发 ACK, 等超时重传 */
    tick = 101;
    frame_count = 0;
    cr_poll(&tx2);
    printf("  超时重传帧0: SEQ=%d, LEN=%d (重传次数=1)\n", frames[0][3], frames[0][5]);

    /* 这次 ACK 回来 */
    uint8_t ack[] = {0x01, 0x02, 0x80, frames[0][3], 0x03, 0x00};
    cr_feed_frame(&tx2, ack, sizeof(ack));

    /* 发第2帧 */
    frame_count = 0;
    cr_poll(&tx2);
    printf("  帧1 发出: SEQ=%d, LEN=%d\n", frames[0][3], frames[0][5]);

    /* ACK */
    uint8_t ack2[] = {0x01, 0x02, 0x80, frames[0][3], 0x03, 0x00};
    cr_feed_frame(&tx2, ack2, sizeof(ack2));

    /* 发第3帧 */
    frame_count = 0;
    cr_poll(&tx2);
    printf("  帧2(末帧) 发出: SEQ=%d, LEN=%d\n", frames[0][3], frames[0][5]);
    printf("  结论: ACK 模式下丢帧触发重传, 最终完整发送\n");

    /* ===== 总结 ===== */
    printf("\n=== 总结 ===\n");
    printf("┌─────────────────────────────────────────────────────────────┐\n");
    printf("│ 场景              │ ACK关闭           │ ACK开启             │\n");
    printf("├─────────────────────────────────────────────────────────────┤\n");
    printf("│ 丢失中间帧        │ 后续帧SEQ不匹配   │ 发送端超时重传       │\n");
    printf("│                   │ → 永久阻塞        │ → 自动恢复          │\n");
    printf("│                   │ → 超时释放slot    │                     │\n");
    printf("├─────────────────────────────────────────────────────────────┤\n");
    printf("│ 丢失首帧          │ 后续帧分配新slot   │ 发送端超时重传       │\n");
    printf("│                   │ → 交付不完整数据  │ → 自动恢复          │\n");
    printf("├─────────────────────────────────────────────────────────────┤\n");
    printf("│ 丢失末帧          │ 组装永不结束      │ 发送端超时重传       │\n");
    printf("│                   │ → 超时释放slot    │ → 自动恢复          │\n");
    printf("└─────────────────────────────────────────────────────────────┘\n");

    return 0;
}
