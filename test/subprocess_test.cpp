#include "common/subprocess.h"
#include <doctest/doctest.h>

#include <cerrno>
#include <chrono>
#include <filesystem>
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
  bool reap_within(subprocess& sub, int* status)
  {
    for (int i = 0; i < 100; ++i)
    {
      if (sub.try_reap(status))
        return true;
      ::usleep(10000);
    }
    return false;
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

  int status = 0;
  REQUIRE(reap_within(sub, &status));
  CHECK(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
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

  int status = 0;
  REQUIRE(sub.wait(&status));
  CHECK(WIFSIGNALED(status));
  CHECK(WTERMSIG(status) == SIGKILL);
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
