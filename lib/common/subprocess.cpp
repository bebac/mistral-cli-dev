#include "common/subprocess.h"

#include "common/executable_finder.h"

#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h> // SYS_close_range
#endif

// environ is not declared by <unistd.h> on all platforms (macOS).
extern char** environ;

namespace mistral::common
{
  namespace
  {
    // Lowest descriptor the child may inherit: 0/1/2 are its stdio.
    constexpr int k_first_child_fd = 3;

    // How long spawn() waits for the child to report the result of
    // execve(). Only reached when the child is stuck before exec'ing
    // (e.g. chdir() on a hung network mount).
    constexpr int k_exec_report_timeout_ms = 100;

    // Moves `fd` up to at least `min_fd` (F_DUPFD) so it cannot collide
    // with the child's stdio descriptors, closing the original. Returns
    // the new descriptor, or -1 on failure.
    int move_fd_above(int fd, int min_fd)
    {
      if (fd < 0 || fd >= min_fd)
      {
        return fd;
      }
      int moved = ::fcntl(fd, F_DUPFD, min_fd);
      ::close(fd);
      return moved;
    }

    // Owns both ends of a pipe and closes whichever ends are still open
    // on destruction. A default-constructed guard owns nothing; create()
    // populates it.
    class pipe_guard
    {
    public:
      pipe_guard() = default;
      ~pipe_guard()
      {
        close_read();
        close_write();
      }
      pipe_guard(const pipe_guard&) = delete;
      pipe_guard& operator=(const pipe_guard&) = delete;

      // Creates the pipe with both ends above the child's stdio
      // descriptors. Returns false on failure (errno preserved).
      bool create()
      {
        int fds[2];
        if (::pipe(fds) != 0)
        {
          return false;
        }
        read_fd_ = move_fd_above(fds[0], k_first_child_fd);
        write_fd_ = move_fd_above(fds[1], k_first_child_fd);
        return valid();
      }

      bool valid() const
      {
        return read_fd_ >= 0 && write_fd_ >= 0;
      }
      int read_fd() const
      {
        return read_fd_;
      }
      int write_fd() const
      {
        return write_fd_;
      }
      int release_read()
      {
        int fd = read_fd_;
        read_fd_ = -1;
        return fd;
      }
      int release_write()
      {
        int fd = write_fd_;
        write_fd_ = -1;
        return fd;
      }
      void close_read()
      {
        if (read_fd_ >= 0)
        {
          ::close(read_fd_);
          read_fd_ = -1;
        }
      }
      void close_write()
      {
        if (write_fd_ >= 0)
        {
          ::close(write_fd_);
          write_fd_ = -1;
        }
      }

    private:
      int read_fd_ = -1;
      int write_fd_ = -1;
    };

    // Sets FD_CLOEXEC on `fd`, preserving the other descriptor flags.
    // Failure is not fatal: the child closes every inherited descriptor
    // above stderr before exec anyway (see close_inherited_fds()).
    void set_cloexec(int fd)
    {
      if (fd < 0)
        return;
      int flags = ::fcntl(fd, F_GETFD);
      if (flags != -1)
      {
        ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
      }
    }

    // Mark a parent-side pipe fd non-blocking (dripcore io_awaiter
    // compatibility) and close-on-exec (so later fork/exec children do
    // not inherit it — prevents pipe-EOF deadlocks when the parent
    // spawns another child while this one is still running).
    //
    // F_SETFL replaces the whole status-flag set, so read the current
    // flags first instead of overwriting them.
    void set_cloexec_nonblock(int fd)
    {
      if (fd < 0)
        return;
      int status_flags = ::fcntl(fd, F_GETFL);
      if (status_flags != -1)
      {
        ::fcntl(fd, F_SETFL, status_flags | O_NONBLOCK);
      }
      set_cloexec(fd);
    }

    // Child-side failure reporting: write a fixed message to stderr,
    // report `error` through the exec-status pipe (so the parent can
    // tell an exec failure apart from a command that exited 127), and
    // exit. Only async-signal-safe calls (write/_exit) — the child must
    // not allocate or take locks after fork().
    [[noreturn]] void child_fail(const char* message, std::size_t length, int report_fd, int error)
    {
      ::write(STDERR_FILENO, message, length);
      if (report_fd >= 0)
      {
        ::write(report_fd, &error, sizeof(error));
      }
      ::_exit(127);
    }

    // Child-side stdio redirection. Exits the child if dup2 fails so it
    // never runs with the wrong streams. No report fd is available yet
    // at this point — the parent learns about it from the exit status
    // and the stderr message.
    void child_dup2(int new_fd, int target_fd)
    {
      if (::dup2(new_fd, target_fd) == -1)
      {
        child_fail("dup2 failed\n", sizeof("dup2 failed\n") - 1, -1, errno);
      }
    }

