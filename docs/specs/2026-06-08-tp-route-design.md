# TP+路由软件 设计规格

## 概述

一个硬件无关、OS 无关的 C99 嵌入式通信传输库，提供多拓扑路由、长数据拆帧传输、ACK 可靠性机制。支持多实例，静态内存分配。

## 约束

- 语言：C99
- 运行环境：裸机 / RTOS 均可
- 内存策略：纯静态，用户提供 buffer，运行时零 malloc
- 硬件：通过抽象接口适配，库不依赖任何具体硬件
- 地址宽度：8-bit（最多 255 个节点，0xFF 保留为广播地址）

---

## 1. 帧格式

```
┌─────┬─────┬─────┬─────┬─────┬──────────────┐
│ DST │ SRC │ CTL │ SEQ │ TTL │   PAYLOAD    │
│ 1B  │ 1B  │ 1B  │ 1B  │ 1B  │  0~MTU B    │
└─────┴─────┴─────┴─────┴─────┴──────────────┘
```

帧头固定 5 字节。

### CTL 控制字节

```
Bit 7   : ACK 帧标记   (1=ACK回复, 0=数据帧)
Bit 6   : 广播标记     (1=广播, 0=单播)
Bit 5   : 分片标记     (1=多帧分片, 0=单帧)
Bit 4   : 末帧标记     (1=最后一帧, 0=非末帧)
Bit 3-0 : 业务ID      (0~15)
```

### 设计决策

- 帧头不含长度字段——底层硬件接口负责帧定界（传入时告知长度）
- SEQ 用 8-bit 循环序号，单次长数据最多 256 帧
- ACK 帧：DST=原始SRC, SRC=自己, CTL.bit7=1, SEQ=被确认帧的SEQ
- 接收端通过 DST 字段区分广播(0xFF)和单播，CTL.bit6 作为显式标记冗余确认

---

## 2. 架构

采用方案 A：单队列 + 双活跃槽位。

```
┌─────────────────────────────────────┐
│            cr_instance_t            │
│  ┌───────────────────────────────┐  │
│  │       TX Queue (环形)         │  │
│  │   [task][task][task]...       │  │
│  └───────────────────────────────┘  │
│  ┌────────────┐  ┌──────────────┐  │
│  │Unicast Slot│  │Broadcast Slot│  │
│  │(单帧/长数据)│  │(仅单帧)      │  │
│  └────────────┘  └──────────────┘  │
│  ┌────────────┐  ┌──────────────┐  │
│  │ RX Assem   │  │Broadcast     │  │
│  │ Buffers    │  │Dedup Table   │  │
│  └────────────┘  └──────────────┘  │
│  ┌───────────────────────────────┐  │
│  │     Static Route Table        │  │
│  └───────────────────────────────┘  │
└─────────────────────────────────────┘
```

- **单播槽**：处理单播单帧和长数据（自动拆帧），一次一个任务
- **广播槽**：处理广播单帧，与单播槽独立并行
- **TX Queue**：等待执行的发送任务队列

---

## 3. 内存布局

用户提供一块 buffer，初始化时内部划分为：

```
┌──────────────┬──────────┬──────────┬───────────┐
│  Instance    │ TX Queue │ RX Assem │ Broadcast │
│  Struct      │ Slots    │ Buffers  │ Dedup Tbl │
└──────────────┴──────────┴──────────┴───────────┘
```

用户通过 `cr_calc_buffer_size()` 计算所需大小后分配。

---

## 4. 配置

```c
typedef enum {
    CR_ACK_MODE_REPLY,     // 目标地址回复
    CR_ACK_MODE_INTERRUPT, // 硬件中断确认
} cr_ack_mode_t;

typedef struct {
    uint8_t                 local_addr;        // 本机地址
    uint16_t                mtu;               // 单帧最大负载字节数
    uint16_t                frame_interval_ms; // 帧间隔（ms）
    uint8_t                 max_retries;       // 最大重传次数
    uint16_t                ack_timeout_ms;    // ACK 超时（ms）
    uint8_t                 ack_enabled;       // ACK 开关 (1=开, 0=关)
    cr_ack_mode_t           ack_mode;          // ACK 方式
    uint8_t                 default_ttl;       // 默认 TTL
    uint8_t                 tx_queue_depth;    // 发送队列深度
    uint8_t                 rx_assem_count;    // 接收组装槽数量
    uint8_t                 dedup_table_size;  // 去重表大小
    uint16_t                rx_assem_timeout_ms; // RX组装超时（ms）
    const cr_route_entry_t *route_table;       // 路由表指针
    uint8_t                 route_count;       // 路由条目数
} cr_config_t;
```

