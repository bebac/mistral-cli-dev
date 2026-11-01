#include "agent/mcp_command.h"
#include "mcp/mcp_manager.h"
#include "slash_command/slash_command_registry.h"
#include <doctest/doctest.h>

#include <memory>
#include <string>

using mistral::slash_command::slash_command_registry;

namespace
{
  std::shared_ptr<slash_command_registry> make_registry()
  {
    auto registry = std::make_shared<slash_command_registry>();
    registry->register_command(std::make_unique<mistral::agent::mcp_command>());
    return registry;
  }

  void configure_servers()
  {
    mistral::agent::mcp_server_config storybook;
    storybook.url = "http://127.0.0.1:6006/mcp";

    mistral::agent::mcp_server_config xerj;
    xerj.command = "xerj";

    mistral::mcp::mcp_manager::instance().configure({{"storybook", storybook}, {"xerj", xerj}});
  }
} // namespace

TEST_CASE("slash_command_registry: completes command names")
{
  auto registry = make_registry();

  const auto state = registry->complete("mc");
  REQUIRE(state.command_names);
  REQUIRE(state.items.size() == 1);
  CHECK(state.items[0].text == "mcp");
  CHECK(!state.complete);
}

TEST_CASE("slash_command_registry: completes mcp subcommands")
{
  auto registry = make_registry();

  const auto state = registry->complete("mcp ");
  REQUIRE(!state.command_names);
  REQUIRE(state.items.size() == 2);
  CHECK(state.items[0].text == "connect");
  CHECK(state.items[1].text == "tools");
  // "/mcp" is runnable on its own, so the line is already complete.
  CHECK(state.complete);
}

TEST_CASE("slash_command_registry: completes mcp server names")
{
  configure_servers();
  auto registry = make_registry();

  const auto all = registry->complete("mcp connect ");
  REQUIRE(all.items.size() == 2);
  CHECK(all.items[0].text == "storybook");
  CHECK(all.items[1].text == "xerj");

  const auto partial = registry->complete("mcp connect x");
  REQUIRE(partial.items.size() == 1);
  CHECK(partial.items[0].text == "xerj");
  CHECK(!partial.complete);

  CHECK(registry->complete("mcp connect xerj").complete);
}

TEST_CASE("slash_command_registry: validates subcommands")
{
  auto registry = make_registry();

  CHECK(registry->validate("mcp", "").empty());
  CHECK(registry->validate("mcp", "connect storybook").empty());

  const auto error = registry->validate("mcp", "conect");
  CHECK(!error.empty());
  CHECK(error.find("conect") != std::string::npos);

  CHECK(!registry->validate("nosuchcommand", "").empty());
}

TEST_CASE("mcp_command: usage is derived from the spec")
{
  mistral::agent::mcp_command cmd;
  CHECK(cmd.usage() == "mcp [connect [server]|tools]");
}
