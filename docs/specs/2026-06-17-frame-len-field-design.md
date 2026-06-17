# 帧格式增加有效数据长度字段 — 设计规格

## 动机

当前帧头不含长度字段，接收端通过 `len - CR_FRAME_HEADER_SIZE` 反算有效载荷长度，完全依赖底层硬件接口传入的帧总长度。

在底层硬件固定长度传输场景下（如 CAN 固定 8 字节、DMA 固定块大小），传入的总长度不等于有效数据长度，接收端无法正确截取有效部分。需要帧头自带长度字段。

## 设计

### 新帧格式

```
┌─────┬─────┬─────┬─────┬─────┬─────┬──────────┐
│ DST │ SRC │ CTL │ SEQ │ TTL │ LEN │ PAYLOAD  │
│ [0] │ [1] │ [2] │ [3] │ [4] │ [5] │ [6~...]  │
│ 1B  │ 1B  │ 1B  │ 1B  │ 1B  │ 1B  │ 0~LEN B  │
└─────┴─────┴─────┴─────┴─────┴─────┴──────────┘
```

- **LEN**：1 字节（uint8），表示有效载荷长度（0~255）
- **位置**：帧头末尾，偏移 5
- **帧头大小**：5 → 6 字节
- **现有字段偏移不变**：DST[0] SRC[1] CTL[2] SEQ[3] TTL[4]

### 设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| LEN 宽度 | 1 字节 | 8-bit 地址空间，典型 MTU 不超过 260 字节，1 字节够用 |
| LEN 位置 | 帧头末尾（偏移5） | 不改变现有字段偏移，兼容性最好 |
| LEN 含义 | 有效载荷长度 | 不含帧头本身，与内部 payload_len 语义一致 |
| ACK 帧 | LEN=0 | ACK 帧无载荷 |

### 改动范围

全部改动在 `src/comm_route.c` 内，不涉及头文件接口变更。

#### 1. 常量

```c
// 修改前
#define CR_FRAME_HEADER_SIZE 5
// 修改后
#define CR_FRAME_HEADER_SIZE 6
```

#### 2. TX 构帧（`cr_tx_send_frame`）

在 `frame[4] = TTL` 之后，添加 `frame[5] = (uint8_t)payload_len`。

payload 写入起始位置 `&frame[CR_FRAME_HEADER_SIZE]` 自动适应（常量已改）。

#### 3. ACK 构帧（`cr_send_ack`）

ACK 帧数组大小改为 `CR_FRAME_HEADER_SIZE`（6字节），添加 `ack_frame[5] = 0`。

#### 4. RX 有效载荷提取

**本地帧处理（`cr_handle_local_frame`）和广播帧处理（`cr_handle_broadcast_frame`）**：

```c
// 修改前
uint16_t payload_len = len - CR_FRAME_HEADER_SIZE;
// 修改后
uint16_t payload_len = data[5];
```

payload 指针 `&data[CR_FRAME_HEADER_SIZE]` 自动适应。

#### 5. 转发帧

`cr_handle_forward_frame` 原样转发，不解析 payload，无需改动。

#### 6. 入口校验

`cr_feed_frame` 中 `if (len < CR_FRAME_HEADER_SIZE)` 自动适应。

### 不变项

- `frame[0]~[4]` 偏移不变
- `hal->send` 接口签名不变
- `cr_feed_frame` 签名不变
- 内存布局不变（MTU 仍为帧总大小，单帧最大载荷 = MTU - 6）
- 多帧组装逻辑不变（内部已用 2 字节 len prefix 存储分片）

### 测试影响

所有测试中手工构造的帧字节数组需要：
1. 在偏移 5 处插入 LEN 字节
2. payload 从偏移 6 开始
3. 验证 LEN 值正确

### 文档影响

更新设计规格 `docs/specs/2026-06-08-tp-route-design.md` 中"1. 帧格式"章节的帧结构图和设计决策描述。
