---
name: sp-git-worktrees
description: 确保工作在隔离空间进行。优先使用平台原生工具，fallback到git worktree。
---

# Git Worktrees

## 概述

确保开发工作在隔离工作空间中进行。

**启动声明：** "我正在使用 git-worktrees 技能来设置隔离工作空间。"

## 流程

### 检测已有隔离

```bash
GIT_DIR=$(cd "$(git rev-parse --git-dir)" 2>/dev/null && pwd -P)
GIT_COMMON=$(cd "$(git rev-parse --git-common-dir)" 2>/dev/null && pwd -P)

# 如果 GIT_DIR != GIT_COMMON → 已在 worktree 中
# 排除子模块：
git rev-parse --show-superproject-working-tree 2>/dev/null
# 非空 → 是子模块，不算 worktree
```

- 已在 worktree 中 → 跳过创建，直接进入项目设置
- 不在 worktree 中 → 询问用户是否需要隔离

### 创建

**优先使用平台原生工具**（如 `EnterWorktree`）。无原生工具时用 git worktree。

**Base branch 选择：**
- 默认：从 `origin/main`（或 `origin/master`）创建
- 如用户指定了基准分支，使用指定的
- 检测默认分支：`git remote show origin | grep 'HEAD branch'`

**创建命令：**
```bash
BRANCH_NAME="feature/<描述性名称>"
WORKTREE_PATH=".worktrees/$BRANCH_NAME"

# 验证 .worktrees 在 .gitignore 中
git check-ignore -q .worktrees 2>/dev/null || echo ".worktrees" >> .gitignore

git worktree add "$WORKTREE_PATH" -b "$BRANCH_NAME" origin/main
cd "$WORKTREE_PATH"
```

### 项目设置

自动检测并运行：
```bash
[ -f package.json ] && npm install
[ -f Cargo.toml ] && cargo build
[ -f requirements.txt ] && pip install -r requirements.txt
[ -f pyproject.toml ] && pip install -e . 2>/dev/null || poetry install
[ -f go.mod ] && go mod download
```

**依赖安装失败处理：**
- 报告具体错误
- 询问用户：继续（可能测试会失败）/ 中止并修复依赖

### 验证基线

运行测试确保工作空间干净。测试失败 → 报告并征询是否继续。

## 约束

- 已在 worktree 中不重复创建
- 有原生工具时不用 git 命令
- 创建前必须验证 .gitignore
- 依赖安装失败必须报告
