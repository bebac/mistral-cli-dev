# Mistral CLI

A **command-line interface** for interacting with [Mistral AI](https://mistral.ai/) models, featuring **real-time streaming**, **tool integration**, an interactive console, and a full-screen **TUI**.

Built with **C++20**, **coroutines** (via [dripcore](https://github.com/bebac/dripcore-dev)), **HTTP/2** (via [stunning-waddle](https://github.com/bebac/stunning-waddle)), and a terminal UI (via [cpp-tui](https://github.com/bebac/cpp-tui)).

---

## Features

### Chat & interface

- **Interactive chat** — stream responses in real-time.
- **TUI mode** — full-screen terminal UI (`mistral-tui`) with selectable themes.
- **Console history** — persists command history between sessions.
- **Session persistence** — conversations are saved to
  `.mistral-cli/chats/` as they happen; restart with `-r`/`--resume`
  to continue the latest session.

### Tools & workspace

- **Tool integration** — file listing/reading, syntax-aware code exploration (tree-sitter), exec, notes, task tracking, and patchset-based file editing.
- **Multi-agent configuration** — models, agents, and tool policy configured via `.mistral-cli/mistral-cli.json`.
- **Workspace bootstrap** — default configuration files are embedded in the executables and seeded automatically on startup.

### Engineering

- **HTTP/2 + TLS** — efficient, secure communication with the API.
- **Cross-platform** — works on Unix-like systems (Linux, macOS).
- **Unit tested** — comprehensive test suite (via [doctest](https://github.com/doctest/doctest)).

---

## Roadmap

Where the project is headed, grouped by theme.

### TUI input & ergonomics

- **Prompt history in the TUI** — the console already persists command
  history between sessions, but the TUI input is a custom widget with no
  history. Arrow-up recall of previous prompts is the main parity gap.
- **Autocomplete in the TUI input** — tab-completion for file paths, and
  potentially symbols, reusing the existing tree-sitter code tools. Long
  paths and symbol names are painful to type by hand.

### Session persistence

- **Save and restore sessions** — conversations are saved to
  `.mistral-cli/chats/` before every API request and after every
  completed turn, and `-r`/`--resume` restores the latest session
  (`chat.latest.json`) at startup, continuing the same chat file (and
  API prompt cache) with a preview of the last turn. Remaining:
  resuming a specific session by path (`/resume <path>`) and session
  listing/management, so older sessions are reachable without
  repointing the `chat.latest.json` symlink by hand.

### Safety & containment

- **Stronger policies and approval** — tool calls already go through a
  policy engine with per-call approval to keep the model inside the
  workspace, but the default policy (`templates/policy.json`) has holes:
  it allowlists `exec` by executable name only, so `find` is allowed with
  any arguments even though `find -exec` / `find -delete` can alter or
  delete files, and `make` runs arbitrary recipes from a Makefile. The
  optional `args` constraint cannot forbid dangerous flags — it only
  checks that at least one argument matches. Next steps: argument-aware
  rules or full command-line patterns, and a better approval UX
  (per-tool approval prompts just landed for `patchset_apply` and
  `exec`).

### Output rendering

- **Markdown rendering of final answers** — agent output currently streams
  as raw text and markdown is shown literally. A streaming markdown
  renderer would style headings, lists, and code fences.

### Agent architecture

- **Subagents and workflows** — subagents currently support one level of
  delegation (e.g. `web_search` → `researcher`). Future directions: deeper
  delegation, and richer task-tool workflows (dependencies between tasks,
  parallel execution, better status tracking) so the model can plan and
  track multi-step work.

### Provider support & naming

- **Beyond Mistral** — the client is provider-agnostic at the wire level:
  the model's `provider` field in `mistral-cli.json` selects the format
  (`mistral`, or the OpenAI-compatible provider that covers GreenPT,
  DeepSeek, and other endpoints, including `reasoning_content` thinking
  streams). Only Mistral and GreenPT are tested; other endpoints are
  expected to work but unverified. What is still Mistral-only is the
  `web_search` subagent (Mistral Agents API; it errors on other
  providers) — and the name: `mistral-cli`, `.mistral-cli/`,
  `mistral-cli.json` all say "Mistral" while the tool works with any
  compatible endpoint. Future directions: provider-neutral web research,
  broader provider support, and a rename that reflects it.

---

## Requirements

- **C++20** compiler (GCC ≥ 10, Clang ≥ 12, or MSVC ≥ 19.29).
- **CMake** ≥ 3.16.
- **OpenSSL** (for TLS).
- **Mistral API Key**: Sign up at [mistral.ai](https://mistral.ai/) and set `MISTRAL_API_KEY`.

---

## Build & Install

### 1. Clone the Repository

```sh
git clone --recurse-submodules https://github.com/your-repo/mistral-cli.git
cd mistral-cli
```

Or, for an existing clone, initialize the submodules (required before building):

```sh
git submodule update --init --recursive
```

### 2. Build

```sh
mkdir build && cd build
cmake ..
make
```

This builds the `mistral-cli` (console) and `mistral-tui` (TUI) executables, plus the `tests` binary.

### 3. Run

Set your Mistral API key:

```sh
export MISTRAL_API_KEY="your_api_key_here"
```

Start the console CLI:

```sh
./mistral-cli
```

or the TUI:

```sh
./mistral-tui
```

On startup the working directory is bootstrapped with a default `.mistral-cli/` workspace (see [Workspace Setup](#workspace-setup)).

#### Command-Line Options — mistral-cli

| Option                   | Description                                                        | Default                                      |
|--------------------------|--------------------------------------------------------------------|----------------------------------------------|
| `-c, --config-file FILE` | Configuration file path.                                           | `.mistral-cli/mistral-cli.json`              |
| `-n, --notes-dir DIR`    | Notes directory.                                                   | `.mistral-cli/notes`                         |
| `-H, --host HOST`        | API host.                                                          | `api.mistral.ai` (or `base_url` from config) |
| `-p, --port PORT`        | API port.                                                          | `443`                                        |
| `-a, --agent AGENT`      | Agent ID to use.                                                   | `main`                                       |
| `-r, --resume`           | Resume the latest chat session                                     |                                              |
| `-e, --system-prompt-show` | Show the resolved system prompt, then exit.                      |                                              |
| `-B, --no-bootstrap`     | Do not bootstrap the workspace.                                    |                                              |
| `-U, --update-managed`   | Force-update managed files (e.g. `sys_patchset_instructions.md`).  |                                              |
| `-h, --help`             | Show help.                                                         |                                              |

#### Command-Line Options — mistral-tui

| Option                   | Description                                                        | Default                         |
|--------------------------|--------------------------------------------------------------------|---------------------------------|
| `-c FILE`                | Config file.                                                       | `.mistral-cli/mistral-cli.json` |
| `-H HOST`                | API host.                                                          | `api.mistral.ai`                |
| `-p PORT`                | API port.                                                          | `443`                           |
| `-a ID`                  | Agent ID.                                                          | `main`                          |
| `--theme NAME`           | Theme: `dark`, `light`, `nord`, `tokyo-night`, `solarized-light`.  | `dark`                          |
| `--no-bootstrap`         | Do not bootstrap the workspace.                                    |                                 |
| `--resume`               | Resume the latest chat session                                     |                                 |
| `--update-managed`       | Force-update managed files.                                        |                                 |

The model is no longer selected on the command line; it comes from the agent's model configuration in `mistral-cli.json`.

---

## Workspace Setup

mistral-cli and mistral-tui load their configuration from a `.mistral-cli/`
directory in the working directory (agents, system prompts, tool policy,
notes and chat history).

No manual setup is needed: the default files from `templates/` are embedded
into the executables and a new `.mistral-cli/` directory is bootstrapped
automatically on startup (unless a custom config file is passed with `-c`,
or `--no-bootstrap` is given):

- `mistral-cli.json`, `policy.json`, `sys_prompt.md`, `sys_prompt_researcher.md`
  are **seeded** — written only if missing, so local customizations are
  never overwritten.
- `sys_patchset_instructions.md` is **managed** — refreshed from the embedded
  content when a newer executable finds an unmodified stock copy (tracked via
  `.mistral-cli/bootstrap.json`). A locally modified copy is left in place
  with a warning; run with `--update-managed` to overwrite it.

See `templates/README.md` for details.

---

## Usage

Both executables share the same agent core — the same tools, tool policy,
slash commands, and approval flow — and differ only in the interface.

### Console — mistral-cli

1. Type your message at the `>` prompt and press **Enter**.
2. The AI responds in real-time (streaming).
3. Press **Ctrl+D** to exit.

Input history persists between sessions (`.mistral-cli/history`, recall
with the up arrow), and **Tab** completes slash commands and file names.

Example:
```
mistral-cli (mistral-medium-latest)
Type your message, Ctrl-D to exit.

> Hello! Who are you?
< I'm a helpful AI assistant created by Mistral AI. How can I help you today?
```

### TUI — mistral-tui

Full-screen interface: a header with a connectivity indicator, a
scrollable transcript, a multi-line input box, and a footer showing the
workspace, agent · model, and token usage. Select a theme with `--theme`
(`dark`, `light`, `nord`, `tokyo-night`, `solarized-light`).

- **Enter** inserts a newline; **Alt+Enter** — or **Enter** on an empty
  last line (double-Enter) — sends the message.
- Typing `/` opens a slash-command popup; **Tab** or **Enter** inserts
  the selected command.
- **Esc** interrupts the running agent turn; during an approval prompt
  it rejects the call.
- **PageUp** / **PageDown** scroll the transcript by page, **Alt+↑** /
  **Alt+↓** by line.
- `/exit` quits the TUI.

### Slash commands

Both interfaces support the same slash commands:

| Command | Description |
|---------|-------------|
| `/help [command]` | List slash commands, or show usage for one. |
| `/model` | Show the current model. |
| `/tools` | List the available tools. |
| `/usage [reset]` | Show API usage statistics (`reset` zeroes them). |
| `/compact [keep]` | Summarize the conversation history, keeping the last `keep` turns verbatim. |
| `/save [file]` | Save the last AI response to a file (default: `last_response.txt`). |
| `/load [file]` | Load a file as the last AI response. |
| `/tool <name> <json>` | Call a tool directly, for debugging. |
| `/clear` | Clear the chat history (not yet implemented). |
| `/exit` | Quit the TUI (in the console, exit with **Ctrl+D**). |

### Tool approval

When a tool call requires approval (see the tool policy in
`.mistral-cli/policy.json`), the interface pauses and shows the tool,
its arguments, and the reason:

```
[APPROVAL REQUIRED]
  Tool: patchset_apply
  ...
  Approve? [y/n/r/a/?]:
```

- `y` — approve this call
- `n` — reject this call
- `r` — reject, with a custom message for the AI
- `a` — approve all similar calls for this session
- `?` — show this help

In the TUI, **Esc** during an approval prompt rejects the call.

### Available Tools

| Tool | Description |
|------|-------------|
| `ls` | List files and directories. |
| `read` | Read file contents (supports line ranges). |
| `outline` | List the symbols defined in a C/C++ source file with their line ranges. |
| `read_symbol` | Extract the exact source text of a symbol definition. |
| `find_symbol` | Find symbol definitions by name across a directory tree. |
| `find_references` | Find usage sites of a named symbol across a directory tree (definition sites excluded). |
| `ast_search` | Search a file or tree with a tree-sitter query (scheme-like syntax). |
| `exec` | Execute an external command (no shell involved). |
| `note_list`, `note_read`, `note_write`, `note_search` | Manage notes in the notes directory. |
| `patchset_add`, `patchset_get`, `patchset_validate`, `patchset_update`, `patchset_remove`, `patchset_apply`, `patchset_clear` | Patchset-based file editing workflow. |
| `task_create`, `task_list`, `task_get`, `task_update`, `task_search` | Project task tracking. |
| `web_search` | Search the web via a server-side research agent (Mistral Agents API `web_search` builtin tool). |

Which tools an agent may use is configured per agent via `available_tools`
in `mistral-cli.json`. File edits go through the patchset workflow
(see `.mistral-cli/sys_patchset_instructions.md`) rather than a direct
write tool. The code exploration tools are syntax-aware (no matches in
comments, strings or preprocessor directives).

### Subagents

An agent can delegate tool calls to another agent ("subagent"). This is how
`web_search` works: the model calls the `web_search` tool, and the call is
routed to a dedicated `researcher` agent that runs on the Mistral Agents API
with the builtin `web_search` tool (server-side web search — the client never
sees the search tool calls, only the researched answer).

Two config keys in `mistral-cli.json` control this:

- `subagent_tools` (on the calling agent): maps a tool name to the local
  agent id that executes it, e.g. `"subagent_tools": {"web_search": "researcher"}`.
  The tool's `query` argument becomes the subagent's prompt; the subagent's
  final answer becomes the tool result.
- `server_agent` (on the subagent): describes a server-side agent entity
  (`POST /v1/agents`). When set, the subagent's task talks to the
  **Conversations API** (`POST /v1/conversations` with `agent_id` +
  `inputs`) instead of `POST /v1/chat/completions`. The server-side agent
  owns the model, `instructions` (supports `file://`) and builtin `tools`
  such as `{"type": "web_search"}` or `{"type": "web_search_premium"}`.
  `completion_args` (e.g. `{"temperature": 0.2}`) is applied at creation
  time. The Conversations API is required for builtin tools like
  `web_search`: Chat Completions responses don't include the search result
  references these tools produce, so the completions endpoint rejects the
  connector (`invalid_tools`). Conversation responses include the answer
  text plus `tool_reference` citation chunks (surfaced as a `Sources:`
  markdown list) and connector token usage.

The server-side agent entity is resolved lazily on first use: an existing
agent with the same `name` is reused (`GET /v1/agents/pages?name=...`),
otherwise a new one is created (`POST /v1/agents`), and the id is cached for
the session. A fingerprint of the `server_agent` config is stored in the
agent's `metadata`; when the local config changes (e.g. switching
`web_search` → `web_search_premium`), the existing agent is updated in
place (`PATCH /v1/agents/{id}`) on the next call — no manual cleanup needed.
The Agents API is Mistral-only; agents configured with a non-Mistral
provider fall back to an error when the subagent runs.

**Troubleshooting:** if `web_search` calls fail with `invalid_tools` /
"connector is not supported" (HTTP 400), the builtin web_search connector
is not enabled for your Mistral workspace — check your plan/region in the
Mistral console, or switch the `server_agent` tool to
`{"type": "web_search_premium"}`.

```json
"agents": {
  "main": {
    "available_tools": ["web_search", "..."],
    "subagent_tools": {"web_search": "researcher"}
  },
  "researcher": {
    "model": "mistral-small",
    "server_agent": {
      "name": "mistral-cli-researcher",
      "instructions": "file://.mistral-cli/sys_prompt_researcher.md",
      "tools": [{"type": "web_search"}],
      "completion_args": {"temperature": 0.2}
    }
  }
}
```

---

## Project Structure

| Directory/File | Purpose |
|----------------|---------|
| `src/` | Executable entry points (`mistral-cli`, `mistral-tui`, demos). |
| `lib/` | Core library (HTTP, API, console, TUI, tools, agent system). |
| `test/` | Unit tests. |
| `templates/` | Default workspace files embedded into the executables at build time. |
| `queries/` | Repo-local tree-sitter queries, embedded into the executables at build time. |
| `third_party/` | Submodule dependencies (`dripcore`, `stunning-waddle`, `isocline`, `just-a-uri`, `cpp-tui`, `tree-sitter` + C++ grammar) and `nlohmann/json`. |
| `CMakeLists.txt` | Build configuration. |
| `README.md` | This file. |

---

## Dependencies

| Dependency           | Purpose                        | License               |
|----------------------|--------------------------------|-----------------------|
| [dripcore](https://github.com/bebac/dripcore-dev) | Coroutine scheduler. | MIT |
| [stunning-waddle](https://github.com/bebac/stunning-waddle) | HTTP/2 client. | MIT |
| [isocline](https://github.com/bebac/isocline) | Readline-like input. | MIT |
| [just-a-uri](https://github.com/bebac/just-a-uri) | URI parsing. | MIT |
| [cpp-tui](https://github.com/bebac/cpp-tui) | Terminal UI library (TUI mode). | MIT |
| [tree-sitter](https://github.com/tree-sitter/tree-sitter) + C++ grammar | Syntax-aware code exploration tools. | MIT |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON parsing. | MIT |
| [doctest](https://github.com/doctest/doctest) | Unit testing. | MIT |
| OpenSSL | TLS support. | Apache License 2.0 |

---

## Testing

Run the test suite from the repository root:

```sh
make test
```

or run the test binary directly:

```sh
./build/tests
```

`ctest` also works from a configured build directory (the `tests` target is
registered with CTest):

```sh
cd build
ctest
```

### Example Tests

- **Conversation Management** (`conversation_test.cpp`):
  Verifies message history and JSON serialization.

- **Tool Dispatching** (`tool_registry_test.cpp`, `exec_tools_test.cpp`, `task_tools_test.cpp`):
  Tests tool registration and execution.

- **Code Exploration** (`code_tools_test.cpp`):
  Symbol navigation, tree-sitter queries, and result truncation.

- **Patchset Workflow** (`edit_parser_test.cpp`, `edit_validator_test.cpp`, `patchset_tools_test.cpp`):
  Patchset parsing, validation, and application.

- **Policy Engine** (`policy_engine_test.cpp`):
  Tool policy evaluation and approvals.

- **Workspace Bootstrap** (`workspace_bootstrap_test.cpp`):
  Seeding of default files and managed-file updates.

- **TUI** (`tui_content_log_test.cpp`, `tui_chat_input_test.cpp`, `tui_slash_command_test.cpp`):
  TUI widgets, input handling, and rendering.

---

## Contributing

Contributions are welcome! Here's how you can help:

1. **Report Bugs**: Open an issue on GitHub.
2. **Suggest Features**: Share your ideas for new tools or improvements.
3. **Submit Pull Requests**: Fix bugs, add features, or improve documentation.

### Development Setup

1. Fork the repository.
2. Clone your fork **with submodules** (the build requires them):
   ```sh
   git clone --recurse-submodules https://github.com/your-username/mistral-cli.git
   cd mistral-cli
   ```
   For an existing clone:
   ```sh
   git submodule update --init --recursive
   ```
3. Create a feature branch:
   ```sh
   git checkout -b feature/your-feature
   ```
4. Commit your changes:
   ```sh
   git commit -am "Add your feature"
   ```
5. Push to the branch:
   ```sh
   git push origin feature/your-feature
   ```
6. Open a pull request.

---

## License

This project is licensed under the MIT License — see the [LICENSE](LICENSE)
file for details.

---

## Acknowledgments

- [Mistral AI](https://mistral.ai/) for the amazing models.
- [dripcore](https://github.com/bebac/dripcore-dev) for the coroutine scheduler.
- [stunning-waddle](https://github.com/bebac/stunning-waddle) for HTTP/2 support.
- [isocline](https://github.com/bebac/isocline) for the interactive console.
- [cpp-tui](https://github.com/bebac/cpp-tui) for the terminal UI.
- [tree-sitter](https://github.com/tree-sitter/tree-sitter)
