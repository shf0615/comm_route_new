---
name: sp-writing-plans
description: 基于行为规格生成 TDD 实现计划。每个步骤遵循 Red-Green-Refactor。
---

# 编写 TDD 实现计划

## 概述

将行为规格转化为逐步 TDD 实现计划。每个 Scenario 对应一个 Task，每个 Task 遵循 Red-Green-Refactor。

**启动声明：** "我正在使用 writing-plans 技能来创建 TDD 实现计划。"

**保存路径：** `docs/plans/YYYY-MM-DD-<feature-name>.md`

## 前置条件

- 经过审查的行为规格文件（`.feature`）
- 经过审查的技术方案

## 流程

```
读取行为规格 + 技术方案
  → Scenario → Task 映射
  → 编写分步 TDD 任务
  → 对抗性审查
  → 用户确认
  → 保存并提交
  → 提供执行选项
```

## 计划文档头

```markdown
# [功能名] TDD 实现计划

**目标：** [一句话]
**行为规格：** `docs/specs/YYYY-MM-DD-<topic>.feature`
**技术方案：** `docs/specs/YYYY-MM-DD-<topic>-design.md`

---
```

## Scenario → Task 映射

每个 Scenario（或 Scenario Outline）= 一个 Task。顺序：
1. **Task 0: 项目脚手架**（如需要）
2. 基础设施/共用代码（Background 相关）
3. 正常路径 Scenarios
4. 异常路径 Scenarios
5. 参数化 Scenarios（Outline）

## Task 0: 项目脚手架

如果是新项目或新模块，第一个 Task 必须建立可运行测试的基础设施：

````markdown
### Task 0: 项目脚手架

- [ ] 创建目录结构和测试配置（按项目语言/框架）

**Python:**
```
src/module_name/
tests/
pyproject.toml  # [tool.pytest.ini_options] testpaths = ["tests"]
```

**Node/TypeScript:**
```
src/
tests/  (or __tests__/)
package.json  # "scripts": { "test": "jest" } 或 vitest
tsconfig.json (如 TS)
```

**Rust:**
```
src/lib.rs  (或 src/main.rs)
Cargo.toml
tests/     (集成测试目录)
```

**Go:**
```
cmd/ 或 pkg/
go.mod
xxx_test.go (与源码同目录)
```

**C/C++:**
```
src/
tests/
CMakeLists.txt 或 Makefile (含测试target)
```

- [ ] 验证空测试可运行

```bash
# 对应语言的测试命令应能执行且无报错
pytest --co -q / npm test / cargo test / go test ./... / make test
```

- [ ] Commit

```bash
git commit -m "chore: project scaffold"
```
````

**跳过条件：** 如果项目已有测试框架配置且测试命令可正常执行，跳过 Task 0。

## Task 结构（Red-Green-Refactor）

````markdown
### Task N: [Scenario 名] 

**Scenario:**
```gherkin
Given [...]
When [...]
Then [...]
```

**文件：**
- 测试: `tests/path/test_xxx.py`
- 实现: `src/path/xxx.py`

- [ ] **Red: 编写失败测试**

将 Scenario 直接翻译为测试代码：

```python
def test_scenario_name():
    # Given
    [setup code]

    # When
    result = [action]

    # Then
    assert [expected]
```

- [ ] **Red: 确认测试失败**

运行: `pytest tests/path/test_xxx.py::test_scenario_name -v`
预期: FAIL

失败类型区分：
- **结构性失败**（ModuleNotFoundError / ImportError）— 文件或模块尚不存在，需先创建空骨架
- **行为性失败**（AssertionError / 逻辑错误）— 模块存在但逻辑未实现

两种都是合法的 RED。但如果是结构性失败，Green 步骤第一步是创建文件骨架（空函数/类），然后再实现逻辑让断言通过。

- [ ] **Green: 最小实现**

用最少代码让测试通过。禁止：
- 超前实现下一个 Scenario 的逻辑
- 添加"以后可能用到"的代码
- 处理其他 Scenario 的边界情况

```python
def function(input):
    # 仅满足当前测试
    return expected
```

- [ ] **Green: 确认测试通过**

运行: `pytest tests/path/test_xxx.py::test_scenario_name -v`
预期: PASS

- [ ] **Refactor: 改善代码**

测试绿灯保护下：
- 消除重复
- 改善命名
- 简化逻辑
- 提取函数/类（如果有明确重复）

运行: `pytest tests/path/ -v`
预期: 全部 PASS（重构不能破坏任何测试）

- [ ] **Commit**

```bash
git add tests/path/test_xxx.py src/path/xxx.py
git commit -m "feat: [scenario description]"
```
````

## Scenario Outline → 参数化测试

```python
@pytest.mark.parametrize("param,expected", [
    ("value1", "result1"),
    ("value2", "result2"),
    ("边界值", "边界结果"),
])
def test_scenario_outline(param, expected):
    # Given
    [setup]

    # When
    result = action(param)

    # Then
    assert result == expected
```

## 禁止占位符

以下必须删除重写：
- "TBD"、"TODO"、"稍后实现"
- "添加适当的错误处理"
- "编写上述的测试"（没有实际代码）
- "类似 Task N"
- 只描述做什么但不展示如何做的步骤

## 对抗性审查

### 自审

1. **Scenario 覆盖：** 行为规格中每个 Scenario 是否都有对应 Task？
2. **TDD 完整性：** 每个 Task 是否都有 Red-Green-Refactor 三步？
3. **最小实现：** Green 步骤是否只写了满足当前测试的代码？
4. **依赖顺序：** 后续 Task 是否依赖前面 Task 的实现？

### 子agent对抗性审查

使用 `sp-adversarial-review`，循环直到通过。

## 执行交接

"计划已保存。执行方式：

1. **子agent驱动（推荐）** — 每个 Task 派遣独立子agent
2. **内联执行** — 在当前会话中逐步执行

选择哪种？"
