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

    // Receives the progress of a running command. Implemented by
    // exec_tools::exec (which only wants the final result) and by
    // exec_run_task (which forwards the output to its channel).
    class exec_sink
    {
    public:
      virtual ~exec_sink() = default;

      // A chunk read from the child. `is_stderr` says which pipe it came
      // from; both pipes are drained in a fixed order each tick, so this
      // is not the child's true interleaving.
      virtual void on_output(const std::string& chunk, bool is_stderr) = 0;

      // Called once per poll tick, whether or not output arrived.
      virtual void on_tick()
      {
      }

      // True when the run should stop and report its partial output.
      virtual bool cancel_requested() const
      {
        return false;
      }

      // True when nobody is listening for the result any more, so the run
      // should stop without reporting anything.
      virtual bool abandoned() const
      {
        return false;
      }
    };

    // Discards the stream: the caller only wants the final JSON.
    class null_exec_sink final : public exec_sink
    {
    public:
      void on_output(const std::string&, bool) override
      {
      }
    };

    // Forwards output to a channel without ever blocking the poll loop.
    // try_send() fails when the channel is full, so the chunk is kept in
    // pending_ and retried on the next tick: the pipe is always drained
    // and no output is lost. pending_ is bounded, because a consumer that
    // stops draining must not turn into unbounded memory growth.
    class channel_exec_sink final : public exec_sink
    {
    public:
      channel_exec_sink(
        std::shared_ptr<dripcore::channel<exec_event>> out_ch,
        std::shared_ptr<std::atomic<bool>> cancel_requested
      )
        : out_ch_(std::move(out_ch)), cancel_requested_(std::move(cancel_requested))
      {
      }

      void on_output(const std::string& chunk, bool is_stderr) override
      {
        if (pending_.size() < k_max_pending_bytes)
        {
          pending_ += chunk;
          pending_is_stderr_ = is_stderr;
        }
        flush_pending();
      }

      void on_tick() override
      {
        flush_pending();
      }

      bool cancel_requested() const override
      {
        return cancel_requested_ && cancel_requested_->load(std::memory_order_relaxed);
      }

      bool abandoned() const override
      {
        return out_ch_->is_closed();
      }

    private:
      // Bound on the retry buffer. Only reached when the consumer drains
      // slower than the child produces; the excess is dropped rather than
      // growing without limit.
      static constexpr size_t k_max_pending_bytes = 256 * 1024;

      void flush_pending()
      {
        if (pending_.empty())
        {
          return;
        }
        // try_send() takes its argument by value, so the payload is copied
        // rather than moved: on failure the chunk must stay in pending_.
        if (out_ch_->try_send({exec_event::type::output, pending_, pending_is_stderr_}))
        {
          pending_.clear();
        }
      }

      std::shared_ptr<dripcore::channel<exec_event>> out_ch_;
      std::shared_ptr<std::atomic<bool>> cancel_requested_;
      std::string pending_;
      bool pending_is_stderr_ = false;
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

  std::optional<std::string> exec_tools::validate(
    const std::string& command,
    const std::vector<std::string>& args,
    const std::string& cwd
  )
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

    // A cwd that does not exist only fails later, in the child's chdir()
    // after approval has been granted - reject it here instead.
    if (!cwd.empty())
    {
      std::error_code ec;
      if (!fs::is_directory(cwd, ec))
      {
        return std::format("working directory does not exist: {}", cwd);
      }
    }

    // Resolve exactly the way exec() will, so a call that passes this
    // check cannot fail at execve().
    if (utils::resolve_executable(command, cwd).empty())
    {
      return std::format("command not found: {}", command);
    }

    return std::nullopt;
  }

  // The child-process lifecycle: spawn, drain both pipes, enforce the
  // wall-clock timeout, and build the result JSON. Shared by
  // exec_tools::exec and exec_run_task, so there is exactly one copy of
  // the poll loop and the two entry points cannot drift apart.
  //
  // `static` (internal linkage) because it is an implementation detail of
  // this file; the anonymous namespace above is already closed.
  static dripcore::awaitable<json> run_exec(
    const std::string& command,
    const std::vector<std::string>& args,
    const std::string& cwd,
    int timeout_seconds,
    exec_sink& sink
  )
  {
    // Resolve to an absolute path: the child chdirs to `cwd` before
    // exec, so a relative path would otherwise be resolved against the
    // wrong directory. `command` stays argv[0].
    std::string exec_path = utils::resolve_executable(command, cwd);
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
    spawn_options.command = command;     // argv[0] as the caller wrote it
    spawn_options.exec_path = exec_path; // absolute, resolved above
    spawn_options.args = args;
    spawn_options.cwd = cwd;

    common::subprocess sub;
    auto spawned = sub.spawn(spawn_options);
    if (spawned != common::spawn_status::ok)
    {
      std::string error;
      if (spawned == common::spawn_status::pipe_failed)
      {
        error = std::format("failed to create pipes: {}", strerror(sub.spawn_errno()));
      }
      else if (spawned == common::spawn_status::fork_failed)
      {
        error = std::format("fork failed: {}", strerror(sub.spawn_errno()));
      }
      else if (spawned == common::spawn_status::exec_failed)
      {
        error = std::format("exec failed: {}", strerror(sub.spawn_errno()));
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
    bool cancelled = false;
    bool abandoned = false;

    auto start_time = std::chrono::steady_clock::now();

    while (true)
    {
      // Stop when the consumer asked to cancel or went away. Checked
      // before draining so a cancelled run stops within one tick.
      if (sink.cancel_requested())
      {
        cancelled = true;
        break;
      }
      if (sink.abandoned())
      {
        abandoned = true;
        break;
      }

      // Read available data from stdout first (before checking exit status)
      if (sub.stdout_fd() >= 0)
      {
        std::array<char, 4096> buffer;
        ssize_t n;
        while ((n = read(sub.stdout_fd(), buffer.data(), buffer.size())) > 0)
        {
          sink.on_output(std::string(buffer.data(), static_cast<size_t>(n)), false);
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
          sink.on_output(std::string(buffer.data(), static_cast<size_t>(n)), true);
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
      if (auto status = sub.try_reap())
      {
        // Child exited normally
        final_status = *status;
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

      // Retry a chunk that did not fit in the channel last time.
      sink.on_tick();

      // Yield to the event loop for 10ms to prevent busy-waiting
      // without blocking other dripcore tasks.
      co_await dripcore::sleep_awaiter{dripcore::executor::current(), nullptr, std::chrono::milliseconds(10)};
    }

    if (cancelled)
    {
      // Same reasoning as the timeout path: SIGKILL is pending for the
      // whole group, so the blocking reap cannot hang.
      sub.kill();
      sub.wait();

      co_return {
        {"error", "cancelled"},
        {"exit_code", -1},
        {"stdout", stdout_content},
        {"stderr", stderr_content},
        {"stdout_truncated", stdout_truncated},
        {"stderr_truncated", stderr_truncated},
        {"command", command},
        {"args", args},
        {"timed_out", false},
        {"cancelled", true}
      };
    }

    if (abandoned)
    {
      // Nobody is listening for the result any more: stop the child and
      // return a minimal payload, which is discarded.
      sub.kill();
      sub.wait();
      co_return json{{"error", "cancelled"}, {"exit_code", -1}};
    }

    if (timed_out)
    {
      // Kill the child's entire process group so grandchildren do not
      // survive the timeout, then reap (blocking is fine here: the
      // group has SIGKILL pending).
      sub.kill();
      sub.wait(); // blocking is fine here: the group has SIGKILL pending

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

  dripcore::awaitable<json> exec_tools::exec(
    std::string command,
    std::vector<std::string> args,
    std::string cwd,
    int timeout_seconds
  )
  {
    null_exec_sink sink;
    co_return co_await run_exec(command, args, cwd, timeout_seconds, sink);
  }

  std::optional<exec_request> exec_tools::parse_request(const json& args)
  {
    if (!args.is_object() || !args.contains("command") || !args["command"].is_string())
    {
      return std::nullopt;
    }

    exec_request request;
    request.command = args["command"].get<std::string>();

    if (args.contains("args"))
    {
      if (!args["args"].is_array())
      {
        return std::nullopt;
      }
      for (const auto& arg : args["args"])
      {
        if (!arg.is_string())
        {
          return std::nullopt;
        }
        request.args.push_back(arg.get<std::string>());
      }
    }

    if (args.contains("cwd"))
    {
      if (!args["cwd"].is_string())
      {
        return std::nullopt;
      }
      request.cwd = args["cwd"].get<std::string>();
    }

    if (args.contains("timeout_seconds"))
    {
      if (!args["timeout_seconds"].is_number_integer())
      {
        return std::nullopt;
      }
      request.timeout_seconds = args["timeout_seconds"].get<int>();
    }

    return request;
  }

  exec_run_task::exec_run_task(
    std::string command,
    std::vector<std::string> args,
    std::string cwd,
    int timeout_seconds,
    std::shared_ptr<dripcore::channel<exec_event>> out_ch,
    std::shared_ptr<std::atomic<bool>> cancel_requested
  )
    : command_(std::move(command)), args_(std::move(args)), cwd_(std::move(cwd)), timeout_seconds_(timeout_seconds),
      out_ch_(std::move(out_ch)), cancel_requested_(std::move(cancel_requested))
  {
  }

  dripcore::awaitable<> exec_run_task::body()
  {
    channel_exec_sink sink(out_ch_, cancel_requested_);
    auto result = co_await run_exec(command_, args_, cwd_, timeout_seconds_, sink);

    // The channel may still be full of output events, so the final result
    // is retried rather than dropped: losing it would hand the model an
    // empty tool result. A closed channel means the consumer is gone.
    const std::string payload = result.dump();
    while (!out_ch_->try_send({exec_event::type::done, payload}))
    {
      if (out_ch_->is_closed())
      {
        co_return;
      }
      co_await sleep_for(std::chrono::milliseconds(1));
    }
    co_return;
  }

  dripcore::awaitable<> exec_run_task::teardown()
  {
    // Always close: the consumer's recv() only returns once the channel
    // is closed, so a missing close would hang the agent.
    out_ch_->close();
    co_return;
  }

} // namespace mistral::tools
