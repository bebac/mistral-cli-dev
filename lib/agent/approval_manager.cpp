#include "agent/approval_manager.h"

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
  // Trim whitespace from both ends
  std::string trim(const std::string& s)
  {
    auto is_not_space = [](unsigned char c) { return !std::isspace(c); };
    auto start = std::find_if(s.begin(), s.end(), is_not_space);
    auto end = std::find_if(s.rbegin(), s.rend(), is_not_space).base();
    return (start < end) ? std::string(start, end) : std::string();
  }

  // Convert to lowercase
  std::string to_lower(const std::string& s)
  {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
  }

  // Escape special regex characters in a string
  std::string escape_regex(const std::string& s)
  {
    std::string result;
    for (char c : s)
    {
      if (c == '.' || c == '^' || c == '$' || c == '*' || c == '+' || c == '?' ||
          c == '(' || c == ')' || c == '[' || c == ']' || c == '{' || c == '}' ||
          c == '|' || c == '\\')
      {
        result += '\\';
      }
      result += c;
    }
    return result;
  }

} // anonymous namespace

// ---- approval_manager Implementation ----

approval_manager::approval_manager() = default;

approval_response approval_manager::prompt_user(const approval_request& request)
{
  // Check if this tool call is already session-approved
  if (is_session_approved(request.tool_name, request.tool_arguments))
  {
    return {approval_action::APPROVE, std::nullopt};
  }

  // Display the approval prompt
  display_prompt(request);

  while (true)
  {
    std::string input = read_user_input();
    input = trim(to_lower(input));

    if (input.empty())
    {
      continue;  // Skip empty input
    }

    if (input == "?" || input == "help")
    {
      display_help();
      continue;
    }

    approval_action action = parse_input(input);

    switch (action)
    {
      case approval_action::APPROVE:
        return {action, std::nullopt};

      case approval_action::REJECT:
        return {action, std::nullopt};

      case approval_action::REJECT_WITH_MESSAGE:
      {
        std::cout << "Enter message for AI (or press Enter for default): " << std::flush;
        std::string message = read_user_input();
        message = trim(message);
        if (message.empty() && request.suggested_rejection_message)
        {
          message = *request.suggested_rejection_message;
        }
        return {action, message};
      }

      case approval_action::APPROVE_ALL:
      {
        // Add session approval for this pattern
        add_session_approval(request.tool_name, request.tool_arguments);
        std::cout << "  → Session approval added for this pattern" << std::endl;
        return {approval_action::APPROVE, std::nullopt};
      }
    }
  }
}

void approval_manager::display_prompt(const approval_request& request)
{
  std::cout << "\n" << std::endl;
  std::cout << "[APPROVAL REQUIRED]" << std::endl;
  std::cout << "  Tool: " << request.tool_name << std::endl;

  // Format and display tool arguments
  if (!request.tool_arguments.empty() && request.tool_arguments.is_object())
  {
    std::cout << "  Arguments:" << std::endl;
    for (auto it = request.tool_arguments.begin(); it != request.tool_arguments.end(); ++it)
    {
      std::string key = it.key();
      json value = it.value();

      // Format the value
      std::string value_str;
      if (value.is_string())
      {
        value_str = value.get<std::string>();
        // Truncate long strings
        if (value_str.length() > 60)
        {
          value_str = value_str.substr(0, 57) + "...";
        }
      }
      else if (value.is_boolean())
      {
        value_str = value.get<bool>() ? "true" : "false";
      }
      else if (value.is_number())
      {
        if (value.is_number_integer())
        {
          value_str = std::to_string(value.get<int64_t>());
        }
        else
        {
          value_str = std::to_string(value.get<double>());
        }
      }
      else
      {
        value_str = value.dump();
        if (value_str.length() > 60)
        {
          value_str = value_str.substr(0, 57) + "...";
        }
      }

      std::cout << "    " << key << " = " << value_str << std::endl;
    }
  }

  std::cout << "\n  Reason: " << request.reason << std::endl;
  std::cout << "\n  Approve? [y/n/r/a/?]: " << std::flush;
}

void approval_manager::display_help() const
{
  std::cout << "\n  Approval Options:" << std::endl;
  std::cout << "    y/yes     - Approve this specific call" << std::endl;
  std::cout << "    n/no      - Reject this call" << std::endl;
  std::cout << "    r/reject  - Reject with custom message for AI" << std::endl;
  std::cout << "    a/allow   - Approve all similar calls for this session" << std::endl;
  std::cout << "    ?/help    - Show this help" << std::endl;
  std::cout << "\n  Approve? [y/n/r/a/?]: " << std::flush;
}

std::string approval_manager::read_user_input() const
{
  std::string line;
  if (!std::getline(std::cin, line))
  {
    if (std::cin.eof())
    {
      throw std::runtime_error("EOF encountered while reading user input");
    }
    throw std::runtime_error("Error reading user input");
  }
  return line;
}

approval_action approval_manager::parse_input(const std::string& input) const
{
  if (input == "y" || input == "yes")
  {
    return approval_action::APPROVE;
  }
  else if (input == "n" || input == "no")
  {
    return approval_action::REJECT;
  }
  else if (input == "r" || input == "reject")
  {
    return approval_action::REJECT_WITH_MESSAGE;
  }
  else if (input == "a" || input == "allow" || input == "all")
  {
    return approval_action::APPROVE_ALL;
  }
  // Default to reject for unrecognized input
  return approval_action::REJECT;
}

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
