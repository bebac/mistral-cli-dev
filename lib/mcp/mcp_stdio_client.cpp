#include "mcp/mcp_stdio_client.h"

#include "common/executable_finder.h"

#include <dripcore/executor.h>
#include <dripcore/io_awaiter.h>
#include <dripcore/sleep_awaiter.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

// environ is not declared by <unistd.h> on all platforms (macOS).
extern char** environ;

namespace fs = std::filesystem;

namespace mistral::mcp
{
  namespace
  {
    // RAII wrapper for a file descriptor.
    class fd_guard
    {
    public:
      explicit fd_guard(int fd = -1) : fd_(fd) {}
      ~fd_guard() { if (fd_ >= 0) ::close(fd_); }
      fd_guard(const fd_guard&) = delete;
      fd_guard& operator=(const fd_guard&) = delete;
      int get() const { return fd_; }
      int release() { int tmp = fd_; fd_ = -1; return tmp; }
    private:
      int fd_;
    };

    // RAII wrapper for a pipe (two fds).
    class pipe_guard
    {
    public:
      pipe_guard()
      {
        int fds[2];
        if (::pipe(fds) == 0) {
          read_fd_ = std::make_unique<fd_guard>(fds[0]);
          write_fd_ = std::make_unique<fd_guard>(fds[1]);
        }
      }
      int read_fd() const { return read_fd_ ? read_fd_->get() : -1; }
      int write_fd() const { return write_fd_ ? write_fd_->get() : -1; }
      bool valid() const { return read_fd() >= 0 && write_fd() >= 0; }
      int release_read() { return read_fd_ ? read_fd_->release() : -1; }
      int release_write() { return write_fd_ ? write_fd_->release() : -1; }
    private:
      std::unique_ptr<fd_guard> read_fd_;
      std::unique_ptr<fd_guard> write_fd_;
    };

    // RAII wrapper for the child process: kill on destruction.
    // Used in connect() to clean up if setup fails after fork().
    // Non-blocking: uses WNOHANG so it never hangs the event loop
    // or process exit (same reasoning as mcp_stdio_client::close).
    class child_guard
    {
    public:
      explicit child_guard(pid_t pid = -1) : pid_(pid) {}
      ~child_guard()
      {
        if (pid_ > 0) {
          // Kill the process group (child called setpgid(0,0)).
          if (::kill(-pid_, SIGKILL) == -1) {
            ::kill(pid_, SIGKILL);
          }
          int status;
          ::waitpid(pid_, &status, WNOHANG);
        }
      }
      child_guard(const child_guard&) = delete;
      child_guard& operator=(const child_guard&) = delete;
      pid_t get() const { return pid_; }
      pid_t release() { pid_t tmp = pid_; pid_ = -1; return tmp; }
    private:
      pid_t pid_;
    };
  } // namespace

  mcp_stdio_client::mcp_stdio_client(
      std::string command,
      std::vector<std::string> args,
      std::map<std::string, std::string> env,
      std::chrono::seconds request_timeout)
      : command_(std::move(command)), args_(std::move(args)), env_(std::move(env)), timeout_(request_timeout)
  {
  }

  mcp_stdio_client::~mcp_stdio_client()
  {
    close();
  }

  dripcore::awaitable<bool> mcp_stdio_client::connect()
  {
    close();

    std::string exec_path = utils::find_executable(command_);
    if (exec_path.empty()) {
      co_return false;
    }

    // Three pipes: child stdin, child stdout, child stderr.
    pipe_guard stdin_pipe;   // parent writes -> child reads
    pipe_guard stdout_pipe;  // child writes  -> parent reads
    pipe_guard stderr_pipe;  // child writes  -> parent reads (drained)
    if (!stdin_pipe.valid() || !stdout_pipe.valid() || !stderr_pipe.valid()) {
      co_return false;
    }

    // Build argv.
    std::vector<char*> argv;
    argv.push_back(::strdup(exec_path.c_str()));
    for (const auto& a : args_) {
      argv.push_back(::strdup(a.c_str()));
    }
    argv.push_back(nullptr);

    // Build envp (inherit parent env + overrides).
    std::vector<std::string> env_strings;
    for (char** e = ::environ; e && *e; ++e) {
      env_strings.emplace_back(*e);
    }
    for (const auto& [key, value] : env_) {
      std::string entry = key + "=" + value;
      // Replace existing entry if present.
      bool replaced = false;
      for (auto& s : env_strings) {
        if (s.starts_with(key + "=")) {
          s = entry;
          replaced = true;
          break;
        }
      }
      if (!replaced) {
        env_strings.push_back(std::move(entry));
      }
    }
    std::vector<char*> envp;
    for (auto& s : env_strings) {
      envp.push_back(s.data());
    }
    envp.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid == -1) {
      for (char* a : argv) ::free(a);
      co_return false;
    }

