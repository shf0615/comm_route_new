# Agent CLI 设计规格

## 概述

一个独立的 CLI 进程，通过 stdin/stdout JSON-RPC 2.0 协议为其他系统提供 LLM agent 能力。支持自定义 tool/skill/agent、完整会话管理（恢复/打断/回退/fork）、同步嵌套子 agent、以及利用 provider 级别的 prompt caching。

## 技术栈

- **语言：** TypeScript (Node.js)
- **通信：** JSON-RPC 2.0 over stdin/stdout（每行一个 JSON 对象）
- **存储：** 本地文件系统（事件溯源式 JSONL）

---

## 1. 整体架构

```
调用方 ←→ [stdin/stdout JSON-RPC] ←→ Agent CLI 进程
                                          │
                    ┌───────────────────────┼───────────────────────┐
                    │                       │                       │
              RPC Router            Session Manager           Registry
              (方法分发)            (会话生命周期)         (tool/skill/agent)
                    │                       │                       │
                    └───────────┬───────────┘                       │
                                │                                   │
                          Agent Loop ←──────────────────────────────┘
                          (对话循环)
                                │
                    ┌───────────┼───────────┐
                    │           │           │
              LLM Provider   Tool Executor  Sub-Agent Spawner
              (多provider)   (外部进程)     (同步嵌套)
```

单进程，通过 session 方法切换活跃会话。

---

## 2. 通信协议

### 2.1 请求方法

| 方法 | 说明 |
|------|------|
| `tools/register` | 注册自定义 tool |
| `skills/register` | 注册自定义 skill |
| `agents/register` | 注册自定义 agent |
| `session/create` | 创建新会话，指定 agent |
| `session/resume` | 恢复已有会话 |
| `session/list` | 列出所有会话 |
| `session/rollback` | 回退到指定 turn_index |
| `session/fork` | 从指定 turn_index 分叉新会话 |
| `session/interrupt` | 打断当前 LLM 调用 |
| `chat/send` | 发送用户消息 |

### 2.2 通知（server → client）

| 通知 | 说明 |
|------|------|
| `chat/chunk` | 流式 token 输出 |
| `chat/tool_call` | LLM 发起 tool 调用 |
| `chat/tool_result` | tool 执行结果 |
| `chat/done` | 本轮对话完成 |

### 2.3 消息格式示例

```json
// 请求
{"jsonrpc":"2.0","id":1,"method":"chat/send","params":{"message":"帮我搜索文件"}}

// 流式通知
{"jsonrpc":"2.0","method":"chat/chunk","params":{"session_id":"s1","content":"正在"}}

// 完成通知
{"jsonrpc":"2.0","method":"chat/done","params":{"session_id":"s1","turn_index":3}}

// 响应
{"jsonrpc":"2.0","id":1,"result":{"status":"done","turn_index":3}}
```

---

## 3. Registry（扩展注册）

### 3.1 Tool 定义

```json
{
  "name": "file_search",
  "description": "搜索文件内容",
  "input_schema": {
    "type": "object",
    "properties": {
      "query": {"type": "string"}
    },
    "required": ["query"]
  },
  "executor": {
    "command": "./scripts/search.sh",
    "args": ["--query", "{{query}}"],
    "timeout_ms": 30000
  }
}
```

- 定义文件为 JSON，包含 JSON Schema 描述参数 + 可执行文件路径
- 执行时通过 stdin 传入参数 JSON，从 stdout 读取结果
- 支持 timeout 配置

### 3.2 Skill 定义

```json
{
  "name": "code-review",
  "system_prompt": "你是一个代码审查专家...",
  "tools": ["file_read", "file_search"],
  "constraints": {
    "max_turns": 10
  }
}
```

- Skill = 系统 prompt + 关联 tool 列表 + 行为约束

### 3.3 Agent 定义

```json
{
  "name": "reviewer",
  "skill": "code-review",
  "model": "claude-sonnet-4-20250514",
  "provider": "anthropic",
  "temperature": 0.3,
  "max_tokens": 4096
}
```

- Agent = skill 引用 + 模型配置
- 可用作主 agent 或子 agent

### 3.4 注册方式

- 启动时从配置目录自动加载（`./agents/`, `./tools/`, `./skills/`）
- 运行时通过 JSON-RPC 方法动态注册

---

## 4. 会话管理

### 4.1 事件溯源存储

每个会话对应一个 JSONL 文件：`sessions/{session_id}.jsonl`

事件类型：
```typescript
type SessionEvent =
  | { type: "session_created"; agent: string; timestamp: string }
  | { type: "user_message"; content: string; turn_index: number }
  | { type: "assistant_message"; content: string; turn_index: number }
  | { type: "tool_call"; name: string; input: object; call_id: string; turn_index: number }
  | { type: "tool_result"; call_id: string; output: string; turn_index: number }
  | { type: "sub_agent_call"; agent: string; input: string; turn_index: number }
  | { type: "sub_agent_result"; output: string; turn_index: number }
```

### 4.2 操作实现

