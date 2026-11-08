#include "common/process_manager.h"
#include "tools/process_tools.h"

#include <doctest/doctest.h>

#include <dripcore/executor.h>
#include <dripcore/scheduler.h>
#include <dripcore/sleep_awaiter.h>
#include <dripcore/task.h>

#include <chrono>
#include <csignal>
#include <functional>
#include <string>
#include <vector>

namespace mistral::tools
{
  using mistral::common::process_manager;

  // Runs a whole scenario (several co_awaited tool calls) as one task
  // on a fresh scheduler, the way the agent runs tool coroutines.
  // Every scenario must leave no live session behind, or scheduler.run()
  // would never return (the pump task loops until the child exits).
  class scenario_task : public dripcore::task
  {
  public:
    scenario_task(process_tools& tools, std::function<dripcore::awaitable<void>(process_tools&)> fn)
      : tools_(tools), fn_(std::move(fn))
    {
    }

    dripcore::awaitable<> body() override
    {
      co_await fn_(tools_);
      co_return;
    }

  private:
    process_tools& tools_;
    std::function<dripcore::awaitable<void>(process_tools&)> fn_;
  };

  inline void run_scenario(process_tools& tools, std::function<dripcore::awaitable<void>(process_tools&)> fn)
  {
    dripcore::scheduler scheduler;
    scheduler.spawn<scenario_task>(tools, std::move(fn));
    scheduler.run();
  }

  // Reads until the accumulated output contains `needle`, the process
  // exits, or the timeout passes. Returns the last process_read result.
  inline dripcore::awaitable<json> wait_read(
    process_tools& tools,
    const std::string& id,
    const std::string& needle,
    int timeout_ms = 5000
  )
  {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true)
    {
      json r = tools.process_read(id, 0, 1024 * 1024);
      if (r.contains("output") && r["output"].get<std::string>().find(needle) != std::string::npos)
      {
        co_return r;
      }
      if (r.value("exited", false))
      {
        co_return r;
      }
      if (std::chrono::steady_clock::now() >= deadline)
      {
        co_return r;
      }
      co_await dripcore::sleep_awaiter{dripcore::executor::current(), nullptr, std::chrono::milliseconds(50)};
    }
  }
} // namespace mistral::tools

TEST_CASE("process_tools: start, write, read echo through cat, then kill")
{
  using namespace mistral::tools;
  ::signal(SIGPIPE, SIG_IGN);

  process_manager mgr;
  process_tools tools(mgr);

  json started;
  json echoed;
  json killed;
  json listed;

  run_scenario(tools, [&](process_tools& t) -> dripcore::awaitable<void> {
    started = co_await t.process_start("cat", {}, "", {}, "", "", 0);
    CHECK(started.contains("id"));
    CHECK(started.value("running", false) == true);
    std::string id = started["id"].get<std::string>();

    json w = co_await t.process_write(id, "hello process\n");
    CHECK(w["written"].get<int64_t>() == 14);

    echoed = co_await wait_read(t, id, "hello process");
    CHECK(echoed["output"].get<std::string>().find("hello process") != std::string::npos);
    CHECK(echoed["next_offset"].get<uint64_t>() > 0);
    CHECK(echoed.value("running", false) == true);

    killed = t.process_kill(id);
    CHECK(killed.value("killed", false) == true);
    CHECK(killed["exit_code"].get<int>() == 128 + SIGKILL);

    listed = t.process_list();
    co_return;
  });

  CHECK(started.contains("pid"));
  CHECK(started["pid"].get<int>() > 0);
  CHECK(killed.contains("exit_code"));
  CHECK(listed["count"].get<int>() == 1);
  CHECK(listed["sessions"][0]["exited"].get<bool>() == true);
  CHECK(listed["sessions"][0]["running"].get<bool>() == false);
  mgr.kill_all();
}

TEST_CASE("process_tools: read offsets deliver only new output")
{
  using namespace mistral::tools;
  ::signal(SIGPIPE, SIG_IGN);

  process_manager mgr;
  process_tools tools(mgr);

  json first, second, third;
  run_scenario(tools, [&](process_tools& t) -> dripcore::awaitable<void> {
    json started = co_await t.process_start("cat", {}, "", {}, "", "", 0);
    std::string id = started["id"].get<std::string>();

    co_await t.process_write(id, "abc\n");
    first = co_await wait_read(t, id, "abc");
    int64_t next = first["next_offset"].get<int64_t>();

    // Nothing new since next_offset.
    second = t.process_read(id, next, 65536);
    CHECK(second["output"].get<std::string>().empty());

    co_await t.process_write(id, "def\n");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (true)
    {
      third = t.process_read(id, next, 65536);
      if (third["output"].get<std::string>().find("def") != std::string::npos)
      {
        break;
      }
      if (std::chrono::steady_clock::now() >= deadline)
      {
        break;
      }
      co_await dripcore::sleep_awaiter{dripcore::executor::current(), nullptr, std::chrono::milliseconds(50)};
    }
    CHECK(third["output"].get<std::string>().find("def") != std::string::npos);
    CHECK(third["output"].get<std::string>().find("abc") == std::string::npos);

    t.process_kill(id);
    co_return;
  });

  CHECK(first.value("truncated", true) == false);
  CHECK(third["next_offset"].get<uint64_t>() > first["next_offset"].get<uint64_t>());
  mgr.kill_all();
}