---

## 5. 路由表

```c
typedef struct {
    uint8_t dest;      // 目标地址
    uint8_t next_hop;  // 下一跳地址
} cr_route_entry_t;
```

- 路由表是 const 指针，库不复制，引用用户的静态数组
- 线性查找，嵌入式节点少场景足够
- next_hop == dest 表示直连
- 广播帧不查路由表，直接调用 HAL send 发出（底层硬件为共享总线时自然广播；点对点硬件时用户需在 HAL send 中处理多邻居分发）
- 未找到路由条目的单播帧丢弃

---

## 6. 硬件抽象层

```c
typedef struct {
    int (*send)(void *hw_ctx, const uint8_t *data, uint16_t len);
    uint32_t (*get_tick_ms)(void);
    void *hw_ctx;
} cr_hal_t;
```

- `send`：发送一帧打包好的数据，返回 0=成功，非0=忙/失败
- `get_tick_ms`：获取当前毫秒时间戳
- `hw_ctx`：传入 send 的用户上下文

接收方向为推模式，用户收到硬件数据后调用 `cr_feed_frame()`。

### 调用上下文约束

- `cr_poll`、`cr_feed_frame`：只能在主循环（非中断）上下文调用
- `cr_notify_send_done`：可在中断中调用（仅设置标志位，不执行复杂逻辑）
- 如果硬件在中断中收到数据，用户应缓存到自己的 FIFO，主循环中再调用 `cr_feed_frame`

---

## 7. 发送状态机

每次 `cr_poll()` 推进一步：

```
IDLE → SENDING → WAIT_ACK → 判断
                              ├─ ACK OK + 还有帧 → SENDING
                              ├─ ACK OK + 无后续 → DONE（回调成功）
                              ├─ 超时未达上限 → 重传（回到 SENDING）
                              └─ 达到最大重传 → FAILED（回调失败）
```

- ACK 关闭时跳过 WAIT_ACK 状态
- 帧间隔在 SENDING 状态判断：距上次发送不足 frame_interval_ms 则跳过本次 poll

---

## 8. ACK 机制

### 方式 1：目标地址回复（CR_ACK_MODE_REPLY）

- 接收端收到数据帧后，库自动构造 ACK 帧（CTL.bit7=1, SEQ=对应帧序号）发回
- 发送端在 `cr_feed_frame()` 中收到 ACK 帧，匹配 SEQ 标记成功
- 超时未收到则重传

### 方式 2：硬件中断（CR_ACK_MODE_INTERRUPT）

- 硬件发送完成后触发中断
- 用户在 ISR 中调用 `cr_notify_send_done(inst)`
- 库标记当前帧发送成功
- 无超时重传逻辑（硬件层保证可靠性）

| | 目标回复 | 中断 |
|--|---------|------|
| 确认含义 | 对端收到 | 硬件发出 |
| 超时重传 | 有 | 无 |
| 用户额外调用 | 无 | `cr_notify_send_done()` |
| 适用场景 | 软件层可靠性 | 硬件层已有可靠性 |

---

## 9. 接收流程

```
cr_feed_frame(inst, data, len)
  │
  ├─ DST == 本机地址?
  │   ├─ CTL.bit7 == 1 (ACK帧)?
  │   │   └─ 匹配活跃发送任务 SEQ，标记成功
  │   ├─ 单帧(CTL.bit5==0) → 触发接收回调 + 发ACK
  │   └─ 分片(CTL.bit5==1) → 写入RX组装槽 + 发ACK
  │       └─ 末帧(CTL.bit4==1) → 组装完成，触发回调，释放槽
  │
  ├─ DST == 0xFF (广播)?
  │   ├─ 去重检查（SRC+SEQ 已见?）→ 丢弃
  │   ├─ 记录去重表
  │   ├─ 触发接收回调
  │   └─ TTL > 0 → TTL-1，转发
  │
  └─ 其他（DST != 本机 && DST != 广播）
      └─ 查路由表转发（ACK帧、数据帧统一透传）
```

