# Mistral CLI

A **command-line interface** for interacting with [Mistral AI](https://mistral.ai/) models, featuring **real-time streaming**, **tool integration** (e.g., file operations), and an interactive console with history.

Built with **C++20**, **coroutines** (via [dripcore](https://github.com/epiclabs-io/dripcore)), and **HTTP/2** (via [stunning-waddle](https://github.com/simon-meusburger/stunning-waddle)).

---

## Features

✅ **Interactive Chat**: Stream responses in real-time.
✅ **Tool Integration**: Call tools (e.g., `ls`, `read`, `write`) from the AI.
✅ **Console History**: Persists command history between sessions.
✅ **HTTP/2 + TLS**: Efficient, secure communication with Mistral's API.
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
git clone https://github.com/your-repo/mistral-cli.git
cd mistral-cli
```

### 2. Build

```sh
mkdir build && cd build
cmake ..
make
```

### 3. Run

Set your Mistral API key:

```sh
export MISTRAL_API_KEY="your_api_key_here"
```

Start the CLI:

```sh
./mistral-cli --model mistral-medium-latest
```

#### Command-Line Options

| Option               | Description                                  | Default                     |
|-----------------------|----------------------------------------------|-----------------------------|
| `-m, --model`         | Mistral model to use.                        | `mistral-medium-latest`     |
| `-s, --system-prompt` | System prompt for the AI.                   | "You are a helpful assistant." |
| `-H, --host`          | API host.                                   | `api.mistral.ai`           |
| `-p, --port`          | API port.                                   | `443`                      |
| `-h, --help`          | Show help.                                   |                             |

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

- **Write a file**:
  ```
  > Create a file named "test.txt" with the content "Hello, world!".
  < Calling tool: write(path="test.txt", content="Hello, world!")
  < File written successfully.
  ```

### Available Tools

| Tool  | Description                                                                 | Arguments                                                                 |
|-------|-----------------------------------------------------------------------------|---------------------------------------------------------------------------|
| `ls`  | List files and directories.                                                 | `path`, `recursive` (bool), `max_depth` (int)                            |
| `read`| Read file contents (supports line ranges).                                  | `path`, `line_from` (int), `line_to` (int)                               |
| `write`| Write content to a file (creates directories if needed).                   | `path`, `content`                                                        |

---

## Project Structure

| Directory/File       | Purpose                                                                 |
|----------------------|-------------------------------------------------------------------------|
| `src/`               | Main source code (entry point, tasks).                                    |
| `lib/`               | Core library (HTTP, API, console, tools).                                |
| `test/`              | Unit tests.                                                            |
| `third_party/`       | Dependencies (e.g., `nlohmann/json`).                                   |
| `references/`        | Vendored dependencies (`dripcore`, `stunning-waddle`, `isocline`).       |
| `CMakeLists.txt`     | Build configuration.                                                   |
| `README.md`          | This file.                                                            |

---

## Dependencies

| Dependency           | Purpose                                  | License               |
|----------------------|------------------------------------------|-----------------------|
| [dripcore](https://github.com/bebac/dripcore-dev) | Coroutine scheduler. | MIT                   |
| [stunning-waddle](https://github.com/bebac/stunning-waddle) | HTTP/2 client. | MIT                   |
| [isocline](https://github.com/gtuk/discord.cpp/tree/master/deps/isocline) | Readline-like input. | MIT                   |
| [nlohmann/json](https://github.com/nlohmann/json) | JSON parsing. | MIT                   |
| [doctest](https://github.com/doctest/doctest) | Unit testing. | MIT                   |
| OpenSSL | TLS support. | Apache License 2.0 |

---

## Testing

Run the test suite:

```sh
cd build
ctest  # or ./tests
```

### Example Tests

- **Conversation Management**:
  Verifies message history, sliding window, and JSON serialization.

- **Tool Dispatching**:
  Tests tool registration and execution (e.g., `ls`, `read`, `write`).

- **API Parsing**:
  Validates request/response parsing and SSE (Server-Sent Events) handling.

---

## Contributing

Contributions are welcome! Here’s how you can help:

1. **Report Bugs**: Open an issue on GitHub.
2. **Suggest Features**: Share your ideas for new tools or improvements.
3. **Submit Pull Requests**: Fix bugs, add features, or improve documentation.

### Development Setup

1. Fork the repository.
2. Clone your fork:
   ```sh
   git clone https://github.com/your-username/mistral-cli.git
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

This project is licensed under the **MIT License**. See [LICENSE](LICENSE) for details.

---

## Acknowledgments

- [Mistral AI](https://mistral.ai/) for the amazing models.
- [dripcore](https://github.com/bebac/dripcore-dev) for the coroutine scheduler.
- [stunning-waddle](https://github.com/bebac/stunning-waddle) for HTTP/2 support.
- [isocline](https://github.com/gtuk/discord.cpp/tree/master/deps/isocline) for the interactive console.
