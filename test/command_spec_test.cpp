#include "slash_command/command_spec.h"
#include <doctest/doctest.h>

#include <string>
#include <utility>
#include <vector>

using mistral::slash_command::arg_spec;
using mistral::slash_command::candidate_source;
using mistral::slash_command::command_catalog;
using mistral::slash_command::command_spec;
using mistral::slash_command::completion_context;
using mistral::slash_command::completion_item;

namespace
{
  // Mirrors the shape of /mcp: two subcommands, one with an optional
  // argument completed from the MCP server list.
  command_spec make_mcp_spec()
  {
    arg_spec server;
    server.name = "server";
    server.source = candidate_source::mcp_servers;

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

  completion_context make_context()
  {
    completion_context ctx;
    ctx.mcp_servers = {{"alpha", ""}, {"beta", ""}};
    return ctx;
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
  const completion_context ctx = make_context();

  // Bare command: the subcommands are offered, but the line is runnable.
  const auto bare = spec.complete("", ctx);
  REQUIRE(bare.items.size() == 2);
  CHECK(bare.items[0].text == "connect");
  CHECK(bare.items[1].text == "tools");
  CHECK(bare.complete);

  // Partial subcommand.
  const auto partial = spec.complete("con", ctx);
  REQUIRE(partial.items.size() == 1);
  CHECK(partial.items[0].text == "connect");
  CHECK(!partial.complete);

  // Exact subcommand with an optional argument: complete.
  const auto exact = spec.complete("connect", ctx);
  REQUIRE(exact.items.size() == 2);
  CHECK(exact.complete);

  // Partial argument: not complete.
  const auto arg = spec.complete("connect al", ctx);
  REQUIRE(arg.items.size() == 1);
  CHECK(arg.items[0].text == "alpha");
  CHECK(!arg.complete);

  // Exact argument: complete.
  CHECK(spec.complete("connect alpha", ctx).complete);

  // Trailing space starts a new token; there is no second argument.
  const auto after = spec.complete("connect alpha ", ctx);
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

  const completion_context ctx;
  CHECK(!spec.complete("", ctx).complete);
  CHECK(spec.complete("ls", ctx).complete);
}

TEST_CASE("command_spec: free-form arguments complete once required ones are met")
{
  command_spec spec;
  spec.name = "load";
  arg_spec file;
  file.name = "file";
  spec.args.push_back(std::move(file));

  const completion_context ctx;
  const auto empty = spec.complete("", ctx);
  CHECK(empty.items.empty());
  CHECK(empty.complete);

  CHECK(spec.complete("notes.md", ctx).complete);
  CHECK(spec.complete("some file.md", ctx).complete);
}

TEST_CASE("command_spec: literal source resolves from the argument")
{
  command_spec spec;
  spec.name = "usage";
  arg_spec reset;
  reset.name = "reset";
  reset.source = candidate_source::literal;
  reset.literals = {{"reset", "reset the counters"}};
  spec.args.push_back(std::move(reset));

  const completion_context ctx;
  const auto partial = spec.complete("r", ctx);
  REQUIRE(partial.items.size() == 1);
  CHECK(partial.items[0].text == "reset");
  CHECK(!partial.complete);

  CHECK(spec.complete("reset", ctx).complete);
}

TEST_CASE("command_spec: complete_line resolves command names and dispatches")
{
  command_catalog catalog;
  catalog.commands.push_back(make_mcp_spec());
  catalog.context.command_names = {{"mcp", "Manage servers"}};
  catalog.context.mcp_servers = {{"alpha", ""}, {"beta", ""}};

  const auto names = mistral::slash_command::complete_line(catalog, "mc");
  REQUIRE(names.command_names);
  REQUIRE(names.items.size() == 1);
  CHECK(names.items[0].text == "mcp");
  CHECK(!names.complete);

  const auto sub = mistral::slash_command::complete_line(catalog, "mcp ");
  REQUIRE(sub.items.size() == 2);
  CHECK(sub.items[0].text == "connect");

  const auto server = mistral::slash_command::complete_line(catalog, "mcp connect al");
  REQUIRE(server.items.size() == 1);
  CHECK(server.items[0].text == "alpha");
  CHECK(!server.complete);

  CHECK(mistral::slash_command::complete_line(catalog, "mcp connect alpha").complete);

  // Unknown command: nothing to offer.
  const auto unknown = mistral::slash_command::complete_line(catalog, "nope ");
  CHECK(unknown.items.empty());
}