    // Portable fallback for close_inherited_fds(): one close() per
    // descriptor.
    void close_fd_range_loop(int first)
    {
      long max_fd = ::sysconf(_SC_OPEN_MAX);
      if (max_fd < 0)
      {
        max_fd = 1024;
      }
      for (long fd = first; fd < max_fd; ++fd)
      {
        ::close(static_cast<int>(fd));
      }
    }

    // Child-side: close every inherited descriptor above stderr except
    // `keep_fd` (the exec-status pipe).
    //
    // The child inherits the dripcore kqueue/epoll fd, the wakeup pipe
    // and any other child's stdio pipes. Without this the child holds
    // the parent's wakeup pipe write ends open — when the parent closes
    // its write ends during shutdown the pipes never see EOF, the
    // dripcore tasks block forever on readable(), and sched.run() never
    // returns (CTRL-C hang). FD_CLOEXEC is not enough: the kqueue fd on
    // macOS has no CLOEXEC variant.
    //
    // The bulk syscalls are preferred because the loop costs one
    // close() per descriptor and _SC_OPEN_MAX is routinely 1,048,576 on
    // Linux.
    void close_inherited_fds(int keep_fd)
    {
      // The gap below keep_fd is a handful of low-numbered pipe fds, so
      // a short loop is cheaper than any bulk syscall here.
      for (int fd = k_first_child_fd; fd < keep_fd; ++fd)
      {
        ::close(fd);
      }
      int first = (keep_fd >= k_first_child_fd) ? keep_fd + 1 : k_first_child_fd;
#if defined(__linux__) && defined(SYS_close_range)
      // Linux 5.9+: one syscall instead of one per descriptor.
      if (::syscall(SYS_close_range, static_cast<unsigned int>(first), ~0U, 0U) != 0)
      {
        close_fd_range_loop(first); // kernel older than 5.9 (ENOSYS)
      }
#else
      // macOS exposes neither closefrom() nor close_range() in its
      // public headers, but its _SC_OPEN_MAX is small (a few thousand),
      // so the loop is cheap there. BSDs could use closefrom() if they
      // ever become targets.
      close_fd_range_loop(first);
#endif
    }

    // Reads the exec result from the status pipe: 0 when the child
    // exec'd successfully (the close-on-exec write end closed, giving
    // EOF), the reported errno otherwise. Bounded by
    // k_exec_report_timeout_ms so a child stuck before execve() cannot
    // freeze spawn().
    int read_exec_status(int fd)
    {
      if (fd < 0)
      {
        return 0;
      }
      struct pollfd pfd{};
      pfd.fd = fd;
      pfd.events = POLLIN;
      if (::poll(&pfd, 1, k_exec_report_timeout_ms) <= 0)
      {
        return 0; // timeout or error: assume the child is exec'ing
      }
      int reported = 0;
      ssize_t n = ::read(fd, &reported, sizeof(reported));
      if (n == static_cast<ssize_t>(sizeof(reported)) && reported != 0)
      {
        return reported;
      }
      return 0;
    }
  } // namespace

  std::vector<std::string> build_child_environment(
    const std::map<std::string, std::string>& env_overrides, bool inherit_env
  )
  {
    std::vector<std::string> env_strings;
    if (inherit_env)
    {
      for (char** e = ::environ; e && *e; ++e)
      {
        env_strings.emplace_back(*e);
      }
    }
    for (const auto& [key, value] : env_overrides)
    {
      std::string entry = key + "=" + value;
      // Replace existing entry if present.
      bool replaced = false;
      for (auto& s : env_strings)
      {
        // Compare in place: building a temporary "key=" string for
        // every inherited entry would allocate per comparison.
        if (s.size() > key.size() && s.compare(0, key.size(), key) == 0 && s[key.size()] == '=')
        {
          s = entry;
          replaced = true;
          break;
        }
      }
      if (!replaced)
      {
        env_strings.push_back(std::move(entry));
      }
    }
    return env_strings;
  }

  subprocess::~subprocess()
  {
    kill();
  }

