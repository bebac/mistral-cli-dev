#include "tools/exec_tools.h"

#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <cstring>
#include <signal.h>
#include <filesystem>
#include <sstream>
#include <vector>
#include <array>
#include <chrono>
#include <dripcore/sleep_awaiter.h>

namespace fs = std::filesystem;

namespace mistral::tools
{
  namespace
  {
    // RAII wrapper for file descriptors
    class fd_guard
    {
    public:
      fd_guard(int fd) : fd_(fd) {}
      ~fd_guard() { if (fd_ >= 0) close(fd_); }
      int get() const { return fd_; }
      int release() { int tmp = fd_; fd_ = -1; return tmp; }
    private:
      int fd_;
    };

    // RAII wrapper for pipe file descriptors
    class pipe_guard
    {
    public:
      pipe_guard()
      {
        int fds[2];
        if (pipe(fds) == 0) {
          read_fd_.reset(new fd_guard(fds[0]));
          write_fd_.reset(new fd_guard(fds[1]));
        }
      }

      int read_fd() const { return read_fd_->get(); }
      int write_fd() const { return write_fd_->get(); }
      bool valid() const { return read_fd_->get() >= 0 && write_fd_->get() >= 0; }

      // Release ownership of file descriptors
      int release_read_fd() { return read_fd_ ? read_fd_->release() : -1; }
      int release_write_fd() { return write_fd_ ? write_fd_->release() : -1; }

    private:
      std::unique_ptr<fd_guard> read_fd_;
      std::unique_ptr<fd_guard> write_fd_;
    };

    // RAII wrapper for child process
    class child_process_guard
    {
    public:
      child_process_guard(pid_t pid) : pid_(pid) {}
      ~child_process_guard() {
        if (pid_ > 0) {
          kill(pid_, SIGKILL);
          // Non-blocking reap attempt. If the child doesn't die immediately
          // (e.g., stuck in uninterruptible I/O / D-state), we leave it
          // rather than blocking the event loop. It will be reaped by init
          // eventually.
          int status;
          waitpid(pid_, &status, WNOHANG);
        }
      }
      pid_t get() const { return pid_; }
      pid_t release() { pid_t tmp = pid_; pid_ = -1; return tmp; }
    private:
      pid_t pid_;
    };
  } // namespace

  std::string exec_tools::find_executable(const std::string& command) const
  {
    // If it's already an absolute or relative path, use it as-is
    if (command.find('/') != std::string::npos) {
      return command;
    }

    // Search PATH
    const char* path_env = std::getenv("PATH");
    if (!path_env) {
      return "";
    }

    std::string path_str(path_env);
    size_t start = 0;
    size_t end = path_str.find(':');

    while (start < path_str.size()) {
      std::string dir = (end == std::string::npos)
        ? path_str.substr(start)
        : path_str.substr(start, end - start);

      if (!dir.empty()) {
        fs::path candidate = fs::path(dir) / command;
        if (fs::exists(candidate) && fs::is_regular_file(candidate)) {
          // Check if it's executable
          if (access(candidate.string().c_str(), X_OK) == 0) {
            return candidate.string();
          }
        }
      }

      if (end == std::string::npos) break;
      start = end + 1;
      end = path_str.find(':', start);
    }

    return "";
  }

