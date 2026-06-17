# 项目帧格式分析报告

## 执行摘要

这是一个嵌入式TP+路由通信库，采用**5字节固定头 + 可变长度载荷**的帧格式。支持单播/广播、多帧分片、ACK可靠性、TTL多跳转发等功能。

---

## 1. 帧格式定义

### 整体结构
```
┌─────┬─────┬─────┬─────┬─────┬──────────────┐
│ DST │ SRC │ CTL │ SEQ │ TTL │   PAYLOAD    │
│ 1B  │ 1B  │ 1B  │ 1B  │ 1B  │  0~(MTU-5)B  │
└─────┴─────┴─────┴─────┴─────┴──────────────┘
```

**帧头固定大小：5字节**（`CR_FRAME_HEADER_SIZE = 5`）

#### 各字段说明

| 字段 | 大小 | 含义 |
|------|------|------|
| **DST** | 1B | 目标地址（0xFF为广播地址） |
| **SRC** | 1B | 源地址 |
| **CTL** | 1B | 控制字节（见下表） |
| **SEQ** | 1B | 8-bit循环序号（0-255，用于去重和ACK匹配） |
| **TTL** | 1B | 生存时间（转发时递减，仅对广播帧有效） |
| **PAYLOAD** | 0~(MTU-5)B | 应用层数据 |

### CTL控制字节位域设计
```
Bit 7   : ACK帧标记    (1=ACK回复, 0=数据帧)
Bit 6   : 广播标记     (1=广播, 0=单播)
Bit 5   : 分片标记     (1=多帧分片, 0=单帧)
Bit 4   : 末帧标记     (1=最后一帧, 0=非末帧)
Bit 3-0 : 业务ID      (0~15，用于消息分类)
```

---

## 2. 帧格式定义位置

### 源代码文件位置

| 文件 | 行号 | 内容 |
|------|------|------|
| **src/comm_route.c** | 6 | `#define CR_FRAME_HEADER_SIZE 5` |
| **src/comm_route.c** | 13-23 | CTL位域常量定义 + 提取宏 |
| **src/comm_route.c** | 420-441 | 帧打包函数 `cr_send_frame()` |
| **src/comm_route.c** | 603-609 | ACK帧构建 `cr_send_ack()` |
| **src/comm_route.c** | 617-809 | 帧接收解析 `cr_handle_local_frame()` 等 |

### 文档文件位置

| 文件 | 章节 | 内容 |
|------|------|------|
| **docs/specs/2026-06-08-tp-route-design.md** | 第1章 | 帧格式规格 + 设计决策 |
| **.plan.md** | 内存池设计 | mtu语义变更（帧总大小） |

---

## 3. 当前帧结构的字段

### 5字节头的具体字段

| 索引 | 字段 | 类型 | 约束 |
|------|------|------|------|
| [0] | DST（目标地址） | uint8_t | 0x00-0xFE=单播, 0xFF=广播 |
| [1] | SRC（源地址） | uint8_t | 0x00-0xFE（不能是0xFF） |
| [2] | CTL（控制字节） | uint8_t | 位域：Bit7(ACK), Bit6(BCAST), Bit5(FRAG), Bit4(LAST), Bit3-0(BizID) |
| [3] | SEQ（序号） | uint8_t | 0-255循环计数，用于去重和ACK匹配 |
| [4] | TTL（生存时间） | uint8_t | 广播转发时递减，单播转发时透传 |
| [5+] | PAYLOAD | uint8_t[] | 0~(MTU-5)字节，长数据拆分到多帧 |

### 具体示例

**单播单帧（业务ID=0）：**
```
Frame: [0x02][0x01][0x00][0x00][0x03]['H']['I']
       |DST |SRC |CTL |SEQ |TTL |-----------|
```

**ACK帧：**
```
Frame: [0x01][0x02][0x80][0x05][0x03]
       |DST |SRC |CTL |SEQ |TTL |
```

**广播多帧（非末帧）：**
```
Frame: [0xFF][0x01][0x60][0x01][0x03][8字节payload]
       |DST |SRC |CTL |SEQ |TTL |-------------|
       （CTL=0x60: Bit6=1广播, Bit5=1分片, Bit4=0非末帧）
```

---

## 4. 帧长度特性

### 是否固定长度

**答：可变长度**

