/*
 * Bug 复现:发送长数据(多片单播)中途插入广播,导致接收端丢掉长数据。
 *
 * 根因:seq_counter 是实例级共享计数器,cr_send / cr_broadcast / cr_send_frame
 * 三处都从中递增。广播在长数据发送途中消耗一个 SEQ 号,使长数据分片的 SEQ
 * 不再连续;而 RX 重组端做严格顺序校验(seq == expected_seq),于是广播之后
 * 的某片被判定为乱序而丢弃,整条长数据永远无法重组交付。
 *
 * 编译运行:
 *   gcc tests/manual/test_bcast_seq_gap.c -Isrc -Lbuild -lcomm_route -o build/test_bcast_seq_gap
 *   ./build/test_bcast_seq_gap
 */
#include "comm_route.h"
#include <stdio.h>
#include <string.h>

#define MAX_FRAMES 64
static uint8_t frames[MAX_FRAMES][80];
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

static uint32_t tick_v = 0;
static uint32_t get_tick(void) { return tick_v; }

static uint8_t rx_data[4096];
static uint16_t rx_len;
static int rx_called;

static void on_recv(cr_instance_t *inst, uint8_t src, uint8_t biz_id,
                    const uint8_t *data, uint16_t len, void *ctx) {
    (void)inst; (void)src; (void)biz_id; (void)ctx;
    if (rx_called == 0) {
        memcpy(rx_data, data, len);
        rx_len = len;
    }
    rx_called++;
}

int main(void) {
    /* TX (A=0x01) */
    static uint8_t tx_buf[8192];
    cr_route_entry_t routes[] = { {.dest = 0x02, .next_hop = 0x02} };
    cr_config_t tx_cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 5000, .rx_buf_per_slot = 512,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = routes, .route_count = 1,
    };
    cr_instance_t tx;
    cr_init(&tx, &tx_cfg, tx_buf, sizeof(tx_buf));
    cr_hal_t tx_hal = { .send = mock_send, .get_tick_ms = get_tick, .hw_ctx = NULL };
    cr_set_hal(&tx, &tx_hal);

    /* RX (B=0x02) */
    static uint8_t rx_buf[8192];
    cr_config_t rx_cfg = {
        .local_addr = 0x02, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 5000, .rx_buf_per_slot = 512,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cr_instance_t rx;
    cr_init(&rx, &rx_cfg, rx_buf, sizeof(rx_buf));
    cr_hal_t rx_hal = { .send = mock_send, .get_tick_ms = get_tick, .hw_ctx = NULL };
    cr_set_hal(&rx, &rx_hal);
    cr_set_recv_callback(&rx, on_recv, NULL);

    /* 130 字节长数据 → 3 片 (58 + 58 + 14), payload_per_frame = 64 - 6 */
    uint8_t data[130];
    for (int i = 0; i < 130; i++) data[i] = (uint8_t)i;

    cr_send(&tx, 0x02, 0, data, 130, NULL, NULL);

    /* poll 发出片1 */
    frame_count = 0;
    tick_v++;
    cr_poll(&tx);

    /* 长数据发送中途:提交广播 */
    uint8_t bcast[] = "BC";
    cr_broadcast(&tx, 1, bcast, 2);

    /* 继续驱动:广播 + 片2 + 片3(LAST) */
    tick_v++;
    cr_poll(&tx);
    tick_v++;
    cr_poll(&tx);

    printf("A 共发出 %d 帧:\n", frame_count);
    for (int i = 0; i < frame_count; i++) {
        uint8_t ctl = frames[i][2];
        const char *tp = (ctl & 0x40) ? "BCAST" : (ctl & 0x20) ? "FRAG" : "DATA";
        const char *last = (ctl & 0x10) ? " LAST" : "";
        printf("  [%d] %-5s%s dst=0x%02X seq=%d len=%d\n",
               i, tp, last, frames[i][0], frames[i][3], frame_lens[i]);
    }

    /* 把所有单播帧(dst=0x02)喂给 B */
    for (int i = 0; i < frame_count; i++) {
        if (frames[i][0] == 0x02) {
            cr_feed_frame(&rx, frames[i], frame_lens[i]);
        }
    }

    printf("\nB 接收结果: rx_called=%d, rx_len=%d (期望 rx_called=1, rx_len=130)\n",
           rx_called, rx_len);

    int pass = (rx_called == 1 && rx_len == 130);
    if (pass) {
        for (int i = 0; i < 130; i++) {
            if (rx_data[i] != (uint8_t)i) { pass = 0; break; }
        }
    }

    printf("\n%s\n", pass ? "=== PASS: B 完整收到 130 字节 ==="
                          : "=== FAIL: B 丢失长数据 (Bug 已复现) ===");
    return pass ? 0 : 1;
}
