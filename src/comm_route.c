#include "comm_route.h"
#include <string.h>
#include <stdint.h>

/* ===== 常量定义 ===== */
#define CR_FRAME_HEADER_SIZE 6

/* Alignment for internal structs (conservative: 4 bytes for uint32_t on ARM) */
#define CR_ALIGN  4

#define CR_BROADCAST_ADDR    0xFF
#define CR_POOL_NONE         0xFF  /* 空闲链表结束标记 / 无效 block 索引 */
#define CR_CTL_ACK_BIT       0x80
#define CR_CTL_BROADCAST_BIT 0x40
#define CR_CTL_FRAG_BIT      0x20
#define CR_CTL_LAST_BIT      0x10

/* CTL bit-field extraction macros */
#define CR_CTL_IS_ACK(ctl)       (((ctl) & CR_CTL_ACK_BIT) != 0)
#define CR_CTL_IS_BROADCAST(ctl) (((ctl) & CR_CTL_BROADCAST_BIT) != 0)
#define CR_CTL_IS_FRAG(ctl)      (((ctl) & CR_CTL_FRAG_BIT) != 0)
#define CR_CTL_IS_LAST(ctl)      (((ctl) & CR_CTL_LAST_BIT) != 0)
#define CR_CTL_BIZ_ID(ctl)       ((ctl) & 0x0F)

/* 错误码 */
#define CR_ERR_QUEUE_FULL    (-1)
#define CR_ERR_PARAM         (-2)
#define CR_ERR_BUFFER        (-3)
#define CR_ERR_POOL_FULL     (-4)

/* Completion status codes passed to on_complete callback */
#define CR_STATUS_OK         0
#define CR_STATUS_FAIL       1

/* TX task states */
#define TX_STATE_IDLE    0
#define TX_STATE_SENDING 1
#define TX_STATE_WAIT_ACK 2

/* ===== 内部结构定义 ===== */

typedef struct {
    uint16_t       total_len;
    uint16_t       offset;
    uint16_t       frame_offset;  /* offset before last send (for retransmit) */
    uint8_t        head_block;    /* block 链头索引, CR_POOL_NONE = 无 */
    uint8_t        num_blocks;    /* 占用 block 数 */
    uint8_t        dest;
    uint8_t        next_hop;      /* resolved at cr_send time */
    uint8_t        biz_id;
    uint8_t        seq;
    uint8_t        ack_seq;       /* SEQ of last sent frame (for ACK matching) */
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
    uint8_t  head_block;     /* 分片 block 链头 */
    uint8_t  tail_block;     /* 分片 block 链尾 */
    uint8_t  num_blocks;     /* 占用 block 数 */
    uint32_t last_active_tick;
} cr_rx_slot_t;

typedef struct {
    uint8_t src;
    uint8_t seq;
    uint8_t valid;
} cr_dedup_entry_t;

typedef struct {
    cr_config_t         cfg;
    /* HAL */
    const cr_hal_t     *hal;
    /* Recv callback */
    cr_on_recv_t        recv_cb;
    void               *recv_ctx;
    /* Memory pool */
    uint8_t            *pool_data;       /* block 数据基地址, pool_size * mtu */
    uint8_t            *pool_next;       /* 链表 next 数组, pool_size bytes */
    uint8_t             pool_free_head;  /* 空闲链表头, CR_POOL_NONE = 耗尽 */
    uint8_t             scratch_block;   /* 预留的帧构建 block */
    /* TX queue */
    cr_tx_task_t       *tx_queue;
    uint8_t             tx_head;
    uint8_t             tx_tail;
    uint8_t             tx_count;
    /* Active unicast slot */
    cr_tx_task_t       *active_unicast;
    /* Broadcast queue */
    cr_tx_task_t       *bcast_queue;
    uint8_t             bcast_head;
    uint8_t             bcast_tail;
    uint8_t             bcast_count;
    /* RX assembly */
    cr_rx_slot_t       *rx_slots;
    uint8_t            *rx_delivery_buf; /* 共享交付缓冲区, rx_buf_per_slot bytes */
    uint8_t             rx_active_count;
    /* Broadcast dedup */
    cr_dedup_entry_t   *dedup_table;
    uint8_t             dedup_write_idx;
    /* Sequence counter */
    uint8_t             seq_counter;
    /* Interrupt ACK flag
     * Design assumption: single-core, ISR sets flag (cr_notify_send_done),
     * main loop reads and clears. No lock needed — volatile ensures visibility.
     * NOT safe for multi-core without memory barrier. */
    volatile uint8_t    send_done_flag;
} cr_internal_t;

