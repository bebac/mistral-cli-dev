#include "agent/subagent_task.h"
#include "agent/agent_system.h"

#include <iostream>

namespace mistral::agent
{

  dripcore::awaitable<> subagent_task::body()
  {
    while (true)
    {
      auto input = co_await input_ch_->recv(this);
      if (!input) break;

      chat_.add_user_message(*input);
      co_await agent_loop();
    }
    co_return;
  }

  void subagent_task::thinking_begin()
  {
    // No-op for subagents
  }

  void subagent_task::thinking_end()
  {
    // No-op for subagents
  }

  void subagent_task::agent_loop_on_sending_chat_request()
  {
    // No-op for subagents
  }

  void subagent_task::agent_loop_on_message_content(const api::message_content& mc)
  {
    // No-op for subagents - content is accumulated automatically by base class
  }

  auto subagent_task::agent_loop_do_tool_call(const api::tool_call& tc) -> dripcore::awaitable<void>
  {
    // Parse tool arguments
    api::json tool_args;
    try
    {
      if (!tc.function.arguments.empty())
      {
        tool_args = api::json::parse(tc.function.arguments);
      }
    }
    catch (const std::exception &e)
    {
      std::string error_msg = std::string("Failed to parse tool arguments: ") + e.what();
      chat_.add_tool_result(tc.function.name, tc.id,
        build_tool_rejection("error", error_msg).dump());
      co_return;
    }

    auto policy_result = evaluate_tool_policy(tc, tool_args);

    if (policy_result.decision == policy_decision::DENY)
    {
      std::string rejection_msg = policy_result.rejection_message
          ? *policy_result.rejection_message
          : "Tool call denied by policy";
      chat_.add_tool_result(tc.function.name, tc.id,
        build_tool_rejection("denied", rejection_msg).dump());
      co_return;
    }

    if (policy_result.decision == policy_decision::REQUIRE_APPROVAL)
    {
      // Subagents don't have approval workflow - deny with message
      std::string rejection_msg = policy_result.rejection_message
          ? *policy_result.rejection_message
          : "Tool call requires approval";
      std::string reason = policy_result.reason
          ? *policy_result.reason
          : "Policy requires approval";
      chat_.add_tool_result(tc.function.name, tc.id,
        build_tool_rejection("rejected",
          rejection_msg + " (" + reason + ")").dump());
      co_return;
    }

    // Execute the tool
    co_await execute_and_record_tool(tc);
    co_return;
  }

  void subagent_task::agent_loop_on_error(const std::string& error)
  {
    // Log error but continue - errors will be in accumulated content
    // and returned via output_ch by base class
  }

} // namespace mistral::agent