  spawn_status subprocess::spawn(const subprocess_options& options)
  {
    kill(); // reset any previously spawned child

    // A previous child that was killed but not reaped is abandoned
    // here (init(8) collects it once this process exits). Clearing the
    // state means a failed spawn never reports the previous child's
    // pid, errno or status.
    pid_ = -1;
    spawn_errno_ = 0;
    status_ = 0;
    reaped_ = false;
    killed_ = false;

    std::string exec_path = options.exec_path.empty() ? utils::find_executable(options.command) : options.exec_path;
    if (exec_path.empty())
    {
      return spawn_status::command_not_found;
    }

    // Pipes for the requested stdio streams only — creating all three
    // would burn six descriptors per spawn, four of which are closed
    // again immediately.
    pipe_guard stdin_pipe;       // parent writes -> child reads
    pipe_guard stdout_pipe;      // child writes  -> parent reads
    pipe_guard stderr_pipe;      // child writes  -> parent reads (drained)
    pipe_guard exec_status_pipe; // child reports execve() failure

    auto create_pipe = [&](bool requested, pipe_guard& pipe) {
      if (!requested)
      {
        return true;
      }
      if (pipe.create())
      {
        return true;
      }
      spawn_errno_ = errno;
      return false;
    };
    if (
      !create_pipe(options.pipe_stdin, stdin_pipe) || !create_pipe(options.pipe_stdout, stdout_pipe) ||
      !create_pipe(options.pipe_stderr, stderr_pipe)
    )
    {
      return spawn_status::pipe_failed;
    }
    if (!exec_status_pipe.create())
    {
      spawn_errno_ = errno;
      return spawn_status::pipe_failed;
    }
    // The write end must close when execve() succeeds; that is how the
    // parent learns the child got past exec.
    set_cloexec(exec_status_pipe.write_fd());

    // Build argv and envp. The storage must live across fork/exec, so
    // it is prepared here (before fork) rather than allocated in the
    // child after fork().
    // argv[0] is the command as the caller wrote it; exec_path may be a
    // different (absolute) path when the caller resolved it separately.
    std::vector<std::string> argv_storage;
    argv_storage.push_back(options.exec_path.empty() ? exec_path : options.command);
    for (const auto& arg : options.args)
    {
      argv_storage.push_back(arg);
    }
    std::vector<char*> argv;
    for (auto& a : argv_storage)
    {
      argv.push_back(a.data());
    }
    argv.push_back(nullptr);

    std::vector<std::string> env_strings = build_child_environment(options.env_overrides, options.inherit_env);
    std::vector<char*> envp;
    for (auto& s : env_strings)
    {
      envp.push_back(s.data());
    }
    envp.push_back(nullptr);

    pid_t pid = ::fork();
    if (pid == -1)
    {
      spawn_errno_ = errno;
      return spawn_status::fork_failed;
    }

    if (pid == 0)
    {
      // Child. Everything below runs between fork() and execve() in a
      // process that may have had other threads at fork time, so it
      // must stay async-signal-safe: no allocation, no locks, no
      // std::string work. The argv/envp buffers were built above for
      // exactly this reason.
      ::setpgid(0, 0);

      if (options.pipe_stdin)
      {
        child_dup2(stdin_pipe.read_fd(), STDIN_FILENO);
      }
      else
      {
        // Never let the child steal terminal input from the CLI.
        int devnull_fd = ::open("/dev/null", O_RDONLY);
        if (devnull_fd >= 0)
        {
          child_dup2(devnull_fd, STDIN_FILENO);
          ::close(devnull_fd);
        }
      }
      if (options.pipe_stdout)
      {
        child_dup2(stdout_pipe.write_fd(), STDOUT_FILENO);
      }
      if (options.pipe_stderr)
      {
        child_dup2(stderr_pipe.write_fd(), STDERR_FILENO);
      }

      // Close the parent-side pipe ends (they're dup'd now). Unrequested
      // pipes have fd -1, which ::close ignores.
      ::close(stdin_pipe.read_fd());
      ::close(stdin_pipe.write_fd());
      ::close(stdout_pipe.read_fd());
      ::close(stdout_pipe.write_fd());
      ::close(stderr_pipe.read_fd());
      ::close(stderr_pipe.write_fd());

      // Close every other inherited descriptor, keeping the exec-status
      // write end so the failures below can still be reported.
      int report_fd = exec_status_pipe.write_fd();
      close_inherited_fds(report_fd);

      if (!options.cwd.empty() && ::chdir(options.cwd.c_str()) != 0)
      {
        child_fail("chdir failed\n", sizeof("chdir failed\n") - 1, report_fd, errno);
      }

      ::execve(exec_path.c_str(), argv.data(), envp.data());
      // If we get here, exec failed.
      child_fail("execve failed\n", sizeof("execve failed\n") - 1, report_fd, errno);
    }

    // Parent: put the child in its own process group from this side
    // as well. After fork() it is undefined which side runs first, so
    // both sides must attempt setpgid — the whole child tree (npm ->
    // storybook -> esbuild, etc.) must be killable as a group.
    ::setpgid(pid, pid);

    // Close the child's end of the status pipe, then read the exec
    // result: EOF means execve() succeeded, a reported errno means the
    // child gave up before exec'ing.
    exec_status_pipe.close_write();
    int exec_errno = read_exec_status(exec_status_pipe.read_fd());
    if (exec_errno != 0)
    {
      // The child has already called _exit(); collect it so no zombie
      // is left behind and pid() does not report a dead child.
      int st = 0;
      while (::waitpid(pid, &st, 0) == -1 && errno == EINTR)
      {
      }
      spawn_errno_ = exec_errno;
      return spawn_status::exec_failed;
    }

    pid_ = pid; // the rest of the state was reset at the top

    // Take the parent-side pipe fds (pipe_guard closes the remaining
    // — child-side — ends on scope exit, so the parent never holds a
    // write end it does not need and stdout/stderr reads see EOF).
    if (options.pipe_stdin)
    {
      stdin_fd_ = stdin_pipe.release_write();
      set_cloexec_nonblock(stdin_fd_);
    }
    if (options.pipe_stdout)
    {
      stdout_fd_ = stdout_pipe.release_read();
      set_cloexec_nonblock(stdout_fd_);
    }
    if (options.pipe_stderr)
    {
      stderr_fd_ = stderr_pipe.release_read();
      set_cloexec_nonblock(stderr_fd_);
    }
    return spawn_status::ok;
  }

