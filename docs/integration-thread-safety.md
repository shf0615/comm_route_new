# 线程安全集成指南

## 并发模型

comm_route 库设计为**单线程驱动**：

| API | 允许的调用上下文 | 说明 |
|-----|-----------------|------|
| `cr_poll` | 主循环/单任务 | 操作 TX 状态机、RX slot |
| `cr_feed_frame` | 主循环/单任务 | 操作 RX 组装、dedup 表 |
| `cr_send` / `cr_broadcast` | 主循环/单任务 | 操作 TX 队列 |
| `cr_notify_send_done` | **可在 ISR 中调用** | 仅设置 volatile 标志位 |

库内部无锁、无原子操作。线程安全性由集成层保证。

---

## 方案 1：裸机（无 OS）

最自然的用法，零额外开销。

### 架构

```
┌─────────────┐         ┌──────────────┐
│  HW RX ISR  │──FIFO──▶│              │
├─────────────┤         │   主循环      │
│  HW TX ISR  │──flag──▶│  (所有库调用) │
└─────────────┘         └──────────────┘
```

### 示例代码

```c
#include "comm_route.h"

/* === FIFO: ISR → 主循环 === */
#define RX_FIFO_SIZE 4
#define RX_FRAME_MAX 64

static uint8_t  rx_fifo_buf[RX_FIFO_SIZE][RX_FRAME_MAX];
static uint16_t rx_fifo_len[RX_FIFO_SIZE];
static volatile uint8_t rx_fifo_head;  /* ISR 写 */
static volatile uint8_t rx_fifo_tail;  /* 主循环读 */

/* === ISR === */

/* 硬件接收中断 — 只入 FIFO，不调库 */
void UART_RX_ISR(void) {
    if ((uint8_t)(rx_fifo_head - rx_fifo_tail) >= RX_FIFO_SIZE) {
        return;  /* FIFO 满，丢弃 */
    }
    uint8_t idx = rx_fifo_head % RX_FIFO_SIZE;
    rx_fifo_len[idx] = hw_read_frame(rx_fifo_buf[idx], RX_FRAME_MAX);
    rx_fifo_head++;
}

/* 硬件发送完成中断 — 唯一可在 ISR 调用的 API */
void UART_TX_ISR(void) {
    cr_notify_send_done(&inst);
}

/* === 主循环 === */

static cr_instance_t inst;

int main(void) {
    /* 初始化（省略配置细节） */
    cr_init(&inst, &cfg, buffer, sizeof(buffer));
    cr_set_hal(&inst, &hal);
    cr_set_recv_callback(&inst, on_recv, NULL);

    while (1) {
        /* 1. 从 FIFO 取出 ISR 收到的帧 */
        while (rx_fifo_tail != rx_fifo_head) {
            uint8_t idx = rx_fifo_tail % RX_FIFO_SIZE;
            cr_feed_frame(&inst, rx_fifo_buf[idx], rx_fifo_len[idx]);
            rx_fifo_tail++;
        }

        /* 2. 驱动状态机 */
        cr_poll(&inst);

        /* 3. 业务逻辑发送 */
        if (need_send) {
            cr_send(&inst, dest, biz_id, data, len, on_complete, ctx);
        }
    }
}
```

### 关键约束

- ISR 中**绝不**调用 `cr_feed_frame` / `cr_send` / `cr_poll`
- ISR 仅做 FIFO 入队和 `cr_notify_send_done`
- FIFO 使用单生产者-单消费者模型，无需锁

---

## 方案 2：RTOS 单任务（推荐）

所有库操作集中到一个专用任务，其他任务通过消息队列请求。

### 架构

```
┌──────────┐   msg_queue   ┌──────────────┐
│ 任务 A   │──────────────▶│              │
├──────────┤               │  comm_task   │
│ 任务 B   │──────────────▶│ (独占库访问)  │
├──────────┤               │              │
│ HW ISR   │───FIFO/flag──▶│              │
└──────────┘               └──────────────┘
```

### 示例代码（FreeRTOS）

