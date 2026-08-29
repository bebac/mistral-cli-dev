#include <doctest/doctest.h>
#include "tools/exec_tools.h"

#include <dripcore/scheduler.h>
#include <dripcore/task.h>

#include <fstream>
#include <filesystem>
#include <string>

namespace mistral::tools
{
  // Helper to run an async exec call from a test
  inline json run_async_exec(
    exec_tools& tools,
    std::string command,
    std::vector<std::string> args,
    std::string cwd = "",
    int timeout_seconds = 30)
  {
    json result;
    bool completed = false;

    // Task that performs the async exec and stores the result
    class exec_task : public dripcore::task
    {
    public:
      exec_task(exec_tools& tools, std::string cmd, std::vector<std::string> a,
                std::string cwd, int timeout,
                json& out_result, bool& out_completed)
        : tools_(tools), command_(std::move(cmd)), args_(std::move(a)),
          cwd_(std::move(cwd)), timeout_(timeout),
          out_result_(out_result), out_completed_(out_completed) {}

      dripcore::awaitable<> body() override
      {
        out_result_ = co_await tools_.exec(std::move(command_), std::move(args_), std::move(cwd_), timeout_);
        out_completed_ = true;
        co_return;
      }

    private:
      exec_tools& tools_;
      std::string command_;
      std::vector<std::string> args_;
      std::string cwd_;
      int timeout_;
      json& out_result_;
      bool& out_completed_;
    };

    dripcore::scheduler scheduler;
    scheduler.spawn<exec_task>(tools, std::move(command), std::move(args),
                                cwd, timeout_seconds, result, completed);
    scheduler.run();

    if (completed) {
      return result;
    }
    return {};
  }

} // namespace mistral::tools

TEST_CASE("exec_tools: basic fork + execvp with echo command")
{
  using namespace mistral::tools;

  exec_tools tools;

  // Test with echo command which should be available on all Unix systems
  auto result = run_async_exec(tools, "echo", {"hello", "world"});

  CHECK(result.contains("exit_code"));
  CHECK(result["exit_code"].get<int>() == 0);
  CHECK(result.contains("stdout"));
  CHECK(result["stdout"].get<std::string>() == "hello world\n");
  CHECK(result.contains("stderr"));
  CHECK(result["stderr"].get<std::string>() == "");
  CHECK(result.contains("command"));
  CHECK(result["command"].get<std::string>() == "echo");
  CHECK(result.contains("args"));
}

TEST_CASE("exec_tools: command not found returns error")
{
  using namespace mistral::tools;

  exec_tools tools;

  // Use a command that definitely doesn't exist
  auto result = run_async_exec(tools, "this_command_does_not_exist_xyz123", {});

  CHECK(result.contains("error"));
  CHECK(result["error"].get<std::string>().find("not found") != std::string::npos);
  CHECK(result.contains("exit_code"));
  CHECK(result["exit_code"].get<int>() == -1);
}

TEST_CASE("exec_tools: non-zero exit code is captured")
{
  using namespace mistral::tools;

  exec_tools tools;

  // Use false command which exits with code 1
  auto result = run_async_exec(tools, "false", {});

  CHECK(result.contains("exit_code"));
  CHECK(result["exit_code"].get<int>() == 1);
  CHECK(result.contains("stdout"));
  CHECK(result["stdout"].get<std::string>() == "");
}

TEST_CASE("exec_tools: stderr is captured separately from stdout")
{
  using namespace mistral::tools;

  exec_tools tools;

  // Use a command that writes to stderr
  // Note: Many standard commands don't write to stderr by default
  // We'll use a shell builtin via /bin/sh for this test
  auto result = run_async_exec(tools, "sh", {"-c", "echo 'error message' >&2"});

  CHECK(result.contains("exit_code"));
  CHECK(result["exit_code"].get<int>() == 0);
  CHECK(result.contains("stdout"));
  CHECK(result["stdout"].get<std::string>() == "");
  CHECK(result.contains("stderr"));
  CHECK(result["stderr"].get<std::string>() == "error message\n");
}

