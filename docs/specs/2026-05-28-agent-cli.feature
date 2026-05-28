Feature: Agent CLI
  A command-line tool for running LLM agents with custom tools, skills, agents,
  session management, and prompt caching.

  # --- Core Execution ---

  Scenario: Single prompt execution
    Given a configured provider "claude"
    And a registered tool "read_file"
    When I run `agent-cli run "read file.txt and summarize"`
    Then the engine sends the prompt to the LLM
    And executes tool calls returned by the LLM
    And loops until no more tool_use in response
    And prints the final assistant response to stdout

  Scenario: Multi-turn conversation with session
    Given an existing session "abc123"
    When I run `agent-cli run -s abc123 "continue the analysis"`
    Then the engine loads session "abc123" from disk
    And appends the new user message to the message tree
    And builds linear history from root to head
    And sends to LLM and processes response
    And persists the updated session

  # --- Session Management ---

  Scenario: Rollback session by N steps
    Given a session "abc123" with 6 messages in the current branch
    When I run `agent-cli session rollback abc123 -n 3`
    Then the session headId moves back 3 ancestors along parentId chain
    And the session is persisted with new headId
    And the rolled-back messages remain in storage (not deleted)

  Scenario: Fork session from a specific message
    Given a session "abc123" with message "msg-4" in its history
    When I run `agent-cli session fork abc123 --at msg-4`
    Then a new session is created with a new id
    And the new session's headId points to "msg-4"
    And the new session contains a copy of all messages from root to "msg-4" (same message IDs, independent file)
    And the original session remains unchanged

  Scenario: Fork session from current head
    Given a session "abc123" with headId "msg-6"
    When I run `agent-cli session fork abc123`
    Then a new session is created forked at "msg-6"

  Scenario: Resume interrupted session
    Given a session "abc123" that was interrupted (persisted mid-execution)
    And the last message in history is from role "user" or "tool_result"
    When I run `agent-cli session resume abc123`
    Then the engine loads the session from headId
    And sends the existing history to the LLM (no new user message appended)
    And processes the LLM response normally (tool loop etc)

  Scenario: Interrupt execution with SIGINT
    Given the engine is running and waiting for LLM response
    When SIGINT is received
    Then the engine waits for current operation to finish or timeout
    And persists the session to disk
    And exits with code 130

  Scenario: List sessions
    Given sessions "abc123" and "def456" exist in sessions directory
    When I run `agent-cli session list`
    Then stdout shows a table with session id, created time, last message preview

  # --- Tool Registration ---

  Scenario: Load custom tools from tools directory
    Given a file "tools/read-file.ts" exporting a valid ToolDefinition
    When the engine starts
    Then it dynamically imports all .ts files from tools/
    And registers their definitions for LLM tool_use

  Scenario: Tool execution during agent loop
    Given the LLM responds with a tool_use block for "read_file"
    When the engine processes the response
    Then it calls the tool's execute function with the parameters
    And appends the result as a tool_result message
    And continues the loop

  Scenario: Tool execution error
    Given a tool "read_file" that throws an error
    When the engine executes the tool
    Then it appends an error tool_result message (is_error: true)
    And continues the loop (LLM sees the error)

  # --- Skill Registration ---

  Scenario: Load skills from skills directory
    Given a file "skills/code-review.md" with valid frontmatter
    When the engine starts
    Then it parses the md file extracting name, trigger, and body
    And makes the skill available for system prompt injection

  Scenario: Skill activation via CLI flag
    Given skill "code-review" is registered
    When I run `agent-cli run --skill code-review "review this"`
    Then the skill body is included in system messages for this run

  Scenario: Skill activation via agent config
    Given agent "reviewer" has skills: ["code-review"] in its definition
    When the agent is spawned
    Then the skill body is included in the agent's system messages

  # --- Agent Registration ---

  Scenario: Load agent definitions from agents directory
    Given a file "agents/researcher.md" with frontmatter (name, model, tools)
    When the engine starts
    Then it parses the agent definition and registers it

  # --- Sub-Agent ---

  Scenario: Spawn sub-agent
    Given an agent definition "researcher" with model and tools config
    And the LLM calls tool "spawn_agent" with agent="researcher" prompt="find X"
    When the engine handles spawn_agent
    Then it creates a new independent session (with parentSessionId)
    And runs the sub-agent engine loop with the agent's config
    And returns the sub-agent's final response as tool_result to parent

  Scenario: Sub-agent cannot spawn nested sub-agents
    Given a sub-agent is running
    When the sub-agent's LLM tries to call "spawn_agent"
    Then the tool returns an error "sub-agents cannot spawn further agents"

  # --- Prompt Caching (Claude) ---

  Scenario: Claude provider applies cache_control
    Given provider is "claude"
    And conversation has 5 prior turns
    When building the API request
    Then the last system message block has cache_control: {type: "ephemeral"}
    And a cache breakpoint is set on the second-to-last user message's last content block

  Scenario: Non-Claude provider ignores cache fields
    Given provider is "openai"
    When building the API request
    Then no cache_control fields are included in the payload

  # --- Provider ---

  Scenario: Switch provider via CLI flag
    Given default provider is "claude"
    When I run `agent-cli run -p openai "hello"`
    Then the engine uses the "openai" provider for this run

  Scenario: Stream response
    Given the LLM streams tokens
    When the engine receives stream events
    Then it prints tokens to stdout incrementally
