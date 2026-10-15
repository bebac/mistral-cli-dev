#ifndef MISTRAL_MCP_MANAGER_H
#define MISTRAL_MCP_MANAGER_H

#include "mcp/mcp_client.h"

#include <dripcore/awaitable.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace mistral::mcp
{
  // Manages configured MCP servers: connecting them (MCP handshake),
  // registering their tools into the agent system's global tool
  // registry, and exposing status for the /mcp slash command.
  //
  // Servers are configured in mistral-cli.json:
  //   "mcpServers": {
  //     "storybook": { "url": "http://127.0.0.1:6006/mcp" }
  //   }
  //
  // Agents opt in to MCP tools via available_tools entries:
  //   "mcp:*"           - all tools from all connected servers
  //   "mcp:storybook"   - all tools from one server
  //   "stories-preview" - a single tool by registered name
  class mcp_manager
  {
  public:
    static mcp_manager& instance();

    // Configure from the "mcpServers" config section (name -> url).
    // Replaces any previous configuration.
    void configure(const std::map<std::string, std::string>& servers);

    // Connect one configured server: MCP initialize handshake,
    // tools/list, register tools into the global tool registry.
    // Returns a summary json. Must run in dripcore task context.
    dripcore::awaitable<json> connect(const std::string& name);

    // Connect all configured servers. Returns an array of summaries.
    dripcore::awaitable<json> connect_all();

    // Status of all configured servers (for /mcp).
    json status() const;

    // Tool names for an available_tools entry suffix: "*" or "" for
    // all connected servers, otherwise the tools of one server.
    std::vector<std::string> tool_names(const std::string& server) const;

    // Combined agent instructions from connected servers (from the
    // initialize result). Empty when nothing is connected.
    const std::string& instructions() const { return instructions_; }

    // Bumped whenever tools are registered; agent tasks compare it
    // against their snapshot to detect registry refreshes.
    uint64_t registry_version() const { return registry_version_; }

  private:
    mcp_manager() = default;

    struct server_state
    {
      std::string url;
      std::shared_ptr<mcp_client> client;
      bool connected = false;
      std::vector<std::string> tool_names;
      std::string instructions;
      std::string error;
    };

    void rebuild_instructions();

    std::map<std::string, server_state> servers_;
    std::string instructions_;
    uint64_t registry_version_ = 0;
  };

} // namespace mistral::mcp

#endif // MISTRAL_MCP_MANAGER_H