TEST_CASE("process_tools: wait_for blocks until the pattern appears")
{
  using namespace mistral::tools;
  process_manager mgr;
  process_tools tools(mgr);

  json started;
  run_scenario(tools, [&](process_tools& t) -> dripcore::awaitable<void> {
    started = co_await t.process_start("sh", {"-c", "sleep 0.3; echo READY_MARKER_123"}, "", {}, "", "READY_MARKER_123", 10);
    co_return;
  });

  CHECK(started.value("ready", false) == true);
  CHECK(started.contains("id"));
  mgr.kill_all();
}

TEST_CASE("process_tools: wait_for times out and reports ready=false")
{
  using namespace mistral::tools;
  process_manager mgr;
  process_tools tools(mgr);

  json started;
  run_scenario(tools, [&](process_tools& t) -> dripcore::awaitable<void> {
    started = co_await t.process_start("sh", {"-c", "sleep 30"}, "", {}, "", "NEVER_APPEARS", 1);
    std::string id = started["id"].get<std::string>();
    CHECK(started.value("ready", true) == false);
    CHECK(started.value("running", false) == true);
    t.process_kill(id);
    co_return;
  });

  CHECK(started.contains("id"));
  mgr.kill_all();
}

TEST_CASE("process_tools: env overrides reach the child")
{
  using namespace mistral::tools;
  process_manager mgr;
  process_tools tools(mgr);

  json out;
  run_scenario(tools, [&](process_tools& t) -> dripcore::awaitable<void> {
    json started = co_await t.process_start("sh", {"-c", "echo \"VAL=$MY_TEST_VAR\""}, "", {"MY_TEST_VAR=bar42"}, "", "", 0);
    std::string id = started["id"].get<std::string>();
    out = co_await wait_read(t, id, "VAL=bar42");
    co_return; // the sh exits by itself after echo
  });

  CHECK(out["output"].get<std::string>().find("VAL=bar42") != std::string::npos);
  mgr.kill_all();
}

TEST_CASE("process_tools: process exit is detected with its exit code")
{
  using namespace mistral::tools;
  process_manager mgr;
  process_tools tools(mgr);

  json out;
  run_scenario(tools, [&](process_tools& t) -> dripcore::awaitable<void> {
    json started = co_await t.process_start("sh", {"-c", "echo done; exit 3"}, "", {}, "", "", 0);
    std::string id = started["id"].get<std::string>();
    // Poll until the pump task has reaped the child.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (true)
    {
      out = t.process_read(id, 0, 65536);
      if (out.value("exited", false))
      {
        break;
      }
      if (std::chrono::steady_clock::now() >= deadline)
      {
        break;
      }
      co_await dripcore::sleep_awaiter{dripcore::executor::current(), nullptr, std::chrono::milliseconds(50)};
    }
    co_return;
  });

  CHECK(out.value("exited", false) == true);
  CHECK(out["exit_code"].get<int>() == 3);
  CHECK(out["output"].get<std::string>().find("done") != std::string::npos);
  mgr.kill_all();
}

TEST_CASE("process_tools: ring buffer truncates old output")
{
  using namespace mistral::tools;
  process_manager mgr;
  process_tools tools(mgr);

  json out;
  run_scenario(tools, [&](process_tools& t) -> dripcore::awaitable<void> {
    // ~300 KiB of output, more than the 256 KiB ring buffer keeps.
    json started = co_await t.process_start("sh", {"-c", "head -c 300000 /dev/zero | tr '\\0' x"}, "", {}, "", "", 0);
    std::string id = started["id"].get<std::string>();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (true)
    {
      out = t.process_read(id, 0, 1024 * 1024);
      if (out.value("exited", false))
      {
        break;
      }
      if (std::chrono::steady_clock::now() >= deadline)
      {
        break;
      }
      co_await dripcore::sleep_awaiter{dripcore::executor::current(), nullptr, std::chrono::milliseconds(50)};
    }
    co_return;
  });

  CHECK(out.value("exited", false) == true);
  CHECK(out["total_bytes"].get<uint64_t>() >= 300000);
  CHECK(out.value("truncated", false) == true);
  // Retained output is bounded by the ring buffer cap (plus one chunk).
  CHECK(out["output"].get<std::string>().size() <= 256 * 1024 + 4096);
  mgr.kill_all();
}