> **关键设计点**：ACK 帧不做特殊前置处理。先判断 DST：若是自己则处理（包括 ACK 匹配）；若不是自己则按路由表转发（ACK 帧和数据帧一样透传）。这保证了多跳场景下 ACK 能正确回传到原始发送端。

### 接收回调

```c
typedef void (*cr_on_recv_t)(
    cr_instance_t *inst,
    uint8_t src_addr,
    uint8_t biz_id,
    const uint8_t *data,
    uint16_t len,
    void *user_ctx
);
```

### RX 组装槽

按 SRC + BizID 匹配，按 SEQ 序号顺序写入。

### RX 组装超时

在 `cr_poll()` 中检查各 RX 组装槽的最后活跃时间。超时值为独立配置项 `rx_assem_timeout_ms`，用户应根据网络中发送端的重传参数合理设置（建议 ≥ 发送端的 ack_timeout × (max_retries + 1)）。超时后释放槽位，丢弃不完整数据。

---

## 10. 广播机制

- 广播仅支持单帧（不拆分）
- 传播方式：多跳转发 + TTL 递减 + 去重
- 去重表记录（SRC + SEQ），避免重复处理和转发
- 去重表满时覆盖最旧记录（环形覆写）
- 去重表大小建议远小于 256（如 16~32），确保 SEQ 回卷前旧记录已被覆盖淘汰
- TTL=0 的广播帧只处理不转发
- 广播帧无 ACK（发完即忘）

---

## 11. API

```c
/* 初始化 */
size_t cr_calc_buffer_size(const cr_config_t *cfg);
int    cr_init(cr_instance_t *inst, const cr_config_t *cfg,
               uint8_t *buffer, size_t buffer_size);

/* 硬件注册 */
void cr_set_hal(cr_instance_t *inst, const cr_hal_t *hal);
void cr_set_recv_callback(cr_instance_t *inst, cr_on_recv_t cb, void *user_ctx);

/* 发送 */
int  cr_send(cr_instance_t *inst, uint8_t dest, uint8_t biz_id,
             const uint8_t *data, uint16_t len,
             void (*on_complete)(uint8_t status, void *ctx), void *user_ctx);
int  cr_broadcast(cr_instance_t *inst, uint8_t biz_id,
                  const uint8_t *data, uint16_t len);

/* 驱动 */
void cr_poll(cr_instance_t *inst);
void cr_feed_frame(cr_instance_t *inst, const uint8_t *data, uint16_t len);
void cr_notify_send_done(cr_instance_t *inst);
```

### 返回值约定

| 值 | 含义 |
|----|------|
| 0  | 成功 |
| -1 | 队列满 |
| -2 | 参数错误 |
| -3 | buffer 不足 |

### 行为说明

- `cr_send`：len ≤ MTU 时单帧发送，len > MTU 时自动拆帧
- `cr_broadcast`：无完成回调（广播无 ACK，发完即忘）
- `cr_poll`：非阻塞，每次推进状态机一步
- `cr_feed_frame`：只能在主循环中调用（非中断上下文）

---

## 12. 拓扑支持

通过静态路由表配置支持任意拓扑：

| 拓扑 | 路由表配置方式 |
|------|--------------|
| 星型 | 中心节点配所有叶子为直连 |
| 链式 | 每个节点配相邻节点为直连 |
| 树形 | 父节点配子节点直连，子节点配非直连目标经父节点 |
| 环形 | 用户必须保证路由表无环（单播不递减 TTL）；广播配合 TTL+去重防循环 |

库本身不感知拓扑形状，完全由路由表决定转发行为。

### 单播转发不递减 TTL

单播帧转发时 TTL 字段不修改（透传）。TTL 仅用于广播帧的循环控制。环形拓扑中用户配置路由表时必须确保单播路径无环——静态路由表的正确性由用户负责。
