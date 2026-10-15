# Mistral CLI

A **command-line interface** for interacting with [Mistral AI](https://mistral.ai/) models, featuring **real-time streaming**, **tool integration**, an interactive console, and a full-screen **TUI**.

Built with **C++20**, **coroutines** (via [dripcore](https://github.com/bebac/dripcore-dev)), **HTTP/2** (via [stunning-waddle](https://github.com/bebac/stunning-waddle)), and a terminal UI (via [cpp-tui](https://github.com/bebac/cpp-tui)).

---

## Features

✅ **Interactive Chat**: Stream responses in real-time.
✅ **TUI Mode**: Full-screen terminal UI (`mistral-tui`) with selectable themes.
✅ **Tool Integration**: File listing/reading, syntax-aware code exploration (tree-sitter), exec, notes, task tracking, and patchset-based file editing.
✅ **Multi-Agent Configuration**: Models, agents, and tool policy configured via `.mistral-cli/mistral-cli.json`.
✅ **Workspace Bootstrap**: Default configuration files are embedded in the executables and seeded automatically on startup.
✅ **Console History**: Persists command history between sessions.
✅ **HTTP/2 + TLS**: Efficient, secure communication with the API.
✅ **Cross-Platform**: Works on Unix-like systems (Linux, macOS).
✅ **Unit Tested**: Comprehensive test suite (via [doctest](https://github.com/doctest/doctest)).

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

| Option                   | Description                                                        | Default                                    |
|--------------------------|--------------------------------------------------------------------|--------------------------------------------|
| `-c, --config-file FILE` | Configuration file path.                                            | `.mistral-cli/mistral-cli.json`            |
| `-n, --notes-dir DIR`    | Notes directory.                                                    | `.mistral-cli/notes`                       |
| `-H, --host HOST`        | API host.                                                          | `api.mistral.ai` (or `base_url` from config) |
| `-p, --port PORT`        | API port.                                                          | `443`                                      |
| `-a, --agent AGENT`      | Agent ID to use.                                                   | `main`                                     |
| `-e, --system-prompt-show` | Show the resolved system prompt, then exit.                      |                                            |
| `-B, --no-bootstrap`     | Do not bootstrap the workspace.                                    |                                            |
| `-U, --update-managed`   | Force-update managed files (e.g. `sys_patchset_instructions.md`).  |                                            |
| `-h, --help`             | Show help.                                                         |                                            |

#### Command-Line Options — mistral-tui

| Option               | Description                                                       | Default                         |
|----------------------|-------------------------------------------------------------------|---------------------------------|
| `-c FILE`            | Config file.                                                      | `.mistral-cli/mistral-cli.json` |
| `-H HOST`            | API host.                                                         | `api.mistral.ai`                |
| `-p PORT`            | API port.                                                         | `443`                           |
| `-a ID`              | Agent ID.                                                         | `main`                          |
| `--theme NAME`       | Theme: `dark`, `light`, `nord`, `tokyo-night`, `solarized-light`. | `dark`                          |
| `--no-bootstrap`     | Do not bootstrap the workspace.                                   |                                 |
| `--update-managed`   | Force-update managed files.                                       |                                 |

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

- `mistral-cli.json`, `policy.json`, `sys_prompt.md` are **seeded** — written
  only if missing, so local customizations are never overwritten.
- `sys_patchset_instructions.md` is **managed** — refreshed from the embedded
  content when a newer executable finds an unmodified stock copy (tracked via
  `.mistral-cli/bootstrap.json`). A locally modified copy is left in place
  with a warning; run with `--update-managed` to overwrite it.

See `templates/README.md` for details.

---

## Usage

### Interactive Chat

1. Type your message and press **Enter**.
2. The AI responds in real-time (streaming).
3. Press **Ctrl+D** to exit.

Example:
```
mistral-cli (mistral-medium-latest)
Type your message, Ctrl-D to exit.

> Hello! Who are you?
< I'm a helpful AI assistant created by Mistral AI. How can I help you today?
```

### Tool Calls

The AI can call tools to perform actions. For example:

- **List files**:
  ```
  > List files in the current directory.
  < Calling tool: ls(path=".")
  < Here are the files:
  d        - build
  f     3555 CMakeLists.txt
  f     4406 src/main.cpp
  ```

- **Read a file**:
  ```
  > Read the first 5 lines of README.md.
  < Calling tool: read(path="README.md", line_from=1, line_to=5)
  < Here are the first 5 lines:
  1|# Mistral CLI
  2|
  3|A command-line interface for Mistral AI models...
  ```

- **Search the codebase**:
  ```
  > Find where the tool registry is populated.
  < Calling tool: find_references(name="register_tool", path="lib")
  < lib/agent/agent_system.cpp:487:    global_tools_.register_tool(
  ```

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

Which tools an agent may use is configured per agent via `available_tools`
in `mistral-cli.json`. File edits go through the patchset workflow
(see `.mistral-cli/sys_patchset_instructions.md`) rather than a direct
write tool. The code exploration tools are syntax-aware (no matches in
comments, strings or preprocessor directives); see `doc/code-tools.md`
for details and caveats.

---

## Project Structure

| Directory/File   | Purpose                                                                 |
|------------------|-------------------------------------------------------------------------|
| `src/`           | Executable entry points (`mistral-cli`, `mistral-tui`, demos).           |
| `lib/`           | Core library (HTTP, API, console, TUI, tools, agent system).             |
| `test/`          | Unit tests.                                                             |
| `templates/`     | Default workspace files embedded into the executables at build time.    |
| `queries/`       | Repo-local tree-sitter queries, embedded into the executables at build time. |
| `doc/`           | Design documents (code tools, TUI, rendering).                          |
| `third_party/`   | Submodule dependencies (`dripcore`, `stunning-waddle`, `isocline`, `just-a-uri`, `cpp-tui`, `tree-sitter` + C++ grammar) and `nlohmann/json`. |
| `CMakeLists.txt` | Build configuration.                                                    |
| `README.md`      | This file.                                                              |

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