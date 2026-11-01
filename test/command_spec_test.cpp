#include "slash_command/command_spec.h"
#include <doctest/doctest.h>

#include <string>
#include <utility>
#include <vector>

using mistral::slash_command::arg_spec;
using mistral::slash_command::command_spec;
using mistral::slash_command::completion_item;

namespace
{
  // Mirrors the shape of /mcp: two subcommands, one with an optional
  // argument that has completion candidates.
  command_spec make_mcp_spec()
  {
    arg_spec server;
    server.name = "server";
    server.completer = [](const std::string&)
    {
      return std::vector<completion_item>{{"alpha", ""}, {"beta", ""}};
    };

    command_spec connect;
    connect.name = "connect";
    connect.summary = "Connect servers";
    connect.args.push_back(std::move(server));

    command_spec tools;
    tools.name = "tools";
    tools.summary = "List tools";

    command_spec out;
    out.name = "mcp";
    out.subcommands.push_back(std::move(connect));
    out.subcommands.push_back(std::move(tools));
    return out;
  }
} // namespace

TEST_CASE("command_spec: usage renders arguments and subcommands")
{
  CHECK(make_mcp_spec().usage() == "mcp [connect [server]|tools]");

  command_spec tool;
  tool.name = "tool";
  arg_spec name;
  name.name = "tool_name";
  name.required = true;
  tool.args.push_back(std::move(name));
  CHECK(tool.usage() == "tool <tool_name>");

  command_spec bare;
  bare.name = "clear";
  CHECK(bare.usage() == "clear");
}

TEST_CASE("command_spec: parse resolves subcommands and arguments")
{
  const command_spec spec = make_mcp_spec();

  const auto bare = spec.parse("");
  REQUIRE(bare.ok());
  REQUIRE(bare.path.size() == 1);
  CHECK(bare.path[0] == "mcp");
  CHECK(bare.args.empty());

  const auto connect = spec.parse("connect alpha");
  REQUIRE(connect.ok());
  REQUIRE(connect.path.size() == 2);
  CHECK(connect.path[1] == "connect");
  REQUIRE(connect.args.size() == 1);
  CHECK(connect.args[0] == "alpha");

  const auto tools = spec.parse("tools");
  REQUIRE(tools.ok());
  REQUIRE(tools.path.size() == 2);
  CHECK(tools.path[1] == "tools");

  const auto typo = spec.parse("conect");
  CHECK(!typo.ok());
  CHECK(typo.error.find("conect") != std::string::npos);
}

TEST_CASE("command_spec: complete offers subcommands and argument candidates")
{
  const command_spec spec = make_mcp_spec();

  // Bare command: the subcommands are offered, but the line is runnable.
  const auto bare = spec.complete("");
  REQUIRE(bare.items.size() == 2);
  CHECK(bare.items[0].text == "connect");
  CHECK(bare.items[1].text == "tools");
  CHECK(bare.complete);

  // Partial subcommand.
  const auto partial = spec.complete("con");
  REQUIRE(partial.items.size() == 1);
  CHECK(partial.items[0].text == "connect");
  CHECK(!partial.complete);

  // Exact subcommand with an optional argument: complete.
  const auto exact = spec.complete("connect");
  REQUIRE(exact.items.size() == 2);
  CHECK(exact.complete);

  // Partial argument: not complete.
  const auto arg = spec.complete("connect al");
  REQUIRE(arg.items.size() == 1);
  CHECK(arg.items[0].text == "alpha");
  CHECK(!arg.complete);

  // Exact argument: complete.
  CHECK(spec.complete("connect alpha").complete);

  // Trailing space starts a new token; there is no second argument.
  const auto after = spec.complete("connect alpha ");
  CHECK(after.items.empty());
  CHECK(after.complete);
}

TEST_CASE("command_spec: required arguments block completion")
{
  command_spec spec;
  spec.name = "tool";
  arg_spec name;
  name.name = "tool_name";
  name.required = true;
  spec.args.push_back(std::move(name));

  CHECK(!spec.complete("").complete);
  CHECK(spec.complete("ls").complete);
}

TEST_CASE("command_spec: free-form arguments complete once required ones are met")
{
  command_spec spec;
  spec.name = "load";
  arg_spec file;
  file.name = "file";
  spec.args.push_back(std::move(file));

  const auto empty = spec.complete("");
  CHECK(empty.items.empty());
  CHECK(empty.complete);

  CHECK(spec.complete("notes.md").complete);
  CHECK(spec.complete("some file.md").complete);
}
