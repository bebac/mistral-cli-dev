#ifndef MISTRAL_LOAD_COMMAND_H
#define MISTRAL_LOAD_COMMAND_H

#include "slash_command/slash_command.h"
#include <dripcore/awaitable.h>
#include <dripcore/task.h>

namespace mistral::agent
{
  /**
   * @brief Slash command to load a file and set it as the last response.
   *
   * Usage: /load [filename]
   * - If filename is not provided, defaults to "last_response.txt"
   * - Sets the loaded content as the last response in the agent system
   */
  class load_command : public slash_command::slash_command
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

#endif // MISTRAL_LOAD_COMMAND_H
