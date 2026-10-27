#ifndef MISTRAL_MCP_COMMAND_H
#define MISTRAL_MCP_COMMAND_H

#include "slash_command/slash_command.h"
#include <dripcore/awaitable.h>
#include <dripcore/task.h>

namespace mistral::agent
{
  /**
   * @brief Slash command to manage MCP servers.
   *
   * Usage: /mcp                  - status of configured servers
   *        /mcp connect          - connect all configured servers
   *        /mcp connect <name>  - connect one server
   *        /mcp tools           - list registered MCP tools
   */
  class mcp_command : public slash_command::slash_command
  {
  public:
    std::string name() const override;
    std::string description() const override;
    std::string usage() const override;
    dripcore::awaitable<std::string> execute(
      dripcore::task& task,
      const std::string& args,
      agent::agent_system& agent_sys
    ) override;
  };
} // namespace mistral::agent

#endif // MISTRAL_MCP_COMMAND_H
