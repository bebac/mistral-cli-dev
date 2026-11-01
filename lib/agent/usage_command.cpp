#include "agent/usage_command.h"
#include "agent/agent_system.h"

#include <format>
#include <string>
#include <utility>

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

  slash_command::command_spec usage_command::spec() const
  {
    using namespace mistral::slash_command;

    arg_spec reset;
    reset.name = "reset";
    reset.description = "reset the usage counters";
    reset.source = candidate_source::literal;
    reset.literals = {{"reset", "reset the usage counters"}};

    command_spec out;
    out.name = name();
    out.summary = description();
    out.args.push_back(std::move(reset));
    return out;
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
