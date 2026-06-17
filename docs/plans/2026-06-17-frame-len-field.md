# 帧格式 LEN 字段 TDD 实现计划

**目标：** 帧头增加 1 字节 LEN 字段（偏移5），支持底层固定长度传输场景下的有效数据截取
**行为规格：** `docs/specs/2026-06-17-frame-len-field.feature`
**技术方案：** `docs/specs/2026-06-17-frame-len-field-design.md`

---

## 实现策略

本次改动的特殊性：`CR_FRAME_HEADER_SIZE` 从 5→6 会**立即破坏所有现有测试**（帧字节数组偏移全部错位）。因此采用以下策略：

1. **Task 1**：先改常量+TX构帧+ACK构帧（生产代码），同时修复所有因帧头变大而破坏的现有测试，新增 LEN 字段 TX 侧验证测试
2. **Task 2**：改 RX 侧用 `data[5]` 读 LEN，新增 RX 侧验证测试
3. **Task 3**：新增 LEN 越界校验 + 多帧 LEN 验证测试
4. **Task 4**：更新设计文档

---

### Task 1: 发送端自动填充 LEN + ACK 帧 LEN=0 + 广播帧携带 LEN

**Scenarios:**
```gherkin
Scenario: 发送端自动填充 LEN 字段
  Given 实例配置本机地址=0x01
  When 用户发送 5 字节 payload 到目标 0x03
  Then 发出的帧 frame[5]=5（LEN=有效载荷长度）
  And 帧头大小为 6 字节

Scenario: ACK 帧 LEN=0
  Given 实例配置 ACK=开启
  When 收到数据帧后发送 ACK 回复
  Then ACK 帧的 LEN=0（ACK 帧无有效载荷）

Scenario: 广播帧携带 LEN
  Given 实例配置本机地址=0x01
  When 用户发送广播帧 payload="HI"（2字节）
  Then 发出的帧 frame[5]=2
  And 帧头目标地址为 0xFF
```

**文件：**
- 实现: `src/comm_route.c`
- 测试: `tests/test_len_field.c`, `tests/test_main.c`
- 现有测试修复: `tests/test_send.c`, `tests/test_recv.c`, `tests/test_ack.c`, `tests/test_broadcast.c`, `tests/test_multihop.c`, `tests/test_extra.c`, `tests/test_coverage.c`
- 构建: `CMakeLists.txt`

- [ ] **Red: 编写失败测试**

创建 `tests/test_len_field.c`，新增三个测试：

