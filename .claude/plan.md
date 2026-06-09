# 内存池重构方案

## 设计目标

引入统一内存池（block 大小 = mtu，块数 = pool_size），替代当前分散的 tx_buffers、bcast_buffers、rx_buffers、tx_scratch。

## 关键语义变更

**mtu 含义改变**：`mtu` = 帧总大小（header + payload）。单帧最大载荷 = `mtu - 5`。

## 1. 内存池设计

```
pool_data[pool_size * mtu]   -- 数据块
pool_next[pool_size]         -- 链表指针 (next block index, 0xFF = end)
pool_free_head               -- 空闲链表头 (0xFF = 池耗尽)
```

- `cr_pool_alloc()`: 从 free_head 弹出一个 block，返回 index（0xFF 表示失败）
- `cr_pool_free()`: 将 block push 回 free_head
- `cr_pool_free_chain()`: 遍历链表释放所有 block

## 2. 配置变更 (cr_config_t)

| 操作 | 字段 | 说明 |
|------|------|------|
| 新增 | `pool_size` | 池中 block 总数 |
| 删除 | `tx_buf_per_slot` | 由池替代 |
| 保留 | `rx_buf_per_slot` | 语义变为"最大重组消息长度"，用于共享交付缓冲区 |
| 语义变更 | `mtu` | 现为帧总大小（含5字节头） |

## 3. 内部结构变更

### cr_internal_t

```c
// 删除:
uint8_t *tx_buffers;
uint8_t *bcast_buffers;
uint8_t *rx_buffers;
uint8_t *tx_scratch;

// 新增:
uint8_t *pool_data;         // pool block 数据基地址
uint8_t *pool_next;         // 链表 next 数组 (pool_size bytes)
uint8_t  pool_free_head;    // 空闲链表头
uint8_t  scratch_block;     // init 时预留的 scratch block index
uint8_t *rx_delivery_buf;   // 共享的多帧交付缓冲区 (rx_buf_per_slot bytes)
```

### cr_tx_task_t

```c
// 删除:
const uint8_t *data;

// 新增:
uint8_t head_block;     // 数据 block 链头 (0xFF = none)
uint8_t num_blocks;     // 占用的 block 数（用于失败时批量释放）
```

### cr_rx_slot_t

```c
// 新增:
uint8_t head_block;     // 片段 block 链头
uint8_t tail_block;     // 片段 block 链尾（便于追加）
uint8_t num_blocks;     // 占用 block 数
```

## 4. 各函数修改

### cr_calc_buffer_size

```
总内存 = sizeof(cr_internal_t)
       + sizeof(cr_tx_task_t) * tx_queue_depth      // TX 队列元数据
       + sizeof(cr_tx_task_t) * bcast_queue_depth   // 广播队列元数据
       + sizeof(cr_rx_slot_t) * rx_assem_count      // RX slot 元数据
       + sizeof(cr_dedup_entry_t) * dedup_table_size
       + mtu * pool_size                            // 池数据
       + pool_size                                  // pool_next 数组
       + rx_buf_per_slot                            // 共享交付缓冲区
       + (CR_ALIGN - 1)                             // 对齐余量
```

### cr_init

- 布局所有区域
- 初始化空闲链表：`pool_next[i] = i+1`，最后一个 = 0xFF，`free_head = 0`
- 从池预留 1 个 scratch_block

### cr_send

```
payload_per_frame = mtu - CR_FRAME_HEADER_SIZE
num_blocks_needed = ceil(len / payload_per_frame)
分配 num_blocks_needed 个 block，链接
逐 block 拷贝用户数据（每 block 存 payload_per_frame 字节）
task->head_block = 链头
task->num_blocks = num_blocks_needed
失败返回 CR_ERR_POOL_FULL（已分配的块全部释放）
```

### cr_broadcast

```
限制: len <= mtu - CR_FRAME_HEADER_SIZE
分配 1 个 block，拷贝数据
task->head_block = block index
```

### cr_send_frame

```
从 task->head_block 读取 payload 数据
构建帧到 scratch_block（header + payload）
调用 hal->send
```

### cr_poll (TX 完成处理)

- **无 ACK 模式**：发送完一帧后释放 head_block 并前进
- **ACK 模式**：收到 ACK 后释放 head_block 并前进
- **任务完成/失败**：释放剩余整条 block 链

### cr_handle_local_frame (RX 多帧)

```
收到分片:
  alloc 1 block
  拷贝 payload 到 block
  追加到 rx_slot 链表尾部 (tail_block)
  
收到最后一片 (is_last):
  遍历 block 链 → memcpy 到 rx_delivery_buf
  调用 recv_cb(inst, src, biz_id, rx_delivery_buf, total_len, ctx)
  释放整条 block 链
  slot->active = 0
```

### cr_handle_broadcast_frame

- 转发时使用 scratch_block（同当前 tx_scratch），无变化

### RX 超时清理 (cr_poll)

- 超时的 rx_slot 释放其 block 链

## 5. 不变的部分

- 公共 API 签名不变（cr_send, cr_broadcast, cr_feed_frame, cr_poll 等）
- 回调签名不变
- 帧格式不变
- 路由、去重、ACK 逻辑不变
- tx_queue / bcast_queue 仍为环形队列（存元数据），深度由配置决定

## 6. 实现步骤

```
1. 添加池管理函数 (alloc/free/free_chain)     → 验证: 单元测试
2. 修改 cr_config_t 和 cr_internal_t 结构     → 验证: 编译通过
3. 修改 cr_calc_buffer_size / cr_init         → 验证: 初始化成功
4. 修改 cr_send (TX 分配 block 链)            → 验证: 发送正常
5. 修改 cr_broadcast (单 block)               → 验证: 广播正常
6. 修改 cr_send_frame (从 block 读数据)       → 验证: 帧构建正确
7. 修改 cr_poll TX 完成路径 (释放 block)      → 验证: 池回收正确
8. 修改 RX 重组 (block 链存储+交付)           → 验证: 多帧接收正确
9. 修改 RX 超时清理                           → 验证: block 释放
10. 修改 mtu 语义 (所有 cfg.mtu 使用处)       → 验证: 全流程测试
```
