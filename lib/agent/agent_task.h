#ifndef MISTRAL_AGENT_TASK_H
#define MISTRAL_AGENT_TASK_H

#include "agent/conversation.h"
#include "agent/tool_registry.h"
#include "api/mistral_api.h"
#include "api/mistral_types.h"

#include <dripcore/task.h>
#include <dripcore/channel.h>
#include <dripcore/coroutine.h>

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace mistral::agent
{
  class agent_task : public dripcore::task
  {
  public:
    agent_task(
      std::string model,
      std::string system_prompt,
      tool_registry& tools,
      std::shared_ptr<dripcore::channel<std::string>> prompt_ch,
      std::shared_ptr<dripcore::channel<std::string>> input_ch,
      std::shared_ptr<dripcore::channel<api::api_request>> request_ch,
      std::shared_ptr<dripcore::channel<api::api_event>> response_ch
    )
      : model_(std::move(model))
      , system_prompt_(std::move(system_prompt))
      , tools_(tools)
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
          request_ch_->try_send({.body = std::move(body), .stream = true});

          // Consume streaming events.
          std::string accumulated_content;
          std::vector<api::tool_call> accumulated_tool_calls;
          bool print_prefix = true;

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

              // Print content tokens as they arrive.
              if (!delta.content.empty()) {
                if (print_prefix) {
                  std::cout << "\033[32m";  // Green.
                  print_prefix = false;
                }
                std::cout << delta.content << std::flush;
                accumulated_content += delta.content;
              }

              // Accumulate tool calls from deltas.
              if (delta.tool_calls) {
                for (auto& tc : *delta.tool_calls) {
                  // Find existing call by index or create new.
                  if (!tc.id.empty()) {
                    // New tool call.
                    accumulated_tool_calls.push_back(std::move(tc));
                  } else if (!accumulated_tool_calls.empty()) {
                    // Append to last tool call's arguments.
                    auto& last = accumulated_tool_calls.back();
                    if (!tc.function.name.empty()) {
                      last.function.name += tc.function.name;
                    }
                    last.function.arguments += tc.function.arguments;
                  }
                }
              }

              // Check finish reason.
              if (delta.finish_reason == "tool_calls" || delta.finish_reason == "stop") {
                // Will be handled after breaking out of the event loop.
              }
            } catch (const std::exception& e) {
              // Skip malformed SSE data.
            }
          }

          // Reset color.
          if (!print_prefix) {
            std::cout << "\033[0m";
          }

          if (!accumulated_tool_calls.empty())
          {
            // Add assistant message with tool calls.
            chat_.add_assistant_message(accumulated_content,
              std::vector<api::tool_call>(accumulated_tool_calls));

            // Execute each tool call.
            for (const auto& tc : accumulated_tool_calls)
            {
              std::cout << "\n[tool] " << tc.function.name
                        << "(" << tc.function.arguments << ")" << std::flush;

              auto result = tools_.dispatch(api::json{
                {"function", {
                  {"name", tc.function.name},
                  {"arguments", tc.function.arguments}
                }}
              });

              std::string result_str = result.dump();
              chat_.add_tool_result(tc.function.name, tc.id, result_str);

              std::cout << " → " << (result_str.size() > 200
                ? result_str.substr(0, 200) + "..." : result_str) << std::endl;
            }
            // Loop back to send tool results to the API.
          }
          else
          {
            // Final text response.
            if (!accumulated_content.empty()) {
              chat_.add_assistant_message(accumulated_content);
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
    std::string model_;
    std::string system_prompt_;
    tool_registry& tools_;
    conversation chat_{50};  // Keep last 50 messages.

    std::shared_ptr<dripcore::channel<std::string>> prompt_ch_;
    std::shared_ptr<dripcore::channel<std::string>> input_ch_;
    std::shared_ptr<dripcore::channel<api::api_request>> request_ch_;
    std::shared_ptr<dripcore::channel<api::api_event>> response_ch_;
  };

} // namespace mistral::agent

#endif // MISTRAL_AGENT_TASK_H
