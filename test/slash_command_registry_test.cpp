#include <doctest/doctest.h>
#include "console/slash_command.h"
#include "console/slash_command_registry.h"

#include <dripcore/task.h>
#include <dripcore/awaitable.h>

#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace mistral::console
{

// Test command implementations
class test_immediate_command : public slash_command
{
public:
  std::string name() const override { return "test_immediate"; }
  std::string description() const override { return "Test immediate command"; }
  std::string usage() const override { return "/test_immediate [args]"; }
  execution_context context() const override { return execution_context::IMMEDIATE; }
  std::string execute_immediate(const std::string& args) override
  {
    return "Immediate: " + args;
  }
};

class test_agent_command : public slash_command
{
public:
  std::string name() const override { return "test_agent"; }
  std::string description() const override { return "Test agent command"; }
  std::string usage() const override { return "/test_agent [args]"; }
  execution_context context() const override { return execution_context::AGENT; }

  dripcore::awaitable<std::string> execute_agent(
      dripcore::task&,
      const std::string& args,
      agent::agent_system&) override
  {
    co_return "Agent: " + args;
  }
};

class test_help_command : public slash_command
{
public:
  std::string name() const override { return "help"; }
  std::string description() const override { return "Show help"; }
  execution_context context() const override { return execution_context::IMMEDIATE; }
  std::string execute_immediate(const std::string& args) override
  {
    if (args.empty()) {
      return "General help";
    }
    return "Help for: " + args;
  }
};

} // namespace mistral::console

TEST_CASE("slash_command_registry: singleton instance")
{
  auto& registry1 = mistral::console::slash_command_registry::instance();
  auto& registry2 = mistral::console::slash_command_registry::instance();
  CHECK(&registry1 == &registry2);
}

TEST_CASE("slash_command_registry: register and get command")
{
  auto& registry = mistral::console::slash_command_registry::instance();

  // Clear any existing commands (tests may run in any order)
  // We'll use a fresh registry for each test by checking state

  auto cmd = std::make_unique<mistral::console::test_immediate_command>();
  registry.register_command(std::move(cmd));

  const mistral::console::slash_command* retrieved = registry.get_command("test_immediate");
  REQUIRE(retrieved != nullptr);
  CHECK(retrieved->name() == "test_immediate");
  CHECK(retrieved->description() == "Test immediate command");
  CHECK(retrieved->usage() == "/test_immediate [args]");
  CHECK(retrieved->context() == mistral::console::slash_command::execution_context::IMMEDIATE);
}

TEST_CASE("slash_command_registry: get unknown command")
{
  auto& registry = mistral::console::slash_command_registry::instance();
  const mistral::console::slash_command* retrieved = registry.get_command("nonexistent");
  CHECK(retrieved == nullptr);
}

TEST_CASE("slash_command_registry: get_command_names")
{
  auto& registry = mistral::console::slash_command_registry::instance();

  // Register multiple commands
  registry.register_command(std::make_unique<mistral::console::test_immediate_command>());
  registry.register_command(std::make_unique<mistral::console::test_agent_command>());
  registry.register_command(std::make_unique<mistral::console::test_help_command>());

  auto names = registry.get_command_names();
  REQUIRE(names.size() >= 3); // At least our 3 commands

  // Check that our commands are in the list
  bool has_immediate = false;
  bool has_agent = false;
  bool has_help = false;
  for (const auto& name : names) {
    if (name == "test_immediate") has_immediate = true;
    if (name == "test_agent") has_agent = true;
    if (name == "help") has_help = true;
  }
  CHECK(has_immediate);
  CHECK(has_agent);
  CHECK(has_help);
}

TEST_CASE("slash_command_registry: needs_agent_execution")
{
  auto& registry = mistral::console::slash_command_registry::instance();

  registry.register_command(std::make_unique<mistral::console::test_immediate_command>());
  registry.register_command(std::make_unique<mistral::console::test_agent_command>());

  CHECK(registry.needs_agent_execution("test_immediate") == false);
  CHECK(registry.needs_agent_execution("test_agent") == true);
  CHECK(registry.needs_agent_execution("nonexistent") == false);
}

TEST_CASE("slash_command_registry: get_usage")
{
  auto& registry = mistral::console::slash_command_registry::instance();

  registry.register_command(std::make_unique<mistral::console::test_immediate_command>());

  CHECK(registry.get_usage("test_immediate") == "/test_immediate [args]");
  CHECK(registry.get_usage("nonexistent") == "");
}

TEST_CASE("slash_command_registry: has_command")
{
  auto& registry = mistral::console::slash_command_registry::instance();

  registry.register_command(std::make_unique<mistral::console::test_immediate_command>());

  CHECK(registry.has_command("test_immediate") == true);
  CHECK(registry.has_command("nonexistent") == false);
}

TEST_CASE("slash_command_registry: register empty name throws")
{
  auto& registry = mistral::console::slash_command_registry::instance();

  // Create a command with empty name
  class empty_name_command : public mistral::console::slash_command
  {
  public:
    std::string name() const override { return ""; }
    std::string description() const override { return "Empty name"; }
    execution_context context() const override { return execution_context::IMMEDIATE; }
    std::string execute_immediate(const std::string& args) override { return "test"; }
  };

  CHECK_THROWS_AS(
    registry.register_command(std::make_unique<empty_name_command>()),
    std::invalid_argument
  );
}

TEST_CASE("slash_command_registry: register null command")
{
  auto& registry = mistral::console::slash_command_registry::instance();

  // Registering null should not throw, just be a no-op
  registry.register_command(nullptr);

  // Verify no crash and command not registered
  CHECK(registry.has_command("") == false);
}