```c
#include "unity.h"
#include "comm_route.h"
#include <string.h>

static uint8_t sent_buf[256];
static uint16_t sent_len;
static int send_count;

static int mock_send(void *ctx, uint8_t next_hop, const uint8_t *data, uint16_t len) {
    (void)ctx; (void)next_hop;
    memcpy(sent_buf, data, len);
    sent_len = len;
    send_count++;
    return 0;
}

static uint32_t mock_tick = 0;
static uint32_t mock_get_tick(void) { return mock_tick; }

/* Helper: 标准初始化 */
static cr_instance_t inst;
static uint8_t buffer[4096];
static cr_hal_t hal;
static cr_route_entry_t routes[1];

static void setup_instance(uint8_t addr, uint16_t mtu, uint8_t ack_enabled) {
    routes[0] = (cr_route_entry_t){.dest = 0x03, .next_hop = 0x02};
    cr_config_t cfg = {
        .local_addr = addr, .mtu = mtu, .frame_interval_ms = 0,
        .ack_enabled = ack_enabled, .ack_mode = CR_ACK_MODE_REPLY,
        .default_ttl = 3, .tx_queue_depth = 4,
        .rx_assem_count = 2, .dedup_table_size = 16,
        .rx_assem_timeout_ms = 1000, .rx_buf_per_slot = 256,
        .pool_size = 32, .bcast_queue_depth = 4,
        .route_table = routes, .route_count = 1,
    };
    cr_init(&inst, &cfg, buffer, sizeof(buffer));
    hal = (cr_hal_t){ .send = mock_send, .get_tick_ms = mock_get_tick, .hw_ctx = NULL };
    cr_set_hal(&inst, &hal);
    send_count = 0;
    sent_len = 0;
    mock_tick = 0;
}

/* Scenario: 发送端自动填充 LEN 字段 */
void test_tx_fills_len_field(void) {
    setup_instance(0x01, 64, 0);
    uint8_t payload[] = "HELLO";  /* 5 bytes */
    cr_send(&inst, 0x03, 0, payload, 5, NULL, NULL);
    cr_poll(&inst);
    TEST_ASSERT_EQUAL_INT(1, send_count);
    /* frame[5] = LEN = 5 */
    TEST_ASSERT_EQUAL_UINT8(5, sent_buf[5]);
    /* 帧头大小 = 6, 总长 = 6 + 5 = 11 */
    TEST_ASSERT_EQUAL_UINT16(11, sent_len);
    /* payload 从 offset 6 开始 */
    TEST_ASSERT_EQUAL_MEMORY("HELLO", &sent_buf[6], 5);
}

/* Scenario: ACK 帧 LEN=0 */
void test_ack_frame_len_zero(void) {
    setup_instance(0x02, 64, 1);
    /* 构造一帧数据帧发给 0x02，触发 ACK 回复 */
    /*         DST   SRC   CTL   SEQ   TTL   LEN   PAYLOAD */
    uint8_t frame[] = {0x02, 0x01, 0x00, 0x00, 0x03, 0x02, 'H', 'I'};
    send_count = 0;
    cr_feed_frame(&inst, frame, sizeof(frame));
    /* 应发送 ACK */
    TEST_ASSERT_EQUAL_INT(1, send_count);
    /* ACK 帧 frame[5] = LEN = 0 */
    TEST_ASSERT_EQUAL_UINT8(0, sent_buf[5]);
    /* ACK 帧总长 = 帧头大小 = 6 */
    TEST_ASSERT_EQUAL_UINT16(6, sent_len);
}

/* Scenario: 广播帧携带 LEN */
void test_broadcast_fills_len_field(void) {
    setup_instance(0x01, 64, 0);
    uint8_t payload[] = "HI";  /* 2 bytes */
    cr_broadcast(&inst, 0, payload, 2, NULL, NULL);
    cr_poll(&inst);
    TEST_ASSERT_EQUAL_INT(1, send_count);
    /* frame[5] = LEN = 2 */
    TEST_ASSERT_EQUAL_UINT8(2, sent_buf[5]);
    /* DST = 0xFF */
    TEST_ASSERT_EQUAL_UINT8(0xFF, sent_buf[0]);
}
```

在 `tests/test_main.c` 中注册这三个测试函数。
在 `CMakeLists.txt` 中将 `tests/test_len_field.c` 加入 `test_all` 编译。

- [ ] **Red: 确认测试失败**

```bash
cd build && cmake --build . && ctest --output-on-failure
```

预期: FAIL（帧头仍为 5 字节，frame[5] 不是 LEN 而是 payload 首字节）

- [ ] **Green: 最小实现**

`src/comm_route.c` 修改：

1. 常量改为 6：
```c
#define CR_FRAME_HEADER_SIZE 6
```

2. `cr_tx_send_frame` 中 `frame[4] = TTL` 之后添加：
```c
frame[5] = (uint8_t)payload_len;           /* LEN */
```

3. `cr_send_ack` 中 `ack_frame[4] = TTL` 之后添加：
```c
ack_frame[5] = 0;                          /* LEN = 0 for ACK */
```

