#ifndef MISTRAL_AGENT_TASK_H
#define MISTRAL_AGENT_TASK_H

#include "agent/agent_task_base.h"
#include "agent/conversation.h"
#include "agent/tool_registry.h"
#include "agent/policy_engine.h"
#include "agent/approval_manager.h"
#include "agent/approval_prompt.h"
#include "agent/agent_system.h"
#include "agent/agent_config.h"
#include "api/mistral_api.h"
#include "api/mistral_types.h"
#include "common/string_utils.h"

#include <dripcore/task.h>
#include <dripcore/channel.h>
#include <dripcore/task_handle.h>

#include <iostream>
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
    using agent_task_base::agent_task_base;

    dripcore::task_handle execute()
    {
      auto tools_schema = tools_.get_schema();

      while (true)
      {
        // Signal console to show prompt.
        if (prompt_ch_) {
          prompt_ch_->try_send("mistral");
        }

        // Wait for user input.
        auto input = co_await input_ch_->recv();

        if (!input || input->empty()) {
          if (!input) break;  // Channel closed (EOF).
          continue;           // Empty input, ask again.
        }

        chat_.add_user_message(*input);

        // Process message through agent loop
        co_await agent_loop();
      }

      // Cleanup: close channels.
      request_ch_->close();
      co_return;
    }

  protected:
    void thinking_begin() override
    {
      std::cout << std::endl;
      in_thinking_ = true;
    }

    void thinking_end() override
    {
      std::cout << std::endl << std::endl;
      in_thinking_ = false;
    }

    void agent_loop_on_sending_chat_request() override final
    {
      chat_.save(".mistral-cli", session_id_);
    }

    void agent_loop_on_message_content(const api::message_content& mc) override final
    {
      if (mc.type == "thinking")
      {
        // Start or continue thinking display.
        if (!in_thinking_) {
          thinking_begin();
        }
        std::cout << thinking_color_ << mc.content << std::flush;
      }
      else if (mc.type == "text")
      {
        // Text content - could be during thinking or final answer.
        if (in_thinking_) {
          thinking_end();
        }
        std::cout << normal_color_ << mc.content << std::flush;
      }
    }

    auto agent_loop_do_tool_call(const api::tool_call& tc) -> dripcore::awaitable<void> override final
    {
      std::cout << tool_color_;
      std::cout << "\n[tool] " << tc.function.name
                << "(" << tc.function.arguments << ")" << std::flush;

      // Check policy for this tool call
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
        // TODO - Need a better way to handle malformed tool arguments.
        std::cerr << "\n[warning] Failed to parse tool arguments: " << e.what() << std::endl;
        tool_args = api::json::object();
      }

      policy_context ctx{
        .tool_name = tc.function.name,
        .tool_arguments = tool_args,
        .working_directory = ""  // TODO: get actual working directory
      };

      auto policy_result = policy_engine_.evaluate(ctx);

      if (policy_result.decision == policy_decision::DENY)
      {
        // Policy denies this tool call
        std::string rejection_msg = policy_result.rejection_message
          ? *policy_result.rejection_message
          : "Tool call denied by policy";
        std::cout << " → [DENIED] " << rejection_msg << std::endl;

        // Add rejection as a tool result to the conversation
        api::json rejection_result = {
          {"status", "denied"},
          {"message", rejection_msg}
        };
        chat_.add_tool_result(tc.function.name, tc.id, rejection_result.dump());
        co_return;
      }

      if (policy_result.decision == policy_decision::REQUIRE_APPROVAL)
      {
        // Check if this is already session-approved
        if (approval_manager_.is_session_approved(tc.function.name, tool_args))
        {
          std::cout << " → [SESSION APPROVED]" << std::endl;
        }
        else
        {
          // Build approval prompt string
          std::string approval_prompt = build_approval_prompt(tc.function.name, tool_args,
            policy_result.reason ? *policy_result.reason : "Policy requires approval");

          // Send approval prompt through prompt channel with marker
          if (prompt_ch_) {
            prompt_ch_->try_send("[APPROVAL_PROMPT]" + approval_prompt);
          }

          // Wait for single-character response
          auto response = co_await input_ch_->recv();
          if (!response) {
            // Handle EOF or channel closed
            std::cout << " → [CANCELLED] Approval prompt cancelled" << std::endl;
            co_return;
          }

          // Parse single character response
          auto parsed_action = parse_approval_response(*response);
          std::optional<std::string> custom_message = std::nullopt;

          // Handle help request
          if (!parsed_action.has_value())
          {
            // Show help and re-prompt
            if (prompt_ch_) {
              prompt_ch_->try_send("[APPROVAL_PROMPT]" + get_approval_help_message());
            }

            // Wait for next response
            response = co_await input_ch_->recv();
            if (!response) {
              std::cout << " → [CANCELLED] Approval prompt cancelled" << std::endl;
              co_return;
            }
            parsed_action = parse_approval_response(*response);
            if (!parsed_action.has_value()) {
              // Still help, just reject
              parsed_action = approval_action::REJECT;
            }
          }

          approval_action action = *parsed_action;

          // For reject with message, we need to prompt for the message
          if (action == approval_action::REJECT_WITH_MESSAGE)
          {
            std::string message_prompt = "  Enter message for AI (or press Enter for default): ";
            if (prompt_ch_) {
              prompt_ch_->try_send(message_prompt);
            }

            auto message_response = co_await input_ch_->recv();
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
            std::cout << " → [REJECTED] " << rejection_msg << std::endl;

            api::json rejection_result = {
              {"status", "rejected"},
              {"message", rejection_msg}
            };
            chat_.add_tool_result(tc.function.name, tc.id, rejection_result.dump());
            co_return;
          }

          // APPROVE or APPROVE_ALL
          if (action == approval_action::APPROVE_ALL)
          {
            approval_manager_.add_session_approval(tc.function.name, tool_args);
            std::cout << " → [APPROVED ALL] Session approval added" << std::endl;
          }
          else
          {
            std::cout << " → [APPROVED]" << std::endl;
          }
        }
      }

      // Execute the tool
      auto res = execute_tool(tc);
      chat_.add_tool_result(tc.function.name, tc.id, res);
      std::cout << " → " << (res.size() > 200 ? res.substr(0, 200) + "..." : res) << std::endl;
      co_return;
    }

    void agent_loop_on_error(const std::string& error) override final
    {
      std::cerr << "\n[error] " << error << std::endl;
    }

    void agent_loop_on_completed(std::vector<api::message_content>& accumulated_content) override final
    {
      if (!accumulated_content.empty()) {
        chat_.save(".mistral-cli", session_id_);
      }
      agent_task_base::agent_loop_on_completed(accumulated_content);
      std::cout << std::endl;
    }

  private:
    // Console esc codes.
    static constexpr char normal_color_[]   = "\033[0m";
    static constexpr char thinking_color_[] = "\033[90m";
    static constexpr char tool_color_[]     = "\033[32m";
  };
} // namespace mistral::agent

#endif // MISTRAL_AGENT_TASK_H
