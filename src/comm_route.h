#ifndef COMM_ROUTE_H
#define COMM_ROUTE_H

#include <stdint.h>
#include <stddef.h>

/* ===== 类型定义 ===== */

typedef enum {
    CR_ACK_MODE_REPLY = 0,
    CR_ACK_MODE_INTERRUPT = 1,
} cr_ack_mode_t;

typedef struct {
    uint8_t dest;
    uint8_t next_hop;
} cr_route_entry_t;

typedef struct {
    uint8_t                 local_addr;
    uint16_t                mtu;
    uint16_t                frame_interval_ms;
    uint8_t                 max_retries;
    uint16_t                ack_timeout_ms;
    uint8_t                 ack_enabled;
    cr_ack_mode_t           ack_mode;
    uint8_t                 default_ttl;
    uint8_t                 tx_queue_depth;
    uint8_t                 rx_assem_count;
    uint8_t                 dedup_table_size;
    uint16_t                rx_assem_timeout_ms;
    uint16_t                rx_buf_per_slot;
    const cr_route_entry_t *route_table;
    uint8_t                 route_count;
} cr_config_t;

/* 实例句柄 — 用户可在栈上声明 */
typedef struct {
    void *_internal;
} cr_instance_t;

/* ===== API ===== */

size_t cr_calc_buffer_size(const cr_config_t *cfg);
int    cr_init(cr_instance_t *inst, const cr_config_t *cfg,
               uint8_t *buffer, size_t buffer_size);

#endif /* COMM_ROUTE_H */