/* ===== 辅助宏 ===== */
#define CR_GET_INTERNAL(inst) ((cr_internal_t *)((inst)->_internal))
#define CR_POOL_PTR(self, idx) ((self)->pool_data + (size_t)(idx) * (self)->cfg.mtu)

/* ===== 内存池管理 ===== */

static uint8_t cr_pool_alloc(cr_internal_t *self) {
    uint8_t idx = self->pool_free_head;
    if (idx == CR_POOL_NONE) {
        return CR_POOL_NONE;
    }
    self->pool_free_head = self->pool_next[idx];
    self->pool_next[idx] = CR_POOL_NONE;
    return idx;
}

static void cr_pool_free(cr_internal_t *self, uint8_t idx) {
    if (idx == CR_POOL_NONE) {
        return;
    }
    self->pool_next[idx] = self->pool_free_head;
    self->pool_free_head = idx;
}

static void cr_pool_free_chain(cr_internal_t *self, uint8_t head) {
    while (head != CR_POOL_NONE) {
        uint8_t next = self->pool_next[head];
        self->pool_next[head] = self->pool_free_head;
        self->pool_free_head = head;
        head = next;
    }
}

/* ===== 初始化与配置 ===== */

size_t cr_calc_buffer_size(const cr_config_t *cfg) {
    if (cfg == NULL) {
        return 0;
    }
    if (cfg->tx_queue_depth == 0 || cfg->bcast_queue_depth == 0 ||
        cfg->dedup_table_size == 0 || cfg->mtu == 0 || cfg->pool_size == 0) {
        return 0;
    }
    size_t size = sizeof(cr_internal_t);
    size += sizeof(cr_tx_task_t) * cfg->tx_queue_depth;       /* TX queue */
    size += sizeof(cr_tx_task_t) * cfg->bcast_queue_depth;    /* Bcast queue */
    size += sizeof(cr_rx_slot_t) * cfg->rx_assem_count;       /* RX slots */
    size += sizeof(cr_dedup_entry_t) * cfg->dedup_table_size; /* Dedup table */
    size += (size_t)cfg->mtu * cfg->pool_size;                /* Pool data */
    size += (size_t)cfg->pool_size;                           /* Pool next array */
    size += (size_t)cfg->rx_buf_per_slot;                     /* RX delivery buffer */
    /* Reserve extra bytes for worst-case alignment adjustment */
    size += CR_ALIGN - 1;
    return size;
}