- **帧头**：固定5字节
- **载荷**：0到(MTU-5)字节不等
- **总帧长**：5到MTU字节

### MTU的含义

**最新语义（从d7ce5a5提交）：**
```
mtu = 帧总大小（含5字节头）

单帧最大payload = mtu - 5

示例：
  如果 mtu = 64，则最大payload = 59字节
```

### 长数据处理

```c
/* 判断是否多帧 */
uint16_t payload_per_frame = mtu - 5;
uint8_t is_multi = (total_len > payload_per_frame) ? 1 : 0;

/* 最多256帧（SEQ为8-bit）
   单次最大 = 256 × (mtu - 5) 字节
*/
```

---

## 5. 数据区域如何组织

### 帧的内存布局

```
[5字节头][可变长payload]
```

**提取方法：**
```c
const uint8_t *payload = &frame[5];
uint16_t payload_len = frame_len - 5;
```

### 多帧消息的组装

**发送：** 库自动拆分长数据到多帧

```c
/* cr_send_frame() 每次构建一帧 */
frame[0] = dest;
frame[1] = local_addr;
frame[2] = (is_broadcast<<6) | (is_multi<<5) | (is_last<<4) | biz_id;
frame[3] = seq;
frame[4] = ttl;
memcpy(&frame[5], payload, payload_len);  /* 填充payload */
```

**接收：** 按SEQ顺序装配到RX槽

```c
/* cr_handle_local_frame() 检查分片标志 */
if (CTL.Bit5 == 1) {  /* 分片 */
    写入RX槽位;
    if (CTL.Bit4 == 1) {  /* 末帧 */
        完整消息已收到，交付给应用;
    }
}
```

### 内存池结构

当前使用统一内存池模式：
```
内存池 = [block0|block1|...|blockN]
每个block = mtu字节
用链表管理空闲/使用状态
```

---

## 6. 最近的帧格式相关提交

| 提交ID | 日期 | 说明 |
|--------|------|------|
| **d7ce5a5** | 2026-06 | **引入统一内存池，改变MTU语义** |
| **e22a7d8** | 2026-06 | 补充TTL/多帧测试 |
| **78c3b30** | 2026-04 | feat: RX assembly timeout |
| **45b38bb** | 2026-04 | feat: multi-frame receive |
| **55ebe03** | 2026-03 | feat: broadcast/unicast frame send |

### 最重要的改动（d7ce5a5）

- **MTU语义变更**：payload上限 → 帧总大小
- **帧结构本身无变化**
- **内存管理改进**：固定偏移 → 池分配

---

## 7. 快速参考

### 帧格式一览

```
字节位置  字段名     取值范围      用途
   0     DST        0x00-0xFF    目标地址
   1     SRC        0x00-0xFE    源地址
   2     CTL        0x00-0xFF    控制（见位域）
   3     SEQ        0x00-0xFF    序号
   4     TTL        0x00-0xFF    生存时间
  5+     PAYLOAD    可变长        应用数据
```

### CTL字节解析

```c
#define CR_CTL_IS_ACK(ctl)       ((ctl) & 0x80)
#define CR_CTL_IS_BROADCAST(ctl) ((ctl) & 0x40)
#define CR_CTL_IS_FRAG(ctl)      ((ctl) & 0x20)
#define CR_CTL_IS_LAST(ctl)      ((ctl) & 0x10)
#define CR_CTL_BIZ_ID(ctl)       ((ctl) & 0x0F)
```

### 打包/解包函数

| 函数 | 作用 | 位置 |
|------|------|------|
| `cr_send_frame()` | 构建并发送帧 | src/comm_route.c:420 |
| `cr_send_ack()` | 构建ACK帧 | src/comm_route.c:603 |
| `cr_handle_local_frame()` | 解析数据帧 | src/comm_route.c:615 |
| `cr_handle_broadcast_frame()` | 解析广播帧 | src/comm_route.c:740 |

---

## 总结

✅ **帧结构**：5字节固定头 + 可变payload  
✅ **帧长度**：可变（5-MTU字节）  
✅ **数据区域**：从字节5开始，长度 = 帧长 - 5  
✅ **长数据**：自动分片到最多256帧（SEQ为8-bit）  
✅ **核心设计**：CTL位域紧凑，支持单播/广播/分片/ACK
