# 帧格式文档索引

本文件帮助你快速找到与帧格式相关的所有信息。

## 📋 快速查询

如果你需要：

- **5分钟快速了解帧格式** → 查看 [FRAME_FORMAT_QUICKREF.txt](./FRAME_FORMAT_QUICKREF.txt)
- **深入理解帧格式细节** → 查看 [FRAME_FORMAT_ANALYSIS.md](./FRAME_FORMAT_ANALYSIS.md)
- **查看协议规格书** → 查看 [specs/2026-06-08-tp-route-design.md](./specs/2026-06-08-tp-route-design.md)（第1章）
- **查看内存池设计** → 查看 [../.plan.md](../.plan.md)

## 🔍 按内容查询

### 帧格式结构
- **帧头大小**：5字节（DST, SRC, CTL, SEQ, TTL）
- **帧头定义**：`src/comm_route.c` 第6行 (`CR_FRAME_HEADER_SIZE = 5`)
- **完整结构图**：[FRAME_FORMAT_ANALYSIS.md](./FRAME_FORMAT_ANALYSIS.md) 第1节

### CTL字节位域
- **宏定义**：`src/comm_route.c` 第13-23行
- **详细说明**：[FRAME_FORMAT_ANALYSIS.md](./FRAME_FORMAT_ANALYSIS.md) 第1节
- **快速参考**：[FRAME_FORMAT_QUICKREF.txt](./FRAME_FORMAT_QUICKREF.txt) "CTL字节详解"

### 帧打包和解包
- **帧打包** (`cr_send_frame`)：`src/comm_route.c` 第420-441行
- **ACK帧** (`cr_send_ack`)：`src/comm_route.c` 第603-609行
- **帧解包** (`cr_handle_local_frame`)：`src/comm_route.c` 第615-700行
- **函数速查**：[FRAME_FORMAT_QUICKREF.txt](./FRAME_FORMAT_QUICKREF.txt) "打包/解包函数速查"

### 多帧处理
- **分片逻辑**：`src/comm_route.c` 第300行及其后续
- **接收组装**：`src/comm_route.c` 第615-700行
- **详细说明**：[FRAME_FORMAT_ANALYSIS.md](./FRAME_FORMAT_ANALYSIS.md) 第5节

### 配置相关
- **MTU含义**：帧总大小（含5字节头）
- **配置结构**：`src/comm_route.h` 第19-37行 (`cr_config_t`)
- **配置说明**：[FRAME_FORMAT_ANALYSIS.md](./FRAME_FORMAT_ANALYSIS.md) 第9节

## 📝 文件列表

| 文件 | 大小 | 说明 |
|------|------|------|
| **FRAME_FORMAT_QUICKREF.txt** | 6.7K | 快速参考卡（问题直接答案） |
| **FRAME_FORMAT_ANALYSIS.md** | 6.6K | 详细分析报告（完整内容） |
| **README_FRAME_FORMAT.md** | 本文件 | 索引和导航 |
| ../specs/2026-06-08-tp-route-design.md | 320L | 协议设计规格书 |
| ../.plan.md | 152L | 内存池实现计划 |
| ../src/comm_route.c | 1300L | 核心实现代码 |
| ../src/comm_route.h | 84L | API头文件 |
| ../tests/test_recv.c | 400+L | 接收测试和帧示例 |

## 🎯 按问题查询

### Q1: 帧格式定义在哪些文件中？
- 源代码：`src/comm_route.c` （行6, 13-23, 420-441, 603-609, 617-809）
- 文档：`docs/specs/2026-06-08-tp-route-design.md` （第1章）
- 计划：`.plan.md` （内存池部分）

**详见**：[FRAME_FORMAT_ANALYSIS.md](./FRAME_FORMAT_ANALYSIS.md) 第2节 或 [FRAME_FORMAT_QUICKREF.txt](./FRAME_FORMAT_QUICKREF.txt)

### Q2: 当前帧结构包含哪些字段？
- **5字节头**：DST(1B), SRC(1B), CTL(1B), SEQ(1B), TTL(1B)
- **CTL位域**：ACK标记 | 广播标记 | 分片标记 | 末帧标记 | 业务ID
- **可变载荷**：0~(MTU-5)字节

**详见**：[FRAME_FORMAT_ANALYSIS.md](./FRAME_FORMAT_ANALYSIS.md) 第3节 或 [FRAME_FORMAT_QUICKREF.txt](./FRAME_FORMAT_QUICKREF.txt)