TEST_CASE("process_tools: named session ids and duplicate/invalid names")
{
  using namespace mistral::tools;
  ::signal(SIGPIPE, SIG_IGN);

  process_manager mgr;
  process_tools tools(mgr);

  json first, dup, bad_name, bad_env;
  run_scenario(tools, [&](process_tools& t) -> dripcore::awaitable<void> {
    first = co_await t.process_start("cat", {}, "", {}, "my.session-1", "", 0);
    CHECK(first["id"].get<std::string>() == "my.session-1");

    dup = co_await t.process_start("cat", {}, "", {}, "my.session-1", "", 0);
    CHECK(dup.contains("error"));

    bad_name = co_await t.process_start("cat", {}, "", {}, "bad name!", "", 0);
    CHECK(bad_name.contains("error"));

    bad_env = co_await t.process_start("cat", {}, "", {"NO_EQUALS_SIGN"}, "", "", 0);
    CHECK(bad_env.contains("error"));

    t.process_kill(first["id"].get<std::string>());
    co_return;
  });

  CHECK(dup["error"].get<std::string>().find("already exists") != std::string::npos);
  CHECK(bad_name["error"].get<std::string>().find("invalid session name") != std::string::npos);
  CHECK(bad_env["error"].get<std::string>().find("KEY=VALUE") != std::string::npos);
  mgr.kill_all();
}

TEST_CASE("process_tools: unknown session and unknown command errors")
{
  using namespace mistral::tools;
  ::signal(SIGPIPE, SIG_IGN);

  process_manager mgr;
  process_tools tools(mgr);

  json r_read, r_write, r_kill, r_start;
  run_scenario(tools, [&](process_tools& t) -> dripcore::awaitable<void> {
    r_read = t.process_read("nope", 0, 1024);
    r_write = co_await t.process_write("nope", "x");
    r_kill = t.process_kill("nope");
    r_start = co_await t.process_start("this_command_does_not_exist_xyz123", {}, "", {}, "", "", 0);
    co_return;
  });

  CHECK(r_read["error"].get<std::string>().find("unknown session") != std::string::npos);
  CHECK(r_write.contains("error"));
  CHECK(r_kill.contains("error"));
  CHECK(r_start["error"].get<std::string>().find("not found") != std::string::npos);
  mgr.kill_all();
}

TEST_CASE("process_tools: write to an exited process fails cleanly")
{
  using namespace mistral::tools;
  ::signal(SIGPIPE, SIG_IGN);

  process_manager mgr;
  process_tools tools(mgr);

  json w;
  run_scenario(tools, [&](process_tools& t) -> dripcore::awaitable<void> {
    json started = co_await t.process_start("sh", {"-c", "exit 0"}, "", {}, "", "", 0);
    std::string id = started["id"].get<std::string>();
    // Wait until the pump has reaped the child.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (true)
    {
      json r = t.process_read(id, 0, 1024);
      if (r.value("exited", false))
      {
        break;
      }
      if (std::chrono::steady_clock::now() >= deadline)
      {
        break;
      }
      co_await dripcore::sleep_awaiter{dripcore::executor::current(), nullptr, std::chrono::milliseconds(50)};
    }
    w = co_await t.process_write(id, "too late\n");
    co_return;
  });

  CHECK(w.contains("error"));
  CHECK(w["error"].get<std::string>().find("exited") != std::string::npos);
  mgr.kill_all();
}

TEST_CASE("process_tools: parse_request accepts command/args/cwd and rejects malformed")
{
  using namespace mistral::tools;

  auto ok = process_tools::parse_request(json{{"command", "npm"}, {"args", json::array({"run", "dev"})}, {"cwd", "/tmp"}});
  REQUIRE(ok.has_value());
  CHECK(ok->command == "npm");
  CHECK(ok->args == std::vector<std::string>({"run", "dev"}));
  CHECK(ok->cwd == "/tmp");

  CHECK(!process_tools::parse_request(json::object()).has_value());
  CHECK(!process_tools::parse_request(json{{"command", 42}}).has_value());
  CHECK(!process_tools::parse_request(json{{"command", "x"}, {"args", "not-an-array"}}).has_value());
  CHECK(!process_tools::parse_request(json{{"command", "x"}, {"cwd", 7}}).has_value());
}
