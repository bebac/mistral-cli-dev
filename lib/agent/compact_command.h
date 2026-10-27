#ifndef MISTRAL_COMPACT_COMMAND_H
#define MISTRAL_COMPACT_COMMAND_H

#include "slash_command/slash_command.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace mistral::agent
{
  // Control message prefix used to route compaction requests from the
  // /compact slash command to the interactive agent task. The command
  // runs in the slash command executor and has no access to the
  // conversation, so it forwards the request through the agent's input
  // channel; the interactive task intercepts messages starting with
  // this prefix instead of adding them to the conversation.
  inline constexpr std::string_view compact_request_prefix = "[COMPACT]";

  // Default number of recent turns kept verbatim by /compact.
  inline constexpr size_t default_compact_keep_turns = 4;

  /**
   * @brief Slash command to compact the conversation history.
   *
   * Usage: /compact [keep]
   * - Requests compaction of the conversation history: the evicted part
   *   is summarized via a side API request and replaced with a single
   *   synthetic summary turn.
   * - keep: number of recent turns to keep verbatim (default 4).
   */
  class compact_command : public slash_command::slash_command
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

#endif // MISTRAL_COMPACT_COMMAND_H