4. 修复所有现有测试中手工构造的帧字节数组——在偏移 5 处插入 LEN 字节，payload 后移到偏移 6。涉及文件：
   - `tests/test_ack.c`：ACK 帧 5 字节→6 字节，数据帧插入 LEN
   - `tests/test_broadcast.c`：所有广播帧插入 LEN
   - `tests/test_recv.c`：所有接收帧插入 LEN
   - `tests/test_extra.c`：所有帧插入 LEN
   - `tests/test_coverage.c`：所有帧插入 LEN，`memset` 起始偏移从 5→6
   - `tests/test_multihop.c`：如有手工帧同样修复
   - `tests/test_send.c`：验证 payload 偏移相关的断言（如有）

- [ ] **Green: 确认测试通过**

```bash
cd build && cmake --build . && ctest --output-on-failure
```

预期: 全部 PASS

- [ ] **Refactor**

检查有无重复的 setup 代码可提取。运行全量测试确认不回归。

- [ ] **Commit**

```bash
git add -A && git commit -m "feat: add LEN field to frame header (TX side)

CR_FRAME_HEADER_SIZE 5->6. TX and ACK frames now fill frame[5]
with payload length. All existing tests updated for new offset."
```

---

### Task 2: 接收端根据 LEN 截取有效数据 + LEN=0 数据帧

**Scenarios:**
```gherkin
Scenario: 接收端根据 LEN 截取有效数据
  Given 实例配置本机地址=0x02
  When 收到一帧（底层传入总长=64, frame[5]=5）
  Then 实例取 LEN=5 作为有效载荷长度
  And 回调交付的数据长度为 5（非 64-6=58）

Scenario: LEN=0 的数据帧
  Given 实例配置本机地址=0x02
  When 收到一帧（frame[5]=0, 非ACK帧）
  Then 回调交付的数据长度为 0
```

**文件：**
- 实现: `src/comm_route.c`
- 测试: `tests/test_len_field.c`

- [ ] **Red: 编写失败测试**

在 `tests/test_len_field.c` 新增：

```c
static uint8_t recv_payload[256];
static uint16_t recv_len;
static int recv_count;

static void mock_recv(cr_instance_t *inst, uint8_t src, uint8_t biz_id,
                      const uint8_t *data, uint16_t len, void *ctx) {
    (void)inst; (void)src; (void)biz_id; (void)ctx;
    memcpy(recv_payload, data, len);
    recv_len = len;
    recv_count++;
}

/* Scenario: 接收端根据 LEN 截取有效数据 */
void test_rx_uses_len_field(void) {
    setup_instance(0x02, 64, 0);
    cr_set_recv_callback(&inst, mock_recv, NULL);
    recv_count = 0;
    recv_len = 0;

    /* 构造 64 字节帧，LEN=5，后面填充垃圾 */
    uint8_t frame[64];
    memset(frame, 0xAA, sizeof(frame));  /* 填充 */
    frame[0] = 0x02;  /* DST = 本机 */
    frame[1] = 0x01;  /* SRC */
    frame[2] = 0x00;  /* CTL */
    frame[3] = 0x00;  /* SEQ */
    frame[4] = 0x03;  /* TTL */
    frame[5] = 5;     /* LEN = 5 */
    memcpy(&frame[6], "HELLO", 5);

    cr_feed_frame(&inst, frame, 64);  /* 底层传入总长 64 */

    TEST_ASSERT_EQUAL_INT(1, recv_count);
    TEST_ASSERT_EQUAL_UINT16(5, recv_len);  /* 应该是 5，不是 64-6=58 */
    TEST_ASSERT_EQUAL_MEMORY("HELLO", recv_payload, 5);
}

/* Scenario: LEN=0 的数据帧 */
void test_rx_len_zero_data_frame(void) {
    setup_instance(0x02, 64, 0);
    cr_set_recv_callback(&inst, mock_recv, NULL);
    recv_count = 0;
    recv_len = 0xFF;  /* 故意设非零 */

    uint8_t frame[] = {0x02, 0x01, 0x00, 0x00, 0x03, 0x00};  /* LEN=0 */
    cr_feed_frame(&inst, frame, sizeof(frame));

    TEST_ASSERT_EQUAL_INT(1, recv_count);
    TEST_ASSERT_EQUAL_UINT16(0, recv_len);
}
```

