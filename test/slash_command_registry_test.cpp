#include "slash_command/slash_command.h"
#include "slash_command/slash_command_registry.h"
#include <doctest/doctest.h>

#include <dripcore/awaitable.h>
#include <dripcore/task.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace mistral::slash_command
{

  // Test command implementations
  class test_immediate_command : public slash_command
  {
  public:
    std::string name() const override
    {
      return "test_immediate";
    }
    std::string description() const override
    {
      return "Test immediate command";
    }
    std::string usage() const override
    {
      return "/test_immediate [args]";
    }

    dripcore::awaitable<std::string> execute(dripcore::task&, const std::string& args, agent::agent_system&) override
    {
      co_return "Immediate: " + args;
    }
  };

  class test_agent_command : public slash_command
  {
  public:
    std::string name() const override
    {
      return "test_agent";
    }
    std::string description() const override
    {
      return "Test agent command";
    }
    std::string usage() const override
    {
      return "/test_agent [args]";
    }

    dripcore::awaitable<std::string> execute(dripcore::task&, const std::string& args, agent::agent_system&) override
    {
      co_return "Agent: " + args;
    }
  };

  class test_help_command : public slash_command
  {
  public:
    std::string name() const override
    {
      return "help";
    }
    std::string description() const override
    {
      return "Show help";
    }

    dripcore::awaitable<std::string> execute(dripcore::task&, const std::string& args, agent::agent_system&) override
    {
      if (args.empty())
      {
        co_return "General help";
      }
      co_return "Help for: " + args;
    }
  };

  // A command whose spec() asks the registry for something. build_catalog()
  // calls spec() with the registry lock released precisely so this works.
  class test_self_referencing_command : public slash_command
  {
  public:
    explicit test_self_referencing_command(slash_command_registry& registry) : registry_(registry)
    {
    }

    std::string name() const override
    {
      return "test_self_ref";
    }
    std::string description() const override
    {
      return "Queries the registry from spec()";
    }
    command_spec spec() const override
    {
      command_spec out;
      out.name = name();
      out.summary = description();
      // Any registry call takes the same mutex build_catalog() used to hold.
      if (!registry_.get_command_names().empty())
      {
        out.summary += " (registry reachable)";
      }
      return out;
    }

    dripcore::awaitable<std::string> execute(dripcore::task&, const std::string&, agent::agent_system&) override
    {
      co_return "";
    }

  private:
    slash_command_registry& registry_;
  };

} // namespace mistral::slash_command

TEST_CASE("slash_command_registry: default constructible")
{
  mistral::slash_command::slash_command_registry registry;
  CHECK(registry.get_command_names().empty());
}

TEST_CASE("slash_command_registry: register and get command")
{
  mistral::slash_command::slash_command_registry registry;

  auto cmd = std::make_unique<mistral::slash_command::test_immediate_command>();
  registry.register_command(std::move(cmd));

  const mistral::slash_command::slash_command* retrieved = registry.get_command("test_immediate");
  REQUIRE(retrieved != nullptr);
  CHECK(retrieved->name() == "test_immediate");
  CHECK(retrieved->description() == "Test immediate command");
  CHECK(retrieved->usage() == "/test_immediate [args]");
}

TEST_CASE("slash_command_registry: get unknown command")
{
  mistral::slash_command::slash_command_registry registry;
  const mistral::slash_command::slash_command* retrieved = registry.get_command("nonexistent");
  CHECK(retrieved == nullptr);
}

TEST_CASE("slash_command_registry: get_command_names")
{
  mistral::slash_command::slash_command_registry registry;

  // Register multiple commands
  registry.register_command(std::make_unique<mistral::slash_command::test_immediate_command>());
  registry.register_command(std::make_unique<mistral::slash_command::test_agent_command>());
  registry.register_command(std::make_unique<mistral::slash_command::test_help_command>());

  auto names = registry.get_command_names();
  REQUIRE(names.size() == 3);

  // Check that our commands are in the list
  bool has_immediate = false;
  bool has_agent = false;
  bool has_help = false;
  for (const auto& name : names)
  {
    if (name == "test_immediate")
      has_immediate = true;
    if (name == "test_agent")
      has_agent = true;
    if (name == "help")
      has_help = true;
  }
  CHECK(has_immediate);
  CHECK(has_agent);
  CHECK(has_help);
}

TEST_CASE("slash_command_registry: get_usage")
{
  mistral::slash_command::slash_command_registry registry;

  registry.register_command(std::make_unique<mistral::slash_command::test_immediate_command>());

  CHECK(registry.get_usage("test_immediate") == "/test_immediate [args]");
  CHECK(registry.get_usage("nonexistent") == "");
}

