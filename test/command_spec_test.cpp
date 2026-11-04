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

TEST_CASE("command_spec: model_ids source resolves from the context")
{
  // Mirrors the shape of /model: one optional argument completed from the
  // current agent's model entry.
  command_spec spec;
  spec.name = "model";
  arg_spec model_id;
  model_id.name = "model_id";
  model_id.source = candidate_source::model_ids;
  spec.args.push_back(std::move(model_id));

  completion_context ctx;
  ctx.model_ids = {{"zai-glm-5-3", ""}, {"zai-glm-5-3-flash", ""}};

  // Empty token offers every id.
  const auto all = spec.complete("", ctx);
  REQUIRE(all.items.size() == 2);
  CHECK(all.items[0].text == "zai-glm-5-3");
  CHECK(all.items[1].text == "zai-glm-5-3-flash");
  CHECK(all.complete);

  // A prefix filters the candidates.
  const auto partial = spec.complete("zai-glm-5-3-f", ctx);
  REQUIRE(partial.items.size() == 1);
  CHECK(partial.items[0].text == "zai-glm-5-3-flash");
  CHECK(!partial.complete);

  // An exact id is complete as typed.
  CHECK(spec.complete("zai-glm-5-3", ctx).complete);

  // An unknown id is free-form: the executor reports the error.
  const auto unknown = spec.complete("not-offered", ctx);
  CHECK(unknown.items.empty());
  CHECK(unknown.complete);
}

TEST_CASE("command_spec: prefer_candidate takes the highlighted candidate")
{
  // Mirrors /model: an optional argument whose candidates are the point of
  // the command.
  command_spec spec;
  spec.name = "model";
  arg_spec model_id;
  model_id.name = "model_id";
  model_id.source = candidate_source::model_ids;
  model_id.prefer_candidate = true;
  spec.args.push_back(std::move(model_id));

  completion_context ctx;
  ctx.model_ids = {{"zai-glm-5-3", ""}, {"zai-glm-5-3-flash", ""}};

  // Empty token: the line is complete, but Enter should take a candidate.
  const auto empty = spec.complete("", ctx);
  CHECK(empty.complete);
  CHECK(empty.prefer_candidate);

  // A typed token is not complete, so Enter accepts the candidate anyway.
  const auto partial = spec.complete("zai-glm-5-3-f", ctx);
  CHECK(!partial.complete);
  CHECK(!partial.prefer_candidate);

  // Without candidates there is nothing to prefer: run the line as typed.
  const completion_context no_models;
  CHECK(!spec.complete("", no_models).prefer_candidate);

  // The flag is opt-in: an optional argument without it keeps running the
  // command when nothing is typed (/mcp connect connects every server).
  command_spec plain;
  plain.name = "connect";
  arg_spec server;
  server.name = "server";
  server.source = candidate_source::mcp_servers;
  plain.args.push_back(std::move(server));

  completion_context servers;
  servers.mcp_servers = {{"alpha", ""}};
  const auto plain_empty = plain.complete("", servers);
  CHECK(plain_empty.complete);
  CHECK(!plain_empty.prefer_candidate);
}

TEST_CASE("command_spec: completing the command name never prefers a candidate")
{
  // "/model" + Enter runs the command; only "/model " prefers a candidate.
  command_spec spec;
  spec.name = "model";
  arg_spec model_id;
  model_id.name = "model_id";
  model_id.source = candidate_source::model_ids;
  model_id.prefer_candidate = true;
  spec.args.push_back(std::move(model_id));

  command_catalog catalog;
  catalog.commands.push_back(spec);
  catalog.context.command_names = {{"model", ""}};
  catalog.context.model_ids = {{"zai-glm-5-3", ""}};

  const auto bare = complete_line(catalog, "model");
  CHECK(bare.complete);
  CHECK(!bare.prefer_candidate);

  const auto spaced = complete_line(catalog, "model ");
  CHECK(spaced.complete);
  CHECK(spaced.prefer_candidate);
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
