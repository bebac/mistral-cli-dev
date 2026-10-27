#ifndef MISTRAL_COMMON_SUBPROCESS_H
#define MISTRAL_COMMON_SUBPROCESS_H

#include <map>
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
    fork_failed
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
  // uses waitpid(WNOHANG) — a child that does not die immediately is
  // left to init(8) rather than freezing the event loop or process
  // exit. wait() is the blocking variant, only sensible after kill()
  // (SIGKILL pending) or once try_reap() is known to be futile.
  //
  // All parent-side pipe fds are opened non-blocking (for dripcore
  // io_awaiter compatibility) and with FD_CLOEXEC so later fork/exec
  // children do not inherit them. The child closes ALL inherited fds
  // above stderr before exec (kqueue fd, wakeup pipe, other children's
  // pipes) — otherwise pipes never see EOF and the event loop hangs.
  class subprocess
  {
  public:
    subprocess() = default;
    ~subprocess();

    subprocess(const subprocess&) = delete;
    subprocess& operator=(const subprocess&) = delete;
    subprocess(subprocess&&) = delete;
    subprocess& operator=(subprocess&&) = delete;

    // Spawns the child process. Returns spawn_status::ok on success;
    // on failure no child is left running.
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

    // errno of the failed fork() call (0 otherwise).
    int spawn_errno() const
    {
      return spawn_errno_;
    }

    // Closes the parent's stdin pipe end, sending EOF to the child.
    // No-op when stdin is not piped.
    void close_stdin();

    // Non-blocking reap attempt. Returns true when the child has
    // exited and stores the waitpid status; after a true return the
    // destructor no longer kills (the pid may have been recycled).
    bool try_reap(int* status);

    // Blocking reap: waits until the child exits and stores the
    // waitpid status. Only call this when the child cannot run for
    // long (e.g. right after kill()); a live child would freeze the
    // caller.
    bool wait(int* status);

    // Kill the entire process group (SIGKILL, falling back to the
    // direct child), reap with WNOHANG, and close all pipe fds.
    void kill();

  private:
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