在 `tests/test_main.c` 注册。

- [ ] **Red: 确认测试失败**

```bash
cd build && cmake --build . && ctest --output-on-failure
```

预期: `test_rx_uses_len_field` FAIL（当前 RX 仍用 `len - HEADER_SIZE` = 58）

- [ ] **Green: 最小实现**

`src/comm_route.c` 修改两处 RX payload_len 计算：

1. `cr_handle_local_frame`：
```c
// 修改前
uint16_t payload_len = len - CR_FRAME_HEADER_SIZE;
// 修改后
uint16_t payload_len = data[5];
```

2. `cr_handle_broadcast_frame`：
```c
// 修改前
uint16_t payload_len = len - CR_FRAME_HEADER_SIZE;
// 修改后
uint16_t payload_len = data[5];
```

- [ ] **Green: 确认测试通过**

```bash
cd build && cmake --build . && ctest --output-on-failure
```

预期: 全部 PASS

- [ ] **Refactor**

无明显重构需要。运行全量测试确认。

- [ ] **Commit**

```bash
git add -A && git commit -m "feat: RX uses LEN field for payload length

cr_handle_local_frame and cr_handle_broadcast_frame now read
data[5] as payload length instead of computing from total frame size."
```

---

### Task 3: LEN 越界校验 + 长数据拆帧 LEN 验证

**Scenarios:**
```gherkin
Scenario: LEN 超过帧实际大小 - 丢弃帧
  Given 实例配置本机地址=0x02
  When 收到一帧（底层传入总长=10, frame[5]=100）
  Then 实例丢弃该帧（LEN 声称 100 字节但帧仅 10 字节）
  And 不触发接收回调

Scenario: 长数据拆帧 - 每帧各自填充 LEN
  Given 实例配置 MTU=16（帧总大小=16, 单帧最大载荷=16-6=10）
  When 用户提交 25 字节的长数据发送请求
  Then 拆为 3 帧：第1帧 LEN=10, 第2帧 LEN=10, 第3帧 LEN=5

Scenario: 长数据组装 - 按各帧 LEN 累计
  Given 实例配置本机地址=0x02
  When 依次收到 3 帧分片（LEN=10, LEN=10, LEN=5，第3帧带末帧标记）
  Then 组装后总有效数据长度为 25 字节
```

**文件：**
- 实现: `src/comm_route.c`
- 测试: `tests/test_len_field.c`

- [ ] **Red: 编写失败测试**

在 `tests/test_len_field.c` 新增：

