#include "tools/exec_tools.h"

#include "common/executable_finder.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <dripcore/sleep_awaiter.h>
#include <fcntl.h>
#include <filesystem>
#include <signal.h>
#include <sstream>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

namespace mistral::tools
{
  namespace
  {
    // Maximum bytes of stdout/stderr kept in the result. Excess output is
    // still drained from the pipe (so the child never blocks on a full pipe
    // buffer) but discarded, to avoid blowing the model's context window.
    constexpr size_t k_max_output_bytes = 64 * 1024;

    // RAII wrapper for file descriptors
    class fd_guard
    {
    public:
      fd_guard(int fd) : fd_(fd)
      {
      }
      ~fd_guard()
      {
        if (fd_ >= 0)
          close(fd_);
      }
      int get() const
      {
        return fd_;
      }
      int release()
      {
        int tmp = fd_;
        fd_ = -1;
        return tmp;
      }

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
        if (pipe(fds) == 0)
        {
          read_fd_.reset(new fd_guard(fds[0]));
          write_fd_.reset(new fd_guard(fds[1]));
        }
      }

      int read_fd() const
      {
        return read_fd_->get();
      }
      int write_fd() const
      {
        return write_fd_->get();
      }
      bool valid() const
      {
        return read_fd_->get() >= 0 && write_fd_->get() >= 0;
      }

      // Release ownership of file descriptors
      int release_read_fd()
      {
        return read_fd_ ? read_fd_->release() : -1;
      }
      int release_write_fd()
      {
        return write_fd_ ? write_fd_->release() : -1;
      }

    private:
      std::unique_ptr<fd_guard> read_fd_;
      std::unique_ptr<fd_guard> write_fd_;
    };

    // RAII wrapper for child process
    class child_process_guard
    {
    public:
      child_process_guard(pid_t pid) : pid_(pid)
      {
      }
      ~child_process_guard()
      {
        if (pid_ > 0)
        {
          // Kill the entire process group (the child is made a process
          // group leader in exec()); fall back to the direct child if
          // the group no longer exists (e.g. setpgid failed).
          if (kill(-pid_, SIGKILL) == -1)
          {
            kill(pid_, SIGKILL);
          }
          // Non-blocking reap attempt. If the child doesn't die immediately
          // (e.g., stuck in uninterruptible I/O / D-state), we leave it
          // rather than blocking the event loop. It will be reaped by init
          // eventually.
          int status;
          waitpid(pid_, &status, WNOHANG);
        }
      }
      pid_t get() const
      {
        return pid_;
      }
      pid_t release()
      {
        pid_t tmp = pid_;
        pid_ = -1;
        return tmp;
      }

    private:
      pid_t pid_;
    };

    // Commands that legitimately receive shell/program source in their
    // arguments; shell-syntax checks in arguments are skipped for these.
    bool is_interpreter(const std::string& command)
    {
      static const std::array<std::string, 14> interpreters = {
        "sh", "bash", "zsh", "dash", "ksh", "fish", "csh", "tcsh", "python", "python3", "node", "perl", "ruby", "awk"
      };
      return std::find(interpreters.begin(), interpreters.end(), command) != interpreters.end();
    }

