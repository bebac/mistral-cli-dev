#ifndef MISTRAL_APPROVAL_MANAGER_H
#define MISTRAL_APPROVAL_MANAGER_H

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>
#include <map>
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

  // ---- Approval Manager ----

  class approval_manager
  {
  public:
    approval_manager();

    // Add a session-level approval for a specific tool call pattern
    void add_session_approval(const std::string& tool_name, const json& argument_pattern);

    // Check if a tool call matches any session-level approval
    bool is_session_approved(const std::string& tool_name, const json& arguments) const;

    // Get all active session approvals (for display/logging)
    const std::vector<session_approval>& get_session_approvals() const;

    // Clear all session approvals
    void clear_session_approvals();

  private:
    // Check if arguments match a session approval pattern
    bool arguments_match(const json& arguments, const session_approval& approval) const;

    // Session approvals storage
    std::vector<session_approval> session_approvals_;
  };

} // namespace mistral::agent

#endif // MISTRAL_APPROVAL_MANAGER_H
