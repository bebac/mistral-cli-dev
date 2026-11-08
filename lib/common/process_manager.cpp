#include "common/process_manager.h"

#include "common/executable_finder.h"

#include <dripcore/executor.h>
#include <dripcore/sleep_awaiter.h>
#include <dripcore/task.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <format>
#include <sys/wait.h>
#include <unistd.h>

namespace mistral::common
{
  namespace
  {
    // Same decoding run_exec() uses: 128+signal when killed by one.
    int decode_exit_code(int status)
    {
      if (WIFEXITED(status))
      {
        return WEXITSTATUS(status);
      }
      if (WIFSIGNALED(status))
      {
        return 128 + WTERMSIG(status);
      }
      return -1;
    }

    bool is_valid_session_name(const std::string& name)
    {
      if (name.empty() || name.size() > 64)
      {
        return false;
      }
      return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' ||
               c == '-';
      });
    }

    // Drains one session's pipe into its ring buffer. Runs on the
    // scheduler thread (from the pump task), the same thread that owns
    // the manager, so no locking is needed.
    void drain_fd(process_manager& mgr, process_session& s, int fd, bool process_session::*eof_flag)
    {
      if (fd < 0 || s.*eof_flag)
      {
        return;
      }
      std::array<char, 4096> buffer;
      while (true)
      {
        ssize_t n = ::read(fd, buffer.data(), buffer.size());
        if (n > 0)
        {
          mgr.append_output(s, buffer.data(), static_cast<size_t>(n));
          continue;
        }
        if (n == 0)
        {
          s.*eof_flag = true; // child closed its end
          return;
        }
        if (errno == EINTR)
        {
          continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
          return;
        }
        // EBADF after kill() closed the fds, or any other hard error:
        // treat as EOF so the pump stops polling this fd.
        s.*eof_flag = true;
        return;
      }
    }

    // One pump task per session: keeps draining stdout/stderr so the
    // child never blocks on a full pipe, and reaps the child when it
    // exits. Mirrors run_exec()'s 10 ms poll loop. The session (and its
    // subprocess) is owned by the manager and must outlive this task,
    // so teardown() deliberately does not kill anything.
    class process_pump_task : public dripcore::task
    {
    public:
      process_pump_task(process_manager* mgr, std::string id)
        : mgr_(mgr), id_(std::move(id))
      {
      }

    protected:
      dripcore::awaitable<> body() override
      {
        while (true)
        {
          auto* s = mgr_->find(id_);
          if (!s)
          {
            co_return;
          }
          drain_fd(*mgr_, *s, s->sub.stdout_fd(), &process_session::stdout_eof);
          drain_fd(*mgr_, *s, s->sub.stderr_fd(), &process_session::stderr_eof);
          if (auto status = s->sub.try_reap())
          {
            s->exited = true;
            s->exit_status = *status;
            // Final drain: data still sitting in the pipes after exit.
            drain_fd(*mgr_, *s, s->sub.stdout_fd(), &process_session::stdout_eof);
            drain_fd(*mgr_, *s, s->sub.stderr_fd(), &process_session::stderr_eof);
            co_return;
          }
          co_await sleep_for(std::chrono::milliseconds(10));
        }
      }

    private:
      process_manager* mgr_;
      std::string id_;
    };
  } // namespace

  process_manager::~process_manager()
  {
    kill_all();
  }

  process_start_result process_manager::start(
    const std::string& command,
    const std::vector<std::string>& args,
    const std::string& cwd,
    const std::map<std::string, std::string>& env_overrides,
    const std::string& name
  )
  {
    process_start_result result;

    // Resolve exactly the way exec does, so a relative path is not
    // re-resolved against the child's cwd after chdir().
    std::string exec_path = utils::resolve_executable(command, cwd);
    if (exec_path.empty())
    {
      result.error = std::format("command not found: {}", command);
      return result;
    }

    std::string id = name;
    if (id.empty())
    {
      id = "p" + std::to_string(next_auto_id_++);
    }
    else if (!is_valid_session_name(id))
    {
      result.error = std::format("invalid session name '{}': use letters, digits, '.', '_' or '-' (max 64)", name);
      return result;
    }
    else if (sessions_.contains(id))
    {
      result.error = std::format("session id already exists: {}", id);
      return result;
    }

    subprocess_options options;
    options.command = command;     // argv[0] as the caller wrote it
    options.exec_path = exec_path; // absolute, resolved above
    options.args = args;
    options.cwd = cwd;
    options.env_overrides = env_overrides;
    options.pipe_stdin = true; // the whole point: drive the child's stdin
    options.pipe_stdout = true;
    options.pipe_stderr = true;

    auto session = std::make_unique<process_session>();
    session->id = id;
    session->name = name;
    session->command = command;
    session->args = args;
    session->cwd = cwd;
    session->started_at = std::chrono::steady_clock::now();

    auto spawned = session->sub.spawn(options);
    if (spawned != spawn_status::ok)
    {
      if (spawned == spawn_status::pipe_failed)
      {
        result.error = std::format("failed to create pipes: {}", strerror(session->sub.spawn_errno()));
      }
      else if (spawned == spawn_status::fork_failed)
      {
        result.error = std::format("fork failed: {}", strerror(session->sub.spawn_errno()));
      }
      else if (spawned == spawn_status::exec_failed)
      {
        result.error = std::format("exec failed: {}", strerror(session->sub.spawn_errno()));
      }
      else
      {
        result.error = std::format("command not found: {}", command);
      }
      return result;
    }

    result.id = id;
    result.pid = session->sub.pid();
    sessions_[id] = std::move(session);

    // The pump task keeps the pipes drained for the session's whole
    // lifetime. start() is only ever called from a tool coroutine on
    // the scheduler thread, so executor::current() is valid.
    if (auto* exec = dripcore::executor::current())
    {
      exec->spawn<process_pump_task>(this, id);
    }
    else
    {
      // No scheduler: nothing would drain the pipes. Fail rather than
      // leave a session that blocks on a full pipe.
      sessions_[id]->sub.kill();
      sessions_.erase(id);
      result.id.clear();
      result.pid = -1;
      result.error = "process_manager must be used on the dripcore scheduler thread";
    }
    return result;
  }

  int process_manager::stdin_fd(const std::string& id) const
  {
    const auto* s = find(id);
    return s ? s->sub.stdin_fd() : -1;
  }

  process_read_result process_manager::read(const std::string& id, uint64_t since, size_t max_bytes) const
  {
    process_read_result result;
    const auto* s = find(id);
    if (!s)
    {
      return result;
    }
    result.found = true;
    result.running = s->sub.running();
    result.exited = s->exited;
    if (s->exited)
    {
      result.exit_code = decode_exit_code(s->exit_status);
    }
    result.total_bytes = s->total_bytes;

    uint64_t start = since > s->total_bytes ? s->total_bytes : since;
    if (!s->chunks.empty() && start < s->chunks.front().first)
    {
      // The reader fell behind the ring buffer: hand back what is
      // still retained and flag the gap.
      result.truncated = true;
      start = s->chunks.front().first;
    }
    for (const auto& [offset, data] : s->chunks)
    {
      if (offset + data.size() <= start)
      {
        continue;
      }
      if (offset >= start + max_bytes)
      {
        break;
      }
      size_t from = offset < start ? static_cast<size_t>(start - offset) : 0;
      size_t take = std::min(data.size() - from, max_bytes - result.output.size());
      result.output.append(data, from, take);
      if (result.output.size() >= max_bytes)
      {
        break;
      }
    }
    result.next_offset = start + result.output.size();
    return result;
  }

  std::vector<process_info> process_manager::list() const
  {
    std::vector<process_info> out;
    out.reserve(sessions_.size());
    auto now = std::chrono::steady_clock::now();
    for (const auto& [id, s] : sessions_)
    {
      process_info info;
      info.id = id;
      info.name = s->name;
      info.command = s->command;
      info.args = s->args;
      info.pid = s->sub.pid();
      info.running = s->sub.running();
      info.exited = s->exited;
      if (s->exited)
      {
        info.exit_code = decode_exit_code(s->exit_status);
      }
      info.total_bytes = s->total_bytes;
      info.uptime_seconds = std::chrono::duration_cast<std::chrono::seconds>(now - s->started_at).count();
      out.push_back(std::move(info));
    }
    return out;
  }

  bool process_manager::kill(const std::string& id)
  {
    auto* s = find(id);
    if (!s)
    {
      return false;
    }
    // SIGKILL is pending for the whole group, so the blocking reap
    // cannot hang (same reasoning as run_exec's timeout path).
    s->sub.kill();
    if (auto status = s->sub.wait())
    {
      s->exited = true;
      s->exit_status = *status;
    }
    else if (!s->exited)
    {
      // Nothing to reap (e.g. already reaped elsewhere): still mark
      // the session as gone so callers see a consistent state.
      s->exited = true;
    }
    return true;
  }

  void process_manager::kill_all()
  {
    for (auto& [id, s] : sessions_)
    {
      // Non-blocking on purpose: kill_all() runs during shutdown.
      s->sub.kill();
    }
  }

  process_session* process_manager::find(const std::string& id)
  {
    auto it = sessions_.find(id);
    return it != sessions_.end() ? it->second.get() : nullptr;
  }

  const process_session* process_manager::find(const std::string& id) const
  {
    auto it = sessions_.find(id);
    return it != sessions_.end() ? it->second.get() : nullptr;
  }

  void process_manager::append_output(process_session& s, const char* data, size_t n)
  {
    s.chunks.emplace_back(s.total_bytes, std::string(data, n));
    s.total_bytes += n;
    s.buffered_bytes += n;
    // Drop from the front past the cap, but always keep at least one
    // chunk so chunks.front().first stays a valid lower bound.
    while (s.buffered_bytes > process_session::k_max_buffered_bytes && s.chunks.size() > 1)
    {
      s.buffered_bytes -= s.chunks.front().second.size();
      s.chunks.pop_front();
    }
  }

  bool process_manager::output_matches(const std::string& id, const std::regex& re) const
  {
    const auto* s = find(id);
    if (!s)
    {
      return false;
    }
    // The buffer is capped at 256 KiB, so this scan is bounded.
    std::string all;
    all.reserve(s->buffered_bytes);
    for (const auto& [offset, data] : s->chunks)
    {
      all += data;
    }
    return std::regex_search(all, re);
  }

  bool process_manager::exited(const std::string& id, int& exit_code) const
  {
    const auto* s = find(id);
    if (!s || !s->exited)
    {
      return false;
    }
    exit_code = decode_exit_code(s->exit_status);
    return true;
  }
} // namespace mistral::common