    // Shell syntax or whitespace is never valid in the executable name
    // passed to exec(): execvp() is invoked directly, with no shell,
    // so these can only be mistakes (e.g. a whole command line).
    bool has_shell_syntax(const std::string& s)
    {
      return s.find_first_of(" \t\n\r|&;<>()$`\"'") != std::string::npos;
    }
  } // namespace

  std::optional<std::string> exec_tools::validate(const std::string& command, const std::vector<std::string>& args)
  {
    if (command.empty())
    {
      return std::string("command is empty");
    }

    if (has_shell_syntax(command))
    {
      return std::format(
        "invalid command '{}': exec uses execvp() directly with no shell - "
        "the command must be a single executable name (e.g. 'git') and each "
        "argument must be a separate element of the args array",
        command
      );
    }

    // Command substitution in arguments is shell syntax; without a shell
    // it would be passed to the command as a literal string. Skip the
    // check for shells and interpreters, which legitimately take program
    // source as arguments.
    if (!is_interpreter(command))
    {
      for (const auto& arg : args)
      {
        if (arg.find('`') != std::string::npos || arg.find("$(") != std::string::npos)
        {
          return std::format(
            "invalid shell syntax in argument '{}': exec uses execvp() "
            "directly with no shell - command substitution is not available; "
            "pass the literal value instead",
            arg
          );
        }
      }
    }

    if (utils::find_executable(command).empty())
    {
      return std::format("command not found: {}", command);
    }

    return std::nullopt;
  }

  dripcore::awaitable<json> exec_tools::exec(
    std::string command,
    std::vector<std::string> args,
    std::string cwd,
    int timeout_seconds
  )
  {
    // Find the executable path
    std::string exec_path = utils::find_executable(command);
    if (exec_path.empty())
    {
      co_return {{"error", std::format("command not found: {}", command)}, {"exit_code", -1}};
    }

    // Build argv for execvp
    std::vector<char*> argv;
    argv.push_back(strdup(exec_path.c_str()));
    for (const auto& arg : args)
    {
      argv.push_back(strdup(arg.c_str()));
    }
    argv.push_back(nullptr);

    // Cleanup argv memory
    auto argv_cleanup = [&argv]() {
      for (size_t i = 0; i < argv.size() - 1; ++i)
      {
        free(argv[i]);
      }
    };

    // Create pipes for stdout and stderr
    pipe_guard stdout_pipe;
    pipe_guard stderr_pipe;

    if (!stdout_pipe.valid() || !stderr_pipe.valid())
    {
      argv_cleanup();
      co_return {{"error", "failed to create pipes"}, {"exit_code", -1}};
    }

    pid_t pid = fork();
    if (pid == -1)
    {
      argv_cleanup();
      co_return {{"error", std::format("fork failed: {}", strerror(errno))}, {"exit_code", -1}};
    }

    // RAII guard to ensure child process is killed if we exit early
    child_process_guard child_guard(pid);

    // Set the process group from the parent side as well (see the
    // comment in the child branch: both sides must attempt it).
    setpgid(pid, pid);

    int final_status = 0;
    bool child_exited_normally = false;

    if (pid == 0)
    {
      // Child process

      // Start a new process group so the entire child tree can be
      // killed on timeout (kill(-pid, SIGKILL)). Without this only
      // the direct child dies and grandchildren survive (e.g. npm ->
      // storybook -> esbuild), keeping ports and file watchers busy.
      // The parent calls setpgid() as well: after fork() it is
      // undefined which side runs first, so both sides must try.
      setpgid(0, 0);

      // Never let the child steal terminal input from the CLI.
      int devnull_fd = open("/dev/null", O_RDONLY);
      if (devnull_fd >= 0)
      {
        dup2(devnull_fd, STDIN_FILENO);
        close(devnull_fd);
      }

      // Close read ends of pipes
      close(stdout_pipe.read_fd());
      close(stderr_pipe.read_fd());

      // Redirect stdout
      dup2(stdout_pipe.write_fd(), STDOUT_FILENO);
      close(stdout_pipe.write_fd());

      // Redirect stderr
      dup2(stderr_pipe.write_fd(), STDERR_FILENO);
      close(stderr_pipe.write_fd());

      // Close ALL other inherited file descriptors (kqueue fd from
      // the dripcore event loop, wakeup pipe fds, MCP stdio pipe
      // fds, etc.). Without this, the child holds the parent's
      // wakeup pipe write ends open — when the parent closes its
      // write ends during shutdown, the pipes never see EOF and the
      // dripcore event loop hangs forever.
      long max_fd = sysconf(_SC_OPEN_MAX);
      if (max_fd < 0)
        max_fd = 1024;
      for (long fd = 3; fd < max_fd; ++fd)
      {
        close(static_cast<int>(fd));
      }

      // Change working directory if specified
      if (!cwd.empty())
      {
        if (chdir(cwd.c_str()) != 0)
        {
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
    if (stdout_write_fd >= 0)
      close(stdout_write_fd);
    if (stderr_write_fd >= 0)
      close(stderr_write_fd);

    // Set pipes to non-blocking mode for timeout support
    if (stdout_read_fd >= 0)
      fcntl(stdout_read_fd, F_SETFL, O_NONBLOCK);
    if (stderr_read_fd >= 0)
      fcntl(stderr_read_fd, F_SETFL, O_NONBLOCK);

    // Set FD_CLOEXEC so future fork/exec children do NOT inherit
    // these pipe read fds (prevents pipe-EOF deadlocks when the
    // parent spawns another child while this exec is in flight).
    if (stdout_read_fd >= 0)
      fcntl(stdout_read_fd, F_SETFD, FD_CLOEXEC);
    if (stderr_read_fd >= 0)
      fcntl(stderr_read_fd, F_SETFD, FD_CLOEXEC);

    // Read stdout and stderr from child with timeout
    std::string stdout_content;
    std::string stderr_content;
    bool stdout_truncated = false;
    bool stderr_truncated = false;
    bool timed_out = false;

    auto start_time = std::chrono::steady_clock::now();

    while (true)
    {
      // Read available data from stdout first (before checking exit status)
      if (stdout_read_fd >= 0)
      {
        std::array<char, 4096> buffer;
        ssize_t n;
        while ((n = read(stdout_read_fd, buffer.data(), buffer.size())) > 0)
        {
          // Cap how much we keep; keep draining the pipe so the child
          // never blocks on a full pipe buffer.
          if (stdout_content.size() < k_max_output_bytes)
          {
            size_t remaining = k_max_output_bytes - stdout_content.size();
            size_t to_append = std::min(remaining, static_cast<size_t>(n));
            stdout_content.append(buffer.data(), to_append);
            if (stdout_content.size() >= k_max_output_bytes)
            {
              stdout_truncated = true;
            }
          }
        }
        // Ignore EAGAIN (no data available)
        if (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
          // Real error - but only if we haven't timed out
          if (!timed_out)
          {
            child_guard.release();
            if (stdout_read_fd >= 0)
              close(stdout_read_fd);
            if (stderr_read_fd >= 0)
              close(stderr_read_fd);
            co_return {
              {"error", std::format("stdout read failed: {}", strerror(errno))},
              {"exit_code", -1},
              {"stdout", stdout_content},
              {"stderr", stderr_content},
              {"stdout_truncated", stdout_truncated},
              {"stderr_truncated", stderr_truncated}
            };
          }
        }
      }

      // Read available data from stderr
      if (stderr_read_fd >= 0)
      {
        std::array<char, 4096> buffer;
        ssize_t n;
        while ((n = read(stderr_read_fd, buffer.data(), buffer.size())) > 0)
        {
          // Cap how much we keep; keep draining the pipe so the child
          // never blocks on a full pipe buffer.
          if (stderr_content.size() < k_max_output_bytes)
          {
            size_t remaining = k_max_output_bytes - stderr_content.size();
            size_t to_append = std::min(remaining, static_cast<size_t>(n));
            stderr_content.append(buffer.data(), to_append);
            if (stderr_content.size() >= k_max_output_bytes)
            {
              stderr_truncated = true;
            }
          }
        }
        // Ignore EAGAIN (no data available)
        if (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
          // Real error - but only if we haven't timed out
          if (!timed_out)
          {
            child_guard.release();
            if (stdout_read_fd >= 0)
              close(stdout_read_fd);
            if (stderr_read_fd >= 0)
              close(stderr_read_fd);
            co_return {
              {"error", std::format("stderr read failed: {}", strerror(errno))},
              {"exit_code", -1},
              {"stdout", stdout_content},
              {"stderr", stderr_content},
              {"stdout_truncated", stdout_truncated},
              {"stderr_truncated", stderr_truncated}
            };
          }
        }
      }

      // Check if child has exited
      int status;
      pid_t waited = waitpid(pid, &status, WNOHANG);
      if (waited == pid)
      {
        // Child exited normally
        final_status = status;
        child_exited_normally = true;
        // Child is already reaped — release the guard so the destructor
        // doesn't send SIGKILL to a potentially recycled pid.
        child_guard.release();
        break;
      }
      else if (waited == -1)
      {
        // Error
        child_guard.release();
        if (stdout_read_fd >= 0)
          close(stdout_read_fd);
        if (stderr_read_fd >= 0)
          close(stderr_read_fd);
        co_return {
          {"error", std::format("waitpid failed: {}", strerror(errno))},
          {"exit_code", -1},
          {"stdout", stdout_content},
          {"stderr", stderr_content},
          {"stdout_truncated", stdout_truncated},
          {"stderr_truncated", stderr_truncated}
        };
      }

      // Check for timeout
      auto elapsed = std::chrono::steady_clock::now() - start_time;
      if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >= timeout_seconds)
      {
        timed_out = true;
        break;
      }

      // Yield to the event loop for 10ms to prevent busy-waiting
      // without blocking other dripcore tasks.
      co_await dripcore::sleep_awaiter{dripcore::executor::current(), nullptr, std::chrono::milliseconds(10)};
    }

    // Clean up read ends of pipes
    if (stdout_read_fd >= 0)
      close(stdout_read_fd);
    if (stderr_read_fd >= 0)
      close(stderr_read_fd);

    if (timed_out)
    {
      // Kill the child's entire process group so grandchildren do not
      // survive the timeout. Fall back to the direct child if the
      // group kill fails.
      child_guard.release();
      if (kill(-pid, SIGKILL) == -1)
      {
        kill(pid, SIGKILL);
      }
      int status;
      waitpid(pid, &status, 0);

      co_return {
        {"error", std::format("command timed out after {} seconds", timeout_seconds)},
        {"exit_code", -1},
        {"stdout", stdout_content},
        {"stderr", stderr_content},
        {"stdout_truncated", stdout_truncated},
        {"stderr_truncated", stderr_truncated},
        {"command", command},
        {"args", args},
        {"timed_out", true}
      };
    }

    // Child exited normally - use the status we captured earlier
    if (!child_exited_normally)
    {
      // This shouldn't happen, but handle it
      child_guard.release();
      co_return {
        {"error", "child did not exit normally"},
        {"exit_code", -1},
        {"stdout", stdout_content},
        {"stderr", stderr_content},
        {"stdout_truncated", stdout_truncated},
        {"stderr_truncated", stderr_truncated},
        {"command", command},
        {"args", args},
        {"timed_out", false}
      };
    }

    int exit_code = 0;
    if (WIFEXITED(final_status))
    {
      exit_code = WEXITSTATUS(final_status);
    }
    else if (WIFSIGNALED(final_status))
    {
      exit_code = 128 + WTERMSIG(final_status);
    }
    else
    {
      exit_code = -1;
    }

    co_return {
      {"exit_code", exit_code},
      {"stdout", stdout_content},
      {"stderr", stderr_content},
      {"stdout_truncated", stdout_truncated},
      {"stderr_truncated", stderr_truncated},
      {"command", command},
      {"args", args},
      {"timed_out", false}
    };
  }

} // namespace mistral::tools