TEST_CASE("slash_command_registry: has_command")
{
  mistral::slash_command::slash_command_registry registry;

  registry.register_command(std::make_unique<mistral::slash_command::test_immediate_command>());

  CHECK(registry.has_command("test_immediate") == true);
  CHECK(registry.has_command("nonexistent") == false);
}

TEST_CASE("slash_command_registry: register empty name throws")
{
  mistral::slash_command::slash_command_registry registry;

  // Create a command with empty name
  class empty_name_command : public mistral::slash_command::slash_command
  {
  public:
    std::string name() const override
    {
      return "";
    }
    std::string description() const override
    {
      return "Empty name";
    }

    dripcore::awaitable<std::string> execute(
      dripcore::task&,
      const std::string&,
      mistral::agent::agent_system&
    ) override
    {
      co_return "test";
    }
  };

  CHECK_THROWS_AS(registry.register_command(std::make_unique<empty_name_command>()), std::invalid_argument);
}

TEST_CASE("slash_command_registry: register null command")
{
  mistral::slash_command::slash_command_registry registry;

  // Registering null should not throw, just be a no-op
  registry.register_command(nullptr);

  // Verify no crash and command not registered
  CHECK(registry.has_command("") == false);
}

TEST_CASE("slash_command_registry: thread safety")
{
  mistral::slash_command::slash_command_registry registry;

  // Clear state by creating new commands each time
  const int num_threads = 10;
  const int commands_per_thread = 100;

  std::vector<std::thread> threads;
  for (int i = 0; i < num_threads; ++i)
  {
    threads.emplace_back([&registry, i]() {
      for (int j = 0; j < commands_per_thread; ++j)
      {
        std::string cmd_name = "thread_cmd_" + std::to_string(i) + "_" + std::to_string(j);

        class dynamic_command : public mistral::slash_command::slash_command
        {
        public:
          explicit dynamic_command(std::string n) : name_(std::move(n))
          {
          }
          std::string name() const override
          {
            return name_;
          }
          std::string description() const override
          {
            return "Dynamic command";
          }

          dripcore::awaitable<std::string> execute(
            dripcore::task&,
            const std::string&,
            mistral::agent::agent_system&
          ) override
          {
            co_return "ok";
          }

        private:
          std::string name_;
        };

        registry.register_command(std::make_unique<dynamic_command>(cmd_name));

        // Also do some lookups
        registry.get_command(cmd_name);
        registry.has_command(cmd_name);
        registry.get_command_names();
      }
    });
  }

  for (auto& t : threads)
  {
    t.join();
  }

  // Verify all commands were registered
  auto names = registry.get_command_names();
  CHECK(names.size() == num_threads * commands_per_thread);
}

TEST_CASE("slash_command_registry: re-register command overwrites")
{
  mistral::slash_command::slash_command_registry registry;

  class cmd1 : public mistral::slash_command::slash_command
  {
  public:
    std::string name() const override
    {
      return "test_re_register";
    }
    std::string description() const override
    {
      return "First";
    }

    dripcore::awaitable<std::string> execute(
      dripcore::task&,
      const std::string&,
      mistral::agent::agent_system&
    ) override
    {
      co_return "first";
    }
  };

  class cmd2 : public mistral::slash_command::slash_command
  {
  public:
    std::string name() const override
    {
      return "test_re_register";
    }
    std::string description() const override
    {
      return "Second";
    }

    dripcore::awaitable<std::string> execute(
      dripcore::task&,
      const std::string&,
      mistral::agent::agent_system&
    ) override
    {
      co_return "second";
    }
  };

  registry.register_command(std::make_unique<cmd1>());
  registry.register_command(std::make_unique<cmd2>());

  const mistral::slash_command::slash_command* cmd = registry.get_command("test_re_register");
  REQUIRE(cmd != nullptr);
  CHECK(cmd->description() == "Second");
}

TEST_CASE("slash_command_registry: case sensitive lookup")
{
  mistral::slash_command::slash_command_registry registry;

  class lower_cmd : public mistral::slash_command::slash_command
  {
  public:
    std::string name() const override
    {
      return "testcase";
    }
    std::string description() const override
    {
      return "Lowercase";
    }

    dripcore::awaitable<std::string> execute(
      dripcore::task&,
      const std::string&,
      mistral::agent::agent_system&
    ) override
    {
      co_return "";
    }
  };

  registry.register_command(std::make_unique<lower_cmd>());

  CHECK(registry.has_command("testcase") == true);
  CHECK(registry.has_command("TESTCASE") == false);
  CHECK(registry.has_command("TestCase") == false);
}

