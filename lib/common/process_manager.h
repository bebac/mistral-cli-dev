#ifndef MISTRAL_COMMON_PROCESS_MANAGER_H
#define MISTRAL_COMMON_PROCESS_MANAGER_H

#include "common/subprocess.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <regex>
#include <string>
#include <utility>
#include <vector>

namespace mistral::common
{
  // One background process started through process_manager: the
  // subprocess itself plus a bounded ring buffer of everything it has
  // printed so far (stdout and stderr merged, in drain order).
  struct process_session
  {
    std::string id;
    std::string name; // optional human-readable label ("" = none)
    std::string command;
    std::vector<std::string> args;
    std::string cwd;
    subprocess sub;

    // Output ring buffer: each chunk carries its absolute byte offset
    // (counted from process start), so a reader can ask for "everything
    // since offset N".
    std::deque<std::pair<uint64_t, std::string>> chunks;
    uint64_t total_bytes = 0;  // bytes ever produced (== next write offset)
    size_t buffered_bytes = 0; // bytes currently held in chunks
    bool stdout_eof = false;
    bool stderr_eof = false;

    bool exited = false;
    int exit_status = 0; // raw waitpid() status once exited

    std::chrono::steady_clock::time_point started_at;

    // Per-session cap on retained output. Excess is dropped from the
    // front; readers that fall behind see truncated=true.
    static constexpr size_t k_max_buffered_bytes = 256 * 1024;
  };

  // Result of process_manager::read().
  struct process_read_result
  {
    bool found = false;
    bool running = false;
    bool exited = false;
    int exit_code = 0; // decoded, valid when exited
    uint64_t total_bytes = 0;
    uint64_t next_offset = 0; // pass as `since` to get only new output
    bool truncated = false;   // `since` fell out of the ring buffer
    std::string output;
  };

  // One row of process_manager::list().
  struct process_info
  {
    std::string id;
    std::string name;
    std::string command;
    std::vector<std::string> args;
    pid_t pid = -1;
    bool running = false;
    bool exited = false;
    int exit_code = 0;
    uint64_t total_bytes = 0;
    long uptime_seconds = 0;
  };

  // Result of process_manager::start(): either an id + pid, or an
  // error message (C++20, so no std::expected).
  struct process_start_result
  {
    std::string id;
    pid_t pid = -1;
    std::string error;
    bool ok() const
    {
      return error.empty();
    }
  };

  // Owns the background process sessions started by the process_* tools.
  //
  // Deliberately NOT a singleton (same pattern as patchset_store): the
  // agent_system composition root owns one instance and hands it to the
  // process_tools constructor; tests construct their own. Not
  // thread-safe either: every method must run on the dripcore scheduler
  // thread. Tool coroutines run there, and kill_all() is registered on
  // agent_system's shutdown_signal, which fires on that thread too.
  // The destructor kills any surviving children as a backstop.
  class process_manager
  {
  public:
    process_manager() = default;
    ~process_manager();

    process_manager(const process_manager&) = delete;
    process_manager& operator=(const process_manager&) = delete;

    // Spawn a background process (stdin/stdout/stderr piped, own
    // process group) and start its pump task. `name`, when non-empty,
    // becomes the session id (must match [A-Za-z0-9._-]+ and be
    // unique); otherwise an id "p<N>" is generated.
    process_start_result start(
      const std::string& command,
      const std::vector<std::string>& args,
      const std::string& cwd,
      const std::map<std::string, std::string>& env_overrides,
      const std::string& name
    );

    // Parent-side stdin fd of a session (-1 when unknown or closed).
    int stdin_fd(const std::string& id) const;

    // Output produced since byte offset `since`, capped at max_bytes.
    process_read_result read(const std::string& id, uint64_t since, size_t max_bytes) const;

    std::vector<process_info> list() const;

    // Kill the whole process group and reap. The session stays listed
    // (exited=true) so its final output remains readable via read().
    bool kill(const std::string& id);

    // Kill every session without blocking. Registered on the shutdown
    // signal; also the destructor's backstop.
    void kill_all();

    process_session* find(const std::string& id);
    const process_session* find(const std::string& id) const;
    size_t session_count() const
    {
      return sessions_.size();
    }

    // Used by the pump task and by process_start's wait_for:
    void append_output(process_session& s, const char* data, size_t n);
    bool output_matches(const std::string& id, const std::regex& re) const;
    bool exited(const std::string& id, int& exit_code) const;

  private:
    std::map<std::string, std::unique_ptr<process_session>> sessions_;
    uint64_t next_auto_id_ = 1;
  };
} // namespace mistral::common

#endif // MISTRAL_COMMON_PROCESS_MANAGER_H
