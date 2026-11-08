# Mistral CLI - AI Agent Guide

A **C++20** command-line interface for Mistral AI models with **real-time streaming**, **tool integration**, and an interactive console.

## Core Technology Stack
- **C++20** with coroutines (via [dripcore](https://github.com/bebac/dripcore-dev))
- **HTTP/2 + TLS** (via [stunning-waddle](https://github.com/bebac/stunning-waddle))
- **Readline-like console** (via [isocline](https://github.com/bebac/isocline))
- **TUI interface** (via [cpp-tui](https://github.com/bebac/cpp-tui))
- **JSON parsing** (via [nlohmann/json](https://github.com/nlohmann/json))
- **Unit testing** (via [doctest](https://github.com/doctest/doctest))

## Project Structure

| Directory/File | Purpose |
|----------------|---------|
| `src/` | Main source code (entry point, tasks) |
| `lib/` | Core library (HTTP, API, console, tools) |
| `test/` | Unit tests |
| `third_party/` | Dependencies (e.g., `nlohmann/json`) |
| `CMakeLists.txt` | Build configuration |

## Tool output streaming

`exec` streams live into its tool block: `exec_run_task` publishes output on a
channel, `agent_task_base::run_exec_tool` consumes it and forwards to the render
sink. Channels are dripcore-thread-only. The model cap (`k_max_output_bytes`)
and the display cap (`ContentLog::k_max_result_bytes`) are independent.

## Key Dependencies

| Dependency | Purpose | License |
|------------|---------|---------|
| dripcore | Coroutine scheduler | MIT |
| stunning-waddle | HTTP/2 client | MIT |
| isocline | Readline-like input | MIT |
| cpp-tui | Header only TUI library | MIT |
| tree-sitter | Syntax-aware code exploration tools. | MIT |
| nlohmann/json | JSON parsing | MIT |
| doctest | Unit testing | MIT |
| OpenSSL | TLS support | Apache License 2.0 |
| SQLite + sqlite-vec | Knowledge store (doc search) | MIT |

## Knowledge Store

Semantic search over workspace documentation (memory and external
corpora come later). SQLite + [sqlite-vec](https://github.com/asg017/sqlite-vec)
(vendored amalgamation in `third_party/sqlite-vec`), hashing-trick
embeddings (offline, deterministic). The index lives at
`.mistral-cli/cache/knowledge.db` and refreshes incrementally before
each search. Tools: `search_docs`, `refresh_docs`. Config: the
`knowledge` section of `mistral-cli.json`. Build with
`-DMISTRAL_ENABLE_KNOWLEDGE=OFF` to disable.
