---
name: sp-review
description: 实现完成后基于行为规格验收。逐 Scenario 验证，确保实现与规格对齐。
---

# 实现审查

## 概述

基于行为规格逐 Scenario 验证实现，确保代码行为与规格一致。

**启动声明：** "我正在使用 review 技能来审查实现。"

## 前置条件

- 实现已完成（所有 Task 执行完毕）
- 行为规格文件可用
- 技术方案可用

## 流程

```
加载行为规格 + 技术方案
  → 逐 Scenario 运行测试验证
  → 检查技术方案一致性
  → 输出审查报告
  → 有未通过？→ 修正后重新审查
  → 全部通过 → 进入 finishing
```

## 审查内容

### 1. Scenario 逐条验证

对每个 Scenario 运行对应测试，收集证据：

```
Feature: 用户登录
  Scenario: 成功登录
  - 测试: test_successful_login
  - 运行: pytest tests/auth/test_login.py::test_successful_login -v
  - 结果: ✅ PASS
```

**铁律：没有运行测试就不能声称通过。**

### 2. 技术方案一致性

- 模块划分是否符合方案？
- 接口是否与方案匹配？
- 有偏离的部分需给出理由

### 3. TDD 纪律回顾

- 每个 Scenario 是否都有对应测试？
- 测试是否覆盖 Given-When-Then 的全部断言？
- 是否有实现了但没有测试的行为？

### 4. 规格遗漏检查

**主动发现行为规格中应有但缺失的 Scenario：**

检查方法：
- 审视实现代码中的条件分支 — 每个分支是否都有 Scenario 覆盖？
- 审视接口的输入空间 — 是否有未覆盖的输入类型/边界值？
- 对称性检查 — 如果 A→B 有异常 Scenario，B→A 是否也有？
- 审视错误处理代码 — 每种错误路径是否都有 Scenario？

发现遗漏时：
```
### 规格遗漏
| 遗漏描述 | 建议补充的 Scenario | 严重程度 |
|----------|---------------------|----------|
| fahrenheit_to_celsius 未覆盖无效输入 | Scenario: F→C 无效输入 | 高 |
```

**高严重程度遗漏 → 必须补充 Scenario + 测试后才能进入 finishing。**
补充流程：更新 `.feature` → 写失败测试 → 实现 → 绿灯。

### 5. 审查报告

```markdown
## 审查报告

### Scenario 覆盖
| Feature | Scenario | 测试 | 状态 | 证据 |
|---------|----------|------|------|------|
| 用户登录 | 成功登录 | test_successful_login | ✅ | PASS |
| 用户登录 | 密码错误 | test_wrong_password | ✅ | PASS |

### 技术方案一致性
- [一致/偏离说明]

### 未通过项
1. [问题] — [需要的修正]

### 结论
- 可以进入 finishing：是/否
```

## 未通过处理

1. 列出未通过项
2. 修正（进入 debugging 技能或直接 Red-Green-Refactor）
3. 重新验证
4. 循环直到全部通过

## 完成后

全部通过 → 调用 `sp-finishing` 技能。
