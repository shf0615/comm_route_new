#include "comm_route.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

/* 模拟 OTA: 300 包 × 2048 字节 = 600KB, ACK 模式 */

#define OTA_PKT_SIZE   2048
#define OTA_PKT_COUNT  300
#define MTU            64
#define HEADER_SIZE    6
#define PAYLOAD_PER    (MTU - HEADER_SIZE)  /* 58 */
#define FRAMES_PER_PKT ((OTA_PKT_SIZE + PAYLOAD_PER - 1) / PAYLOAD_PER) /* 36 */

/* TX→RX 虚拟链路 */
static uint8_t link_buf[64];
static uint16_t link_len;
static int link_has_data;

/* RX→TX ACK 回传 */
static uint8_t ack_buf[64];
static uint16_t ack_len;
static int ack_has_data;

static uint32_t g_tick = 0;
static uint32_t get_tick(void) { return g_tick; }

/* TX send: 数据帧放入链路 */
static int tx_send(void *ctx, uint8_t next_hop, const uint8_t *data, uint16_t len) {
    (void)ctx; (void)next_hop;
    memcpy(link_buf, data, len);
    link_len = len;
    link_has_data = 1;
    return 0;
}

/* RX send: ACK 回传 */
static int rx_send(void *ctx, uint8_t next_hop, const uint8_t *data, uint16_t len) {
    (void)ctx; (void)next_hop;
    memcpy(ack_buf, data, len);
    ack_len = len;
    ack_has_data = 1;
    return 0;
}

/* RX 接收统计 */
static int rx_pkt_count;
static uint16_t rx_pkt_lens[OTA_PKT_COUNT];
static int rx_data_errors;

static void on_recv(cr_instance_t *inst, uint8_t src, uint8_t biz_id,
                    const uint8_t *data, uint16_t len, void *ctx) {
    (void)inst; (void)src; (void)biz_id; (void)ctx;
    if (rx_pkt_count < OTA_PKT_COUNT) {
        rx_pkt_lens[rx_pkt_count] = len;
        /* 校验数据 */
        uint8_t seed = (uint8_t)(rx_pkt_count & 0xFF);
        for (uint16_t i = 0; i < len; i++) {
            if (data[i] != (uint8_t)((i + seed) & 0xFF))
                rx_data_errors++;
        }
    }
    rx_pkt_count++;
}

/* TX 完成统计 */
static int tx_done_count;
static int tx_fail_count;
static void on_done(uint8_t status, void *ctx) {
    (void)ctx;
    tx_done_count++;
    if (status != 0) tx_fail_count++;
}

