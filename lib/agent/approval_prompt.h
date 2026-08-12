#ifndef MISTRAL_APPROVAL_PROMPT_H
#define MISTRAL_APPROVAL_PROMPT_H

#include "approval_types.h"

#include <string>

namespace mistral::agent
{
  // Build approval prompt string for display
  std::string build_approval_prompt(std::string_view tool_name, const json& tool_arguments, std::string_view reason);

  // Get the approval help message
  std::string get_approval_help_message();

  // Parse single-character approval response
  // Returns std::nullopt if the input is a help request ('?')
  std::optional<approval_action> parse_approval_response(std::string_view input);
} // namespace mistral::agent

#endif // MISTRAL_APPROVAL_PROMPT_H
