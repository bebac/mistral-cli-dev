#include "agent/agent_task_base.h"

#include "common/string_utils.h"

#include <iostream>

namespace mistral::agent
{
  agent_task_base::agent_task_base(
    const std::string &agent_id,
    std::shared_ptr<dripcore::channel<std::string>> input_ch,
    std::shared_ptr<dripcore::channel<std::string>> output_ch,
    std::shared_ptr<dripcore::channel<std::string>> prompt_ch)
  :
    agent_id_(agent_id),
    input_ch_(std::move(input_ch)),
    output_ch_(std::move(output_ch)),
    prompt_ch_(std::move(prompt_ch)),
    chat_(200),
    session_id_(mistral::utils::generate_session_id()
  )
  {
    // Load configuration from agent_system
    const auto &agent_cfg = agent_system::instance().get_agent_config(agent_id_);
    const auto &model_cfg = agent_system::instance().get_model_config(agent_cfg.model);

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
    if (!system_prompt_.empty())
    {
      chat_.set_system_message(system_prompt_);
    }
  }

  auto agent_task_base::agent_loop() -> dripcore::awaitable<void>
  {
    auto tools_schema = tools_.get_schema();
    bool final_answer = false;

    while (!final_answer)
    {
      // Build and send API request.
      auto body = api::build_chat_request(model_, chat_.messages(), tools_schema, true, session_id_);

      agent_loop_on_sending_chat_request();

      // TODO - try_send can fail, we need ti handle it!
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
          agent_loop_on_error(evt->payload);
          final_answer = true;
          break;
        }

        // Parse SSE data JSON.
        try {
          auto j = api::json::parse(evt->payload);
          auto delta = api::parse_stream_delta(j);

          // Handle content
          for (const auto& mc : delta.content) {
            agent_loop_on_message_content(mc);
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
            agent_loop_on_error("got finish_reason 'error'");
            // TODO - Should this end the loop?
          }
        }
        catch (const std::exception& e) {
          agent_loop_on_error(std::format("malformed SSE data '{}' {}", evt->payload, e.what()));
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
          co_await agent_loop_do_tool_call(tc);
        }
      }
      else
      {
        // Final text response.
        if (!accumulated_content.empty()) {
          chat_.add_assistant_message(std::move(accumulated_content));
        }
        agent_loop_on_completed(accumulated_content);
        final_answer = true;
      }

      chat_.ensure_window_size();
    }
    co_return;
  }

  void agent_task_base::agent_loop_on_completed(std::vector<api::message_content>& accumulated_content)
  {
    if (output_ch_)
    {
      std::string final_answer;
      for (const auto& mc : accumulated_content)
      {
        if (mc.type == "text") {
          final_answer += mc.content;
        }
      }
      output_ch_->try_send(final_answer);
    }
  }

  // Helper methods
  void agent_task_base::accumulate_message_content(std::vector<api::message_content> &accumulated, api::message_content mc)
  {
    for (auto &acc_mc : accumulated)
    {
      if (acc_mc.type == mc.type)
      {
        acc_mc.content += mc.content;
        if (mc.closed)
        {
          acc_mc.closed = mc.closed;
        }
        return;
      }
    }
    accumulated.push_back(std::move(mc));
  }

  void agent_task_base::accumulate_tool_call(std::vector<api::tool_call> &accumulated, api::tool_call tc)
  {
    if (!tc.id.empty())
    {
      accumulated.push_back(std::move(tc));
    }
    else if (!accumulated.empty())
    {
      auto &last = accumulated.back();
      if (!tc.function.name.empty())
      {
        last.function.name += tc.function.name;
      }
      last.function.arguments += tc.function.arguments;
    }
  }

  auto agent_task_base::execute_tool(const api::tool_call& tc) -> std::string
  {
    auto result = tools_.dispatch(
      api::json{{
        "function", {
          {"name", tc.function.name},
          {"arguments", tc.function.arguments}
        }
      }}
    );
    return result.dump();
  }
} // namespace mistral::agent