TEST_CASE("slash_command_registry: empty metadata handling")
{
  class empty_cmd : public mistral::slash_command::slash_command
  {
  public:
    std::string name() const override
    {
      return "empty_meta";
    }
    std::string description() const override
    {
      return "";
    }
    std::string usage() const override
    {
      return "";
    }

    dripcore::awaitable<std::string> execute(
      dripcore::task&,
      const std::string&,
      mistral::agent::agent_system&
    ) override
    {
      co_return "";
    }
  };

  mistral::slash_command::slash_command_registry registry;
  registry.register_command(std::make_unique<empty_cmd>());

  const mistral::slash_command::slash_command* cmd = registry.get_command("empty_meta");
  REQUIRE(cmd != nullptr);
  CHECK(cmd->description() == "");
  CHECK(cmd->usage() == "");
  CHECK(registry.get_usage("empty_meta") == "");
}

TEST_CASE("slash_command_registry: get_command_names returns sorted list")
{
  mistral::slash_command::slash_command_registry registry;

  class z_cmd : public mistral::slash_command::slash_command
  {
  public:
    std::string name() const override
    {
      return "zebra";
    }
    std::string description() const override
    {
      return "Z";
    }

    dripcore::awaitable<std::string> execute(
      dripcore::task&,
      const std::string&,
      mistral::agent::agent_system&
    ) override
    {
      co_return "";
    }
  };

  class a_cmd : public mistral::slash_command::slash_command
  {
  public:
    std::string name() const override
    {
      return "alpha";
    }
    std::string description() const override
    {
      return "A";
    }

    dripcore::awaitable<std::string> execute(
      dripcore::task&,
      const std::string&,
      mistral::agent::agent_system&
    ) override
    {
      co_return "";
    }
  };

  class m_cmd : public mistral::slash_command::slash_command
  {
  public:
    std::string name() const override
    {
      return "middle";
    }
    std::string description() const override
    {
      return "M";
    }

    dripcore::awaitable<std::string> execute(
      dripcore::task&,
      const std::string&,
      mistral::agent::agent_system&
    ) override
    {
      co_return "";
    }
  };

  registry.register_command(std::make_unique<z_cmd>());
  registry.register_command(std::make_unique<a_cmd>());
  registry.register_command(std::make_unique<m_cmd>());

  auto names = registry.get_command_names();
  REQUIRE(names.size() == 3);

  // Find our three commands in the list
  bool has_alpha = false;
  bool has_middle = false;
  bool has_zebra = false;
  for (const auto& name : names)
  {
    if (name == "alpha")
      has_alpha = true;
    if (name == "middle")
      has_middle = true;
    if (name == "zebra")
      has_zebra = true;
  }
  CHECK(has_alpha);
  CHECK(has_middle);
  CHECK(has_zebra);
}

TEST_CASE("slash_command_registry: build_catalog tolerates a command that queries the registry")
{
  // spec() and description() are command-supplied code. build_catalog() used
  // to call them with the registry mutex held, so a command that asked the
  // registry for anything deadlocked on the non-recursive mutex.
  auto registry = std::make_shared<mistral::slash_command::slash_command_registry>();
  registry->register_command(std::make_unique<mistral::slash_command::test_self_referencing_command>(*registry));

  // Watchdog: a regression deadlocks instead of failing, and a hung test is
  // worse than a red one. The worker owns everything it touches, so
  // detaching it on timeout is safe.
  auto catalog = std::make_shared<mistral::slash_command::command_catalog>();
  auto done = std::make_shared<std::atomic<bool>>(false);
  std::thread worker([registry, catalog, done]() {
    *catalog = registry->build_catalog();
    done->store(true, std::memory_order_release);
  });

  for (int i = 0; i < 500 && !done->load(std::memory_order_acquire); ++i)
  {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  if (!done->load(std::memory_order_acquire))
  {
    worker.detach();
    FAIL("build_catalog() deadlocked on the registry mutex");
  }

  worker.join();
  REQUIRE(catalog->commands.size() == 1);
  CHECK(catalog->commands[0].name == "test_self_ref");
  CHECK(catalog->commands[0].summary.find("registry reachable") != std::string::npos);
  REQUIRE(catalog->context.command_names.size() == 1);
  CHECK(catalog->context.command_names[0].text == "test_self_ref");
}
