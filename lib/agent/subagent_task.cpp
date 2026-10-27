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
      if (!input)
        break;

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
    catch (const std::exception& e)
    {
      std::string error_msg = std::string("Failed to parse tool arguments: ") + e.what();
      chat_.add_tool_result(tc.function.name, tc.id, build_tool_rejection("error", error_msg).dump());
      co_return;
    }

    if (auto precheck_error = precheck_tool_call(tc, tool_args))
    {
      chat_.add_tool_result(tc.function.name, tc.id, build_tool_rejection("error", *precheck_error).dump());
      co_return;
    }

    auto policy_result = evaluate_tool_policy(tc, tool_args);

    if (policy_result.decision == policy_decision::DENY)
    {
      std::string rejection_msg =
        policy_result.rejection_message ? *policy_result.rejection_message : "Tool call denied by policy";
      chat_.add_tool_result(tc.function.name, tc.id, build_tool_rejection("denied", rejection_msg).dump());
      co_return;
    }

    if (policy_result.decision == policy_decision::REQUIRE_APPROVAL)
    {
      // Subagents don't have approval workflow - deny with message
      std::string rejection_msg =
        policy_result.rejection_message ? *policy_result.rejection_message : "Tool call requires approval";
      std::string reason = policy_result.reason ? *policy_result.reason : "Policy requires approval";
      chat_.add_tool_result(
        tc.function.name, tc.id, build_tool_rejection("rejected", rejection_msg + " (" + reason + ")").dump()
      );
      co_return;
    }

    // Execute the tool
    co_await execute_and_record_tool(tc);
    co_return;
  }

  void subagent_task::agent_loop_on_error(const std::string& error)
  {
    // Record the error so it can be surfaced to the caller when the
    // turn ends without content.
    last_error_ = error;
  }

  void subagent_task::agent_loop_on_completed(std::vector<api::message_content>& accumulated_content)
  {
    // A failed turn must not look like an empty answer: surface the
    // recorded error as the subagent's response.
    if (accumulated_content.empty() && !last_error_.empty())
    {
      if (output_ch_)
      {
        std::string message = "Error: " + last_error_;
        if (last_error_.find("connector is not supported") != std::string::npos)
        {
          message += " (hint: the builtin tool's connector is not enabled "
                     "for this Mistral workspace — check the Mistral console "
                     "or plan, or switch the server_agent tool type, e.g. "
                     "web_search_premium)";
        }
        output_ch_->try_send(message);
      }
      return;
    }
    agent_task_base::agent_loop_on_completed(accumulated_content);
  }

  auto subagent_task::agent_loop() -> dripcore::awaitable<void>
  {
    last_error_.clear();
    co_await agent_task_base::agent_loop();
  }

} // namespace mistral::agent
