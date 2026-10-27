#include "agent/tool_command.h"
#include "agent/agent_system.h"
#include "agent/tool_registry.h"

#include <dripcore/task.h>
#include <nlohmann/json.hpp>

namespace mistral::agent
{
  std::string tool_command::name() const
  {
    return "tool";
  }

  std::string tool_command::description() const
  {
    return "Call a tool directly for debugging";
  }

  std::string tool_command::usage() const
  {
    return "/tool <tool_name> <json_arguments>";
  }

  dripcore::awaitable<std::string> tool_command::execute(
    dripcore::task&,
    const std::string& args,
    agent::agent_system& agent_sys
  )
  {
    // Parse the command: /tool <name> <json_args>
    // Find first space to separate tool name from arguments
    size_t first_space = args.find(' ');
    if (first_space == std::string::npos)
    {
      co_return "Error: Missing tool name or arguments. Usage: /tool <tool_name> <json_arguments>";
    }

    std::string tool_name = args.substr(0, first_space);
    std::string json_args_str = args.substr(first_space + 1);

    // Trim whitespace
    tool_name.erase(0, tool_name.find_first_not_of(" \t"));
    tool_name.erase(tool_name.find_last_not_of(" \t") + 1);
    json_args_str.erase(0, json_args_str.find_first_not_of(" \t"));
    json_args_str.erase(json_args_str.find_last_not_of(" \t") + 1);

    if (tool_name.empty())
    {
      co_return "Error: Tool name cannot be empty";
    }

    if (json_args_str.empty())
    {
      co_return "Error: JSON arguments cannot be empty";
    }

    // Parse JSON arguments
    json json_args;
    try
    {
      json_args = json::parse(json_args_str);
    }
    catch (const std::exception& e)
    {
      co_return std::string("Error: Invalid JSON arguments: ") + e.what();
    }

    // Check if tool exists
    if (!agent_sys.global_tools().has_tool(tool_name))
    {
      co_return "Error: Tool '" + tool_name + "' not found. Use /tools to list available tools.";
    }

    // Build tool call JSON
    json tool_call = {{"function", {{"name", tool_name}, {"arguments", json_args.dump()}}}};

    // Execute the tool
    try
    {
      json result = co_await agent_sys.global_tools().dispatch(tool_call);
      if (result.is_string())
      {
        co_return result.get<std::string>();
      }
      co_return result.dump(2); // Pretty print with 2-space indent
    }
    catch (const std::exception& e)
    {
      co_return std::string("Error executing tool: ") + e.what();
    }
  }
} // namespace mistral::agent
