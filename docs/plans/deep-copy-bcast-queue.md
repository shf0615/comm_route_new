# 实现计划：TX 深拷贝 + 广播队列

## 变更概要

将 `cr_send` 和 `cr_broadcast` 从浅拷贝（存指针）改为深拷贝（memcpy 到内部缓冲区），
并为广播增加环形队列。

## 1. 配置变更 (`cr_config_t` in comm_route.h)

新增两个字段：

```c
uint16_t tx_buf_per_slot;    // 单个 TX 槽位的最大 payload 字节数
uint8_t  bcast_queue_depth;  // 广播队列深度
```

## 2. 内存布局变更

当前：
```
[cr_internal_t] [TX Queue (task structs)] [RX Slots] [RX Buffers] [Dedup Table]
```

变更后：
```
[cr_internal_t] [TX Queue] [TX Buffers] [Bcast Queue] [Bcast Buffers] [RX Slots] [RX Buffers] [Dedup Table]
```

新增区域：
- **TX Buffers**: `tx_buf_per_slot × tx_queue_depth` — 单播数据深拷贝缓冲区
- **Bcast Queue**: `cr_tx_task_t × bcast_queue_depth` — 广播任务结构体数组
- **Bcast Buffers**: `mtu × bcast_queue_depth` — 广播数据深拷贝缓冲区（广播≤MTU）

## 3. 内部结构变更 (`cr_internal_t`)

```c
// 删除:
cr_tx_task_t  bcast_task;
uint8_t       bcast_pending;

// 新增:
uint8_t      *tx_buffers;          // TX 深拷贝缓冲区基址
cr_tx_task_t *bcast_queue;         // 广播队列数组
uint8_t      *bcast_buffers;       // 广播深拷贝缓冲区基址
uint8_t       bcast_head;
uint8_t       bcast_tail;
uint8_t       bcast_count;
```

## 4. API 行为变更

### `cr_send`
- 现在：`task->data = data`（浅拷贝）
- 变更：`memcpy(tx_buf_slot, data, len); task->data = tx_buf_slot`
- 新增校验：`len > cfg.tx_buf_per_slot` → 返回 `CR_ERR_PARAM`

### `cr_broadcast`
- 现在：单槽，`bcast_pending` 时返回 -1
- 变更：环形队列，`bcast_count >= bcast_queue_depth` 时返回 -1
- 数据深拷贝到 `bcast_buffers` 对应槽位

### `cr_poll`（广播处理部分）
- 现在：`if (bcast_pending) { send + clear }`
- 变更：`if (bcast_count > 0) { send head + advance head + count-- }`

### `cr_calc_buffer_size`
- 追加：`tx_buf_per_slot × tx_queue_depth` + `sizeof(cr_tx_task_t) × bcast_queue_depth` + `mtu × bcast_queue_depth`

## 5. cr_init 内存分配顺序

```c
ptr += sizeof(cr_internal_t);
// TX queue (struct array)
self->tx_queue = (cr_tx_task_t *)ptr;
ptr += sizeof(cr_tx_task_t) * cfg->tx_queue_depth;
// TX buffers (data copy area) — NEW
self->tx_buffers = ptr;
ptr += (size_t)cfg->tx_buf_per_slot * cfg->tx_queue_depth;
// Broadcast queue (struct array) — NEW
self->bcast_queue = (cr_tx_task_t *)ptr;
ptr += sizeof(cr_tx_task_t) * cfg->bcast_queue_depth;
// Broadcast buffers — NEW
self->bcast_buffers = ptr;
ptr += (size_t)cfg->mtu * cfg->bcast_queue_depth;
// RX slots (unchanged)
// RX buffers (unchanged)
// Dedup table (unchanged)
```

## 6. 测试更新

- 所有现有测试需更新 cfg 添加 `tx_buf_per_slot` 和 `bcast_queue_depth` 字段
- 新增测试：
  - `test_send_deep_copy` — 验证 cr_send 后修改原数据不影响发送内容
  - `test_broadcast_deep_copy` — 验证 cr_broadcast 后修改原数据不影响
  - `test_broadcast_queue_multiple` — 连续多次 cr_broadcast 排队成功
  - `test_broadcast_queue_full` — 广播队列满时返回 -1
  - `test_send_exceeds_tx_buf` — len > tx_buf_per_slot 返回 -2

## 7. RAM 影响估算（典型配置）

| 配置项 | 值 | 新增 RAM |
|--------|---|---------|
| tx_buf_per_slot | 256 | 256 × 4 = 1024 B |
| bcast_queue_depth | 4 | 48×4 + 64×4 = 448 B |

典型配置总 RAM：899B → ~2371B（+1472B）

## 8. 执行步骤

1. 修改 `comm_route.h`：`cr_config_t` 新增 2 个字段
2. 修改 `comm_route.c`：
   - `cr_internal_t` 结构体变更
   - `cr_calc_buffer_size` 追加新区域
   - `cr_init` 追加内存分配
   - `cr_send` 改为深拷贝 + 长度校验
   - `cr_broadcast` 改为队列 + 深拷贝
   - `cr_poll` 广播处理改为队列模式
3. 更新所有测试文件 cfg 初始化
4. 新增深拷贝/广播队列测试
5. 逐步构建验证

验证标准：全部测试通过 + 深拷贝测试（修改原 buffer 不影响发送）