```c
#include "comm_route.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

/* === 消息定义 === */

typedef enum {
    COMM_MSG_SEND,
    COMM_MSG_BROADCAST,
    COMM_MSG_RX_FRAME,
} comm_msg_type_t;

typedef struct {
    comm_msg_type_t type;
    uint8_t         dest;
    uint8_t         biz_id;
    const uint8_t  *data;
    uint16_t        len;
    void          (*on_complete)(uint8_t status, void *ctx);
    void           *ctx;
} comm_msg_t;

static QueueHandle_t comm_queue;
static cr_instance_t inst;

/* === 通信任务 — 唯一接触库的任务 === */

void comm_task(void *arg) {
    (void)arg;
    comm_msg_t msg;

    while (1) {
        /* 非阻塞取消息 */
        while (xQueueReceive(comm_queue, &msg, 0) == pdTRUE) {
            switch (msg.type) {
            case COMM_MSG_SEND:
                cr_send(&inst, msg.dest, msg.biz_id,
                        msg.data, msg.len,
                        msg.on_complete, msg.ctx);
                break;
            case COMM_MSG_BROADCAST:
                cr_broadcast(&inst, msg.biz_id, msg.data, msg.len);
                break;
            case COMM_MSG_RX_FRAME:
                cr_feed_frame(&inst, msg.data, msg.len);
                break;
            }
        }

        /* 驱动状态机 */
        cr_poll(&inst);

        /* 让出 CPU — 间隔可根据 frame_interval_ms 调整 */
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* === 对外接口 — 任何任务均可安全调用 === */

int comm_send_async(uint8_t dest, uint8_t biz_id,
                    const uint8_t *data, uint16_t len,
                    void (*cb)(uint8_t, void*), void *ctx) {
    comm_msg_t msg = {
        .type = COMM_MSG_SEND,
        .dest = dest,
        .biz_id = biz_id,
        .data = data,
        .len = len,
        .on_complete = cb,
        .ctx = ctx,
    };
    if (xQueueSend(comm_queue, &msg, pdMS_TO_TICKS(10)) != pdTRUE) {
        return -1;  /* 队列满 */
    }
    return 0;
}

int comm_broadcast_async(uint8_t biz_id, const uint8_t *data, uint16_t len) {
    comm_msg_t msg = {
        .type = COMM_MSG_BROADCAST,
        .biz_id = biz_id,
        .data = data,
        .len = len,
    };
    if (xQueueSend(comm_queue, &msg, pdMS_TO_TICKS(10)) != pdTRUE) {
        return -1;
    }
    return 0;
}

/* ISR 中收到帧后入队 */
void comm_rx_from_isr(const uint8_t *data, uint16_t len) {
    /* 注意：data 需拷贝到持久缓冲区（此处简化） */
    comm_msg_t msg = {
        .type = COMM_MSG_RX_FRAME,
        .data = data,
        .len = len,
    };
    BaseType_t wake = pdFALSE;
    xQueueSendFromISR(comm_queue, &msg, &wake);
    portYIELD_FROM_ISR(wake);
}

/* === 初始化 === */

void comm_init(void) {
    comm_queue = xQueueCreate(8, sizeof(comm_msg_t));
    cr_init(&inst, &cfg, buffer, sizeof(buffer));
    cr_set_hal(&inst, &hal);
    cr_set_recv_callback(&inst, on_recv, NULL);
    xTaskCreate(comm_task, "comm", 512, NULL, 3, NULL);
}
```

### 关键约束

- `data` 指针在 `on_complete` 回调前必须保持有效（库不拷贝数据）
- ISR 中使用 `xQueueSendFromISR`，不能用普通 `xQueueSend`
- `vTaskDelay(1)` 决定最小 poll 间隔，根据 `frame_interval_ms` 调整

### 优点

- 无互斥锁，无死锁风险
- 确定性延迟（队列深度可控）
- 符合库"单线程驱动"的设计意图

---

## 方案 3：RTOS 互斥锁

多个任务直接调用库 API，通过互斥锁串行化。适用于遗留系统改造或任务不易拆分的场景。

### 架构

```
┌──────────┐
│ 任务 A   │──┐
├──────────┤  │  mutex
│ 任务 B   │──┼─────────▶ cr_* API
├──────────┤  │
│ 定时任务  │──┘
└──────────┘
```

### 示例代码（FreeRTOS）