TEST_CASE("slash_command_registry: thread safety")
{
  auto& registry = mistral::console::slash_command_registry::instance();

  // Clear state by creating new commands each time
  const int num_threads = 10;
  const int commands_per_thread = 100;

  std::vector<std::thread> threads;
  for (int i = 0; i < num_threads; ++i) {
    threads.emplace_back([&registry, i]() {
      for (int j = 0; j < commands_per_thread; ++j) {
        std::string cmd_name = "thread_cmd_" + std::to_string(i) + "_" + std::to_string(j);

        class dynamic_command : public mistral::console::slash_command
        {
        public:
          explicit dynamic_command(std::string n) : name_(std::move(n)) {}
          std::string name() const override { return name_; }
          std::string description() const override { return "Dynamic command"; }
          execution_context context() const override { return execution_context::IMMEDIATE; }
          std::string execute_immediate(const std::string&) override { return "ok"; }
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

  for (auto& t : threads) {
    t.join();
  }

  // Verify all commands were registered
  auto names = registry.get_command_names();
  CHECK(names.size() >= num_threads * commands_per_thread);
}

TEST_CASE("slash_command_registry: re-register command overwrites")
{
  auto& registry = mistral::console::slash_command_registry::instance();

  class cmd1 : public mistral::console::slash_command
  {
  public:
    std::string name() const override { return "test_re_register"; }
    std::string description() const override { return "First"; }
    execution_context context() const override { return execution_context::IMMEDIATE; }
    std::string execute_immediate(const std::string&) override { return "first"; }
  };

  class cmd2 : public mistral::console::slash_command
  {
  public:
    std::string name() const override { return "test_re_register"; }
    std::string description() const override { return "Second"; }
    execution_context context() const override { return execution_context::AGENT; }

    dripcore::awaitable<std::string> execute_agent(
        dripcore::task&,
        const std::string&,
        mistral::agent::agent_system&) override
    {
      co_return "second";
    }
  };

  registry.register_command(std::make_unique<cmd1>());
  registry.register_command(std::make_unique<cmd2>());

  const mistral::console::slash_command* cmd = registry.get_command("test_re_register");
  REQUIRE(cmd != nullptr);
  CHECK(cmd->description() == "Second");
  CHECK(cmd->context() == mistral::console::slash_command::execution_context::AGENT);
}

TEST_CASE("slash_command_registry: case sensitive lookup")
{
  auto& registry = mistral::console::slash_command_registry::instance();

  class lower_cmd : public mistral::console::slash_command
  {
  public:
    std::string name() const override { return "testcase"; }
    std::string description() const override { return "Lowercase"; }
    execution_context context() const override { return execution_context::IMMEDIATE; }
    std::string execute_immediate(const std::string&) override { return ""; }
  };

  registry.register_command(std::make_unique<lower_cmd>());

  CHECK(registry.has_command("testcase") == true);
  CHECK(registry.has_command("TESTCASE") == false);
  CHECK(registry.has_command("TestCase") == false);
}

TEST_CASE("slash_command_registry: empty metadata handling")
{
  class empty_cmd : public mistral::console::slash_command
  {
  public:
    std::string name() const override { return "empty_meta"; }
    std::string description() const override { return ""; }
    std::string usage() const override { return ""; }
    execution_context context() const override { return execution_context::IMMEDIATE; }
    std::string execute_immediate(const std::string&) override { return ""; }
  };

  auto& registry = mistral::console::slash_command_registry::instance();
  registry.register_command(std::make_unique<empty_cmd>());

  const mistral::console::slash_command* cmd = registry.get_command("empty_meta");
  REQUIRE(cmd != nullptr);
  CHECK(cmd->description() == "");
  CHECK(cmd->usage() == "");
  CHECK(registry.get_usage("empty_meta") == "");
}

TEST_CASE("slash_command_registry: get_command_names returns sorted list")
{
  auto& registry = mistral::console::slash_command_registry::instance();

  class z_cmd : public mistral::console::slash_command
  {
  public:
    std::string name() const override { return "zebra"; }
    std::string description() const override { return "Z"; }
    execution_context context() const override { return execution_context::IMMEDIATE; }
    std::string execute_immediate(const std::string&) override { return ""; }
  };

  class a_cmd : public mistral::console::slash_command
  {
  public:
    std::string name() const override { return "alpha"; }
    std::string description() const override { return "A"; }
    execution_context context() const override { return execution_context::IMMEDIATE; }
    std::string execute_immediate(const std::string&) override { return ""; }
  };

  class m_cmd : public mistral::console::slash_command
  {
  public:
    std::string name() const override { return "middle"; }
    std::string description() const override { return "M"; }
    execution_context context() const override { return execution_context::IMMEDIATE; }
    std::string execute_immediate(const std::string&) override { return ""; }
  };

  registry.register_command(std::make_unique<z_cmd>());
  registry.register_command(std::make_unique<a_cmd>());
  registry.register_command(std::make_unique<m_cmd>());

  auto names = registry.get_command_names();
  REQUIRE(names.size() >= 3);

  // Find our three commands in the list
  bool has_alpha = false;
  bool has_middle = false;
  bool has_zebra = false;
  for (const auto& name : names) {
    if (name == "alpha") has_alpha = true;
    if (name == "middle") has_middle = true;
    if (name == "zebra") has_zebra = true;
  }
  CHECK(has_alpha);
  CHECK(has_middle);
  CHECK(has_zebra);
}