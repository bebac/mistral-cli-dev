#ifndef MISTRAL_MCP_MANAGER_H
#define MISTRAL_MCP_MANAGER_H

#include "agent/agent_config.h"
#include "mcp/mcp_client_interface.h"

#include <dripcore/awaitable.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mistral::mcp
{
  // Manages configured MCP servers: connecting them (MCP handshake),
  // registering their tools into the agent system's global tool
  // registry, and exposing status for the /mcp slash command.
  //
  // Servers are configured in mistral-cli.json. Two transports:
  //
  //   HTTP (Streamable HTTP):
  //     "mcpServers": {
  //       "storybook": { "url": "http://127.0.0.1:6006/mcp" }
  //     }
  //
  //   stdio (spawn a child process, JSON-RPC over pipes):
  //     "mcpServers": {
  //       "xerj": {
  //         "command": "xerj",
  //         "args": ["mcp"],
  //         "env": { "XERJ_URL": "http://localhost:9200" },
  //         "preflight": {
  //           "check_url": "http://localhost:9200",
  //           "start_command": ["xerj", "--insecure",
  //                             "--data-dir", "~/xerj-data"],
  //           "post_start_command": ["xerj", "autoindex", "."]
  //         }
  //       }
  //     }
  //
  // Agents opt in to MCP tools via available_tools entries:
  //   "mcp:*"           - all tools from all connected servers
  //   "mcp:storybook"   - all tools from one server
  //   "stories-preview" - a single tool by registered name
  class mcp_manager
  {
  public:
    static mcp_manager& instance();

    // Configure from the "mcpServers" config section.
    // Replaces any previous configuration.
    void configure(const std::map<std::string, agent::mcp_server_config>& servers);

    // Connect one configured server: run preflight (if any), MCP
    // initialize handshake, tools/list, register tools into the
    // global tool registry. Returns a summary json.
    // Must run in dripcore task context.
    dripcore::awaitable<json> connect(const std::string& name);

    // Connect all configured servers. Returns an array of summaries.
    dripcore::awaitable<json> connect_all();

    // Connect all servers marked auto_connect=true. Returns an array
    // of summaries (same shape as connect_all).
    dripcore::awaitable<json> connect_auto();

    // Names of servers with auto_connect=true (for display).
    std::vector<std::string> auto_connect_servers() const;

    // True if all auto-connect servers are connected.
    bool auto_connect_all_connected() const;

    // Close all connected clients (kill stdio child processes).
    // Called during TUI shutdown so child processes are reaped
    // before process exit, not from static destructors.
    void shutdown();

    // Status of all configured servers (for /mcp).
    json status() const;

    // Tool names for an available_tools entry suffix: "*" or "" for
    // all connected servers, otherwise the tools of one server.
    std::vector<std::string> tool_names(const std::string& server) const;

    // Combined agent instructions from connected servers (from the
    // initialize result). Empty when nothing is connected.
    const std::string& instructions() const
    {
      return instructions_;
    }

    // Bumped whenever tools are registered; agent tasks compare it
    // against their snapshot to detect registry refreshes.
    uint64_t registry_version() const
    {
      return registry_version_;
    }

  private:
    mcp_manager() = default;

    struct server_state
    {
      agent::mcp_server_config config;
      std::shared_ptr<mcp_client_interface> client;
      bool connected = false;
      std::vector<std::string> tool_names;
      std::string instructions;
      std::string error;
    };

    // Run the preflight for one server: check_url, start_command,
    // post_start_command. Returns an error string or empty on success.
    dripcore::awaitable<std::string> run_preflight(const agent::mcp_server_config& cfg);

    // HTTP GET to check if a URL is reachable. Returns true on 2xx.
    dripcore::awaitable<bool> http_check(const std::string& url);

    // Spawn a detached process (start_command). Returns the pid or -1.
    pid_t spawn_detached(const std::vector<std::string>& command);

    // Run a command and wait for it to complete (post_start_command).
    dripcore::awaitable<int> run_command(const std::vector<std::string>& command);

    void rebuild_instructions();

    std::map<std::string, server_state> servers_;
    std::string instructions_;
    uint64_t registry_version_ = 0;
  };

} // namespace mistral::mcp

#endif // MISTRAL_MCP_MANAGER_H