```c
#include "comm_route.h"
#include "FreeRTOS.h"
#include "semphr.h"

static SemaphoreHandle_t cr_mutex;
static cr_instance_t inst;

/* === 线程安全包装 === */

int safe_cr_send(uint8_t dest, uint8_t biz_id,
                 const uint8_t *data, uint16_t len,
                 void (*cb)(uint8_t, void*), void *ctx) {
    xSemaphoreTake(cr_mutex, portMAX_DELAY);
    int ret = cr_send(&inst, dest, biz_id, data, len, cb, ctx);
    xSemaphoreGive(cr_mutex);
    return ret;
}

int safe_cr_broadcast(uint8_t biz_id, const uint8_t *data, uint16_t len) {
    xSemaphoreTake(cr_mutex, portMAX_DELAY);
    int ret = cr_broadcast(&inst, biz_id, data, len);
    xSemaphoreGive(cr_mutex);
    return ret;
}

void safe_cr_poll(void) {
    xSemaphoreTake(cr_mutex, portMAX_DELAY);
    cr_poll(&inst);
    xSemaphoreGive(cr_mutex);
}

void safe_cr_feed_frame(const uint8_t *data, uint16_t len) {
    xSemaphoreTake(cr_mutex, portMAX_DELAY);
    cr_feed_frame(&inst, data, len);
    xSemaphoreGive(cr_mutex);
}

/* cr_notify_send_done 无需锁 — 仅写 volatile 标志 */

/* === 使用示例 === */

/* 定时任务负责 poll */
void poll_task(void *arg) {
    (void)arg;
    while (1) {
        safe_cr_poll();
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

/* 业务任务发送 */
void app_task(void *arg) {
    (void)arg;
    while (1) {
        /* ... 业务逻辑 ... */
        safe_cr_send(0x03, BIZ_DATA, payload, len, on_done, NULL);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* === 初始化 === */

void comm_init(void) {
    cr_mutex = xSemaphoreCreateMutex();
    cr_init(&inst, &cfg, buffer, sizeof(buffer));
    cr_set_hal(&inst, &hal);
    cr_set_recv_callback(&inst, on_recv, NULL);
    xTaskCreate(poll_task, "poll", 256, NULL, 3, NULL);
}
```

### ⚠️ 死锁风险

`on_complete` 和 `recv_cb` 回调在锁持有期间被触发。如果回调中再次调用 `safe_cr_send`，会死锁。

**规避方法**：

```c
/* 方法 A：回调中只设标志，主逻辑中发送 */
static volatile uint8_t retry_flag;
void on_complete(uint8_t status, void *ctx) {
    if (status != 0) {
        retry_flag = 1;  /* 不在此处重发 */
    }
}

/* 方法 B：使用递归互斥锁（不推荐，掩盖设计问题） */
cr_mutex = xSemaphoreCreateRecursiveMutex();
```

---

## 方案对比

| | 裸机 | RTOS 单任务 | RTOS 互斥锁 |
|--|------|-------------|-------------|
| 额外 RAM | FIFO 缓冲区 | 消息队列 | 互斥量 |
| CPU 开销 | 无 | 队列操作 | 锁竞争 |
| 死锁风险 | 无 | 无 | **有**（回调中） |
| 延迟确定性 | 最好 | 好 | 取决于竞争 |
| 代码复杂度 | 低 | 中 | 中 |
| 适用场景 | Cortex-M 裸机 | FreeRTOS/Zephyr/RT-Thread | 遗留系统改造 |

### 推荐选择

1. **裸机** → 方案 1（零成本）
2. **RTOS 新项目** → 方案 2（单任务，最安全）
3. **RTOS 遗留集成** → 方案 3（互斥锁，注意回调死锁）

---

## 通用注意事项

1. **`data` 指针生命周期**：`cr_send` 不拷贝数据，`data` 指针在 `on_complete` 回调前必须保持有效
2. **`cr_notify_send_done`**：这是唯一可在 ISR 中调用的 API，无需任何锁
3. **多实例**：每个 `cr_instance_t` 独立，不同实例可以由不同任务独占操作而无需共享锁
4. **多核**：当前 `send_done_flag` 使用 `volatile` 但无 memory barrier，多核架构需额外处理（如 `__DMB()`）