```c
/* Scenario: LEN 超过帧实际大小 - 丢弃帧 */
void test_rx_len_overflow_drops_frame(void) {
    setup_instance(0x02, 64, 0);
    cr_set_recv_callback(&inst, mock_recv, NULL);
    recv_count = 0;

    /* 帧总长 10 字节，但 LEN 声称 100 */
    uint8_t frame[10] = {0x02, 0x01, 0x00, 0x00, 0x03, 100, 'A', 'B', 'C', 'D'};
    cr_feed_frame(&inst, frame, 10);

    TEST_ASSERT_EQUAL_INT(0, recv_count);  /* 应丢弃，不回调 */
}

/* Scenario: 长数据拆帧 - 每帧各自填充 LEN */
void test_tx_multiframe_each_fills_len(void) {
    /* MTU=16, 帧头=6, 单帧最大载荷=10 */
    setup_instance(0x01, 16, 0);

    /* 用 send_history 记录每帧 */
    /* 替换 mock_send 为可记录多帧的版本 */
    static uint8_t frames[3][16];
    static uint16_t frame_lens[3];
    static int frame_idx;
    frame_idx = 0;

    int multi_send(void *ctx, uint8_t next_hop, const uint8_t *data, uint16_t len) {
        (void)ctx; (void)next_hop;
        if (frame_idx < 3) {
            memcpy(frames[frame_idx], data, len);
            frame_lens[frame_idx] = len;
            frame_idx++;
        }
        return 0;
    }

    hal.send = multi_send;
    cr_set_hal(&inst, &hal);

    uint8_t data[25];
    memset(data, 0xCC, 25);
    cr_send(&inst, 0x03, 0, data, 25, NULL, NULL);

    /* 发 3 帧 */
    mock_tick = 0;  cr_poll(&inst);  /* 第1帧 */
    mock_tick = 1;  cr_poll(&inst);  /* 第2帧 */
    mock_tick = 2;  cr_poll(&inst);  /* 第3帧 */

    TEST_ASSERT_EQUAL_INT(3, frame_idx);
    TEST_ASSERT_EQUAL_UINT8(10, frames[0][5]);  /* LEN=10 */
    TEST_ASSERT_EQUAL_UINT8(10, frames[1][5]);  /* LEN=10 */
    TEST_ASSERT_EQUAL_UINT8(5,  frames[2][5]);  /* LEN=5 */
}

/* Scenario: 长数据组装 - 按各帧 LEN 累计 */
void test_rx_multiframe_assembly_uses_len(void) {
    setup_instance(0x02, 64, 0);
    cr_set_recv_callback(&inst, mock_recv, NULL);
    recv_count = 0;
    recv_len = 0;

    /* 3 帧分片：LEN=10, LEN=10, LEN=5 */
    uint8_t f1[16] = {0x02, 0x01, 0x20, 0x00, 0x03, 10};  /* CTL: FRAG=1 */
    memset(&f1[6], 'A', 10);
    uint8_t f2[16] = {0x02, 0x01, 0x20, 0x01, 0x03, 10};  /* FRAG, SEQ=1 */
    memset(&f2[6], 'B', 10);
    uint8_t f3[11] = {0x02, 0x01, 0x30, 0x02, 0x03, 5};   /* FRAG+LAST, SEQ=2 */
    memset(&f3[6], 'C', 5);

    cr_feed_frame(&inst, f1, sizeof(f1));
    cr_feed_frame(&inst, f2, sizeof(f2));
    cr_feed_frame(&inst, f3, sizeof(f3));

    TEST_ASSERT_EQUAL_INT(1, recv_count);
    TEST_ASSERT_EQUAL_UINT16(25, recv_len);
}
```

在 `tests/test_main.c` 注册。

- [ ] **Red: 确认测试失败**

```bash
cd build && cmake --build . && ctest --output-on-failure
```

预期: `test_rx_len_overflow_drops_frame` FAIL（当前无 LEN 越界校验）

- [ ] **Green: 最小实现**

`src/comm_route.c` 在 `cr_handle_local_frame` 和 `cr_handle_broadcast_frame` 中，`payload_len = data[5]` 之后添加越界校验：

```c
uint16_t payload_len = data[5];
if (CR_FRAME_HEADER_SIZE + payload_len > len) {
    return; /* LEN 超过帧实际大小，丢弃 */
}
```

- [ ] **Green: 确认测试通过**

```bash
cd build && cmake --build . && ctest --output-on-failure
```

预期: 全部 PASS

- [ ] **Refactor**

无明显重构需要。运行全量测试确认。

- [ ] **Commit**

```bash
git add -A && git commit -m "feat: LEN overflow guard + multiframe LEN tests

RX drops frames where LEN exceeds actual frame size.
Multiframe TX verified to fill per-frame LEN correctly.
Multiframe RX assembly verified to use LEN for accumulation."
```

---

### Task 4: 更新设计文档

**文件：**
- `docs/specs/2026-06-08-tp-route-design.md`

- [ ] 更新"1. 帧格式"章节：帧结构图、帧头大小、设计决策

- [ ] **Commit**

```bash
git add -A && git commit -m "docs: update frame format spec for LEN field"
```