TEST_CASE("exec_tools: working directory change is respected")
{
  using namespace mistral::tools;

  exec_tools tools;

  // Create a temporary directory with a unique name
  std::string temp_dir = std::filesystem::temp_directory_path().string();
  std::string test_dir = temp_dir + "/exec_tools_test_dir_unique_12345";
  std::filesystem::create_directories(test_dir);

  // Run pwd from the test directory
  auto result = run_async_exec(tools, "pwd", {}, test_dir, 30);

  CHECK(result.contains("exit_code"));
  CHECK(result["exit_code"].get<int>() == 0);
  CHECK(result.contains("stdout"));
  // Check that the output contains our unique directory name
  CHECK(result["stdout"].get<std::string>().find("exec_tools_test_dir_unique_12345") != std::string::npos);

  // Clean up
  std::filesystem::remove_all(test_dir);
}

TEST_CASE("exec_tools: empty arguments list works")
{
  using namespace mistral::tools;

  exec_tools tools;

  // Test with no arguments
  auto result = run_async_exec(tools, "echo", {});

  CHECK(result.contains("exit_code"));
  CHECK(result["exit_code"].get<int>() == 0);
  CHECK(result.contains("stdout"));
  CHECK(result["stdout"].get<std::string>() == "\n");
  CHECK(result.contains("stderr"));
  CHECK(result["stderr"].get<std::string>() == "");
}

TEST_CASE("exec_tools: multiple arguments are passed correctly")
{
  using namespace mistral::tools;

  exec_tools tools;

  // Test with multiple arguments
  auto result = run_async_exec(tools, "echo", {"arg1", "arg2", "arg3"});

  CHECK(result.contains("exit_code"));
  CHECK(result["exit_code"].get<int>() == 0);
  CHECK(result.contains("stdout"));
  CHECK(result["stdout"].get<std::string>() == "arg1 arg2 arg3\n");
  CHECK(result.contains("args"));
  auto args = result["args"].get<std::vector<std::string>>();
  CHECK(args.size() == 3);
  CHECK(args[0] == "arg1");
  CHECK(args[1] == "arg2");
  CHECK(args[2] == "arg3");
}

TEST_CASE("exec_tools: timeout kills long-running command")
{
  using namespace mistral::tools;

  exec_tools tools;

  // Use sleep command which should be available on all Unix systems
  // Sleep for 10 seconds but timeout after 1 second
  auto result = run_async_exec(tools, "sleep", {"10"}, "", 1);

  CHECK(result.contains("error"));
  CHECK(result["error"].get<std::string>().find("timed out") != std::string::npos);
  CHECK(result.contains("exit_code"));
  CHECK(result["exit_code"].get<int>() == -1);
  CHECK(result.contains("timed_out"));
  CHECK(result["timed_out"].get<bool>() == true);
}

TEST_CASE("exec_tools: command completes within timeout")
{
  using namespace mistral::tools;

  exec_tools tools;

  // Sleep for 1 second with 5 second timeout - should complete normally
  auto result = run_async_exec(tools, "sleep", {"1"}, "", 5);

  CHECK(result.contains("exit_code"));
  CHECK(result["exit_code"].get<int>() == 0);
  CHECK(result.contains("timed_out"));
  CHECK(result["timed_out"].get<bool>() == false);
  CHECK(result.contains("error") == false);
}

TEST_CASE("exec_tools: timeout with output before timeout")
{
  using namespace mistral::tools;

  exec_tools tools;

  // Use a command that produces output and then sleeps
  // We'll use sh to echo and then sleep
  auto result = run_async_exec(tools, "sh", {"-c", "echo 'partial output'; sleep 10"}, "", 1);

  CHECK(result.contains("error"));
  CHECK(result["error"].get<std::string>().find("timed out") != std::string::npos);
  CHECK(result.contains("stdout"));
  CHECK(result["stdout"].get<std::string>().find("partial output") != std::string::npos);
  CHECK(result.contains("timed_out"));
  CHECK(result["timed_out"].get<bool>() == true);
}

TEST_CASE("exec_tools: default timeout is 30 seconds")
{
  using namespace mistral::tools;

  exec_tools tools;

  // Test that the default timeout parameter works
  // We can't easily test the default without modifying the function signature,
  // but we can at least verify the function accepts the default
  auto result = run_async_exec(tools, "echo", {"test"});

  CHECK(result.contains("exit_code"));
  CHECK(result["exit_code"].get<int>() == 0);
  CHECK(result.contains("timed_out"));
  CHECK(result["timed_out"].get<bool>() == false);
}