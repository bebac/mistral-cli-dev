#ifndef MISTRAL_USAGE_COMMAND_H
#define MISTRAL_USAGE_COMMAND_H

#include "console/slash_command.h"

namespace mistral::agent
{
  struct usage_stats;

  /**
   * @brief Slash command to display API usage statistics.
   */
  class usage_command : public console::slash_command
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
} // namespace mistral::agent

#endif // MISTRAL_USAGE_COMMAND_H
