#include "common/subprocess.h"
#include <doctest/doctest.h>

#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

using mistral::common::build_child_environment;
using mistral::common::spawn_status;
using mistral::common::subprocess;
using mistral::common::subprocess_options;

namespace
{
  // Reads whatever arrives on a non-blocking fd until the deadline
  // expires (the child may need a moment to produce output) or the
  // writer closes the pipe (EOF).
  std::string read_available(int fd, int timeout_ms)
  {
    std::string out;
    if (fd < 0)
      return out;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    char buf[4096];
    while (std::chrono::steady_clock::now() < deadline)
    {
      ssize_t n = ::read(fd, buf, sizeof(buf));
      if (n > 0)
      {
        out.append(buf, static_cast<size_t>(n));
        continue;
      }
      if (n == 0)
        break; // EOF: child closed the pipe
      if (errno == EAGAIN || errno == EWOULDBLOCK)
      {
        ::usleep(5000);
        continue;
      }
      break;
    }
    return out;
  }

  // Reaps the child, retrying for up to ~1s (the child may still be
  // running when we first check).
  std::optional<int> reap_within(subprocess& sub)
  {
    for (int i = 0; i < 100; ++i)
    {
      if (auto status = sub.try_reap())
      {
        return status;
      }
      ::usleep(10000);
    }
    return std::nullopt;
  }
} // namespace

TEST_CASE("build_child_environment inherits parent environment and applies overrides")
{
  auto env = build_child_environment({{"SUBPROCESS_TEST_VAR", "42"}, {"PATH", "/custom"}});
  bool found_override = false;
  bool found_path = false;
  for (const auto& entry : env)
  {
    if (entry == "SUBPROCESS_TEST_VAR=42")
      found_override = true;
    if (entry.starts_with("PATH="))
    {
      found_path = true;
      CHECK(entry == "PATH=/custom");
    }
  }
  CHECK(found_override);
  CHECK(found_path);
}

TEST_CASE("build_child_environment with inherit_env=false contains only overrides")
{
  auto env = build_child_environment({{"A", "1"}, {"B", "2"}}, false);
  REQUIRE(env.size() == 2);
  CHECK(env[0] == "A=1");
  CHECK(env[1] == "B=2");
}

TEST_CASE("spawn reports command_not_found for a missing executable")
{
  subprocess_options opts;
  opts.command = "definitely-not-a-real-command-42";
  subprocess sub;
  CHECK(sub.spawn(opts) == spawn_status::command_not_found);
  CHECK_FALSE(sub.running());
}

TEST_CASE("spawn runs a child and collects its output and exit status")
{
  subprocess_options opts;
  opts.command = "echo";
  opts.args = {"hello", "world"};
  subprocess sub;
  REQUIRE(sub.spawn(opts) == spawn_status::ok);
  REQUIRE(sub.running());
  CHECK(sub.stdin_fd() == -1); // stdin not piped by default
  CHECK(sub.stdout_fd() >= 0);
  CHECK(sub.stderr_fd() >= 0);

  std::string out = read_available(sub.stdout_fd(), 5000);
  CHECK(out == "hello world\n");

  auto status = reap_within(sub);
  REQUIRE(status.has_value());
  CHECK(WIFEXITED(*status));
  CHECK(WEXITSTATUS(*status) == 0);
  CHECK_FALSE(sub.running());
}

TEST_CASE("spawn pipes env overrides into the child")
{
  subprocess_options opts;
  opts.command = "sh";
  opts.args = {"-c", "printf %s \"$SUBPROCESS_TEST_VAR\""};
  opts.env_overrides = {{"SUBPROCESS_TEST_VAR", "from-test"}};
  subprocess sub;
  REQUIRE(sub.spawn(opts) == spawn_status::ok);

  std::string out = read_available(sub.stdout_fd(), 5000);
  CHECK(out == "from-test");
}

TEST_CASE("spawn pipes stdin to the child when requested")
{
  subprocess_options opts;
  opts.command = "cat";
  opts.pipe_stdin = true;
  opts.pipe_stderr = false;
  subprocess sub;
  REQUIRE(sub.spawn(opts) == spawn_status::ok);
  CHECK(sub.stdin_fd() >= 0);
  CHECK(sub.stderr_fd() == -1);

  std::string payload = "round trip\n";
  size_t written = 0;
  while (written < payload.size())
  {
    ssize_t n = ::write(sub.stdin_fd(), payload.data() + written, payload.size() - written);
    if (n > 0)
    {
      written += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    {
      ::usleep(5000);
      continue;
    }
    break;
  }
  sub.close_stdin(); // send EOF to the child

  std::string out = read_available(sub.stdout_fd(), 5000);
  CHECK(out == payload);
}

TEST_CASE("spawn respects the cwd option")
{
  // Use the current working directory as the target (rather than
  // /tmp, which is a symlink on macOS and would make pwd report the
  // resolved path).
  std::string target = std::filesystem::current_path().string();
  subprocess_options opts;
  opts.command = "sh";
  opts.args = {"-c", "pwd"};
  opts.cwd = target;
  subprocess sub;
  REQUIRE(sub.spawn(opts) == spawn_status::ok);

  std::string out = read_available(sub.stdout_fd(), 5000);
  CHECK(out == target + "\n");
}

TEST_CASE("kill terminates the child process group")
{
  subprocess_options opts;
  opts.command = "sleep";
  opts.args = {"30"};
  subprocess sub;
  REQUIRE(sub.spawn(opts) == spawn_status::ok);
  REQUIRE(sub.running());

  sub.kill();
  CHECK_FALSE(sub.running());

  auto status = sub.wait();
  REQUIRE(status.has_value());
  CHECK(WIFSIGNALED(*status));
  CHECK(WTERMSIG(*status) == SIGKILL);
}

TEST_CASE("destructor does not block on a running child")
{
  subprocess_options opts;
  opts.command = "sleep";
  opts.args = {"30"};
  auto start = std::chrono::steady_clock::now();
  {
    subprocess sub;
    REQUIRE(sub.spawn(opts) == spawn_status::ok);
    // Destroy without kill(): the destructor must reap with WNOHANG
    // and return immediately instead of waiting for the child.
  }
  auto elapsed = std::chrono::steady_clock::now() - start;
  CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() < 2000);
}

