#ifndef MISTRAL_APPROVAL_MANAGER_H
#define MISTRAL_APPROVAL_MANAGER_H

#include "approval_types.h"

#include <map>
#include <vector>

namespace mistral::agent
{

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
