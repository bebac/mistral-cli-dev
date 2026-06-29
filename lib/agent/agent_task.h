#ifndef MISTRAL_AGENT_TASK_H
#define MISTRAL_AGENT_TASK_H

#include "agent/conversation.h"
#include "agent/tool_registry.h"
#include "agent/policy_engine.h"
#include "agent/approval_manager.h"
#include "agent/approval_prompt.h"
#include "api/mistral_api.h"
#include "api/mistral_types.h"

#include <dripcore/task.h>
#include <dripcore/channel.h>
#include <dripcore/coroutine.h>

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
      std::string model,
      std::string system_prompt,
      tool_registry& tools,
      policy_engine policy_engine,
      approval_manager approval_manager,
      std::shared_ptr<dripcore::channel<std::string>> prompt_ch,
      std::shared_ptr<dripcore::channel<std::string>> input_ch,
      std::shared_ptr<dripcore::channel<api::api_request>> request_ch,
      std::shared_ptr<dripcore::channel<api::api_event>> response_ch
    )
      : model_(std::move(model))
      , system_prompt_(std::move(system_prompt))
      , tools_(tools)
      , policy_engine_(std::move(policy_engine))
      , approval_manager_(std::move(approval_manager))
      , prompt_ch_(std::move(prompt_ch))
      , input_ch_(std::move(input_ch))
      , request_ch_(std::move(request_ch))
      , response_ch_(std::move(response_ch))
    {}

    dripcore::coroutine execute()
    {
      if (!system_prompt_.empty()) {
        chat_.set_system_message(system_prompt_);
      }

      auto tools_schema = tools_.get_schema();

#if 0
      std::cerr << "[debug] " << tools_schema << std::endl;
#endif

      while (true)
      {
        // Signal console to show prompt.
        prompt_ch_->try_send("mistral");

        // Wait for user input.
        auto input = co_await input_ch_->recv();
        if (!input || input->empty()) {
          if (!input) break;  // Channel closed (EOF).
          continue;           // Empty input, ask again.
        }

        chat_.add_user_message(*input);

        // Agent loop: may iterate for tool calls.
        bool final_answer = false;
        while (!final_answer)
        {
          // Build and send API request.
          auto body = api::build_chat_request(model_, chat_.messages(), tools_schema, true);

          chat_.save(".mistral-cli/chat.latest.json");

          request_ch_->try_send({.body = std::move(body), .stream = true});

          // Consume streaming events.
          std::vector<api::message_content> accumulated_content;
          std::vector<api::tool_call> accumulated_tool_calls;
          bool print_prefix = true;
          bool in_thinking = false;

          while (true)
          {
            auto evt = co_await response_ch_->recv();
            if (!evt) { final_answer = true; break; }

            if (evt->type == api::event_type::done) break;

            if (evt->type == api::event_type::error) {
              std::cerr << "\n[error] " << evt->payload << std::endl;
              final_answer = true;
              break;
            }

            // Parse SSE data JSON.
            try {
              auto j = api::json::parse(evt->payload);
              auto delta = api::parse_stream_delta(j);

              auto usage = j.value("usage", json());

              if ( !usage.is_null() ) {
                //std::cerr << "\n\n[debug] usage '" << usage << "'" << std::endl;
              }

              //std::cerr << "\n[debug] SSE data '" << evt->payload << "'" << std::endl;

              // Handle content - always a vector of message_content.
              for (const auto& mc : delta.content) {
                if (mc.type == "thinking") {
                  // Start or continue thinking display.
                  if (!in_thinking) {
                    if (print_prefix) {
                      std::cout << "\n\033[90m";  // Gray for thinking.
                      print_prefix = false;
                    }
                    in_thinking = true;
                  }

                  std::cout << mc.content << std::flush;

                  // Accumulate thinking content.
                  accumulate_message_content(accumulated_content, mc);

                  // Check if thinking is closed.
                  if (mc.closed.value_or(false)) {
                    // Thinking phase is done, but there might be text chunks in the same delta.
                    // Don't reset in_thinking yet as there might be text chunks following.
                  }
                }
                else if (mc.type == "text") {
                  // Text content - could be during thinking or final answer.
                  if (in_thinking) {
                    // End thinking display.
                    std::cout << "\n\n\033[0m";  // Reset color.
                    in_thinking = false;
                  }
                  if (print_prefix) {
                    std::cout << "\033[32m";  // Green for answer text.
                    print_prefix = false;
                  }
                  std::cout << mc.content << std::flush;

                  // Accumulate text content.
                  accumulate_message_content(accumulated_content, mc);
                }
              }

              // Accumulate tool calls from deltas.
              if (delta.tool_calls)
              {
                for (auto &tc : *delta.tool_calls) {
                  accumulate_tool_call(accumulated_tool_calls, std::move(tc));
                }
              }

              // Check finish reason.
              if (delta.finish_reason == "tool_calls" || delta.finish_reason == "stop") {
                // Will be handled after breaking out of the event loop.
              }
              else if ( delta.finish_reason == "error") {
                std::cerr << "\n[error] got finish_reason 'error'" << std::endl;
              }
            }
            catch (const std::exception& e) {
              // Skip malformed SSE data.
              std::cerr << "\n[error] malformed SSE data '" << evt->payload << "' " << e.what() << std::endl;
            }
          }

          // Reset color.
          if (!print_prefix) {
            std::cout << "\033[0m";
          }

          if (!accumulated_tool_calls.empty())
          {
            // Add assistant message with accumulated content and tool calls.
            chat_.add_assistant_message(
              std::move(accumulated_content),
              std::vector<api::tool_call>(accumulated_tool_calls)
            );

            // Execute each tool call.
            for (const auto& tc : accumulated_tool_calls)
            {
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
                continue;
              }

              if (policy_result.decision == policy_decision::REQUIRE_APPROVAL)
              {
                // Check if this is already session-approved
                if (approval_manager_.is_session_approved(tc.function.name, tool_args))
                {
                  std::cout << " → [SESSION APPROVED]" << std::endl;
                  // Proceed with tool execution
                }
                else
                {
                  // Build approval prompt string
                  std::string approval_prompt = build_approval_prompt(tc.function.name, tool_args,
                    policy_result.reason ? *policy_result.reason : "Policy requires approval");

                  // Send approval prompt through prompt channel with marker
                  prompt_ch_->try_send("[APPROVAL_PROMPT]" + approval_prompt);

                  // Wait for single-character response
                  auto response = co_await input_ch_->recv();
                  if (!response) {
                    // Handle EOF or channel closed
                    std::cout << " → [CANCELLED] Approval prompt cancelled" << std::endl;
                    continue;
                  }

                  // Parse single character response
                  auto parsed_action = parse_approval_response(*response);
                  std::optional<std::string> custom_message = std::nullopt;

                  // Handle help request
                  if (!parsed_action.has_value())
                  {
                    // Show help and re-prompt
                    prompt_ch_->try_send("[APPROVAL_PROMPT]" + get_approval_help_message());

                    // Wait for next response
                    response = co_await input_ch_->recv();
                    if (!response) {
                      std::cout << " → [CANCELLED] Approval prompt cancelled" << std::endl;
                      continue;
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
                    // Send a follow-up prompt for the rejection message
                    // Use regular prompt (not approval) so user can type a full line
                    std::string message_prompt = "  Enter message for AI (or press Enter for default): ";
                    prompt_ch_->try_send(message_prompt);

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

                    // Add rejection as a tool result to the conversation
                    api::json rejection_result = {
                      {"status", "rejected"},
                      {"message", rejection_msg}
                    };
                    chat_.add_tool_result(tc.function.name, tc.id, rejection_result.dump());
                    continue;
                  }

                  // APPROVE or APPROVE_ALL
                  if (action == approval_action::APPROVE_ALL)
                  {
                    // Add session approval for this pattern
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
            }
            // Loop back to send tool results to the API.
          }
          else
          {
            // Final text response.
            if (!accumulated_content.empty()) {
              chat_.add_assistant_message(std::move(accumulated_content));
            }
            std::cout << std::endl;
            final_answer = true;
          }

          chat_.ensure_window_size();
        }
      }

      // Cleanup: close channels.
      request_ch_->close();
      co_return;
    }

  private:
    void accumulate_message_content(std::vector<api::message_content> &accumulated, api::message_content mc)
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

    void accumulate_tool_call(std::vector<api::tool_call> &accumulated, api::tool_call tc)
    {
      if (!tc.id.empty())
      {
        // New tool call.
        accumulated.push_back(std::move(tc));
      }
      else if (!accumulated.empty())
      {
        // Append to last tool call's arguments.
        auto &last = accumulated.back();
        if (!tc.function.name.empty()) {
          last.function.name += tc.function.name;
        }
        last.function.arguments += tc.function.arguments;
      }
    }

    void execute_tool_and_handle_result(const api::tool_call &tc)
    {
      auto result = tools_.dispatch(
        api::json{{
          "function", {
            {"name", tc.function.name},
            {"arguments", tc.function.arguments}
          }
        }}
      );
      std::string result_str = result.dump();
      chat_.add_tool_result(tc.function.name, tc.id, result_str);
      std::cout << " → " << (result_str.size() > 200 ? result_str.substr(0, 200) + "..." : result_str) << std::endl;
    }

    api::json parse_tool_arguments(const std::string &arguments_str)
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

  private:
    std::string model_;
    std::string system_prompt_;
    tool_registry& tools_;
    policy_engine policy_engine_;
    approval_manager approval_manager_;
    conversation chat_{200};  // Keep last 200 messages. We need a better way to track context usage.

    std::shared_ptr<dripcore::channel<std::string>> prompt_ch_;
    std::shared_ptr<dripcore::channel<std::string>> input_ch_;
    std::shared_ptr<dripcore::channel<api::api_request>> request_ch_;
    std::shared_ptr<dripcore::channel<api::api_event>> response_ch_;
  };

} // namespace mistral::agent

#endif // MISTRAL_AGENT_TASK_H
