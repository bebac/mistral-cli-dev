#ifndef MISTRAL_MODEL_COMMAND_H
#define MISTRAL_MODEL_COMMAND_H

#include "slash_command/slash_command.h"

#include <string_view>

namespace mistral::agent
{
  // Control message prefix used to route model switches from the /model
  // slash command to the interactive agent task (see compact_command.h for
  // the same routing pattern). The command runs in the slash command
  // executor and has no access to the conversation, so it forwards the
  // request through the agent's input channel; the interactive task
  // intercepts messages starting with this prefix instead of adding them
  // to the conversation.
  inline constexpr std::string_view model_request_prefix = "[MODEL]";

  /**
   * @brief Slash command to show and switch the active model mid-session.
   *
   * Usage: /model [model_id]
   * - Without arguments: lists the model ids offered by the current
   *   agent's model entry, marking the active one.
   * - With an id: switches subsequent requests to that id. The
   *   conversation history is kept.
   */
  class model_command : public slash_command::slash_command
  {
  public:
    std::string name() const override;
    std::string description() const override;
    mistral::slash_command::command_spec spec() const override;
    dripcore::awaitable<std::string> execute(
      dripcore::task& task,
      const std::string& args,
      agent::agent_system& agent_sys
    ) override;
  };
} // namespace mistral::agent

#endif // MISTRAL_MODEL_COMMAND_H