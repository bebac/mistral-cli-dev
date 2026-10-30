#ifndef MISTRAL_COMMON_SUBPROCESS_H
#define MISTRAL_COMMON_SUBPROCESS_H

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <sys/types.h>

namespace mistral::common
{
  // Result of subprocess::spawn().
  enum class spawn_status
  {
    ok,
    command_not_found,
    pipe_failed,
    fork_failed,
    // The child was forked but execve() failed (bad interpreter,
    // ETXTBSY, wrong architecture, ...). spawn_errno() holds the errno
    // the child reported through the exec-status pipe.
    exec_failed
  };

  // Options for spawning a child process.
  struct subprocess_options
  {
    // Executable name (resolved via utils::find_executable) or path.
    std::string command;
    std::vector<std::string> args;

    // Environment entries applied over the inherited parent environment
    // (existing entries with the same key are replaced). Ignored when
    // inherit_env is false.
    std::map<std::string, std::string> env_overrides;
    bool inherit_env = true;

    // When pipe_stdin is true the parent gets a writable pipe to the
    // child's stdin; when false the child's stdin is redirected from
    // /dev/null (so it can never steal terminal input from the CLI).
    bool pipe_stdin = false;
    bool pipe_stdout = true;
    bool pipe_stderr = true;

    // Working directory for the child; empty = inherit.
    std::string cwd;
  };

  // Owns a spawned child process and its pipe file descriptors.
  //
  // The child is started in its own process group (setpgid on both
  // sides) so kill() takes down the whole child tree, including any
  // grandchildren. kill() and the destructor never block: the reap
  // uses waitpid(WNOHANG). A child that does not die immediately stays
  // a zombie until it is collected — init(8) only reaps it once this
  // process exits — so a long-running process that kills many children
  // must either call wait() (blocking, safe once SIGKILL is pending) or
  // keep calling kill()/try_reap() to collect them. wait() is the
  // blocking variant, only sensible after kill() or once try_reap() is
  // known to be futile.
  //
  // All parent-side pipe fds are opened non-blocking (for dripcore
  // io_awaiter compatibility) and with FD_CLOEXEC so later fork/exec
  // children do not inherit them. The child closes ALL inherited fds
  // above stderr before exec (kqueue fd, wakeup pipe, other children's
  // pipes) — otherwise pipes never see EOF and the event loop hangs.
  //
  // Callers that write to the stdin pipe must ignore SIGPIPE (every CLI
  // entry point does): writing to a child that has already exited
  // raises it, and the default action terminates the process.
  class subprocess
  {
  public:
    subprocess() = default;
    ~subprocess();

    subprocess(const subprocess&) = delete;
    subprocess& operator=(const subprocess&) = delete;
    subprocess(subprocess&&) = delete;
    subprocess& operator=(subprocess&&) = delete;

    // Spawns the child process, killing any previously spawned child
    // first. Returns spawn_status::ok on success. On failure no child
    // is left running and pid()/spawn_errno() are reset, so they never
    // report a previous spawn.
    spawn_status spawn(const subprocess_options& options);

    // True while the child has been spawned and has not been reaped
    // or killed yet.
    bool running() const
    {
      return pid_ > 0 && !reaped_ && !killed_;
    }

    pid_t pid() const
    {
      return pid_;
    }
    int stdin_fd() const
    {
      return stdin_fd_;
    }
    int stdout_fd() const
    {
      return stdout_fd_;
    }
    int stderr_fd() const
    {
      return stderr_fd_;
    }

    // errno of the failed spawn step — pipe creation, fork or exec
    // (0 otherwise).
    int spawn_errno() const
    {
      return spawn_errno_;
    }

    // Closes the parent's stdin pipe end, sending EOF to the child.
    // No-op when stdin is not piped.
    void close_stdin();

    // Non-blocking reap attempt. Returns the waitpid status once the
    // child is known to be gone, or nullopt while it is still running.
    // An already-reaped child returns its stored status immediately. A
    // child reaped elsewhere — or one whose SIGCHLD is ignored, so
    // waitpid reports ECHILD — also counts as gone, with an
    // unrecoverable status of 0. After a successful reap the destructor
    // no longer kills (the pid may have been recycled).
    std::optional<int> try_reap();

    // Blocking reap: waits until the child exits and returns the
    // waitpid status, or nullopt when there is nothing to reap. Retries
    // on EINTR — SIGINT is installed without SA_RESTART, so a signal
    // would otherwise report failure. Only call this when the child
    // cannot run for long (e.g. right after kill()); a live child would
    // freeze the caller.
    std::optional<int> wait();

    // Kill the entire process group (SIGKILL, falling back to the
    // direct child), reap with WNOHANG, and close all pipe fds.
    // Safe to call repeatedly: later calls skip the signal but retry
    // the non-blocking reap, so a child that was still dying on the
    // first call is collected on the next one.
    void kill();

  private:
    // Single non-blocking reap attempt; true when the child is known
    // to be gone (collected here, or already collected elsewhere).
    bool reap_wnohang();

    void close_fds();

    pid_t pid_ = -1;
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
    int stderr_fd_ = -1;
    int spawn_errno_ = 0;
    int status_ = 0;
    bool reaped_ = false;
    bool killed_ = false;
  };

  // Builds the child's "KEY=VALUE" environment strings: the inherited
  // parent environment (when inherit_env) with env_overrides applied
  // over it (existing entries replaced). Pure function, unit-testable.
  std::vector<std::string> build_child_environment(
    const std::map<std::string, std::string>& env_overrides, bool inherit_env = true
  );
} // namespace mistral::common

#endif // MISTRAL_COMMON_SUBPROCESS_H
