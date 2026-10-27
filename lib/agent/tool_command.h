#ifndef MISTRAL_TOOL_COMMAND_H
#define MISTRAL_TOOL_COMMAND_H

#include "slash_command/slash_command.h"
#include <dripcore/awaitable.h>
#include <dripcore/task.h>

namespace mistral::agent
{
  /**
   * @brief Slash command to call a tool directly for debugging.
   *
   * Usage: /tool <tool_name> <json_arguments>
   * Example: /tool ls {"path": "."}
   * Example: /tool grep {"pattern": "hello", "path": "src"}
   */
  class tool_command : public slash_command::slash_command
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

#endif // MISTRAL_TOOL_COMMAND_H
