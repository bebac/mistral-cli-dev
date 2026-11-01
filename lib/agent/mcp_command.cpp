#include "agent/mcp_command.h"
#include "mcp/mcp_manager.h"

#include <format>
#include <string>
#include <utility>

namespace mistral::agent
{
  using json = nlohmann::ordered_json;

  std::string mcp_command::name() const
  {
    return "mcp";
  }

  std::string mcp_command::description() const
  {
    return "Manage MCP servers (status, connect, tools)";
  }

  slash_command::command_spec mcp_command::spec() const
  {
    using namespace mistral::slash_command;

    arg_spec server;
    server.name = "server";
    server.description = "configured MCP server";
    server.completer = [](const std::string&)
    {
      std::vector<completion_item> items;
      for (const auto& name : mistral::mcp::mcp_manager::instance().server_names())
      {
        items.push_back({name, ""});
      }
      return items;
    };

    command_spec connect;
    connect.name = "connect";
    connect.summary = "Connect servers (all when no server is given)";
    connect.args.push_back(std::move(server));

    command_spec tools;
    tools.name = "tools";
    tools.summary = "List registered MCP tools";

    command_spec out;
    out.name = name();
    out.summary = description();
    out.subcommands.push_back(std::move(connect));
    out.subcommands.push_back(std::move(tools));
    return out;
  }

  dripcore::awaitable<std::string> mcp_command::execute(dripcore::task&, const std::string& args, agent::agent_system&)
  {
    auto& mgr = mistral::mcp::mcp_manager::instance();

    const mistral::slash_command::invocation inv = spec().parse(args);
    if (!inv.ok())
    {
      co_return "Error: " + inv.error + "\nusage: /" + usage();
    }

    // /mcp connect [server]
    if (inv.path.size() == 2 && inv.path[1] == "connect")
    {
      std::string server;
      for (const auto& token : inv.args)
      {
        if (!server.empty())
        {
          server += " ";
        }
        server += token;
      }

      json results;
      if (server.empty())
      {
        results = co_await mgr.connect_all();
      }
      else
      {
        results = json::array();
        results.push_back(co_await mgr.connect(server));
      }

      std::string out;
      for (const auto& r : results)
      {
        if (r.value("connected", false))
        {
          out += std::format(
            "connected '{}': {} tools registered\n", r["server"].get<std::string>(), r["tools"].get<size_t>()
          );
        }
        else
        {
          out += std::format(
            "failed to connect '{}': {}\n",
            r["server"].get<std::string>(),
            r.value("error", std::string("unknown error"))
          );
        }
        if (r.contains("warning"))
        {
          out += std::format(
            "warning '{}': {}\n", r["server"].get<std::string>(), r["warning"].get<std::string>()
          );
        }
      }
      if (out.empty())
      {
        out = "no mcp servers configured "
              "(add \"mcpServers\" to mistral-cli.json)\n";
      }
      out += "\nMCP tools are picked up by the agent on the next message.";
      co_return out;
    }

    // /mcp tools
    if (inv.path.size() == 2 && inv.path[1] == "tools")
    {
      auto names = mgr.tool_names("*");
      if (names.empty())
      {
        co_return "no MCP tools registered (connect first: /mcp connect)";
      }
      std::string out = "registered MCP tools:\n";
      for (const auto& n : names)
      {
        out += "  - " + n + "\n";
      }
      co_return out;
    }

    // /mcp (status)
    json servers = mgr.status();
    if (servers.empty())
    {
      co_return std::string("no mcp servers configured\n") + "add to mistral-cli.json:\n" +
        "  \"mcpServers\": { \"storybook\": "
        "{ \"url\": \"http://127.0.0.1:6006/mcp\" } }\n" +
        "and \"mcp:*\" to the agent's available_tools";
    }

    std::string out = "mcp servers:\n";
    for (const auto& s : servers)
    {
      std::string endpoint;
      if (s.contains("url"))
      {
        endpoint = s["url"].get<std::string>();
      }
      else if (s.contains("command"))
      {
        endpoint = s["command"].get<std::string>() + " (stdio)";
      }
      out += std::format(
        "  {} ({}) - {}\n",
        s["name"].get<std::string>(),
        endpoint,
        s["connected"].get<bool>() ? "connected" : "disconnected"
      );
      if (s.contains("error"))
      {
        out += std::format("    error: {}\n", s["error"].get<std::string>());
      }
      if (s.contains("tools"))
      {
        for (const auto& t : s["tools"])
        {
          out += std::format("    - {}\n", t.get<std::string>());
        }
      }
    }
    co_return out;
  }
} // namespace mistral::agent
