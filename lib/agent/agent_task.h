#ifndef MISTRAL_AGENT_TASK_H
#define MISTRAL_AGENT_TASK_H

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
  class agent_task : public dripcore::task
  {
  public:
    agent_task(
      const std::string& agent_id,
      std::shared_ptr<dripcore::channel<std::string>> input_ch,
      std::shared_ptr<dripcore::channel<std::string>> output_ch = nullptr,
      std::shared_ptr<dripcore::channel<std::string>> prompt_ch = nullptr
    )
      : agent_id_(agent_id)
      , input_ch_(std::move(input_ch))
      , output_ch_(std::move(output_ch))
      , prompt_ch_(std::move(prompt_ch))
      , session_id_(mistral::utils::generate_session_id())
    {
      // Load configuration from agent_system
      const auto& agent_cfg = agent_system::instance().get_agent_config(agent_id_);
      const auto& model_cfg = agent_system::instance().get_model_config(agent_cfg.model);

      // Set model and system prompt
      model_ = agent_system::instance().resolve_model_id(agent_cfg.model);
      system_prompt_ = agent_system::instance().resolve_system_prompt(agent_cfg, model_);

      // Create tool registry with filtered tools
      tools_ = agent_system::instance().create_tool_registry(agent_cfg.available_tools);

      // Set up policy engine from resolved tool policy
      auto tool_policy_config = agent_system::instance().resolve_tool_policy(agent_cfg);
      policy_engine_.load_configuration(tool_policy_config);

      // Get shared HTTP channels
      request_ch_ = agent_system::instance().request_ch();
      response_ch_ = agent_system::instance().response_ch();

      // Set system message
      if (!system_prompt_.empty()) {
        chat_.set_system_message(system_prompt_);
      }
    }

    dripcore::task_handle execute() // TODO override
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
    // Virtual methods for customizing behavior
    virtual void handle_content(const api::message_content& mc)
    {
      if (mc.type == "thinking") {
        // Start or continue thinking display.
        if (!in_thinking_) {
          thinking_begin();
        }
        std::cout << thinking_color_ << mc.content << std::flush;
      }
      else if (mc.type == "text") {
        // Text content - could be during thinking or final answer.
        if (in_thinking_) {
          thinking_end();
        }
        std::cout << normal_color_ << mc.content << std::flush;
      }
    }

    virtual dripcore::awaitable<void> handle_tool_call(const api::tool_call& tc)
    {
      std::cout << tool_color_;
      std::cout << "\n[tool] " << tc.function.name
                << "(" << tc.function.arguments << ")" << std::flush;

      // Check policy for this tool call
      auto tool_args = parse_tool_arguments(tc.function.arguments);

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
      execute_tool_and_handle_result(tc);
      co_return;
    }

    virtual void thinking_begin()
    {
      std::cout << std::endl;
      in_thinking_ = true;
    }

    virtual void thinking_end()
    {
      std::cout << std::endl << std::endl;
      in_thinking_ = false;
    }

    virtual void handle_error(const std::string& error)
    {
      std::cerr << "\n[error] " << error << std::endl;
    }

    virtual void handle_complete()
    {
      std::cout << std::endl;
    }

    // Common agent loop that processes a single message
    dripcore::awaitable<void> agent_loop()
    {
      auto tools_schema = tools_.get_schema();
      bool final_answer = false;

      while (!final_answer)
      {
        // Build and send API request.
        auto body = api::build_chat_request(model_, chat_.messages(), tools_schema, true, session_id_);
        chat_.save(".mistral-cli", session_id_);
        request_ch_->try_send({.body = std::move(body), .stream = true});

        // Consume streaming events.
        std::vector<api::message_content> accumulated_content;
        std::vector<api::tool_call> accumulated_tool_calls;

        while (true)
        {
          auto evt = co_await response_ch_->recv();

          if (!evt) {
            final_answer = true;
            break;
          }
          if (evt->type == api::event_type::done) {
            break;
          }
          if (evt->type == api::event_type::error) {
            handle_error(evt->payload);
            final_answer = true;
            break;
          }

          // Parse SSE data JSON.
          try {
            auto j = api::json::parse(evt->payload);
            auto delta = api::parse_stream_delta(j);

            // Handle content
            for (const auto& mc : delta.content) {
              handle_content(mc);
              accumulate_message_content(accumulated_content, mc);
            }

            // Accumulate tool calls from deltas.
            if (delta.tool_calls) {
              for (auto &tc : *delta.tool_calls) {
                accumulate_tool_call(accumulated_tool_calls, std::move(tc));
              }
            }

            // Check finish reason.
            if (delta.finish_reason == "tool_calls" || delta.finish_reason == "stop") {
              // Will be handled after breaking out of the event loop.
            }
            else if (delta.finish_reason == "error") {
              std::cerr << "\n[error] got finish_reason 'error'" << std::endl;
            }
          }
          catch (const std::exception& e) {
            std::cerr << "\n[error] malformed SSE data '" << evt->payload << "' " << e.what() << std::endl;
          }
        }

        if (!accumulated_tool_calls.empty())
        {
          // Add assistant message with accumulated content and tool calls.
          chat_.add_assistant_message(
            std::move(accumulated_content),
            std::vector<api::tool_call>(accumulated_tool_calls)
          );

          // Execute each tool call.
          for (const auto& tc : accumulated_tool_calls) {
            co_await handle_tool_call(tc);
          }
        }
        else
        {
          // Final text response.
          if (!accumulated_content.empty()) {
            chat_.add_assistant_message(std::move(accumulated_content));
            chat_.save(".mistral-cli", session_id_);
          }
          handle_complete();
          final_answer = true;
        }

        chat_.ensure_window_size();
      }

      co_return;
    }

  private:
    // Helper methods
    void accumulate_message_content(std::vector<api::message_content>& accumulated, api::message_content mc)
    {
      for (auto &acc_mc : accumulated)
      {
        if (acc_mc.type == mc.type)
        {
          acc_mc.content += mc.content;
          if (mc.closed) {
            acc_mc.closed = mc.closed;
          }
          return;
        }
      }
      accumulated.push_back(std::move(mc));
    }

    void accumulate_tool_call(std::vector<api::tool_call>& accumulated, api::tool_call tc)
    {
      if (!tc.id.empty())
      {
        accumulated.push_back(std::move(tc));
      }
      else if (!accumulated.empty())
      {
        auto &last = accumulated.back();
        if (!tc.function.name.empty()) {
          last.function.name += tc.function.name;
        }
        last.function.arguments += tc.function.arguments;
      }
    }

    void execute_tool_and_handle_result(const api::tool_call& tc)
    {
      auto result = tools_.dispatch(
        api::json{
          {"function", {
            {"name", tc.function.name},
            {"arguments", tc.function.arguments}
          }}
        });
      std::string result_str = result.dump();
      chat_.add_tool_result(tc.function.name, tc.id, result_str);
      std::cout << " → " << (result_str.size() > 200 ? result_str.substr(0, 200) + "..." : result_str) << std::endl;
    }

    api::json parse_tool_arguments(const std::string& arguments_str)
    {
      api::json tool_args;
      try
      {
        if (!arguments_str.empty()) {
          tool_args = api::json::parse(arguments_str);
        }
      }
      catch (const std::exception &e)
      {
        std::cerr << "\n[warning] Failed to parse tool arguments: " << e.what() << std::endl;
        tool_args = api::json::object();
      }
      return tool_args;
    }

    // Agent identifier
    std::string agent_id_;

    // Configuration-derived properties
    std::string model_;
    std::string system_prompt_;
    tool_registry tools_;
    policy_engine policy_engine_;
    approval_manager approval_manager_;

    // Shared state
    conversation chat_{200};
    std::string session_id_;

    // Channels
    std::shared_ptr<dripcore::channel<std::string>> input_ch_;
    std::shared_ptr<dripcore::channel<std::string>> output_ch_;
    std::shared_ptr<dripcore::channel<std::string>> prompt_ch_;
    std::shared_ptr<dripcore::channel<api::api_request>> request_ch_;
    std::shared_ptr<dripcore::channel<api::api_event>> response_ch_;

    bool in_thinking_ = false;

    // Console esc codes.
    static constexpr char normal_color_[]   = "\033[0m";
    static constexpr char thinking_color_[] = "\033[90m";
    static constexpr char tool_color_[]     = "\033[32m";
  };

} // namespace mistral::agent

#endif // MISTRAL_AGENT_TASK_H
