Feature: Agent CLI 核心功能

  Background:
    Given agent-cli 进程已启动并监听 stdin
    And 通信协议为 JSON-RPC over stdin/stdout

  # === 自定义 Tool ===

  Scenario: 注册自定义 tool
    Given 一个 tool 定义文件 "tools/search.json" 包含 JSON Schema 和可执行路径
    When 客户端发送 "tools/register" 请求引用该定义文件
    Then CLI 返回成功确认
    And 该 tool 在后续对话中可被 LLM 调用

  Scenario: LLM 调用自定义 tool
    Given 已注册 tool "file_search" 指向脚本 "./scripts/search.sh"
    When LLM 决定调用 "file_search" 并传入参数 {"query": "hello"}
    Then CLI 执行 "./scripts/search.sh" 并传入参数
    And 将脚本 stdout 作为 tool 结果返回给 LLM

  # === 自定义 Skill & Agent ===

  Scenario: 注册自定义 skill
    Given 一个 skill 定义文件包含系统 prompt 和关联的 tool 列表
    When 客户端发送 "skills/register" 请求
    Then 该 skill 可在后续会话中被引用

  Scenario: 注册自定义 agent
    Given 一个 agent 定义文件包含 skill 引用、模型配置和行为约束
    When 客户端发送 "agents/register" 请求
    Then 该 agent 可被用作主 agent 或子 agent

  # === 会话管理 ===

  Scenario: 创建新会话
    When 客户端发送 "session/create" 请求指定 agent
    Then CLI 返回 session_id
    And 会话状态被持久化

  Scenario: 恢复已有会话
    Given 一个已存在的 session_id
    When 客户端发送 "session/resume" 请求
    Then CLI 加载历史对话上下文
    And 后续消息在该上下文上继续

  Scenario: 打断正在执行的请求
    Given 一个正在处理 LLM 调用的会话
    When 客户端发送 "session/interrupt" 请求
    Then CLI 中止当前 LLM 调用
    And 返回已生成的部分结果（如有）
    And 会话状态保持一致

  Scenario: 回退到历史点
    Given 一个有 5 轮对话的会话
    When 客户端发送 "session/rollback" 请求指定 turn_index=3
    Then 会话状态回退到第 3 轮之后的状态
    And 第 4、5 轮的消息被移除

  Scenario: 从历史点 fork 新会话
    Given 一个有 5 轮对话的会话 "session-A"
    When 客户端发送 "session/fork" 请求指定 source="session-A" turn_index=3
    Then 创建新会话 "session-B" 包含前 3 轮对话
    And "session-A" 保持不变

  # === 子 Agent ===

  Scenario: 主 agent 调用子 agent
    Given 已注册 agent "code-reviewer"
    And 主 agent 在对话中决定调用子 agent
    When 子 agent "code-reviewer" 被启动
    Then 子 agent 独立完成任务并返回结果
    And 主 agent 将结果纳入自身对话继续

  Scenario: 子 agent 继承父 agent 的 tool 访问权限
    Given 主 agent 有 tool "file_read"
    When 子 agent 被启动且定义中未限制 tool 访问
    Then 子 agent 可以使用 "file_read"

  # === Prompt 缓存 ===

  Scenario: 利用 provider prompt caching
    Given 使用 Anthropic provider 且会话有较长的系统 prompt
    When 连续发送多条用户消息
    Then 系统 prompt 部分命中 provider 的 prompt cache
    And 响应中报告 cache hit 的 token 数

  Scenario: 会话恢复时最大化缓存命中
    Given 一个恢复的会话有 20 轮历史
    When 发送新消息
    Then CLI 构造请求时保持历史消息前缀稳定
    And provider 缓存命中率最大化

  # === 多 Provider 支持 ===

  Scenario: 切换 LLM provider
    Given 配置中定义了 "anthropic" 和 "openai" 两个 provider
    When agent 定义中指定 model="gpt-4o"
    Then CLI 使用 OpenAI provider 发送请求

  Scenario: Provider 级别配置
    Given provider "anthropic" 配置了 api_key 和 base_url
    When 使用该 provider 的 agent 发送请求
    Then 请求使用对应的认证信息和端点