int cr_init(cr_instance_t *inst, const cr_config_t *cfg,
            uint8_t *buffer, size_t buffer_size) {
    if (inst == NULL || cfg == NULL || buffer == NULL) {
        return CR_ERR_PARAM;
    }

    /* Validate config fields */
    if (cfg->tx_queue_depth == 0 || cfg->bcast_queue_depth == 0 ||
        cfg->dedup_table_size == 0 || cfg->mtu == 0 || cfg->pool_size == 0) {
        return CR_ERR_PARAM;
    }
    if (cfg->mtu <= CR_FRAME_HEADER_SIZE) {
        return CR_ERR_PARAM;
    }

    size_t required = cr_calc_buffer_size(cfg);
    if (buffer_size < required) {
        return CR_ERR_BUFFER;
    }

    /* Align buffer to CR_ALIGN boundary */
    uintptr_t misalign = (uintptr_t)buffer % CR_ALIGN;
    if (misalign != 0) {
        size_t pad = CR_ALIGN - misalign;
        buffer += pad;
        buffer_size -= pad;
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

    /* Broadcast queue */
    self->bcast_queue = (cr_tx_task_t *)ptr;
    ptr += sizeof(cr_tx_task_t) * cfg->bcast_queue_depth;
    memset(self->bcast_queue, 0, sizeof(cr_tx_task_t) * cfg->bcast_queue_depth);

    /* RX slots */
    self->rx_slots = (cr_rx_slot_t *)ptr;
    ptr += sizeof(cr_rx_slot_t) * cfg->rx_assem_count;
    memset(self->rx_slots, 0, sizeof(cr_rx_slot_t) * cfg->rx_assem_count);

    /* Dedup table */
    self->dedup_table = (cr_dedup_entry_t *)ptr;
    ptr += sizeof(cr_dedup_entry_t) * cfg->dedup_table_size;
    memset(self->dedup_table, 0, sizeof(cr_dedup_entry_t) * cfg->dedup_table_size);

    /* Memory pool data */
    self->pool_data = ptr;
    ptr += (size_t)cfg->mtu * cfg->pool_size;

    /* Memory pool next array */
    self->pool_next = ptr;
    ptr += (size_t)cfg->pool_size;

    /* Initialize free list: 0 -> 1 -> 2 -> ... -> pool_size-1 -> NONE */
    for (uint8_t i = 0; i < cfg->pool_size - 1; i++) {
        self->pool_next[i] = i + 1;
    }
    self->pool_next[cfg->pool_size - 1] = CR_POOL_NONE;
    self->pool_free_head = 0;

    /* Reserve scratch block from pool */
    self->scratch_block = cr_pool_alloc(self);

    /* RX delivery buffer */
    self->rx_delivery_buf = ptr;
    ptr += (size_t)cfg->rx_buf_per_slot;

    /* Link instance handle to internal */
    inst->_internal = self;

    return 0;
}

void cr_set_hal(cr_instance_t *inst, const cr_hal_t *hal) {
    if (inst == NULL) {
        return;
    }
    cr_internal_t *self = CR_GET_INTERNAL(inst);
    self->hal = hal;
}

void cr_set_recv_callback(cr_instance_t *inst, cr_on_recv_t cb, void *user_ctx) {
    if (inst == NULL) {
        return;
    }
    cr_internal_t *self = CR_GET_INTERNAL(inst);
    self->recv_cb = cb;
    self->recv_ctx = user_ctx;
}

/* ===== 路由与发送 ===== */

static uint8_t cr_route_lookup(cr_internal_t *self, uint8_t dest) {
    for (uint8_t i = 0; i < self->cfg.route_count; i++) {
        if (self->cfg.route_table[i].dest == dest) {
            return self->cfg.route_table[i].next_hop;
        }
    }
    return CR_BROADCAST_ADDR; /* not found — broadcast addr as sentinel (never a valid next_hop) */
}

int cr_send(cr_instance_t *inst, uint8_t dest, uint8_t biz_id,
            const uint8_t *data, uint16_t len,
            void (*on_complete)(uint8_t status, void *ctx), void *user_ctx) {
    if (inst == NULL || (data == NULL && len > 0)) {
        return CR_ERR_PARAM;
    }
    cr_internal_t *self = CR_GET_INTERNAL(inst);

    if (self->tx_count >= self->cfg.tx_queue_depth) {
        return CR_ERR_QUEUE_FULL;
    }

    /* Calculate blocks needed */
    uint16_t payload_per_frame = self->cfg.mtu - CR_FRAME_HEADER_SIZE;
    uint8_t num_blocks = 0;
    if (len > 0) {
        num_blocks = (uint8_t)((len + payload_per_frame - 1) / payload_per_frame);
    }

    /* Allocate block chain */
    uint8_t head = CR_POOL_NONE;
    uint8_t prev = CR_POOL_NONE;
    for (uint8_t i = 0; i < num_blocks; i++) {
        uint8_t blk = cr_pool_alloc(self);
        if (blk == CR_POOL_NONE) {
            /* Allocation failed — free already allocated chain */
            cr_pool_free_chain(self, head);
            return CR_ERR_POOL_FULL;
        }
        if (head == CR_POOL_NONE) {
            head = blk;
        } else {
            self->pool_next[prev] = blk;
        }
        prev = blk;

        /* Copy user data into this block */
        uint16_t copy_offset = (uint16_t)i * payload_per_frame;
        uint16_t copy_len = payload_per_frame;
        if (copy_offset + copy_len > len) {
            copy_len = len - copy_offset;
        }
        memcpy(CR_POOL_PTR(self, blk), data + copy_offset, copy_len);
    }

    cr_tx_task_t *task = &self->tx_queue[self->tx_tail];
    task->head_block = head;
    task->num_blocks = num_blocks;
    task->total_len = len;
    task->offset = 0;
    task->dest = dest;
    task->next_hop = cr_route_lookup(self, dest);
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
    if (inst == NULL || (data == NULL && len > 0)) {
        return CR_ERR_PARAM;
    }
    cr_internal_t *self = CR_GET_INTERNAL(inst);

    if (self->bcast_count >= self->cfg.bcast_queue_depth) {
        return CR_ERR_QUEUE_FULL;
    }

    uint16_t payload_per_frame = self->cfg.mtu - CR_FRAME_HEADER_SIZE;
    if (len > payload_per_frame) {
        return CR_ERR_PARAM;  /* 广播不支持分片 */
    }

    /* Allocate 1 block */
    uint8_t blk = CR_POOL_NONE;
    if (len > 0) {
        blk = cr_pool_alloc(self);
        if (blk == CR_POOL_NONE) {
            return CR_ERR_POOL_FULL;
        }
        memcpy(CR_POOL_PTR(self, blk), data, len);
    }

    cr_tx_task_t *task = &self->bcast_queue[self->bcast_tail];
    task->head_block = blk;
    task->num_blocks = (len > 0) ? 1 : 0;
    task->total_len = len;
    task->offset = 0;
    task->dest = CR_BROADCAST_ADDR;
    task->biz_id = biz_id & 0x0F;
    task->seq = self->seq_counter++;
    task->state = TX_STATE_SENDING;

    self->bcast_tail = (self->bcast_tail + 1) % self->cfg.bcast_queue_depth;
    self->bcast_count++;

    return 0;
}

/* ===== TX 状态机 ===== */

static void cr_finish_active_task(cr_internal_t *self, uint8_t status) {
    cr_tx_task_t *task = self->active_unicast;
    /* Free remaining block chain */
    cr_pool_free_chain(self, task->head_block);
    task->head_block = CR_POOL_NONE;
    task->num_blocks = 0;
    if (task->on_complete) {
        task->on_complete(status, task->user_ctx);
    }
    self->active_unicast = NULL;
}

static void cr_send_frame(cr_internal_t *self, cr_tx_task_t *task, uint8_t next_hop) {
    uint16_t payload_per_frame = self->cfg.mtu - CR_FRAME_HEADER_SIZE;
    uint16_t remaining = task->total_len - task->offset;
    uint8_t is_broadcast = (task->dest == CR_BROADCAST_ADDR) ? 1 : 0;
    uint8_t is_multi = (task->total_len > payload_per_frame) ? 1 : 0;
    uint8_t is_last = 0;

    uint16_t payload_len = remaining;
    if (is_multi) {
        if (payload_len > payload_per_frame) {
            payload_len = payload_per_frame;
        } else {
            is_last = 1;
        }
    }

    /* Build frame in scratch block */
    uint8_t *frame = CR_POOL_PTR(self, self->scratch_block);
    frame[0] = task->dest;                     /* DST */
    frame[1] = self->cfg.local_addr;           /* SRC */
    frame[2] = (uint8_t)((is_broadcast ? CR_CTL_BROADCAST_BIT : 0) |
                          (is_multi ? CR_CTL_FRAG_BIT : 0) |
                          (is_last ? CR_CTL_LAST_BIT : 0) |
                          (task->biz_id & 0x0F));
    frame[3] = task->seq;                      /* SEQ */
    frame[4] = self->cfg.default_ttl;          /* TTL */
    frame[5] = (uint8_t)payload_len;           /* LEN */

    /* Copy payload from head_block */
    if (task->head_block != CR_POOL_NONE && payload_len > 0) {
        memcpy(&frame[CR_FRAME_HEADER_SIZE], CR_POOL_PTR(self, task->head_block), payload_len);
    }

    self->hal->send(self->hal->hw_ctx, next_hop, frame, CR_FRAME_HEADER_SIZE + payload_len);

    task->ack_seq = task->seq;
    task->frame_offset = task->offset;
    task->offset += payload_len;
    task->last_tick = self->hal->get_tick_ms();

    if (is_multi && !is_last) {
        task->seq = self->seq_counter++;
    }
}

/* Release head block and advance chain to next block */
static void cr_tx_advance_block(cr_internal_t *self, cr_tx_task_t *task) {
    if (task->head_block == CR_POOL_NONE) {
        return;
    }
    uint8_t consumed = task->head_block;
    task->head_block = self->pool_next[consumed];
    self->pool_next[consumed] = CR_POOL_NONE;
    cr_pool_free(self, consumed);
    task->num_blocks--;
}

void cr_poll(cr_instance_t *inst) {
    if (inst == NULL) {
        return;
    }
    cr_internal_t *self = CR_GET_INTERNAL(inst);

    if (self->hal == NULL) {
        return;
    }

    /* Check RX assembly timeouts (skip if no active slots) */
    if (self->rx_active_count > 0 && self->cfg.rx_assem_timeout_ms > 0) {
        uint32_t now = self->hal->get_tick_ms();
        for (uint8_t i = 0; i < self->cfg.rx_assem_count; i++) {
            if (self->rx_slots[i].active &&
                (now - self->rx_slots[i].last_active_tick) >= self->cfg.rx_assem_timeout_ms) {
                cr_pool_free_chain(self, self->rx_slots[i].head_block);
                self->rx_slots[i].head_block = CR_POOL_NONE;
                self->rx_slots[i].tail_block = CR_POOL_NONE;
                self->rx_slots[i].num_blocks = 0;
                self->rx_slots[i].active = 0;
                self->rx_active_count--;
            }
        }
    }

    /* Process broadcast queue (independent of unicast) */
    if (self->bcast_count > 0) {
        cr_tx_task_t *bcast = &self->bcast_queue[self->bcast_head];
        cr_send_frame(self, bcast, CR_BROADCAST_ADDR);
        /* Release block immediately (broadcast has no ACK) */
        cr_pool_free_chain(self, bcast->head_block);
        bcast->head_block = CR_POOL_NONE;
        bcast->num_blocks = 0;
        self->bcast_head = (self->bcast_head + 1) % self->cfg.bcast_queue_depth;
        self->bcast_count--;
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

            cr_send_frame(self, task, task->next_hop);

            if (!self->cfg.ack_enabled) {
                /* No ACK — release block immediately and check if done */
                cr_tx_advance_block(self, task);
                if (task->offset >= task->total_len) {
                    cr_finish_active_task(self, CR_STATUS_OK);
                }
                /* else: more frames to send, stay in SENDING */
            } else {
                task->state = TX_STATE_WAIT_ACK;
            }
        } else if (task->state == TX_STATE_WAIT_ACK) {
            /* Check interrupt ACK mode */
            if (self->cfg.ack_mode == CR_ACK_MODE_INTERRUPT && self->send_done_flag) {
                self->send_done_flag = 0;
                cr_tx_advance_block(self, task);
                if (task->offset >= task->total_len) {
                    cr_finish_active_task(self, CR_STATUS_OK);
                } else {
                    task->state = TX_STATE_SENDING;
                }
                return;
            }

            /* Reply mode: check timeout (interrupt mode has no timeout/retransmit) */
            if (self->cfg.ack_mode != CR_ACK_MODE_REPLY) {
                return;
            }
            uint32_t now = self->hal->get_tick_ms();
            if ((now - task->last_tick) >= self->cfg.ack_timeout_ms) {
                if (task->retries >= self->cfg.max_retries) {
                    cr_finish_active_task(self, CR_STATUS_FAIL);
                } else {
                    /* Retransmit: rewind offset, resend from same block (not freed yet) */
                    task->retries++;
                    task->offset = task->frame_offset;
                    task->state = TX_STATE_SENDING;
                    cr_send_frame(self, task, task->next_hop);
                    task->state = TX_STATE_WAIT_ACK;
                }
            }
        }
    }
}

