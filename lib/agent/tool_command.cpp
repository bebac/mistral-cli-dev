#include "agent/tool_command.h"
#include "agent/agent_system.h"
#include "agent/tool_registry.h"

#include <dripcore/task.h>
#include <nlohmann/json.hpp>

#include <utility>

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

  slash_command::command_spec tool_command::spec() const
  {
    using namespace mistral::slash_command;

    arg_spec tool_name;
    tool_name.name = "tool_name";
    tool_name.description = "tool to call";
    tool_name.required = true;
    // Completion offers the tools available to the current agent. Execution
    // stays unrestricted, so a tool outside that set can still be typed.
    tool_name.completer = [](const std::string&)
    {
      std::vector<completion_item> items;
      for (const auto& name : agent_system::instance().current_agent_tool_names())
      {
        items.push_back({name, ""});
      }
      return items;
    };

    arg_spec json_arguments;
    json_arguments.name = "json_arguments";
    json_arguments.description = "JSON arguments";
    json_arguments.required = true;

    command_spec out;
    out.name = name();
    out.summary = description();
    out.args.push_back(std::move(tool_name));
    out.args.push_back(std::move(json_arguments));
    return out;
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
