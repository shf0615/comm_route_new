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
    /* HAL */
    const cr_hal_t     *hal;
    /* Recv callback */
    cr_on_recv_t        recv_cb;
    void               *recv_ctx;
    /* TX queue */
    cr_tx_task_t       *tx_queue;
    uint8_t             tx_head;
    uint8_t             tx_tail;
    uint8_t             tx_count;
    /* Active unicast slot */
    cr_tx_task_t       *active_unicast;
    /* Broadcast slot */
    cr_tx_task_t        bcast_task;
    uint8_t             bcast_pending;
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

void cr_set_hal(cr_instance_t *inst, const cr_hal_t *hal) {
    cr_internal_t *self = CR_GET_INTERNAL(inst);
    self->hal = hal;
}

void cr_set_recv_callback(cr_instance_t *inst, cr_on_recv_t cb, void *user_ctx) {
    cr_internal_t *self = CR_GET_INTERNAL(inst);
    self->recv_cb = cb;
    self->recv_ctx = user_ctx;
}

/* TX task states */
#define TX_STATE_IDLE    0
#define TX_STATE_SENDING 1
#define TX_STATE_WAIT_ACK 2

static uint8_t cr_route_lookup(cr_internal_t *self, uint8_t dest) {
    for (uint8_t i = 0; i < self->cfg.route_count; i++) {
        if (self->cfg.route_table[i].dest == dest) {
            return self->cfg.route_table[i].next_hop;
        }
    }
    return 0xFF; /* not found — use 0xFF as sentinel (broadcast addr never a valid next_hop) */
}

int cr_send(cr_instance_t *inst, uint8_t dest, uint8_t biz_id,
            const uint8_t *data, uint16_t len,
            void (*on_complete)(uint8_t status, void *ctx), void *user_ctx) {
    cr_internal_t *self = CR_GET_INTERNAL(inst);

    if (self->tx_count >= self->cfg.tx_queue_depth) {
        return -1;
    }

    cr_tx_task_t *task = &self->tx_queue[self->tx_tail];
    task->data = data;
    task->total_len = len;
    task->offset = 0;
    task->dest = dest;
    task->biz_id = biz_id & 0x0F;
    task->seq = self->seq_counter++;
    task->retries = 0;
    task->state = TX_STATE_SENDING;
    task->last_tick = 0;
    task->on_complete = on_complete;
    task->user_ctx = user_ctx;

    self->tx_tail = (self->tx_tail + 1) % self->cfg.tx_queue_depth;
    self->tx_count++;

    return 0;
}

int cr_broadcast(cr_instance_t *inst, uint8_t biz_id,
                 const uint8_t *data, uint16_t len) {
    cr_internal_t *self = CR_GET_INTERNAL(inst);

    if (len > self->cfg.mtu) {
        return -2;
    }

    self->bcast_task.data = data;
    self->bcast_task.total_len = len;
    self->bcast_task.offset = 0;
    self->bcast_task.dest = 0xFF;
    self->bcast_task.biz_id = biz_id & 0x0F;
    self->bcast_task.seq = self->seq_counter++;
    self->bcast_task.state = TX_STATE_SENDING;
    self->bcast_pending = 1;

    return 0;
}

static void cr_send_frame(cr_internal_t *self, cr_tx_task_t *task) {
    uint16_t payload_len = task->total_len - task->offset;
    uint8_t is_broadcast = (task->dest == 0xFF) ? 1 : 0;
    uint8_t is_multi = (task->total_len > self->cfg.mtu) ? 1 : 0;
    uint8_t is_last = 0;

    if (is_multi) {
        if (payload_len > self->cfg.mtu) {
            payload_len = self->cfg.mtu;
        } else {
            is_last = 1;
        }
    }

    uint8_t frame[CR_FRAME_HEADER_SIZE + 256]; /* max frame */
    frame[0] = task->dest;                     /* DST */
    frame[1] = self->cfg.local_addr;           /* SRC */
    /* CTL: bit7=ACK(0), bit6=broadcast, bit5=frag, bit4=last, bit3-0=biz_id */
    frame[2] = (uint8_t)((is_broadcast << 6) | (is_multi << 5) | (is_last << 4) | (task->biz_id & 0x0F));
    frame[3] = task->seq;                      /* SEQ */
    frame[4] = self->cfg.default_ttl;          /* TTL */

    memcpy(&frame[CR_FRAME_HEADER_SIZE], task->data + task->offset, payload_len);

    self->hal->send(self->hal->hw_ctx, frame, CR_FRAME_HEADER_SIZE + payload_len);

    task->offset += payload_len;
    task->last_tick = self->hal->get_tick_ms();

    if (is_multi && !is_last) {
        task->seq = self->seq_counter++;
    }
}

void cr_poll(cr_instance_t *inst) {
    cr_internal_t *self = CR_GET_INTERNAL(inst);

    /* Process broadcast slot (independent of unicast) */
    if (self->bcast_pending) {
        cr_send_frame(self, &self->bcast_task);
        self->bcast_pending = 0;
    }

    /* Process unicast queue */
    if (self->active_unicast == NULL && self->tx_count > 0) {
        self->active_unicast = &self->tx_queue[self->tx_head];
        self->tx_head = (self->tx_head + 1) % self->cfg.tx_queue_depth;
        self->tx_count--;
    }

    if (self->active_unicast != NULL) {
        cr_tx_task_t *task = self->active_unicast;

        if (task->state == TX_STATE_SENDING) {
            /* Check frame interval */
            uint32_t now = self->hal->get_tick_ms();
            if (task->offset > 0 && (now - task->last_tick) < self->cfg.frame_interval_ms) {
                return; /* wait for interval */
            }

            cr_send_frame(self, task);

            if (!self->cfg.ack_enabled) {
                /* Check if done */
                if (task->offset >= task->total_len) {
                    if (task->on_complete) {
                        task->on_complete(0, task->user_ctx);
                    }
                    self->active_unicast = NULL;
                }
                /* else: more frames to send, stay in SENDING */
            } else {
                task->state = TX_STATE_WAIT_ACK;
            }
        }
    }
}

void cr_feed_frame(cr_instance_t *inst, const uint8_t *data, uint16_t len) {
    (void)inst; (void)data; (void)len; /* stub */
}

void cr_notify_send_done(cr_instance_t *inst) {
    (void)inst; /* stub */
}
