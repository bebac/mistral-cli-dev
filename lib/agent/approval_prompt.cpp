#include "agent/approval_prompt.h"

#include <sstream>
#include <cctype>

namespace mistral::agent
{

// Build approval prompt string for display
std::string build_approval_prompt(const std::string& tool_name, const json& tool_arguments, const std::string& reason)
{
  std::ostringstream oss;
  oss << "\n[APPROVAL REQUIRED]\n";
  oss << "  Tool: " << tool_name << "\n";

  // Format and display tool arguments
  if (!tool_arguments.empty() && tool_arguments.is_object())
  {
    oss << "  Arguments:\n";
    for (auto it = tool_arguments.begin(); it != tool_arguments.end(); ++it)
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

      oss << "    " << key << " = " << value_str << "\n";
    }
  }

  oss << "\n  Reason: " << reason << "\n";
  oss << "  Approve? [y/n/r/a/?]: ";
  return oss.str();
}

// Get the approval help message
std::string get_approval_help_message()
{
  return "\n  Approval Options:\n"
         "    y - Approve this specific call\n"
         "    n - Reject this call\n"
         "    r - Reject with custom message for AI\n"
         "    a - Approve all similar calls for this session\n"
         "    ? - Show this help\n"
         "  Approve? [y/n/r/a/?]: ";
}

// Parse single-character approval response
// Returns std::nullopt if the input is a help request ('?')
std::optional<approval_action> parse_approval_response(const std::string& input)
{
  if (input.empty()) {
    return approval_action::REJECT;
  }

  char first_char = std::tolower(static_cast<unsigned char>(input[0]));

  if (first_char == 'y')
  {
    return approval_action::APPROVE;
  }
  else if (first_char == 'n')
  {
    return approval_action::REJECT;
  }
  else if (first_char == 'r')
  {
    return approval_action::REJECT_WITH_MESSAGE;
  }
  else if (first_char == 'a')
  {
    return approval_action::APPROVE_ALL;
  }
  else if (first_char == '?')
  {
    // Help request - return nullopt to signal caller to show help
    return std::nullopt;
  }

  // Default to reject for unrecognized input
  return approval_action::REJECT;
}

} // namespace mistral::agent
