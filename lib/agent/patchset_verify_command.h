#ifndef MISTRAL_PATCHSET_VERIFY_COMMAND_H
#define MISTRAL_PATCHSET_VERIFY_COMMAND_H

#include "console/slash_command.h"

namespace mistral::agent
{
  /**
   * @brief Slash command to verify patchset parsing from AI responses.
   *
   * Usage: /patchset_verify
   * - Parses the last AI response for patchset edit blocks
   * - Validates patchset ID and edit blocks
   * - Returns structured verification results
   */
  class patchset_verify_command : public console::slash_command
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

#endif // MISTRAL_PATCHSET_VERIFY_COMMAND_H