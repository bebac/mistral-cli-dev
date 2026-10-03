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

## Key Dependencies

| Dependency | Purpose | License |
|------------|---------|---------|
| dripcore | Coroutine scheduler | MIT |
| stunning-waddle | HTTP/2 client | MIT |
| isocline | Readline-like input | MIT |
| cpp-tui | Header only TUI library | MIT |
| nlohmann/json | JSON parsing | MIT |
| doctest | Unit testing | MIT |
| OpenSSL | TLS support | Apache License 2.0 |

