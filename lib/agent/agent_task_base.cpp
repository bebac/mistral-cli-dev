#include "agent/agent_task_base.h"

#include "common/string_utils.h"

#include <chrono>
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
    const auto& agent_cfg = agent_system::instance().get_agent_config(agent_id_);
    const auto& model_cfg = agent_system::instance().get_model_config(agent_cfg.model);

    // Set model, api_version, and system prompt
    model_ = agent_system::instance().resolve_model_id(agent_cfg.model);
    api_version_ = model_cfg.api_version;
    system_prompt_ = agent_system::instance().resolve_system_prompt(agent_cfg, model_);

    // Resolve sampling parameters (model defaults + agent overrides)
    sampling_params_ = agent_system::instance().resolve_sampling_params(agent_cfg);

    // Create tool registry with filtered tools
    tools_ = agent_system::instance().create_tool_registry(agent_cfg.available_tools);

    // Set up policy engine from resolved tool policy
    auto tool_policy_config = agent_system::instance().resolve_tool_policy(agent_cfg);
    policy_engine_.load_configuration(tool_policy_config);

    // Get shared HTTP request channel
    request_ch_ = agent_system::instance().request_ch();

    // Get stats channel
    stats_ch_ = agent_system::instance().stats_ch();

    // Create response channel for this agent
    response_ch_ = std::make_shared<dripcore::channel<api::api_event>>(32);

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
    int retry_count = 0;
    constexpr int max_retries = 3;

    while (!final_answer)
    {
      // Create a fresh response channel for each request attempt.
      // This allows cancellation (closing the channel) without affecting
      // future requests, and ensures retries get a clean channel.
      response_ch_ = std::make_shared<dripcore::channel<api::api_event>>(32);

      // Build and send API request.
      auto body = api::build_chat_request(model_, chat_.messages(), tools_schema, true, session_id_, sampling_params_);

      agent_loop_on_sending_chat_request();

      // Register response channel for cancellation support.
      agent_system::instance().set_current_response_ch(response_ch_);

      // Build request with path from api_version
      std::string path = "/" + api_version_ + "/chat/completions";
      request_ch_->try_send({.path = path, .body = std::move(body), .stream = true, .response_ch = response_ch_});

      // Consume streaming events.
      std::vector<api::message_content> accumulated_content;
      std::vector<api::tool_call> accumulated_tool_calls;
      bool cancelled = false;
      bool retry = false;
      std::string retry_error;

      while (true)
      {
        auto evt = co_await response_ch_->recv(this);

        if (!evt) {
          cancelled = true;
          final_answer = true;
          break;
        }
        if (evt->type == api::event_type::done) {
          break;
        }
        if (evt->type == api::event_type::error) {
          if (retry_count < max_retries && is_retryable_error(evt->payload)) {
            retry_error = evt->payload;
            retry = true;
            break;
          }
          agent_loop_on_error(evt->payload);
          final_answer = true;
          break;
        }

        // Parse SSE data JSON.
        try {
          auto j = api::json::parse(evt->payload);

          if (j.contains("usage")) {
            if (stats_ch_) {
              stats_ch_->try_send({stat_id::api_usage, j.at("usage")});
            }
          }

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

      // Unregister response channel — request is no longer in flight.
      agent_system::instance().clear_current_response_ch();

      if (retry)
      {
        retry_count++;
        if (in_thinking_) thinking_end();
        auto delay = std::chrono::seconds(1 << (retry_count - 1));
        agent_loop_on_error(std::format(
          "Retrying ({}/{}) after {}s: {}",
          retry_count, max_retries, delay.count(), retry_error));
        co_await sleep_for(delay);
        continue;
      }

      if (cancelled)
      {
        agent_loop_on_cancelled();
      }
      else if (!accumulated_tool_calls.empty())
      {
        // PARSE AND STORE PATCHSET FROM accumulated_content BEFORE tool execution
        if (!try_parse_and_store_patchset(accumulated_content))
        {
          // No patchset ID marker found — try to parse edit blocks and
          // append to an ongoing patchset (supports interleaved tool calls).
          try_parse_and_append_edits(accumulated_content);
        }

        // Close any open thinking block before emitting tool call output.
        // If the model goes directly from thinking to tool calls (no text
        // in between), thinking_end() would never be called, leaving
        // in_thinking_ true and missing the newline separator.
        if (in_thinking_) thinking_end();

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
          // PARSE AND STORE PATCHSET FROM accumulated_content
          if (!try_parse_and_store_patchset(accumulated_content))
          {
            // No patchset ID marker found — try to parse edit blocks and
            // append to an ongoing patchset (supports interleaved tool calls).
            try_parse_and_append_edits(accumulated_content);
          }

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
  std::optional<std::pair<std::string, std::vector<file_edit_t>>>
  agent_task_base::try_parse_and_store_patchset(const std::vector<api::message_content>& accumulated_content)
  {
    // Pre-allocate string to avoid reallocations
    size_t total_size = 0;
    for (const auto& mc : accumulated_content) {
      if (mc.type == "text") {
        total_size += mc.content.size();
      }
    }

    std::string accumulated_text;
    accumulated_text.reserve(total_size);

    for (const auto& mc : accumulated_content) {
      if (mc.type == "text") {
        accumulated_text += mc.content;
      }
    }

    auto parse_result = parse_edits_with_patchset(accumulated_text);
    if (parse_result) {
      auto& sys = agent_system::instance();
      // If a non-applied patchset with the same ID already exists, append
      // to it instead of overwriting. This supports interleaved tool calls
      // during patchset emission.
      auto existing = sys.get_patchset(parse_result->first);
      if (existing && !existing->applied)
      {
        sys.append_edits(parse_result->first, parse_result->second);
        // Return the full merged edit list
        auto merged = sys.get_patchset(parse_result->first);
        if (merged) {
          return std::make_pair(merged->patchset_id, merged->edits);
        }
      }
      else
      {
        sys.store_patchset(parse_result->first, parse_result->second);
      }
    }
    return parse_result;
  }

  std::optional<std::pair<std::string, std::vector<file_edit_t>>>
  agent_task_base::try_parse_and_append_edits(const std::vector<api::message_content>& accumulated_content)
  {
    // Pre-allocate string to avoid reallocations
    size_t total_size = 0;
    for (const auto& mc : accumulated_content) {
      if (mc.type == "text") {
        total_size += mc.content.size();
      }
    }

    std::string accumulated_text;
    accumulated_text.reserve(total_size);

    for (const auto& mc : accumulated_content) {
      if (mc.type == "text") {
        accumulated_text += mc.content;
      }
    }

    // Try to parse edit blocks without requiring a patchset ID marker
    auto edits = parse_edit_blocks(accumulated_text);
    if (!edits) {
      return std::nullopt;
    }

    // Only append if there's an ongoing non-applied patchset
    auto& sys = agent_system::instance();
    auto current_id = sys.get_current_patchset_id();
    if (!current_id) {
      return std::nullopt;
    }

    auto existing = sys.get_patchset(*current_id);
    if (!existing || existing->applied) {
      return std::nullopt;
    }

    sys.append_edits(*current_id, *edits);

    // Return the full merged edit list
    auto merged = sys.get_patchset(*current_id);
    if (merged) {
      return std::make_pair(merged->patchset_id, merged->edits);
    }
    return std::nullopt;
  }

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

  auto agent_task_base::execute_tool(const api::tool_call& tc) -> dripcore::awaitable<std::string>
  {
    auto result = co_await tools_.dispatch(
      api::json{{
        "function", {
          {"name", tc.function.name},
          {"arguments", tc.function.arguments}
        }
      }}
    );
    if (result.is_string()) {
      co_return result.get<std::string>();
    }
    co_return result.dump();
  }

  bool agent_task_base::is_retryable_error(const std::string& error)
  {
    // HTTP 503 (service unavailable), 502 (bad gateway), 429 (rate limited)
    if (error.starts_with("HTTP 503") ||
        error.starts_with("HTTP 502") ||
        error.starts_with("HTTP 429")) {
      return true;
    }
    // Connection-level errors (server closed, network error, stream reset,
    // send failure — can happen if the server closes the connection between
    // the connected() check and the actual send).
    if (error.find("connection closed") != std::string::npos ||
        error.find("connection error") != std::string::npos ||
        error.find("stream reset") != std::string::npos ||
        error.find("send error") != std::string::npos) {
      return true;
    }
    return false;
  }
} // namespace mistral::agent