#include "common/subprocess.h"

#include "common/executable_finder.h"

#include <cerrno>
#include <memory>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

// environ is not declared by <unistd.h> on all platforms (macOS).
extern char** environ;

namespace mistral::common
{
  namespace
  {
    // RAII wrapper for a file descriptor.
    class fd_guard
    {
    public:
      explicit fd_guard(int fd = -1) : fd_(fd)
      {
      }
      ~fd_guard()
      {
        if (fd_ >= 0)
          ::close(fd_);
      }
      fd_guard(const fd_guard&) = delete;
      fd_guard& operator=(const fd_guard&) = delete;
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

    // RAII wrapper for a pipe (two fds).
    class pipe_guard
    {
    public:
      pipe_guard()
      {
        int fds[2];
        if (::pipe(fds) == 0)
        {
          read_fd_ = std::make_unique<fd_guard>(fds[0]);
          write_fd_ = std::make_unique<fd_guard>(fds[1]);
        }
      }
      int read_fd() const
      {
        return read_fd_ ? read_fd_->get() : -1;
      }
      int write_fd() const
      {
        return write_fd_ ? write_fd_->get() : -1;
      }
      bool valid() const
      {
        return read_fd() >= 0 && write_fd() >= 0;
      }
      int release_read()
      {
        return read_fd_ ? read_fd_->release() : -1;
      }
      int release_write()
      {
        return write_fd_ ? write_fd_->release() : -1;
      }

    private:
      std::unique_ptr<fd_guard> read_fd_;
      std::unique_ptr<fd_guard> write_fd_;
    };

    // Mark a parent-side pipe fd non-blocking (dripcore io_awaiter
    // compatibility) and close-on-exec (so later fork/exec children do
    // not inherit it — prevents pipe-EOF deadlocks when the parent
    // spawns another child while this one is still running).
    void set_cloexec_nonblock(int fd)
    {
      if (fd < 0)
        return;
      ::fcntl(fd, F_SETFL, O_NONBLOCK);
      ::fcntl(fd, F_SETFD, FD_CLOEXEC);
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
        if (s.starts_with(key + "="))
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

    std::string exec_path = utils::find_executable(options.command);
    if (exec_path.empty())
    {
      return spawn_status::command_not_found;
    }

    // Pipes for the requested stdio streams.
    pipe_guard stdin_pipe;  // parent writes -> child reads
    pipe_guard stdout_pipe; // child writes  -> parent reads
    pipe_guard stderr_pipe; // child writes  -> parent reads (drained)
    if (
      (options.pipe_stdin && !stdin_pipe.valid()) || (options.pipe_stdout && !stdout_pipe.valid()) ||
      (options.pipe_stderr && !stderr_pipe.valid())
    )
    {
      return spawn_status::pipe_failed;
    }

    // Build argv and envp. The storage must live across fork/exec, so
    // it is prepared here (before fork) rather than allocated in the
    // child after fork().
    std::vector<std::string> argv_storage;
    argv_storage.push_back(exec_path);
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
      // Child: wire up the requested stdio streams and exec.
      ::setpgid(0, 0);

      if (options.pipe_stdin)
      {
        ::dup2(stdin_pipe.read_fd(), STDIN_FILENO);
      }
      else
      {
        // Never let the child steal terminal input from the CLI.
        int devnull_fd = ::open("/dev/null", O_RDONLY);
        if (devnull_fd >= 0)
        {
          ::dup2(devnull_fd, STDIN_FILENO);
          ::close(devnull_fd);
        }
      }
      if (options.pipe_stdout)
      {
        ::dup2(stdout_pipe.write_fd(), STDOUT_FILENO);
      }
      if (options.pipe_stderr)
      {
        ::dup2(stderr_pipe.write_fd(), STDERR_FILENO);
      }

      // Close all pipe fds (they're dup'd now). Unrequested pipes
      // have fd -1, which ::close ignores.
      ::close(stdin_pipe.read_fd());
      ::close(stdin_pipe.write_fd());
      ::close(stdout_pipe.read_fd());
      ::close(stdout_pipe.write_fd());
      ::close(stderr_pipe.read_fd());
      ::close(stderr_pipe.write_fd());

      // Close ALL other inherited file descriptors (kqueue fd from
      // the dripcore event loop, wakeup pipe fds, other children's
      // stdio pipe fds, etc.). Without this, the child holds the
      // parent's wakeup pipe write ends open — when the parent closes
      // its write ends during shutdown, the pipes never see EOF, the
      // dripcore tasks block forever on readable(), and sched.run()
      // never returns (CTRL-C hang). Note: wakeup_pipe now sets
      // FD_CLOEXEC, but the kqueue fd (macOS) has no CLOEXEC variant,
      // so we close everything here.
      long max_fd = ::sysconf(_SC_OPEN_MAX);
      if (max_fd < 0)
        max_fd = 1024;
      for (long fd = 3; fd < max_fd; ++fd)
      {
        ::close(static_cast<int>(fd));
      }

      if (!options.cwd.empty() && ::chdir(options.cwd.c_str()) != 0)
      {
        ::write(STDERR_FILENO, "chdir failed\n", 13);
        ::_exit(127);
      }

      ::execve(exec_path.c_str(), argv.data(), envp.data());
      // If we get here, exec failed.
      ::write(STDERR_FILENO, "execve failed\n", 14);
      ::_exit(127);
    }

    // Parent: put the child in its own process group from this side
    // as well. After fork() it is undefined which side runs first, so
    // both sides must attempt setpgid — the whole child tree (npm ->
    // storybook -> esbuild, etc.) must be killable as a group.
    ::setpgid(pid, pid);

    pid_ = pid;
    spawn_errno_ = 0;
    status_ = 0;
    reaped_ = false;
    killed_ = false;

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

  bool subprocess::try_reap(int* status)
  {
    if (pid_ <= 0 || reaped_)
    {
      return false;
    }
    int st = 0;
    if (::waitpid(pid_, &st, WNOHANG) != pid_)
    {
      return false; // still running (or transient waitpid error)
    }
    reaped_ = true;
    status_ = st;
    if (status)
    {
      *status = st;
    }
    return true;
  }

  bool subprocess::wait(int* status)
  {
    if (pid_ <= 0)
    {
      return false;
    }
    if (!reaped_)
    {
      int st = 0;
      if (::waitpid(pid_, &st, 0) == pid_)
      {
        reaped_ = true;
        status_ = st;
      }
    }
    if (status)
    {
      *status = status_;
    }
    return reaped_;
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
      // Reap without blocking. kill() runs in two contexts:
      //   1. error paths in dripcore coroutines — a blocking waitpid()
      //      here would freeze the entire event loop (TUI hang);
      //   2. the destructor during process exit — a blocking
      //      waitpid() here would hang the process exit if the child
      //      is stuck (e.g. D-state, slow shutdown).
      // The child will be reaped by init(8) when it exits if we don't
      // reap it here; a zombie is harmless.
      int st = 0;
      if (::waitpid(pid_, &st, WNOHANG) == pid_)
      {
        reaped_ = true;
        status_ = st;
      }
    }
    killed_ = true;
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
