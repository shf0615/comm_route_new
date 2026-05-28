# Agent CLI - Technical Design

## Architecture

```
CLI (commander) → Engine → Provider (pluggable)
                    ↕
              Session (message tree, JSON persistence)
                    ↕
              Registry (tools, skills, agents)
                    ↕
              Sub-Agent (independent session + engine loop)
```

## Components

### Message Tree (`src/core/message-tree.ts`)

- `Message`: `{ id, parentId, role, content, timestamp }`
- `Session`: `{ id, headId, messages: Record<id, Message>, metadata }`
- Operations: append, rollback(n), fork(msgId), buildHistory() → Message[]
- buildHistory: walk parentId chain from head to root, reverse

### Session Manager (`src/core/session.ts`)

- Persistence: `sessions/{session-id}.json` — full session written on every mutation
- SIGINT handler: persist current state, exit 130
- List/show/rollback/fork/resume commands delegate here

### Engine (`src/core/engine.ts`)

- Main loop: send messages → process response → execute tools → repeat until no tool_use
- Builds system prompt (base + active skills)
- Handles SIGINT gracefully

### Provider (`src/providers/`)

- Interface: `chat(params) → AsyncIterable<StreamEvent>`
- StreamEvent: `{ type: 'text_delta' | 'tool_use' | 'done', ... }`
- Claude: uses `@anthropic-ai/sdk`, adds cache_control
- OpenAI: uses `openai` SDK, standard format

### Prompt Cache Strategy (Claude only)

- System: add `cache_control: { type: "ephemeral" }` to the last system message block
- Messages: add `cache_control` to the last content block of the second-to-last user message
- Effect: system + conversation prefix cached, only latest turn billed fully

### Tool Registry (`src/registry/tool.ts`)

- Scans `tools/*.ts`, dynamic import, collects ToolDefinition[]
- ToolDefinition: `{ name, description, parameters (JSON Schema), execute(params) → any }`
- Execute wraps in try/catch, returns `{ result }` or `{ error, is_error: true }`

### Skill Registry (`src/registry/skill.ts`)

- Scans `skills/*.md`, parses YAML frontmatter + body
- SkillDefinition: `{ name, trigger, body }`
- Activation: via `--skill` CLI flag or agent definition's `skills` field; injects body into system messages

### Agent Registry (`src/registry/agent.ts`)

- Scans `agents/*.md`, parses YAML frontmatter + body
- AgentDefinition: `{ name, description, model, tools: string[], systemPrompt }`

### Sub-Agent (`src/sub-agent.ts`)

- `spawn_agent` is a built-in tool
- Creates independent Session (metadata.parentSessionId links to parent)
- Runs Engine with agent's model/tools/system prompt
- Single-level only: sub-agent's tool registry excludes `spawn_agent`
- Returns final assistant content as tool_result

### CLI (`src/cli.ts`)

- `agent-cli run <prompt>` — main execution
- `agent-cli session {list|show|rollback|fork|resume}` — session management
- `agent-cli {tool|skill|agent} list` — registry inspection
- `agent-cli config {init|set}` — configuration

## Configuration (`agent-cli.json`)

```json
{
  "provider": "claude",
  "model": "claude-sonnet-4-20250514",
  "providers": {
    "claude": { "apiKey": "env:ANTHROPIC_API_KEY" },
    "openai": { "apiKey": "env:OPENAI_API_KEY", "baseUrl": "..." }
  },
  "toolsDir": "./tools",
  "skillsDir": "./skills",
  "agentsDir": "./agents",
  "sessionsDir": "./sessions"
}
```

## Error Handling

- Tool errors: caught, returned as `is_error: true` tool_result, loop continues
- Provider errors: retry once with backoff, then throw (CLI prints error, exits 1)
- Session file corruption: error message, suggest rollback or new session
- SIGINT: graceful persist + exit

## Dependencies

- `commander` — CLI parsing
- `@anthropic-ai/sdk` — Claude provider
- `openai` — OpenAI-compatible provider
- `uuid` — message IDs
- `gray-matter` — markdown frontmatter parsing
- `tsx` — dynamic TS import for tools