/* ===== RX 接收处理 ===== */

/* Find existing RX slot for (src, biz_id) or allocate a new one.
 * Returns slot pointer. Returns NULL if no slot available. */
static cr_rx_slot_t *cr_rx_find_or_alloc_slot(cr_internal_t *self, uint8_t src,
                                              uint8_t biz_id) {
    /* Search for existing active slot matching src+biz_id */
    for (uint8_t i = 0; i < self->cfg.rx_assem_count; i++) {
        if (self->rx_slots[i].active &&
            self->rx_slots[i].src == src &&
            self->rx_slots[i].biz_id == biz_id) {
            return &self->rx_slots[i];
        }
    }
    /* Allocate first free slot */
    for (uint8_t i = 0; i < self->cfg.rx_assem_count; i++) {
        if (!self->rx_slots[i].active) {
            cr_rx_slot_t *slot = &self->rx_slots[i];
            slot->active = 1;
            self->rx_active_count++;
            slot->src = src;
            slot->biz_id = biz_id;
            slot->expected_seq = 0;
            slot->received_len = 0;
            slot->head_block = CR_POOL_NONE;
            slot->tail_block = CR_POOL_NONE;
            slot->num_blocks = 0;
            slot->last_active_tick = self->hal->get_tick_ms();
            return slot;
        }
    }
    return NULL;
}

