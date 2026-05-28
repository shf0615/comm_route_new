---
name: sp-finishing
description: 实现完成、review通过后，引导分支集成（merge/PR/保留/丢弃）。
---

# 完成分支

## 概述

验证测试 → 提供集成选项 → 执行 → 清理。

**启动声明：** "我正在使用 finishing 技能来完成这项工作。"

## 前置条件

- 当前在 feature branch 上（非 main/master）
- 实现已完成且 review 通过

如果在主分支上 → 停止，提示："当前在主分支上，finishing 技能适用于 feature branch。"

## 流程

### Step 1: 验证全部测试通过

自动检测项目类型并运行对应测试命令：

```bash
# 自动检测顺序
[ -f pyproject.toml ] && pytest
[ -f package.json ] && npm test
[ -f Cargo.toml ] && cargo test
[ -f go.mod ] && go test ./...
[ -f Makefile ] && make test
```

测试失败 → 停止，先修复。

### Step 2: 提供选项

```
实现完成，全部测试通过。接下来：

1. 合并回 <base-branch>
2. 推送并创建 Pull Request
3. 保持当前分支
4. 丢弃这项工作
```

### Step 3: 执行

#### 合并
```bash
git checkout <base-branch> && git pull && git merge <feature-branch>
# 验证合并后测试仍通过
# 清理：删除 feature branch + worktree（如有）
```

#### PR
```bash
git push -u origin <feature-branch>
gh pr create --title "<title>" --body "<body>"
# 不清理 worktree（需迭代 PR 反馈）
```

#### 保持
报告分支名和路径。

#### 丢弃
必须用户确认后执行。清理 worktree（如有）+ 删除分支。

## 约束

- 测试失败时不提供选项
- 非 feature branch 不执行
- 丢弃前必须确认
- 合并后验证测试通过
