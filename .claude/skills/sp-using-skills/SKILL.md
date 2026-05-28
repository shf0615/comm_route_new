---
name: sp-using-skills
description: 技能调度入口。决定何时调用哪个技能。
---

<SUBAGENT-STOP>
如果你是被派遣的子agent来执行特定任务，跳过此技能。
</SUBAGENT-STOP>

# 技能调度

## 优先级

1. **用户显式指令** — 最高
2. **技能** — 覆盖默认行为
3. **默认系统提示** — 最低

## 规则

在任何响应之前，调用相关技能。即使只有 1% 可能性适用。

## 主链条（BDD + TDD）

```
brainstorming（需求 → 行为规格 .feature）
  → writing-plans（行为规格 → TDD 实现计划）
  → git-worktrees（隔离工作空间）
  → executing-plans（Red-Green-Refactor 逐步执行）
  → review（逐 Scenario 验证）
  → finishing（集成/PR/丢弃）
```

## 独立链条

```
sp-testing — 基于行为规格全面测试
sp-optimization — 测试绿灯保护下优化
debugging — 根因调查 → TDD 修复
```

## 技能列表

| 技能 | 触发条件 |
|------|---------|
| `sp-brainstorming` | 新功能、新需求 |
| `sp-writing-plans` | 有行为规格，准备规划 |
| `sp-executing-plans` | 有计划，准备执行 |
| `sp-dispatching-agents` | 2+ 独立任务可并行 |
| `sp-review` | 实现完成，需验收 |
| `sp-optimization` | 优化现有代码 |
| `sp-testing` | 全面测试 |
| `sp-debugging` | bug、测试失败、意外行为 |
| `sp-finishing` | 审查通过，准备集成 |
| `sp-git-worktrees` | 需要隔离工作空间 |

## 技能优先级

1. **流程技能**（brainstorming、debugging）— 决定如何处理
2. **实现技能**（executing-plans、dispatching-agents）— 指导执行