/* Send an ACK frame back to the original sender */
static void cr_send_ack(cr_internal_t *self, uint8_t dest, uint8_t seq) {
    uint8_t next_hop = cr_route_lookup(self, dest);
    uint8_t ack_frame[CR_FRAME_HEADER_SIZE];
    ack_frame[0] = dest;                   /* DST = original sender */
    ack_frame[1] = self->cfg.local_addr;   /* SRC = us */
    ack_frame[2] = CR_CTL_ACK_BIT;         /* CTL: bit7=1 (ACK) */
    ack_frame[3] = seq;                    /* SEQ = same as received */
    ack_frame[4] = self->cfg.default_ttl;  /* TTL */
    ack_frame[5] = 0;                      /* LEN = 0 for ACK */
    self->hal->send(self->hal->hw_ctx, next_hop, ack_frame, CR_FRAME_HEADER_SIZE);
}

static void cr_handle_local_frame(cr_internal_t *self, cr_instance_t *inst,
                                  const uint8_t *data, uint16_t len) {
    uint8_t src = data[1];
    uint8_t ctl = data[2];
    uint8_t biz_id = CR_CTL_BIZ_ID(ctl);
    const uint8_t *payload = &data[CR_FRAME_HEADER_SIZE];
    uint16_t payload_len = len - CR_FRAME_HEADER_SIZE;

    uint8_t is_ack = CR_CTL_IS_ACK(ctl) ? 1 : 0;
    if (is_ack) {
        /* Match active unicast task by SEQ */
        uint8_t seq = data[3];
        if (self->active_unicast != NULL &&
            self->active_unicast->state == TX_STATE_WAIT_ACK &&
            self->active_unicast->ack_seq == seq) {
            cr_tx_task_t *task = self->active_unicast;
            /* ACK confirmed — release consumed block */
            cr_tx_advance_block(self, task);
            if (task->offset >= task->total_len) {
                cr_finish_active_task(self, CR_STATUS_OK);
            } else {
                /* More frames — continue sending, reset retries for next frame */
                task->retries = 0;
                task->state = TX_STATE_SENDING;
            }
        }
        return;
    }

    /* Data frame for us — deliver to user */
    uint8_t is_frag = CR_CTL_IS_FRAG(ctl) ? 1 : 0;
    uint8_t is_last = CR_CTL_IS_LAST(ctl) ? 1 : 0;
    uint8_t seq = data[3];

    if (!is_frag) {
        /* Single frame — deliver directly */
        if (self->recv_cb) {
            self->recv_cb(inst, src, biz_id, payload, payload_len, self->recv_ctx);
        }
    } else {
        /* Multi-frame fragment — find or create RX assembly slot */
        cr_rx_slot_t *slot = cr_rx_find_or_alloc_slot(self, src, biz_id);

        if (slot == NULL) {
            return; /* No free slot, drop */
        }

        /* Check for duplicate (seq < expected) */
        if (seq < slot->expected_seq) {
            /* Duplicate — still ACK but don't write */
        } else if (seq == slot->expected_seq) {
            /* Alloc block for this fragment */
            uint8_t blk = cr_pool_alloc(self);
            if (blk == CR_POOL_NONE) {
                /* Pool full — discard this assembly */
                cr_pool_free_chain(self, slot->head_block);
                slot->head_block = CR_POOL_NONE;
                slot->tail_block = CR_POOL_NONE;
                slot->num_blocks = 0;
                slot->active = 0;
                self->rx_active_count--;
                return;
            }

            /* Check total length won't exceed delivery buffer */
            if (slot->received_len + payload_len > self->cfg.rx_buf_per_slot) {
                cr_pool_free(self, blk);
                cr_pool_free_chain(self, slot->head_block);
                slot->head_block = CR_POOL_NONE;
                slot->tail_block = CR_POOL_NONE;
                slot->num_blocks = 0;
                slot->active = 0;
                self->rx_active_count--;
                return;
            }

            /* Store payload with 2-byte length prefix in block:
             * [len_lo][len_hi][payload...] */
            uint8_t *blk_ptr = CR_POOL_PTR(self, blk);
            blk_ptr[0] = (uint8_t)(payload_len & 0xFF);
            blk_ptr[1] = (uint8_t)(payload_len >> 8);
            memcpy(&blk_ptr[2], payload, payload_len);

            /* Append to chain */
            if (slot->head_block == CR_POOL_NONE) {
                slot->head_block = blk;
            } else {
                self->pool_next[slot->tail_block] = blk;
            }
            slot->tail_block = blk;
            slot->num_blocks++;
            slot->received_len += payload_len;
            slot->expected_seq++;
            slot->last_active_tick = self->hal->get_tick_ms();

            if (is_last) {
                /* Assembly complete — copy chain to delivery buffer */
                uint8_t *dst_ptr = self->rx_delivery_buf;
                uint8_t cur = slot->head_block;
                uint16_t copied = 0;
                while (cur != CR_POOL_NONE) {
                    uint8_t *blk_data = CR_POOL_PTR(self, cur);
                    uint16_t chunk = (uint16_t)blk_data[0] | ((uint16_t)blk_data[1] << 8);
                    memcpy(dst_ptr + copied, &blk_data[2], chunk);
                    copied += chunk;
                    cur = self->pool_next[cur];
                }
                if (self->recv_cb) {
                    self->recv_cb(inst, src, biz_id, self->rx_delivery_buf,
                                  slot->received_len, self->recv_ctx);
                }
                cr_pool_free_chain(self, slot->head_block);
                slot->head_block = CR_POOL_NONE;
                slot->tail_block = CR_POOL_NONE;
                slot->num_blocks = 0;
                slot->active = 0;
                self->rx_active_count--;
            }
        }
        /* else: out of order — drop (simple sequential protocol) */
    }

    /* Auto-send ACK if enabled and mode is REPLY */
    if (self->cfg.ack_enabled && self->cfg.ack_mode == CR_ACK_MODE_REPLY) {
        cr_send_ack(self, src, data[3]);
    }
}

