#include "mcp/mcp_manager.h"

#include "mcp/mcp_tool.h"
#include "agent/agent_system.h"

#include <stdexcept>

namespace mistral::mcp
{
  mcp_manager& mcp_manager::instance()
  {
    static mcp_manager mgr;
    return mgr;
  }

  void mcp_manager::configure(const std::map<std::string, std::string>& servers)
  {
    servers_.clear();
    for (const auto& [name, url] : servers) {
      server_state st;
      st.url = url;
      servers_[name] = std::move(st);
    }
    instructions_.clear();
  }

  dripcore::awaitable<json> mcp_manager::connect(const std::string& name)
  {
    auto it = servers_.find(name);
    if (it == servers_.end()) {
      co_return json{{"error", "unknown mcp server '" + name + "'"}};
    }

    server_state& st = it->second;
    try {
      st.client = std::make_shared<mcp_client>(st.url);
      if (!(co_await st.client->initialize())) {
        throw std::runtime_error("initialize failed");
      }

      json tools_result = co_await st.client->list_tools();
      st.connected = true;
      st.error.clear();
      st.instructions = st.client->instructions();
      st.tool_names.clear();

      auto& registry = agent::agent_system::instance().global_tools();
      size_t registered = 0;
      if (tools_result.contains("tools") && tools_result["tools"].is_array()) {
        for (const auto& t : tools_result["tools"]) {
          if (!t.contains("name")) continue;
          std::string tool_name = t["name"].get<std::string>();
          registry.add_tool(tool_name, std::make_shared<mcp_tool>(
            st.client, tool_name,
            t.value("description", std::string()),
            t.contains("inputSchema") ? t["inputSchema"] : json::object()));
          st.tool_names.push_back(tool_name);
          ++registered;
        }
      }

      rebuild_instructions();
      ++registry_version_;

      co_return json{
        {"server", name},
        {"connected", true},
        {"tools", registered},
        {"instructions_chars", st.instructions.size()}
      };
    } catch (const std::exception& e) {
      st.connected = false;
      st.error = e.what();
      st.client.reset();
      co_return json{
        {"server", name},
        {"connected", false},
        {"error", st.error}
      };
    }
  }

  dripcore::awaitable<json> mcp_manager::connect_all()
  {
    json results = json::array();
    for (const auto& [name, st] : servers_) {
      results.push_back(co_await connect(name));
    }
    co_return results;
  }

  json mcp_manager::status() const
  {
    json out = json::array();
    for (const auto& [name, st] : servers_) {
      json s = {
        {"name", name},
        {"url", st.url},
        {"connected", st.connected}
      };
      if (!st.error.empty()) {
        s["error"] = st.error;
      }
      if (st.connected) {
        s["tools"] = st.tool_names;
      }
      out.push_back(std::move(s));
    }
    return out;
  }

  std::vector<std::string> mcp_manager::tool_names(
    const std::string& server) const
  {
    std::vector<std::string> names;
    if (server.empty() || server == "*") {
      for (const auto& [name, st] : servers_) {
        if (st.connected) {
          names.insert(
            names.end(), st.tool_names.begin(), st.tool_names.end());
        }
      }
    } else {
      auto it = servers_.find(server);
      if (it != servers_.end() && it->second.connected) {
        names = it->second.tool_names;
      }
    }
    return names;
  }

  void mcp_manager::rebuild_instructions()
  {
    instructions_.clear();
    for (const auto& [name, st] : servers_) {
      if (st.connected && !st.instructions.empty()) {
        if (!instructions_.empty()) {
          instructions_ += "\n\n";
        }
        instructions_ += "# MCP server: " + name + "\n\n" + st.instructions;
      }
    }
  }

} // namespace mistral::mcp
