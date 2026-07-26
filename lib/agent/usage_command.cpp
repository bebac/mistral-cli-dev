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

  console::slash_command::execution_context usage_command::context() const
  {
    return execution_context::AGENT;
  }

  std::string usage_command::execute_agent(
    const std::string &args,
    agent::agent_system &agent_sys)
  {
    if (args == "reset")
    {
      agent_sys.reset_usage_stats();
      return "Usage statistics reset to zero.";
    }

    auto stats = agent_sys.get_usage_stats();

    return stats.dump(2);
  }
} // namespace mistral::agent