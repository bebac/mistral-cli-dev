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

    // Prompt user for approval of a tool call
    // Returns the user's decision
    approval_response prompt_user(const approval_request& request);

    // Add a session-level approval for a specific tool call pattern
    void add_session_approval(const std::string& tool_name, const json& argument_pattern);

    // Check if a tool call matches any session-level approval
    bool is_session_approved(const std::string& tool_name, const json& arguments) const;

    // Get all active session approvals (for display/logging)
    const std::vector<session_approval>& get_session_approvals() const;

    // Clear all session approvals
    void clear_session_approvals();

  private:
    // Display the approval prompt to the user
    void display_prompt(const approval_request& request);

    // Display help for approval options
    void display_help() const;

    // Read user input from stdin
    std::string read_user_input() const;

    // Parse user input into approval action
    approval_action parse_input(const std::string& input) const;

    // Check if arguments match a session approval pattern
    bool arguments_match(const json& arguments, const session_approval& approval) const;

    // Session approvals storage
    std::vector<session_approval> session_approvals_;
  };

} // namespace mistral::agent

#endif // MISTRAL_APPROVAL_MANAGER_H