int main(void) {
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║  OTA 压测: %d 包 × %d 字节 = %d KB           ║\n",
           OTA_PKT_COUNT, OTA_PKT_SIZE, OTA_PKT_COUNT * OTA_PKT_SIZE / 1024);
    printf("║  MTU=%d, 载荷=%d/帧, %d 帧/包, 共 %d 帧       ║\n",
           MTU, PAYLOAD_PER, FRAMES_PER_PKT, FRAMES_PER_PKT * OTA_PKT_COUNT);
    printf("║  ACK 模式开启, 逐帧确认                        ║\n");
    printf("╚══════════════════════════════════════════════════╝\n\n");

    clock_t t_start = clock();

    /* TX 实例 */
    static uint8_t tx_mem[32768];
    cr_route_entry_t routes[] = { {.dest = 0x02, .next_hop = 0x02} };
    cr_config_t tx_cfg = {
        .local_addr = 0x01, .mtu = MTU, .frame_interval_ms = 0,
        .ack_enabled = 1, .ack_mode = CR_ACK_MODE_REPLY,
        .ack_timeout_ms = 200, .max_retries = 5,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 5000, .rx_buf_per_slot = 2560,
        .pool_size = 200, .bcast_queue_depth = 4,
        .route_table = routes, .route_count = 1,
    };
    cr_instance_t tx_inst;
    if (cr_init(&tx_inst, &tx_cfg, tx_mem, sizeof(tx_mem)) != 0) {
        printf("FAIL: TX init\n"); return 1;
    }
    cr_hal_t tx_hal = { .send = tx_send, .get_tick_ms = get_tick, .hw_ctx = NULL };
    cr_set_hal(&tx_inst, &tx_hal);

    /* RX 实例 */
    static uint8_t rx_mem[32768];
    cr_config_t rx_cfg = {
        .local_addr = 0x02, .mtu = MTU, .frame_interval_ms = 0,
        .ack_enabled = 1, .ack_mode = CR_ACK_MODE_REPLY,
        .ack_timeout_ms = 200, .max_retries = 5,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 8,
        .rx_assem_timeout_ms = 5000, .rx_buf_per_slot = 2560,
        .pool_size = 200, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cr_instance_t rx_inst;
    if (cr_init(&rx_inst, &rx_cfg, rx_mem, sizeof(rx_mem)) != 0) {
        printf("FAIL: RX init\n"); return 1;
    }
    cr_hal_t rx_hal = { .send = rx_send, .get_tick_ms = get_tick, .hw_ctx = NULL };
    cr_set_hal(&rx_inst, &rx_hal);
    cr_set_recv_callback(&rx_inst, on_recv, NULL);

    /* 逐包发送 */
    int total_ticks = 0;
    int total_frames = 0;

    for (int pkt = 0; pkt < OTA_PKT_COUNT; pkt++) {
        /* 构造数据 */
        uint8_t data[OTA_PKT_SIZE];
        uint8_t seed = (uint8_t)(pkt & 0xFF);
        for (int i = 0; i < OTA_PKT_SIZE; i++)
            data[i] = (uint8_t)((i + seed) & 0xFF);

        cr_send(&tx_inst, 0x02, 0, data, OTA_PKT_SIZE, on_done, NULL);

        /* 驱动 TX↔RX 交互直到本包完成 */
        int prev_done = tx_done_count;
        int safety = 0;
        while (tx_done_count == prev_done && safety < 50000) {
            g_tick++;
            total_ticks++;
            link_has_data = 0;
            ack_has_data = 0;

            cr_poll(&tx_inst);

            /* TX→RX: 投递数据帧 */
            if (link_has_data) {
                total_frames++;
                cr_feed_frame(&rx_inst, link_buf, link_len);
            }

            /* RX→TX: 投递 ACK */
            if (ack_has_data) {
                cr_feed_frame(&tx_inst, ack_buf, ack_len);
            }

            safety++;
        }

        if (safety >= 50000) {
            printf("FAIL: 包 %d 超时 (死循环)\n", pkt);
            return 1;
        }

        /* 进度条 */
        if ((pkt + 1) % 30 == 0 || pkt == OTA_PKT_COUNT - 1) {
            int pct = (pkt + 1) * 100 / OTA_PKT_COUNT;
            printf("  [");
            for (int j = 0; j < 40; j++)
                printf("%c", j < pct * 40 / 100 ? '#' : '.');
            printf("] %3d%%  %d/%d 包  RX=%d\n", pct, pkt + 1, OTA_PKT_COUNT, rx_pkt_count);
        }
    }

    clock_t t_end = clock();
    double elapsed = (double)(t_end - t_start) / CLOCKS_PER_SEC;

    /* 结果 */
    printf("\n═══ 结果 ═══\n");
    printf("  发送包数:      %d / %d\n", tx_done_count, OTA_PKT_COUNT);
    printf("  发送失败:      %d\n", tx_fail_count);
    printf("  接收包数:      %d / %d\n", rx_pkt_count, OTA_PKT_COUNT);
    printf("  总帧数:        %d (预期 %d)\n", total_frames, FRAMES_PER_PKT * OTA_PKT_COUNT);
    printf("  总数据量:      %d KB\n", OTA_PKT_COUNT * OTA_PKT_SIZE / 1024);
    printf("  数据校验错误:  %d\n", rx_data_errors);
    printf("  模拟 tick 数:  %d\n", total_ticks);
    printf("  执行耗时:      %.3f 秒\n", elapsed);
    printf("  吞吐量:        %.1f KB/s (模拟时间)\n",
           (double)(OTA_PKT_COUNT * OTA_PKT_SIZE) / 1024.0 / elapsed);

    /* 抽检每包长度 */
    int len_errors = 0;
    for (int i = 0; i < rx_pkt_count && i < OTA_PKT_COUNT; i++) {
        if (rx_pkt_lens[i] != OTA_PKT_SIZE) len_errors++;
    }
    printf("  包长度错误:    %d\n", len_errors);

    int pass = (tx_done_count == OTA_PKT_COUNT &&
                tx_fail_count == 0 &&
                rx_pkt_count == OTA_PKT_COUNT &&
                rx_data_errors == 0 &&
                len_errors == 0);

    printf("\n%s\n", pass
        ? "═══ OTA 压测通过 ✓ ═══"
        : "═══ OTA 压测失败 ✗ ═══");
    return pass ? 0 : 1;
}
