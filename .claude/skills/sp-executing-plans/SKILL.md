---
name: sp-executing-plans
description: 按 TDD 实现计划逐步执行 Red-Green-Refactor。支持子agent驱动和内联执行。
---

# 执行 TDD 实现计划

## 概述

加载计划，逐 Task 执行 Red-Green-Refactor 循环。

**启动声明：** "我正在使用 executing-plans 技能来执行 TDD 计划。"

## 模式 A：子agent驱动（推荐）

```
读取计划，提取所有 Task
  → 每个 Task：
    → 派遣实现子agent（提供 Task 全文 + 上下文）
    → 子agent执行 Red-Green-Refactor + commit
    → 派遣审查子agent（验证 TDD 纪律）
    → 通过？→ 下一个 Task
  → 所有 Task 完成 → 进入 review
```

**TDD 审查子agent检查项：**
- 测试是否先于实现编写？
- 测试是否确实先失败过？
- 实现是否是让测试通过的最小代码？
- 重构后测试是否仍然通过？

**子agent状态：**
- **DONE：** 进入审查
- **NEEDS_CONTEXT：** 补充上下文后重新派遣
- **BLOCKED：** 评估原因，上报用户

## 模式 B：内联执行

在当前会话逐步执行每个 Task 的 Red-Green-Refactor。

## Task 合并

当多个 Task 共享实现逻辑时（如异常处理的多个 Scenario 共用同一个验证函数），允许合并执行：

**合并条件（全部满足才可合并）：**
- Task 的 Green 步骤会产出相同的函数/类
- 合并后每个 Scenario 的测试仍然独立存在
- 合并不会导致实现超前

**合并方式：**
- Red: 同时写出所有相关 Scenario 的测试
- Green: 一次实现共享逻辑
- Refactor: 正常进行
- 每个原始 Scenario 的测试必须单独可运行

**声明格式：**
```
合并执行 Task 3 + Task 4（共享 _validate 函数）
```

## 持续执行

不要在任务间暂停询问"继续吗？"。连续执行。唯一停止理由：
- 被阻塞无法自行解决
- 测试反复失败（3次以上）→ 进入 debugging 技能
- 所有任务完成

## TDD 纪律

**铁律：**
1. 没有失败测试，不写实现代码
2. 只写让当前测试通过的最少代码
3. 测试通过后必须审视是否需要重构
4. 重构后必须重跑测试

**RED 的两种形态：**
- **结构性 RED**（ImportError/ModuleNotFoundError）— 文件不存在。Green 第一步：创建空骨架文件，重跑测试看到行为性 RED，再实现逻辑。
- **行为性 RED**（AssertionError）— 文件存在但逻辑未实现。Green：直接实现最小逻辑。

**违规信号（必须停止并修正）：**
- 写了测试还没运行就开始写实现
- 实现了下一个 Scenario 的逻辑
- 跳过了 Refactor 步骤
- 重构后没重跑测试

## 完成后

所有 Task 完成 → 调用 `sp-review` 技能。
