#ifndef MISTRAL_APPROVAL_TYPES_H
#define MISTRAL_APPROVAL_TYPES_H

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <regex>

namespace mistral::agent
{
  using json = nlohmann::ordered_json;

  // ---- Approval Action Types ----

  enum class approval_action { APPROVE, REJECT, REJECT_WITH_MESSAGE, APPROVE_ALL };

  // ---- Approval Request ----

  struct approval_request
  {
    std::string tool_name;
    json tool_arguments;
    std::string reason;
    std::optional<std::string> suggested_rejection_message;
  };

  // ---- Approval Response ----

  struct approval_response
  {
    approval_action action;
    std::optional<std::string> custom_message;
  };

  // ---- Session Approval Pattern ----

  struct session_approval
  {
    std::string tool_name;
    json argument_pattern;  // JSON object describing the pattern that was approved
    std::regex path_regex;   // Compiled regex for path matching (if applicable)
  };

} // namespace mistral::agent

#endif // MISTRAL_APPROVAL_TYPES_H
