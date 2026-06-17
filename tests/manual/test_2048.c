#include "comm_route.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* 记录发送的所有帧 */
#define MAX_FRAMES 256
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

static void on_recv(cr_instance_t *inst, uint8_t src, uint8_t biz_id,
                    const uint8_t *data, uint16_t len, void *ctx) {
    (void)inst; (void)src; (void)biz_id; (void)ctx;
    memcpy(rx_buf, data, len);
    rx_len = len;
    rx_called++;
}

int main(void) {
    printf("=== 2048 字节长数据拆帧+组包验证 ===\n\n");

    /* ===== TX 侧：拆帧验证 ===== */
    uint8_t buffer[16384];
    cr_route_entry_t routes[] = { {.dest = 0x02, .next_hop = 0x02} };
    cr_config_t cfg = {
        .local_addr = 0x01, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 5000, .rx_buf_per_slot = 2560,
        .pool_size = 128, .bcast_queue_depth = 4,
        .route_table = routes, .route_count = 1,
    };
    cr_instance_t tx_inst;
    int ret = cr_init(&tx_inst, &cfg, buffer, sizeof(buffer));
    if (ret != 0) {
        printf("FAIL: cr_init (TX) returned %d\n", ret);
        return 1;
    }

    cr_hal_t hal = { .send = mock_send, .get_tick_ms = get_tick, .hw_ctx = NULL };
    cr_set_hal(&tx_inst, &hal);

    /* 构造 2048 字节数据 */
    uint8_t data[2048];
    for (int i = 0; i < 2048; i++) data[i] = (uint8_t)(i & 0xFF);

    frame_count = 0;
    tick = 0;
    ret = cr_send(&tx_inst, 0x02, 0, data, 2048, NULL, NULL);
    if (ret != 0) {
        printf("FAIL: cr_send returned %d\n", ret);
        return 1;
    }

    /* 发送所有帧 */
    for (int i = 0; i < 300; i++) {
        tick = (uint32_t)(i * 1);
        cr_poll(&tx_inst);
    }

    /* 计算预期帧数: payload_per_frame = MTU - 6 = 58, 2048/58 = 35.3 → 36 帧 */
    int expected_frames = (2048 + 57) / 58;  /* ceil(2048/58) = 36 */
    printf("TX 结果:\n");
    printf("  数据长度: 2048 字节\n");
    printf("  MTU: 64, 帧头: 6, 单帧最大载荷: 58\n");
    printf("  预期帧数: %d\n", expected_frames);
    printf("  实际帧数: %d\n", frame_count);

    if (frame_count != expected_frames) {
        printf("  FAIL: 帧数不匹配!\n");
        return 1;
    }

    /* 验证每帧 LEN */
    int total_payload = 0;
    int len_errors = 0;
    for (int i = 0; i < frame_count; i++) {
        uint8_t len_field = frames[i][5];  /* LEN 字段 */
        uint16_t actual_payload = frame_lens[i] - 6;

        if (len_field != actual_payload) {
            printf("  FAIL: 帧 %d LEN=%d 但实际载荷=%d\n", i, len_field, actual_payload);
            len_errors++;
        }
        total_payload += len_field;

        /* 验证分片标记 */
        uint8_t ctl = frames[i][2];
        int is_frag = (ctl & 0x20) != 0;
        int is_last = (ctl & 0x10) != 0;

        if (!is_frag) {
            printf("  FAIL: 帧 %d 应该有分片标记\n", i);
            len_errors++;
        }
        if (i < frame_count - 1 && is_last) {
            printf("  FAIL: 帧 %d 不应有末帧标记\n", i);
            len_errors++;
        }
        if (i == frame_count - 1 && !is_last) {
            printf("  FAIL: 最后一帧应有末帧标记\n");
            len_errors++;
        }
    }
    printf("  载荷总和: %d (预期 2048)\n", total_payload);
    printf("  LEN 字段错误: %d\n", len_errors);

    if (total_payload != 2048 || len_errors > 0) {
        printf("  FAIL!\n");
        return 1;
    }
    printf("  TX 拆帧: PASS ✓\n\n");

    /* ===== RX 侧：组包验证 ===== */
    printf("RX 组包:\n");

    uint8_t rx_buffer[16384];
    cr_config_t rx_cfg = {
        .local_addr = 0x02, .mtu = 64, .frame_interval_ms = 0,
        .ack_enabled = 0, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 5000, .rx_buf_per_slot = 2560,
        .pool_size = 128, .bcast_queue_depth = 4,
        .route_table = NULL, .route_count = 0,
    };
    cr_instance_t rx_inst;
    ret = cr_init(&rx_inst, &rx_cfg, rx_buffer, sizeof(rx_buffer));
    if (ret != 0) {
        printf("  FAIL: cr_init (RX) returned %d\n", ret);
        return 1;
    }
    cr_hal_t rx_hal = { .send = mock_send, .get_tick_ms = get_tick, .hw_ctx = NULL };
    cr_set_hal(&rx_inst, &rx_hal);
    cr_set_recv_callback(&rx_inst, on_recv, NULL);

    rx_called = 0;
    rx_len = 0;

    /* 将 TX 发出的所有帧喂给 RX */
    for (int i = 0; i < frame_count; i++) {
        cr_feed_frame(&rx_inst, frames[i], frame_lens[i]);
    }

    printf("  喂入帧数: %d\n", frame_count);
    printf("  接收回调次数: %d (预期 1)\n", rx_called);
    printf("  接收数据长度: %d (预期 2048)\n", rx_len);

    if (rx_called != 1 || rx_len != 2048) {
        printf("  FAIL: 组包失败!\n");
        return 1;
    }

    /* 逐字节验证数据完整性 */
    int data_errors = 0;
    for (int i = 0; i < 2048; i++) {
        if (rx_buf[i] != (uint8_t)(i & 0xFF)) {
            if (data_errors < 5)
                printf("  FAIL: 字节 %d 期望 0x%02X 实际 0x%02X\n", i, (uint8_t)(i & 0xFF), rx_buf[i]);
            data_errors++;
        }
    }
    printf("  数据校验错误: %d\n", data_errors);

    if (data_errors > 0) {
        printf("  FAIL!\n");
        return 1;
    }
    printf("  RX 组包: PASS ✓\n\n");

    printf("=== 2048 字节拆帧+组包 全部通过 ✓ ===\n");
    return 0;
}