static void cr_handle_broadcast_frame(cr_internal_t *self, cr_instance_t *inst,
                                      const uint8_t *data, uint16_t len) {
    uint8_t src = data[1];

    /* Drop our own broadcasts reflected back */
    if (src == self->cfg.local_addr) {
        return;
    }

    uint8_t ctl = data[2];
    uint8_t biz_id = CR_CTL_BIZ_ID(ctl);
    const uint8_t *payload = &data[CR_FRAME_HEADER_SIZE];
    uint16_t payload_len = len - CR_FRAME_HEADER_SIZE;
    uint8_t seq = data[3];
    uint8_t ttl = data[4];

    /* Dedup check */
    for (uint8_t i = 0; i < self->cfg.dedup_table_size; i++) {
        if (self->dedup_table[i].valid &&
            self->dedup_table[i].src == src &&
            self->dedup_table[i].seq == seq) {
            return; /* duplicate, drop */
        }
    }

    /* Record in dedup table (ring overwrite) */
    self->dedup_table[self->dedup_write_idx].src = src;
    self->dedup_table[self->dedup_write_idx].seq = seq;
    self->dedup_table[self->dedup_write_idx].valid = 1;
    self->dedup_write_idx = (self->dedup_write_idx + 1) % self->cfg.dedup_table_size;

    /* Deliver to user */
    if (self->recv_cb) {
        self->recv_cb(inst, src, biz_id, payload, payload_len, self->recv_ctx);
    }

    /* Forward with TTL-1 if TTL > 0 */
    if (ttl > 0) {
        if (len <= self->cfg.mtu) {
            uint8_t *fwd_frame = CR_POOL_PTR(self, self->scratch_block);
            memcpy(fwd_frame, data, len);
            fwd_frame[4] = ttl - 1;
            self->hal->send(self->hal->hw_ctx, CR_BROADCAST_ADDR, fwd_frame, len);
        }
        /* else: oversized frame, drop silently */
    }
}

