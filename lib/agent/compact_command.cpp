#include "agent/compact_command.h"
#include "agent/agent_system.h"

#include <dripcore/channel.h>

#include <charconv>
#include <string>

namespace mistral::agent
{
  std::string compact_command::name() const
  {
    return "compact";
  }

  std::string compact_command::description() const
  {
    return "Compact the conversation history into a summary";
  }

  std::string compact_command::usage() const
  {
    return "compact [keep]";
  }

  dripcore::awaitable<std::string> compact_command::execute(
    dripcore::task&,
    const std::string& args,
    agent::agent_system& agent_sys)
  {
    // Parse the optional keep argument: the number of recent turns to
    // preserve verbatim.
    std::string trimmed = args;
    trimmed.erase(0, trimmed.find_first_not_of(" \t"));
    trimmed.erase(trimmed.find_last_not_of(" \t") + 1);

    size_t keep_turns = default_compact_keep_turns;
    if (!trimmed.empty())
    {
      size_t value = 0;
      auto result = std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), value);
      if (result.ec != std::errc())
      {
        co_return "Error: /compact expects the number of turns to keep, e.g. /compact 6";
      }
      keep_turns = value;
    }

    // Route the request to the interactive agent task, which owns the
    // conversation and the API request machinery.
    auto input_ch = agent_sys.get_current_input_ch();
    if (!input_ch)
    {
      co_return "Error: no active conversation to compact";
    }

    if (!input_ch->try_send(std::string(compact_request_prefix) + std::to_string(keep_turns)))
    {
      co_return "Error: agent input is busy, try again later";
    }

    co_return "Compaction started...";
  }
} // namespace mistral::agent