    if (pid == 0) {
      // Child: wire up pipes and exec.
      ::setpgid(0, 0);

      // stdin: read from pipe
      ::dup2(stdin_pipe.read_fd(), STDIN_FILENO);
      // stdout: write to pipe
      ::dup2(stdout_pipe.write_fd(), STDOUT_FILENO);
      // stderr: write to pipe (drained by parent)
      ::dup2(stderr_pipe.write_fd(), STDERR_FILENO);

      // Close all pipe fds (they're dup'd now).
      ::close(stdin_pipe.read_fd());
      ::close(stdin_pipe.write_fd());
      ::close(stdout_pipe.read_fd());
      ::close(stdout_pipe.write_fd());
      ::close(stderr_pipe.read_fd());
      ::close(stderr_pipe.write_fd());

      // Close ALL other inherited file descriptors (kqueue fd from
      // the dripcore event loop, wakeup pipe fds, etc.). Without
      // this, the child holds the parent's wakeup pipe write ends
      // open — when the parent closes its write ends during shutdown,
      // the pipes never see EOF, the dripcore tasks block forever on
      // readable(), and sched.run() never returns (CTRL-C hang).
      // Note: wakeup_pipe now sets FD_CLOEXEC, but the kqueue fd
      // (macOS) has no CLOEXEC variant, so we close everything here.
      long max_fd = ::sysconf(_SC_OPEN_MAX);
      if (max_fd < 0) max_fd = 1024;
      for (long fd = 3; fd < max_fd; ++fd) {
        ::close(static_cast<int>(fd));
      }

      ::execve(exec_path.c_str(), argv.data(), envp.data());
      // If we get here, exec failed.
      ::write(STDERR_FILENO, "execve failed\n", 14);
      ::_exit(127);
    }

    // Parent: free argv, take ownership of pipe fds.
    for (char* a : argv) ::free(a);

    child_guard child(pid);
    ::setpgid(pid, pid);

    // Take the parent-side fds.
    stdin_fd_ = stdin_pipe.release_write();
    stdout_fd_ = stdout_pipe.release_read();
    stderr_fd_ = stderr_pipe.release_read();

    // Close child-side fds in parent.
    // (pipe_guard will close the remaining ones on scope exit.)

    // Set non-blocking for io_awaiter compatibility.
    if (stdin_fd_ >= 0) ::fcntl(stdin_fd_, F_SETFL, O_NONBLOCK);
    if (stdout_fd_ >= 0) ::fcntl(stdout_fd_, F_SETFL, O_NONBLOCK);
    if (stderr_fd_ >= 0) ::fcntl(stderr_fd_, F_SETFL, O_NONBLOCK);

    // Set FD_CLOEXEC so future fork/exec children (e.g. another MCP
    // server, or an exec tool call) do NOT inherit these pipe fds.
    // Without this, a second child would hold the first child's stdin
    // write end open, preventing EOF when the parent closes it.
    if (stdin_fd_ >= 0) ::fcntl(stdin_fd_, F_SETFD, FD_CLOEXEC);
    if (stdout_fd_ >= 0) ::fcntl(stdout_fd_, F_SETFD, FD_CLOEXEC);
    if (stderr_fd_ >= 0) ::fcntl(stderr_fd_, F_SETFD, FD_CLOEXEC);