  void subprocess::close_stdin()
  {
    if (stdin_fd_ >= 0)
    {
      ::close(stdin_fd_);
      stdin_fd_ = -1;
    }
  }

  std::optional<int> subprocess::try_reap()
  {
    if (!reap_wnohang())
    {
      return std::nullopt; // still running, or interrupted (EINTR)
    }
    return status_;
  }

  bool subprocess::reap_wnohang()
  {
    if (pid_ <= 0 || reaped_)
    {
      return reaped_;
    }
    int st = 0;
    pid_t result = ::waitpid(pid_, &st, WNOHANG);
    if (result == pid_)
    {
      reaped_ = true;
      status_ = st;
      return true;
    }
    if (result == -1 && errno == ECHILD)
    {
      // Reaped by someone else (or SIGCHLD is ignored, so the kernel
      // auto-reaps): the status is unrecoverable, but the child is
      // definitively gone. Mark it reaped so pid_ is never touched
      // again — it may already have been recycled.
      reaped_ = true;
      return true;
    }
    return false;
  }

  std::optional<int> subprocess::wait()
  {
    if (pid_ <= 0)
    {
      return std::nullopt;
    }
    if (!reaped_)
    {
      int st = 0;
      while (true)
      {
        pid_t result = ::waitpid(pid_, &st, 0);
        if (result == pid_)
        {
          reaped_ = true;
          status_ = st;
          break;
        }
        if (result == -1 && errno == EINTR)
        {
          // Interrupted by a signal handler (SIGINT is installed
          // without SA_RESTART): retry rather than reporting failure.
          continue;
        }
        if (result == -1 && errno == ECHILD)
        {
          reaped_ = true; // gone, status unrecoverable
        }
        break;
      }
    }
    if (!reaped_)
    {
      return std::nullopt;
    }
    return status_;
  }

  void subprocess::kill()
  {
    if (pid_ > 0 && !reaped_ && !killed_)
    {
      // Kill the entire process group (the child called setpgid(0,0)
      // so it leads its own group — this also kills any grandchildren
      // the child may have spawned). Fall back to the direct child if
      // the group kill fails.
      if (::kill(-pid_, SIGKILL) == -1)
      {
        ::kill(pid_, SIGKILL);
      }
    }
    killed_ = true;

    // Reap without blocking. kill() runs in two contexts:
    //   1. error paths in dripcore coroutines — a blocking waitpid()
    //      here would freeze the entire event loop (TUI hang);
    //   2. the destructor during process exit — a blocking
    //      waitpid() here would hang the process exit if the child
    //      is stuck (e.g. D-state, slow shutdown).
    // A child that is not dead yet stays a zombie until someone
    // collects it (init(8) only reaps it once this process exits), so
    // every call retries the reap — not just the first one.
    reap_wnohang();

    close_fds();
  }

  void subprocess::close_fds()
  {
    if (stdin_fd_ >= 0)
    {
      ::close(stdin_fd_);
      stdin_fd_ = -1;
    }
    if (stdout_fd_ >= 0)
    {
      ::close(stdout_fd_);
      stdout_fd_ = -1;
    }
    if (stderr_fd_ >= 0)
    {
      ::close(stderr_fd_);
      stderr_fd_ = -1;
    }
  }
} // namespace mistral::common
