---
name: sp-testing
description: 独立测试链条。基于行为规格编写 BDD 测试并执行，循环迭代直到连续多次通过。
---

# 全面测试

## 概述

基于行为规格（`.feature`）编写并执行测试，确保所有 Scenario 被覆盖。

**启动声明：** "我正在使用 sp-testing 技能来进行全面测试。"

## 前置条件

- 行为规格文件（`.feature`）
- 已实现的代码

## 流程

```
用户配置测试范围
  → 派遣测试子agent
  → 子agent编写测试并执行
  → 有失败？→ 修复并重新执行
  → 连续 N 次通过？→ 结束
```

## Step 1: 用户配置

### 测试层级（可多选）：
1. **单元测试** — 函数/模块级别
2. **集成测试** — 模块间交互
3. **端到端测试** — 完整 Scenario 流程

### 额外覆盖（可多选）：
1. **边界情况** — 极值、空值、溢出
2. **异常路径** — 错误输入、网络失败、超时
3. **性能** — 压力测试、响应时间
4. **安全** — 注入、越权、数据泄漏

### 连续通过次数：默认 3 次。

## Step 2: 派遣测试子agent

子agent职责：

1. **从行为规格 Scenario 直接派生测试**（一个 Scenario = 一个测试函数）：

```python
# 源自 Feature: 用户登录 / Scenario: 成功登录
def test_successful_login():
    # Given 用户已注册且账户激活
    user = create_active_user(email="test@example.com", password="valid123")

    # When 用户提交正确凭证
    result = login(email="test@example.com", password="valid123")

    # Then 返回有效会话
    assert result.session is not None
    assert result.session.is_valid()
```

2. **从 Scenario Outline Examples 派生参数化测试：**

```python
@pytest.mark.parametrize("email,password,expected", [
    ("valid@test.com", "correct", LoginResult.SUCCESS),
    ("valid@test.com", "wrong", LoginResult.INVALID),
    ("", "any", LoginResult.INVALID_INPUT),
])
def test_login_scenarios(email, password, expected):
    # Given ...
    # When ...
    # Then ...
```

3. 根据用户配置补充额外测试

每个测试标注来源：
- `[Feature/Scenario名]` — 行为规格直接派生
- `[Feature/Outline/Examples]` — 参数化派生
- `[额外-边界]` / `[额外-异常]` 等 — 独立补充

## Step 3: 执行与修复循环

```
执行 → 有失败？
  → 修复代码bug → 通过计数归零
  → 修复测试bug → 通过计数不归零
  → 全部通过 → 计数+1
  → 达到连续通过次数？→ 结束
```

## Step 4: 测试报告

```markdown
## 测试报告

### 概览
- 总用例数：N
- Scenario 派生：X
- 额外补充：Y
- 最终状态：连续 M 次通过

### Scenario 覆盖
| Feature | Scenario | 测试函数 | 状态 |
|---------|----------|----------|------|
| 用户登录 | 成功登录 | test_successful_login | ✅ |
| 用户登录 | 密码错误 | test_wrong_password | ✅ |

### 修复记录
- 迭代1：发现 X bug，修复方式 Y
- 迭代2-4：连续 3 次通过，结束
```

## 约束

- 行为规格中每个 Scenario 必须有对应测试，不可跳过
- 测试结构必须映射 Given-When-Then（setup-act-assert）
- 修复代码bug后通过计数归零
