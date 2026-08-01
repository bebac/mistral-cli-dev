#include "tools/exec_tools.h"

#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <fcntl.h>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <vector>
#include <array>

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

    private:
      std::unique_ptr<fd_guard> read_fd_;
      std::unique_ptr<fd_guard> write_fd_;
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

    // Close write ends of pipes in parent
    close(stdout_pipe.write_fd());
    close(stderr_pipe.write_fd());

    // Read stdout and stderr from child
    std::string stdout_content;
    std::string stderr_content;

    {
      std::array<char, 4096> buffer;
      ssize_t n;

      // Read stdout
      while ((n = read(stdout_pipe.read_fd(), buffer.data(), buffer.size())) > 0) {
        stdout_content.append(buffer.data(), n);
      }

      // Read stderr
      while ((n = read(stderr_pipe.read_fd(), buffer.data(), buffer.size())) > 0) {
        stderr_content.append(buffer.data(), n);
      }
    }

    // Wait for child to finish
    int status;
    pid_t waited = waitpid(pid, &status, 0);

    if (waited == -1) {
      co_return {
        {"error", std::format("waitpid failed: {}", strerror(errno))},
        {"exit_code", -1},
        {"stdout", stdout_content},
        {"stderr", stderr_content}
      };
    }

    int exit_code = 0;
    if (WIFEXITED(status)) {
      exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
      exit_code = 128 + WTERMSIG(status);
    } else {
      exit_code = -1;
    }

    co_return {
      {"exit_code", exit_code},
      {"stdout", stdout_content},
      {"stderr", stderr_content},
      {"command", command},
      {"args", args}
    };
  }

} // namespace mistral::tools