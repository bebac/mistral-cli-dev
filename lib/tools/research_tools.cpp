#include "tools/research_tools.h"

namespace mistral::tools
{
  json research_tools::web_search(std::string query)
  {
    (void)query;
    return {
      {"error",
       "web_search is backed by a server-side researcher agent and is "
       "executed by the agent loop. Configure it on the calling agent: "
       "\"subagent_tools\": {\"web_search\": \"researcher\"} (and add "
       "\"web_search\" to available_tools)."}
    };
  }
} // namespace mistral::tools
