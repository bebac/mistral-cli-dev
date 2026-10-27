#include "agent/usage_command.h"
#include "agent/agent_system.h"

#include <format>
#include <string>

namespace mistral::agent
{
  std::string usage_command::name() const
  {
    return "usage";
  }

  std::string usage_command::description() const
  {
    return "Show API usage statistics";
  }

  std::string usage_command::usage() const
  {
    return "/usage [reset]";
  }

  dripcore::awaitable<std::string> usage_command::execute(
    dripcore::task&,
    const std::string& args,
    agent::agent_system& agent_sys
  )
  {
    if (args == "reset")
    {
      agent_sys.reset_usage_stats();
      co_return "Usage statistics reset to zero.";
    }

    auto stats = agent_sys.get_usage_stats();

    co_return stats.dump(2);
  }
} // namespace mistral::agent
