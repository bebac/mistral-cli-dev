#ifndef MISTRAL_INTERACTIVE_AGENT_TASK_H
#define MISTRAL_INTERACTIVE_AGENT_TASK_H

#include "agent/agent_task_base.h"
#include "agent/conversation.h"
#include "agent/tool_registry.h"
#include "agent/policy_engine.h"
#include "agent/approval_manager.h"
#include "agent/approval_prompt.h"
#include "agent/agent_system.h"
#include "agent/agent_config.h"
#include "agent/render_sink.h"
#include "api/mistral_api.h"
#include "api/mistral_types.h"
#include "common/string_utils.h"

#include <dripcore/task.h>
#include <dripcore/channel.h>
#include <dripcore/task_handle.h>

#include <memory>
#include <string>
#include <variant>
#include <vector>
#include <cctype>

namespace mistral::agent
{
  class interactive_agent_task : public agent_task_base
  {
  public:
    interactive_agent_task(
      const std::string& agent_id,
      std::shared_ptr<dripcore::channel<std::string>> input_ch,
      std::shared_ptr<dripcore::channel<std::string>> output_ch = nullptr,
      std::shared_ptr<dripcore::channel<std::string>> prompt_ch = nullptr,
      std::shared_ptr<render_sink> sink = nullptr
    )
      : agent_task_base(agent_id, input_ch, output_ch, prompt_ch)
      , sink_(std::move(sink))
    {
    }

    dripcore::awaitable<> body() override
    {
      auto tools_schema = tools_.get_schema();

      while (true)
      {
        // Signal console to show prompt.
        if (prompt_ch_) {
          prompt_ch_->try_send("mistral");
        }

        // Wait for user input.
        auto input = co_await input_ch_->recv(this);

        if (!input || input->empty()) {
          if (!input) break;  // Channel closed (EOF).
          continue;           // Empty input, ask again.
        }

        chat_.add_user_message(*input);

        // Process message through agent loop
        co_await agent_loop();
      }

      co_return;
    }

    dripcore::awaitable<> teardown() override
    {
      request_ch_->close();

      if (output_ch_) {
        output_ch_->close();
      }

      if (stats_ch_) {
        stats_ch_->close();
      }

      if (prompt_ch_) {
        prompt_ch_->close();
      }

      agent_system::instance().signal_shutdown();

      co_return;
    }

  protected:
    void thinking_begin() override
    {
      sink_->thinking_begin();
      in_thinking_ = true;
    }

    void thinking_end() override
    {
      sink_->thinking_end();
      in_thinking_ = false;
    }

    void agent_loop_on_sending_chat_request() override
    {
      chat_.save(".mistral-cli", session_id_);
    }

    void agent_loop_on_message_content(const api::message_content& mc) override
    {
      if (mc.type == "thinking")
      {
        if (!in_thinking_) {
          thinking_begin();
        }
        sink_->output_thinking(mc.content);
      }
      else if (mc.type == "text")
      {
        if (in_thinking_) {
          thinking_end();
        }
        sink_->output(mc.content);
      }
    }

    auto agent_loop_do_tool_call(const api::tool_call& tc) -> dripcore::awaitable<void> override
    {
      sink_->output_tool_call(tc.function.name, tc.function.arguments);

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
        sink_->output_warning(std::string("Failed to parse tool arguments: ") + e.what());
        tool_args = api::json::object();
      }

      auto policy_result = evaluate_tool_policy(tc, tool_args);

      if (policy_result.decision == policy_decision::DENY)
      {
        std::string rejection_msg = policy_result.rejection_message
          ? *policy_result.rejection_message
          : "Tool call denied by policy";
        sink_->output_line(" \xe2\x86\x92 [DENIED] " + rejection_msg);

        chat_.add_tool_result(tc.function.name, tc.id,
          build_tool_rejection("denied", rejection_msg).dump());
        co_return;
      }

      if (policy_result.decision == policy_decision::REQUIRE_APPROVAL)
      {
        if (approval_manager_.is_session_approved(tc.function.name, tool_args))
        {
          sink_->output_line(" \xe2\x86\x92 [SESSION APPROVED]");
        }
        else
        {
          std::string approval_prompt = build_approval_prompt(tc.function.name, tool_args,
            policy_result.reason ? *policy_result.reason : "Policy requires approval");

          if (prompt_ch_) {
            prompt_ch_->try_send("[APPROVAL_PROMPT]" + approval_prompt);
          }

          auto response = co_await input_ch_->recv(this);
          if (!response) {
            sink_->output_line(" \xe2\x86\x92 [CANCELLED] Approval prompt cancelled");
            co_return;
          }

          auto parsed_action = parse_approval_response(*response);
          std::optional<std::string> custom_message = std::nullopt;

          if (!parsed_action.has_value())
          {
            if (prompt_ch_) {
              prompt_ch_->try_send("[APPROVAL_PROMPT]" + get_approval_help_message());
            }

            response = co_await input_ch_->recv(this);
            if (!response) {
              sink_->output_line(" \xe2\x86\x92 [CANCELLED] Approval prompt cancelled");
              co_return;
            }
            parsed_action = parse_approval_response(*response);
            if (!parsed_action.has_value()) {
              parsed_action = approval_action::REJECT;
            }
          }

          approval_action action = *parsed_action;

          if (action == approval_action::REJECT_WITH_MESSAGE)
          {
            std::string message_prompt = "  Enter message for AI (or press Enter for default): ";
            if (prompt_ch_) {
              prompt_ch_->try_send(message_prompt);
            }

            auto message_response = co_await input_ch_->recv(this);
            if (message_response && !message_response->empty()) {
              custom_message = *message_response;
            } else if (policy_result.rejection_message) {
              custom_message = *policy_result.rejection_message;
            } else {
              custom_message = "Tool call rejected by user";
            }
          }

          if (action == approval_action::REJECT || action == approval_action::REJECT_WITH_MESSAGE)
          {
            std::string rejection_msg = custom_message
              ? *custom_message
              : (policy_result.rejection_message
                 ? *policy_result.rejection_message
                 : "Tool call rejected by user");
            sink_->output_line(" \xe2\x86\x92 [REJECTED] " + rejection_msg);

            chat_.add_tool_result(tc.function.name, tc.id,
              build_tool_rejection("rejected", rejection_msg).dump());
            co_return;
          }

          if (action == approval_action::APPROVE_ALL)
          {
            approval_manager_.add_session_approval(tc.function.name, tool_args);
            sink_->output_line(" \xe2\x86\x92 [APPROVED ALL] Session approval added");
          }
          else
          {
            sink_->output_line(" \xe2\x86\x92 [APPROVED]");
          }
        }
      }

      auto res = co_await execute_and_record_tool(tc);
      sink_->output_tool_result(res);
      co_return;
    }

    void agent_loop_on_error(const std::string& error) override
    {
      sink_->output_error(error);
    }

    void agent_loop_on_completed(std::vector<api::message_content>& accumulated_content) override
    {
      if (!accumulated_content.empty()) {
        chat_.save(".mistral-cli", session_id_);
      }
      agent_task_base::agent_loop_on_completed(accumulated_content);
      sink_->completed();
    }

    void agent_loop_on_cancelled() override
    {
      if (in_thinking_) {
        thinking_end();
      }
      sink_->cancelled();
    }

  private:
    std::shared_ptr<render_sink> sink_;
  };
} // namespace mistral::agent

#endif // MISTRAL_INTERACTIVE_AGENT_TASK_H