#ifndef MISTRAL_RESEARCH_TOOLS_H
#define MISTRAL_RESEARCH_TOOLS_H

#include <nlohmann/json.hpp>

#include <string>

namespace mistral::tools
{
  using json = nlohmann::ordered_json;

  // Client-side face of the web_search tool. The actual research runs in
  // a server-side agent (Mistral Agents API with the web_search builtin
  // tool); the agent loop routes web_search calls to that subagent (see
  // agent_config::subagent_tools and agent_task_base::execute_tool). This
  // implementation only serves direct/debug invocation (e.g. /tool).
  class research_tools
  {
  public:
    // Search the web via the researcher subagent. Only reachable when the
    // calling agent has no subagent_tools routing for web_search.
    json web_search(std::string query);
  };
} // namespace mistral::tools

#endif // MISTRAL_RESEARCH_TOOLS_H
