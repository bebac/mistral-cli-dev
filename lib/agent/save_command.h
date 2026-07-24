#ifndef MISTRAL_SAVE_COMMAND_H
#define MISTRAL_SAVE_COMMAND_H

#include "console/slash_command.h"

namespace mistral::agent
{
  /**
   * @brief Slash command to save the last AI response to a file.
   *
   * Usage: /save [filename]
   * - If filename is not provided, defaults to "last_response.txt"
   * - Creates parent directories if needed
   * - Overwrites existing files
   */
  class save_command : public console::slash_command
  {
  public:
    std::string name() const override;
    std::string description() const override;
    std::string usage() const override;
    execution_context context() const override;

    std::string execute_agent(
        const std::string& args,
        agent::agent_system& agent_sys
    ) override;
  };
} // namespace mistral::console

#endif // MISTRAL_SAVE_COMMAND_H