TEST_CASE("try_reap returns false while the child is still running")
{
  subprocess_options opts;
  opts.command = "sleep";
  opts.args = {"30"};
  subprocess sub;
  REQUIRE(sub.spawn(opts) == spawn_status::ok);

  CHECK_FALSE(sub.try_reap().has_value());
  CHECK(sub.running());
  sub.kill();
}

TEST_CASE("try_reap is idempotent once the child has been reaped")
{
  subprocess_options opts;
  opts.command = "true";
  subprocess sub;
  REQUIRE(sub.spawn(opts) == spawn_status::ok);

  auto status = reap_within(sub);
  REQUIRE(status.has_value());

  // A second attempt must keep reporting the child as gone with the
  // same status instead of falling through to waitpid() again.
  auto again = sub.try_reap();
  REQUIRE(again.has_value());
  CHECK(*again == *status);
}

TEST_CASE("kill is idempotent")
{
  subprocess_options opts;
  opts.command = "sleep";
  opts.args = {"30"};
  subprocess sub;
  REQUIRE(sub.spawn(opts) == spawn_status::ok);

  sub.kill();
  sub.kill(); // must not signal a recycled pid
  CHECK_FALSE(sub.running());

  auto status = sub.wait();
  REQUIRE(status.has_value());
  CHECK(WIFSIGNALED(*status));
}

TEST_CASE("spawn twice kills the first child")
{
  subprocess_options first_opts;
  first_opts.command = "sleep";
  first_opts.args = {"30"};

  subprocess sub;
  REQUIRE(sub.spawn(first_opts) == spawn_status::ok);
  pid_t first_pid = sub.pid();

  subprocess_options second_opts;
  second_opts.command = "true";
  REQUIRE(sub.spawn(second_opts) == spawn_status::ok);
  CHECK(sub.pid() != first_pid);

  auto status = reap_within(sub);
  REQUIRE(status.has_value());
  CHECK(WIFEXITED(*status));
  CHECK(WEXITSTATUS(*status) == 0);

  // The first child is gone: either spawn() already collected it
  // (ECHILD) or it is still collectable here as a SIGKILL victim.
  int first_status = 0;
  pid_t collected = ::waitpid(first_pid, &first_status, 0);
  if (collected == first_pid)
  {
    CHECK(WIFSIGNALED(first_status));
  }
  else
  {
    CHECK(errno == ECHILD);
  }
}

TEST_CASE("a failed spawn resets pid and spawn_errno")
{
  subprocess_options ok_opts;
  ok_opts.command = "true";

  subprocess sub;
  REQUIRE(sub.spawn(ok_opts) == spawn_status::ok);
  REQUIRE(sub.pid() > 0);

  subprocess_options bad_opts;
  bad_opts.command = "definitely-not-a-real-command-42";
  CHECK(sub.spawn(bad_opts) == spawn_status::command_not_found);
  CHECK(sub.pid() == -1);
  CHECK(sub.spawn_errno() == 0);
  CHECK_FALSE(sub.running());
}

TEST_CASE("close_stdin is a no-op when stdin is not piped")
{
  subprocess_options opts;
  opts.command = "cat";
  subprocess sub;
  REQUIRE(sub.spawn(opts) == spawn_status::ok);
  REQUIRE(sub.stdin_fd() == -1);

  sub.close_stdin(); // must not close an unrelated fd
  CHECK(sub.stdin_fd() == -1);
  CHECK(sub.stdout_fd() >= 0);
  sub.kill();
}

TEST_CASE("spawn does not leak file descriptors")
{
  // Counts the descriptors this process has open. The directory
  // iterator itself holds one, so the count is only comparable with
  // another count taken the same way.
  auto count_open_fds = [] {
    std::filesystem::path fd_dir = "/dev/fd";
    if (!std::filesystem::exists(fd_dir))
    {
      fd_dir = "/proc/self/fd";
    }
    int count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(fd_dir))
    {
      (void)entry;
      ++count;
    }
    return count;
  };

  subprocess_options opts;
  opts.command = "true";
  opts.pipe_stdin = true;

  int before = count_open_fds();
  for (int i = 0; i < 10; ++i)
  {
    subprocess sub;
    REQUIRE(sub.spawn(opts) == spawn_status::ok);
    sub.kill();
    sub.wait();
  }
  int after = count_open_fds();
  CHECK(after <= before + 2);
}

TEST_CASE("spawn reports exec_failed when execve fails")
{
  // An executable regular file that is not a valid binary: execve()
  // fails with ENOEXEC, and the child reports the errno through the
  // exec-status pipe instead of the parent guessing from exit code 127.
  auto path = std::filesystem::temp_directory_path() / "mistral-subprocess-not-a-binary";
  {
    std::ofstream out(path);
    out << "not a binary\n";
  }
  std::filesystem::permissions(path, std::filesystem::perms::owner_all);

  subprocess_options opts;
  opts.command = path.string();
  subprocess sub;
  CHECK(sub.spawn(opts) == spawn_status::exec_failed);
  CHECK(sub.spawn_errno() == ENOEXEC);
  CHECK(sub.pid() == -1);
  CHECK_FALSE(sub.running());

  std::filesystem::remove(path);
}