### Q3: 帧的总长度是固定的还是可变的？
- **可变长度**
- **范围**：5字节（仅头）到 MTU字节（含最大载荷）
- **计算**：总长 = 5 + payload_len，其中 payload_len ≤ (mtu - 5)

**详见**：[FRAME_FORMAT_ANALYSIS.md](./FRAME_FORMAT_ANALYSIS.md) 第4节 或 [FRAME_FORMAT_QUICKREF.txt](./FRAME_FORMAT_QUICKREF.txt)

### Q4: 数据区域如何组织？
- **布局**：[5字节头] [可变长数据从字节5开始]
- **单帧**：`payload = &frame[5]`, `len = frame_len - 5`
- **多帧**：按SEQ顺序装配，末帧到达时交付

**详见**：[FRAME_FORMAT_ANALYSIS.md](./FRAME_FORMAT_ANALYSIS.md) 第5节 或 [FRAME_FORMAT_QUICKREF.txt](./FRAME_FORMAT_QUICKREF.txt)

### Q5: 最近与帧格式相关的git提交？
- **最重要**：d7ce5a5 (2026-06-17) - 引入统一内存池，改变MTU语义
- **其他**：e22a7d8, 7c4d194, 78c3b30, 45b38bb, 55ebe03...

**详见**：[FRAME_FORMAT_ANALYSIS.md](./FRAME_FORMAT_ANALYSIS.md) 第6节 或 [FRAME_FORMAT_QUICKREF.txt](./FRAME_FORMAT_QUICKREF.txt)

## 🔧 常用查询命令

```bash
# 查找帧头大小定义
grep "CR_FRAME_HEADER_SIZE" ../src/comm_route.c

# 查找CTL位域宏
grep "CR_CTL_" ../src/comm_route.c | head -10

# 查找帧打包函数
grep -n "cr_send_frame" ../src/comm_route.c

# 查找帧解包函数
grep -n "cr_handle.*frame" ../src/comm_route.c

# 查看帧格式规格
head -45 ./specs/2026-06-08-tp-route-design.md

# 查看实际帧示例
grep -A 5 "uint8_t frame" ../tests/test_recv.c

# 查看CTL位域值
grep -E "0x[0-9a-f]{2}\s*:" ./specs/2026-06-08-tp-route-design.md
```

## 💡 常见问题

**Q: 我需要理解帧格式，应该从哪里开始？**

A: 按以下顺序：
1. 先读 [FRAME_FORMAT_QUICKREF.txt](./FRAME_FORMAT_QUICKREF.txt) 了解基本概念（5分钟）
2. 再读 [FRAME_FORMAT_ANALYSIS.md](./FRAME_FORMAT_ANALYSIS.md) 深入理解（15分钟）
3. 最后看 `src/comm_route.c` 的实现代码（30分钟）

**Q: 我需要找某个特定的函数？**

A: 使用上面的"常用查询命令"或查阅 [FRAME_FORMAT_QUICKREF.txt](./FRAME_FORMAT_QUICKREF.txt) 的"打包/解包函数速查"部分。

**Q: MTU的含义是什么？**

A: **帧总大小**（含5字节头），最大载荷 = mtu - 5。这是从 d7ce5a5 提交开始的定义。

**Q: 帧如何分片？**

A: 设置 CTL 的 Bit5=1 表示分片，Bit4=1 表示末帧。接收端按 SEQ 顺序装配，末帧到达时完成。详见 [FRAME_FORMAT_ANALYSIS.md](./FRAME_FORMAT_ANALYSIS.md) 第5节。

## 📚 相关文档链接

- 通信协议设计：[specs/2026-06-08-tp-route-design.md](./specs/2026-06-08-tp-route-design.md)
- 实现计划：[../.plan.md](../.plan.md)
- 线程安全指南：[integration-thread-safety.md](./integration-thread-safety.md)
- API文档：[../src/comm_route.h](../src/comm_route.h)

## 🎓 学习路径

1. **初级**（理解基础）
   - 读 FRAME_FORMAT_QUICKREF.txt（问题直接答案）
   - 看协议规格第1章（帧格式定义）

2. **中级**（理解实现）
   - 读 FRAME_FORMAT_ANALYSIS.md（详细分析）
   - 看源代码中的打包/解包函数

3. **高级**（理解设计）
   - 研究 .plan.md 中的内存池设计
   - 分析 git 历史中的演变（d7ce5a5 提交最重要）
   - 学习多帧分片、去重、TTL等机制

---

**最后更新**：2026-06-17  
**本文档生成工具**：automated frame format analysis  
**相关分支**：HEAD (d7ce5a5)