static void cr_handle_forward_frame(cr_internal_t *self, const uint8_t *data,
                                    uint16_t len, uint8_t dst) {
    uint8_t next_hop = cr_route_lookup(self, dst);
    if (next_hop != CR_BROADCAST_ADDR) {
        self->hal->send(self->hal->hw_ctx, next_hop, data, len);
    }
    /* else: no route, drop */
}

/* ===== 帧分发入口 ===== */

void cr_feed_frame(cr_instance_t *inst, const uint8_t *data, uint16_t len) {
    if (inst == NULL || data == NULL) {
        return;
    }
    cr_internal_t *self = CR_GET_INTERNAL(inst);

    if (len < CR_FRAME_HEADER_SIZE) {
        return; /* invalid frame */
    }

    if (self->hal == NULL) {
        return;
    }

    uint8_t dst = data[0];

    if (dst == self->cfg.local_addr) {
        cr_handle_local_frame(self, inst, data, len);
    } else if (dst == CR_BROADCAST_ADDR) {
        cr_handle_broadcast_frame(self, inst, data, len);
    } else {
        cr_handle_forward_frame(self, data, len, dst);
    }
}

void cr_notify_send_done(cr_instance_t *inst) {
    if (inst == NULL) {
        return;
    }
    cr_internal_t *self = CR_GET_INTERNAL(inst);
    self->send_done_flag = 1;
}
