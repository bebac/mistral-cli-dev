#include "tools/exec_tools.h"

#include "common/executable_finder.h"
#include "common/subprocess.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <dripcore/sleep_awaiter.h>
#include <filesystem>
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

    // Spawn the child: stdout/stderr piped, stdin from /dev/null.
    // spawn() also puts the child in its own process group (both
    // sides attempt setpgid), marks the pipe fds non-blocking and
    // close-on-exec, and makes the child close all inherited
    // descriptors above stderr before exec.
    common::subprocess_options spawn_options;
    spawn_options.command = exec_path; // already resolved by find_executable
    spawn_options.args = args;
    spawn_options.cwd = cwd;

    common::subprocess sub;
    auto spawned = sub.spawn(spawn_options);
    if (spawned != common::spawn_status::ok)
    {
      std::string error;
      if (spawned == common::spawn_status::pipe_failed)
      {
        error = "failed to create pipes";
      }
      else if (spawned == common::spawn_status::fork_failed)
      {
        error = std::format("fork failed: {}", strerror(sub.spawn_errno()));
      }
      else
      {
        error = std::format("command not found: {}", command);
      }
      co_return {{"error", error}, {"exit_code", -1}};
    }

    int final_status = 0;
    bool child_exited_normally = false;

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
      if (sub.stdout_fd() >= 0)
      {
        std::array<char, 4096> buffer;
        ssize_t n;
        while ((n = read(sub.stdout_fd(), buffer.data(), buffer.size())) > 0)
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
          // Real error - but only if we haven't timed out. The child
          // is killed and the pipes closed by ~subprocess() on return.
          if (!timed_out)
          {
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
      if (sub.stderr_fd() >= 0)
      {
        std::array<char, 4096> buffer;
        ssize_t n;
        while ((n = read(sub.stderr_fd(), buffer.data(), buffer.size())) > 0)
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
          // Real error - but only if we haven't timed out. The child
          // is killed and the pipes closed by ~subprocess() on return.
          if (!timed_out)
          {
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

      // Check if child has exited (non-blocking reap; ~subprocess()
      // knows the child was reaped and will not kill a recycled pid).
      int status;
      if (sub.try_reap(&status))
      {
        // Child exited normally
        final_status = status;
        child_exited_normally = true;
        break;
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

    if (timed_out)
    {
      // Kill the child's entire process group so grandchildren do not
      // survive the timeout, then reap (blocking is fine here: the
      // group has SIGKILL pending).
      sub.kill();
      int status;
      sub.wait(&status);

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
