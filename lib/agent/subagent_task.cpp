#include "agent/subagent_task.h"
#include "agent/agent_system.h"

#include <iostream>

namespace mistral::agent
{

  dripcore::task_handle subagent_task::execute()
  {
    while (true)
    {
      auto input = co_await input_ch_->recv();
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
      tool_args = api::json::object();
    }

    // Check policy
    policy_context ctx{
      .tool_name = tc.function.name,
      .tool_arguments = tool_args,
      .working_directory = ""
    };

    auto policy_result = policy_engine_.evaluate(ctx);

    if (policy_result.decision == policy_decision::DENY)
    {
      std::string rejection_msg = policy_result.rejection_message
          ? *policy_result.rejection_message
          : "Tool call denied by policy";

      api::json rejection_result = {
        {"status", "denied"},
        {"message", rejection_msg}
      };
      chat_.add_tool_result(tc.function.name, tc.id, rejection_result.dump());
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

      api::json rejection_result = {
        {"status", "rejected"},
        {"message", rejection_msg + " (" + reason + ")"}
      };
      chat_.add_tool_result(tc.function.name, tc.id, rejection_result.dump());
      co_return;
    }

    // Execute the tool
    auto res = execute_tool(tc);
    chat_.add_tool_result(tc.function.name, tc.id, res);
    co_return;
  }

  void subagent_task::agent_loop_on_error(const std::string& error)
  {
    // Log error but continue - errors will be in accumulated content
    // and returned via output_ch by base class
  }

} // namespace mistral::agent