#include "agent/approval_manager.h"
#include "common/string_utils.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <iostream>
#include <sstream>
#include <regex>

namespace mistral::agent
{

namespace
{
  using namespace mistral::utils;
}

// ---- approval_manager Implementation ----

approval_manager::approval_manager() = default;

void approval_manager::add_session_approval(const std::string& tool_name, const json& argument_pattern)
{
  session_approval approval;
  approval.tool_name = tool_name;
  approval.argument_pattern = argument_pattern;

  // Extract path from arguments if present and create regex
  if (argument_pattern.contains("path") && argument_pattern["path"].is_string())
  {
    std::string path = argument_pattern["path"].get<std::string>();
    // Convert path to regex pattern
    // For now, we'll do exact matching or prefix matching based on directory
    // This is a simple implementation - can be enhanced later
    std::string regex_pattern = escape_regex(path);

    // If path ends with /, match any subpath
    if (!path.empty() && path.back() == '/')
    {
      regex_pattern += ".*";
    }

    //try
    //{
      approval.path_regex = std::regex(regex_pattern, std::regex::ECMAScript | std::regex::optimize);
    //}
    //catch (const std::regex_error&)
    //{
    //  // If regex compilation fails, use a simple string match
    //  approval.path_regex = std::regex();
    //}
  }
  else
  {
    approval.path_regex = std::regex();
  }

  session_approvals_.push_back(std::move(approval));
}

bool approval_manager::is_session_approved(const std::string& tool_name, const json& arguments) const
{
  for (const auto& approval : session_approvals_)
  {
    // Check tool name match
    if (approval.tool_name != tool_name)
    {
      continue;
    }

    // Check if arguments match the pattern
    if (arguments_match(arguments, approval))
    {
      return true;
    }
  }
  return false;
}

bool approval_manager::arguments_match(const json& arguments, const session_approval& approval) const
{
  // If the approval has an empty pattern, it matches any arguments for this tool
  if (approval.argument_pattern.empty())
  {
    return true;
  }

  // Check each key in the pattern
  for (auto it = approval.argument_pattern.begin(); it != approval.argument_pattern.end(); ++it)
  {
    std::string key = it.key();
    json pattern_value = it.value();

    // If the arguments don't have this key, no match
    if (!arguments.contains(key))
    {
      return false;
    }

    json arg_value = arguments[key];

    // Special handling for path with regex
    //if (key == "path" && !approval.path_regex.empty()) // std::regex does not have an empty member!!!
    if (key == "path")
    {
      if (arg_value.is_string())
      {
        std::string path = arg_value.get<std::string>();
        if (!std::regex_search(path, approval.path_regex))
        {
          return false;
        }
      }
      else
      {
        return false;
      }
    }
    // For other fields, do exact match
    else if (pattern_value != arg_value)
    {
      return false;
    }
  }

  return true;
}

const std::vector<session_approval>& approval_manager::get_session_approvals() const
{
  return session_approvals_;
}

void approval_manager::clear_session_approvals()
{
  session_approvals_.clear();
}

} // namespace mistral::agent