- **创建：** 生成 session_id，写入 `session_created` 事件
- **恢复：** 读取 JSONL 文件，重放事件重建消息数组
- **打断：** 中止当前 LLM 请求（AbortController），记录已生成部分
- **回退：** 截断 JSONL 文件到指定 turn_index 对应的最后一个事件
- **Fork：** 复制前 N 个事件到新 JSONL 文件，分配新 session_id

### 4.3 元数据索引

维护 `sessions/index.json` 用于快速列出会话：
```json
{
  "sessions": [
    {"id": "s1", "agent": "reviewer", "created_at": "...", "last_turn": 5}
  ]
}
```

---

## 5. Agent Loop（对话循环）

```
用户消息 → 构造 LLM 请求 → 调用 Provider → 解析响应
                                                    │
                                          ┌─────────┴─────────┐
                                          │                   │
                                    纯文本响应           tool_use 响应
                                          │                   │
                                    记录事件，           执行 tool/子agent，
                                    发送 done            记录结果，循环
```

- 循环直到 LLM 返回纯文本（无 tool 调用）
- 每一步都写入事件日志
- 支持流式输出（通过 chat/chunk 通知）

---

## 6. Tool Executor

- 通过 `child_process.spawn` 执行外部命令
- 参数传递：将 tool input JSON 写入子进程 stdin
- 结果获取：读取子进程 stdout
- 错误处理：超时 kill、非零退出码报错
- 子进程环境变量继承父进程

---

## 7. 子 Agent

- 主 agent 在 tool 定义中暴露一个内置 tool `spawn_agent`
- LLM 调用 `spawn_agent(agent_name, task)` 时：
  1. 根据 agent_name 从 Registry 加载 agent 定义
  2. 创建内存中的临时会话（不持久化到磁盘，随父请求完成后释放）
  3. 运行独立的 Agent Loop 直到完成
  4. 将最终结果作为 tool_result 返回给父 agent
- 子 agent 默认继承父 agent 的 tool 列表，除非 agent 定义中有 `tools_override`
- 嵌套深度限制：默认最大 3 层

---

## 8. LLM Provider

### 8.1 接口抽象

```typescript
interface LLMProvider {
  name: string;
  chat(params: ChatParams): AsyncIterable<ChatChunk>;
  supportsPromptCache(): boolean;
}
```

### 8.2 已支持 Provider

- **Anthropic：** Claude 系列，支持 prompt caching（cache_control 标记）
- **OpenAI：** GPT 系列，支持 structured outputs

### 8.3 配置

```json
{
  "providers": {
    "anthropic": {
      "api_key": "sk-...",
      "base_url": "https://api.anthropic.com"
    },
    "openai": {
      "api_key": "sk-...",
      "base_url": "https://api.openai.com/v1"
    }
  }
}
```

---

## 9. Prompt Cache 策略

- 对支持 prompt caching 的 provider（如 Anthropic），在构造请求时：
  1. 系统 prompt 标记为可缓存（`cache_control: {type: "ephemeral"}`）
  2. 历史消息保持前缀稳定（不重排、不截断前部）
  3. 会话恢复时完整加载历史，确保前缀与上次请求一致
- 响应中提取 `cache_creation_input_tokens` 和 `cache_read_input_tokens`，通过 `chat/done` 通知报告给调用方
- 不做本地语义缓存

---

## 10. 错误处理

- JSON-RPC 标准错误码（-32600 无效请求、-32601 方法不存在等）
- 业务错误使用自定义错误码（1000+）：
  - 1001: session_not_found
  - 1002: agent_not_found
  - 1003: tool_execution_failed
  - 1004: llm_provider_error
  - 1005: interrupt_received
- Tool 执行超时/失败不终止会话，作为 tool_result 错误返回给 LLM

---

## 11. 项目结构

```
agent-cli/
├── src/
│   ├── index.ts              # 入口，启动 RPC server
│   ├── rpc/
│   │   ├── router.ts         # 方法路由
│   │   └── transport.ts      # stdin/stdout 读写
│   ├── registry/
│   │   ├── tools.ts
│   │   ├── skills.ts
│   │   └── agents.ts
│   ├── session/
│   │   ├── manager.ts        # 会话 CRUD
│   │   ├── event-store.ts    # JSONL 读写
│   │   └── types.ts
│   ├── agent-loop/
│   │   ├── loop.ts           # 核心循环
│   │   ├── tool-executor.ts  # 外部进程调用
│   │   └── sub-agent.ts      # 子 agent 生成
│   └── providers/
│       ├── interface.ts       # LLMProvider 接口
│       ├── anthropic.ts
│       └── openai.ts
├── config/                    # 默认配置目录
│   ├── providers.json
│   ├── tools/
│   ├── skills/
│   └── agents/
├── sessions/                  # 会话存储目录
├── package.json
└── tsconfig.json
```

---

## 12. 非功能需求

- **无鉴权：** 信任调用方（同机进程间通信）
- **单会话活跃：** 同一时刻只有一个活跃会话在执行 LLM 调用
- **优雅退出：** 收到 SIGTERM 时完成当前事件写入后退出
- **日志：** stderr 输出运行日志（不干扰 stdout 的 JSON-RPC 通信）