    pid_ = child.release();
    co_return true;
  }

  void mcp_stdio_client::close()
  {
    if (stdin_fd_ >= 0) { ::close(stdin_fd_); stdin_fd_ = -1; }
    if (stdout_fd_ >= 0) { ::close(stdout_fd_); stdout_fd_ = -1; }
    if (stderr_fd_ >= 0) { ::close(stderr_fd_); stderr_fd_ = -1; }
    if (pid_ > 0) {
      // Kill the entire process group (the child called setpgid(0,0)
      // so it leads its own group — this also kills any grandchildren
      // the MCP server may have spawned). Fall back to the direct
      // child if the group kill fails.
      if (::kill(-pid_, SIGKILL) == -1) {
        ::kill(pid_, SIGKILL);
      }
      // Reap without blocking. close() is called from two contexts:
      //   1. send_and_receive() error paths — runs in a dripcore
      //      coroutine; a blocking waitpid() here would freeze the
      //      entire event loop (TUI hang).
      //   2. The destructor during process exit — a blocking
      //      waitpid() here would hang the process exit if the
      //      child is stuck (e.g. D-state, slow shutdown).
      // The child will be reaped by init(8) when it exits if we
      // don't reap it here; a zombie is harmless.
      int status;
      ::waitpid(pid_, &status, WNOHANG);
      pid_ = -1;
    }
    initialized_ = false;
    read_buffer_.clear();
  }

  void mcp_stdio_client::drain_stderr()
  {
    if (stderr_fd_ < 0) return;
    char buf[4096];
    while (true) {
      ssize_t n = ::read(stderr_fd_, buf, sizeof(buf));
      if (n > 0) continue;  // discard
      break;  // EAGAIN or EOF or error
    }
  }

  dripcore::awaitable<bool> mcp_stdio_client::write_line(const std::string& line)
  {
    if (stdin_fd_ < 0) co_return false;

    std::string data = line + "\n";
    size_t written = 0;
    auto deadline = std::chrono::steady_clock::now() + timeout_;

    while (written < data.size())
    {
      ssize_t n = ::write(stdin_fd_, data.data() + written,
                          data.size() - written);
      if (n > 0) {
        written += static_cast<size_t>(n);
        continue;
      }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        // Pipe full: poll with a short sleep (io_awaiter has no
        // timeout support; same pattern as the HTTP MCP client).
        if (std::chrono::steady_clock::now() >= deadline) {
          co_return false;
        }
        co_await dripcore::sleep_awaiter{
          dripcore::executor::current(), nullptr,
          std::chrono::milliseconds(5)};
        continue;
      }
      // Real error or child closed stdin (EPIPE).
      co_return false;
    }
    co_return true;
  }

  dripcore::awaitable<std::optional<std::string>> mcp_stdio_client::read_line()
  {
    if (stdout_fd_ < 0) co_return std::nullopt;

    auto deadline = std::chrono::steady_clock::now() + timeout_;

    while (true) {
      // Check if we already have a complete line in the buffer.
      auto pos = read_buffer_.find('\n');
      if (pos != std::string::npos) {
        std::string line = read_buffer_.substr(0, pos);
        read_buffer_.erase(0, pos + 1);
        // Strip trailing \r if present.
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        co_return line;
      }

      // Drain stderr while we wait (prevent child blocking on full
      // stderr pipe).
      drain_stderr();

      // Non-blocking read.
      char buf[4096];
      ssize_t n = ::read(stdout_fd_, buf, sizeof(buf));
      if (n > 0) {
        read_buffer_.append(buf, static_cast<size_t>(n));
        continue;
      }
      if (n == 0) {
        // EOF: child closed stdout.
        co_return std::nullopt;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // No data: poll with a short sleep (io_awaiter has no
        // timeout support; same pattern as the HTTP MCP client).
        if (std::chrono::steady_clock::now() >= deadline) {
          co_return std::nullopt;
        }
        co_await dripcore::sleep_awaiter{
          dripcore::executor::current(), nullptr,
          std::chrono::milliseconds(5)
        };
        continue;
      }
      // Real error.
      co_return std::nullopt;
    }
  }

  dripcore::awaitable<json> mcp_stdio_client::send_and_receive(
    const json& message, int expect_id)
  {
    if (pid_ <= 0) {
      if (!(co_await connect())) {
        throw std::runtime_error("cannot spawn mcp stdio server: " + command_);
      }
    }

    if (!(co_await write_line(message.dump()))) {
      close();
      throw std::runtime_error("failed to write to mcp stdio server: " + command_);
    }

    // Read lines until we find the response with the matching id.
    // Notifications and server-initiated messages are skipped.
    while (true) {
      auto line = co_await read_line();
      if (!line) {
        close();
        throw std::runtime_error("mcp stdio server closed stdout: " + command_);
      }
      if (line->empty()) {
        continue;  // skip empty lines
      }

      json response;
      try {
        response = json::parse(*line);
      }
      catch (const std::exception&) {
        continue;  // skip non-JSON lines (e.g. log output)
      }

      // Skip notifications (no id) and responses for other ids.
      if (!response.contains("id") || response["id"] != expect_id) {
        continue;
      }

      co_return response;
    }
  }

  dripcore::awaitable<json> mcp_stdio_client::request(
    const std::string& method, const json& params)
  {
    int id = next_id_++;
    json message = {
      {"jsonrpc", "2.0"},
      {"id", id},
      {"method", method}
    };
    if (!params.is_null()) {
      message["params"] = params;
    }
    json response = co_await send_and_receive(message, id);

    if (response.contains("error")) {
      const auto& err = response["error"];
      throw std::runtime_error(
        std::format("mcp error {}: {}", err.value("code", 0), err.value("message", "unknown error"))
      );
    }
    co_return response.value("result", json::object());
  }

  dripcore::awaitable<bool> mcp_stdio_client::notify(
    const std::string& method)
  {
    json message = {
      {"jsonrpc", "2.0"},
      {"method", method}
    };
    try {
      // Notifications get no response; we send and don't wait.
      // Use a dummy id of -1 and don't read a response.
      if (pid_ <= 0) {
        if (!(co_await connect())) co_return false;
      }
      co_return co_await write_line(message.dump());
    }
    catch (const std::exception&) {
      co_return false;
    }
  }

  dripcore::awaitable<bool> mcp_stdio_client::initialize()
  {
    json params = {
      {"protocolVersion", "2025-06-18"},
      {"capabilities", json::object()},
      {"clientInfo", {{"name", "mistral-cli"}, {"version", "0.1.0"}}}
    };
    json result = co_await request("initialize", params);

    if (result.contains("serverInfo")) {
      server_info_ = result["serverInfo"];
    }
    instructions_ = result.value("instructions", std::string());

    co_await notify("notifications/initialized");
    initialized_ = true;
    co_return true;
  }

  dripcore::awaitable<json> mcp_stdio_client::list_tools()
  {
    co_return co_await request("tools/list", json());
  }

  dripcore::awaitable<json> mcp_stdio_client::call_tool(const std::string& name, const json& args)
  {
    json params = {{"name", name}, {"arguments", args}};
    co_return co_await request("tools/call", params);
  }

} // namespace mistral::mcp