  dripcore::awaitable<json> exec_tools::exec(
    std::string command,
    std::vector<std::string> args,
    std::string cwd,
    int timeout_seconds
  )
  {
    // Find the executable path
    std::string exec_path = find_executable(command);
    if (exec_path.empty()) {
      co_return {
        {"error", std::format("command not found: {}", command)},
        {"exit_code", -1}
      };
    }

    // Build argv for execvp
    std::vector<char*> argv;
    argv.push_back(strdup(exec_path.c_str()));
    for (const auto& arg : args) {
      argv.push_back(strdup(arg.c_str()));
    }
    argv.push_back(nullptr);

    // Cleanup argv memory
    auto argv_cleanup = [&argv]() {
      for (size_t i = 0; i < argv.size() - 1; ++i) {
        free(argv[i]);
      }
    };

    // Create pipes for stdout and stderr
    pipe_guard stdout_pipe;
    pipe_guard stderr_pipe;

    if (!stdout_pipe.valid() || !stderr_pipe.valid()) {
      argv_cleanup();
      co_return {
        {"error", "failed to create pipes"},
        {"exit_code", -1}
      };
    }

    pid_t pid = fork();
    if (pid == -1) {
      argv_cleanup();
      co_return {
        {"error", std::format("fork failed: {}", strerror(errno))},
        {"exit_code", -1}
      };
    }

    // RAII guard to ensure child process is killed if we exit early
    child_process_guard child_guard(pid);
    int final_status = 0;
    bool child_exited_normally = false;

    if (pid == 0) {
      // Child process

      // Close read ends of pipes
      close(stdout_pipe.read_fd());
      close(stderr_pipe.read_fd());

      // Redirect stdout
      dup2(stdout_pipe.write_fd(), STDOUT_FILENO);
      close(stdout_pipe.write_fd());

      // Redirect stderr
      dup2(stderr_pipe.write_fd(), STDERR_FILENO);
      close(stderr_pipe.write_fd());

      // Change working directory if specified
      if (!cwd.empty()) {
        if (chdir(cwd.c_str()) != 0) {
          std::cerr << "chdir failed: " << strerror(errno) << std::endl;
          _exit(127);
        }
      }

      // Execute the command
      execvp(exec_path.c_str(), argv.data());

      // If we get here, execvp failed
      std::cerr << "execvp failed: " << strerror(errno) << std::endl;
      _exit(127);
    }

    // Parent process
    argv_cleanup();

    // Release ownership of pipe file descriptors from guards
    // so they don't get closed when guards go out of scope
    int stdout_read_fd = stdout_pipe.release_read_fd();
    int stderr_read_fd = stderr_pipe.release_read_fd();
    int stdout_write_fd = stdout_pipe.release_write_fd();
    int stderr_write_fd = stderr_pipe.release_write_fd();

    // Close write ends of pipes in parent (we only need to read)
    if (stdout_write_fd >= 0) close(stdout_write_fd);
    if (stderr_write_fd >= 0) close(stderr_write_fd);

    // Set pipes to non-blocking mode for timeout support
    if (stdout_read_fd >= 0) fcntl(stdout_read_fd, F_SETFL, O_NONBLOCK);
    if (stderr_read_fd >= 0) fcntl(stderr_read_fd, F_SETFL, O_NONBLOCK);

    // Read stdout and stderr from child with timeout
    std::string stdout_content;
    std::string stderr_content;
    bool timed_out = false;

    auto start_time = std::chrono::steady_clock::now();

    while (true) {
      // Read available data from stdout first (before checking exit status)
      if (stdout_read_fd >= 0) {
        std::array<char, 4096> buffer;
        ssize_t n;
        while ((n = read(stdout_read_fd, buffer.data(), buffer.size())) > 0) {
          stdout_content.append(buffer.data(), n);
        }
        // Ignore EAGAIN (no data available)
        if (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
          // Real error - but only if we haven't timed out
          if (!timed_out) {
            child_guard.release();
            if (stdout_read_fd >= 0) close(stdout_read_fd);
            if (stderr_read_fd >= 0) close(stderr_read_fd);
            co_return {
              {"error", std::format("stdout read failed: {}", strerror(errno))},
              {"exit_code", -1},
              {"stdout", stdout_content},
              {"stderr", stderr_content}
            };
          }
        }
      }

      // Read available data from stderr
      if (stderr_read_fd >= 0) {
        std::array<char, 4096> buffer;
        ssize_t n;
        while ((n = read(stderr_read_fd, buffer.data(), buffer.size())) > 0) {
          stderr_content.append(buffer.data(), n);
        }
        // Ignore EAGAIN (no data available)
        if (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
          // Real error - but only if we haven't timed out
          if (!timed_out) {
            child_guard.release();
            if (stdout_read_fd >= 0) close(stdout_read_fd);
            if (stderr_read_fd >= 0) close(stderr_read_fd);
            co_return {
              {"error", std::format("stderr read failed: {}", strerror(errno))},
              {"exit_code", -1},
              {"stdout", stdout_content},
              {"stderr", stderr_content}
            };
          }
        }
      }

      // Check if child has exited
      int status;
      pid_t waited = waitpid(pid, &status, WNOHANG);
      if (waited == pid) {
        // Child exited normally
        final_status = status;
        child_exited_normally = true;
        // Child is already reaped — release the guard so the destructor
        // doesn't send SIGKILL to a potentially recycled pid.
        child_guard.release();
        break;
      } else if (waited == -1) {
        // Error
        child_guard.release();
        if (stdout_read_fd >= 0) close(stdout_read_fd);
        if (stderr_read_fd >= 0) close(stderr_read_fd);
        co_return {
          {"error", std::format("waitpid failed: {}", strerror(errno))},
          {"exit_code", -1},
          {"stdout", stdout_content},
          {"stderr", stderr_content}
        };
      }

      // Check for timeout
      auto elapsed = std::chrono::steady_clock::now() - start_time;
      if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= timeout_seconds) {
        timed_out = true;
        break;
      }

      // Yield to the event loop for 10ms to prevent busy-waiting
      // without blocking other dripcore tasks.
      co_await dripcore::sleep_awaiter{
        dripcore::executor::current(),
        nullptr,
        std::chrono::milliseconds(10)
      };
    }

    // Clean up read ends of pipes
    if (stdout_read_fd >= 0) close(stdout_read_fd);
    if (stderr_read_fd >= 0) close(stderr_read_fd);

    if (timed_out) {
      // Kill the child process
      child_guard.release();
      kill(pid, SIGKILL);
      int status;
      waitpid(pid, &status, 0);

      co_return {
        {"error", std::format("command timed out after {} seconds", timeout_seconds)},
        {"exit_code", -1},
        {"stdout", stdout_content},
        {"stderr", stderr_content},
        {"command", command},
        {"args", args},
        {"timed_out", true}
      };
    }

    // Child exited normally - use the status we captured earlier
    if (!child_exited_normally) {
      // This shouldn't happen, but handle it
      child_guard.release();
      co_return {
        {"error", "child did not exit normally"},
        {"exit_code", -1},
        {"stdout", stdout_content},
        {"stderr", stderr_content},
        {"command", command},
        {"args", args},
        {"timed_out", false}
      };
    }

    int exit_code = 0;
    if (WIFEXITED(final_status)) {
      exit_code = WEXITSTATUS(final_status);
    } else if (WIFSIGNALED(final_status)) {
      exit_code = 128 + WTERMSIG(final_status);
    } else {
      exit_code = -1;
    }

    co_return {
      {"exit_code", exit_code},
      {"stdout", stdout_content},
      {"stderr", stderr_content},
      {"command", command},
      {"args", args},
      {"timed_out", false}
    };
  }

} // namespace mistral::tools