#include "comm_route.h"
#include <string.h>

/* ===== 帧头大小 ===== */
#define CR_FRAME_HEADER_SIZE 5

/* ===== 内部结构定义 ===== */

typedef struct {
    const uint8_t *data;
    uint16_t       total_len;
    uint16_t       offset;
    uint8_t        dest;
    uint8_t        biz_id;
    uint8_t        seq;
    uint8_t        retries;
    uint8_t        state;
    uint32_t       last_tick;
    void         (*on_complete)(uint8_t status, void *user_ctx);
    void          *user_ctx;
} cr_tx_task_t;

typedef struct {
    uint8_t  src;
    uint8_t  biz_id;
    uint8_t  expected_seq;
    uint16_t received_len;
    uint8_t  active;
    uint32_t last_active_tick;
} cr_rx_slot_t;

typedef struct {
    uint8_t src;
    uint8_t seq;
} cr_dedup_entry_t;

typedef struct {
    cr_config_t         cfg;
    /* TX queue */
    cr_tx_task_t       *tx_queue;
    uint8_t             tx_head;
    uint8_t             tx_tail;
    uint8_t             tx_count;
    /* RX assembly */
    cr_rx_slot_t       *rx_slots;
    uint8_t            *rx_buffers;
    /* Broadcast dedup */
    cr_dedup_entry_t   *dedup_table;
    uint8_t             dedup_write_idx;
    /* Sequence counter */
    uint8_t             seq_counter;
} cr_internal_t;

/* ===== 辅助宏 ===== */
#define CR_GET_INTERNAL(inst) ((cr_internal_t *)((inst)->_internal))

/* ===== 实现 ===== */

size_t cr_calc_buffer_size(const cr_config_t *cfg) {
    size_t size = sizeof(cr_internal_t);
    size += sizeof(cr_tx_task_t) * cfg->tx_queue_depth;
    size += sizeof(cr_rx_slot_t) * cfg->rx_assem_count;
    size += (size_t)cfg->rx_buf_per_slot * cfg->rx_assem_count;
    size += sizeof(cr_dedup_entry_t) * cfg->dedup_table_size;
    return size;
}

int cr_init(cr_instance_t *inst, const cr_config_t *cfg,
            uint8_t *buffer, size_t buffer_size) {
    size_t required = cr_calc_buffer_size(cfg);
    if (buffer_size < required) {
        return -3;
    }

    uint8_t *ptr = buffer;

    /* Internal struct at start of buffer */
    cr_internal_t *self = (cr_internal_t *)ptr;
    ptr += sizeof(cr_internal_t);

    memset(self, 0, sizeof(cr_internal_t));
    self->cfg = *cfg;

    /* TX queue */
    self->tx_queue = (cr_tx_task_t *)ptr;
    ptr += sizeof(cr_tx_task_t) * cfg->tx_queue_depth;
    memset(self->tx_queue, 0, sizeof(cr_tx_task_t) * cfg->tx_queue_depth);

    /* RX slots */
    self->rx_slots = (cr_rx_slot_t *)ptr;
    ptr += sizeof(cr_rx_slot_t) * cfg->rx_assem_count;
    memset(self->rx_slots, 0, sizeof(cr_rx_slot_t) * cfg->rx_assem_count);

    /* RX buffers */
    self->rx_buffers = ptr;
    ptr += (size_t)cfg->rx_buf_per_slot * cfg->rx_assem_count;

    /* Dedup table */
    self->dedup_table = (cr_dedup_entry_t *)ptr;
    ptr += sizeof(cr_dedup_entry_t) * cfg->dedup_table_size;
    memset(self->dedup_table, 0, sizeof(cr_dedup_entry_t) * cfg->dedup_table_size);

    /* Link instance handle to internal */
    inst->_internal = self;

    return 0;
}
