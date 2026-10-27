#include "agent/approval_prompt.h"

#include <cctype>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>

namespace mistral::agent
{
  // Helper function to truncate strings longer than max_length
  std::string truncate_string(std::string str, size_t max_length)
  {
    if (str.length() > max_length)
    {
      str.resize(max_length - 3);
      str += "...";
    }
    return str;
  }

  // Build approval prompt string for display
  std::string build_approval_prompt(std::string_view tool_name, const json& tool_arguments, std::string_view reason)
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
        std::string_view key = it.key();
        const json& value = it.value();

        // Format the value
        std::string value_str;
        if (value.is_string())
        {
          value_str = value.get<std::string>();
          value_str = truncate_string(std::move(value_str), 60);
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
          value_str = truncate_string(std::move(value_str), 60);
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
  std::optional<approval_action> parse_approval_response(std::string_view input)
  {
    if (input.empty())
    {
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
