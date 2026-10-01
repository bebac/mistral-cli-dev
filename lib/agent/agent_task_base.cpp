#include "agent/agent_task_base.h"

#include "common/string_utils.h"

#include <algorithm>
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
    chat_(500),
    session_id_(mistral::utils::generate_session_id()
  )
  {
    // Load configuration from agent_system
    const auto& agent_cfg = agent_system::instance().get_agent_config(agent_id_);
    const auto& model_cfg = agent_system::instance().get_model_config(agent_cfg.model);

    // Set model, api_version, provider, and system prompt
    model_ = agent_system::instance().resolve_model_id(agent_cfg.model);
    api_version_ = model_cfg.api_version;
    provider_ = api::provider_registry::create(model_cfg.provider);
    system_prompt_ = agent_system::instance().resolve_system_prompt(agent_cfg, model_);

    // Resolve sampling parameters (model defaults + agent overrides)
    sampling_params_ = agent_system::instance().resolve_sampling_params(agent_cfg);

    // Create tool registry with filtered tools
    tools_ = agent_system::instance().create_tool_registry(agent_cfg.available_tools);

    // Apply tool schema options (client-side, not part of the API request)
    auto schema_opts = agent_system::instance().resolve_tool_schema_options(agent_cfg);
    tools_.set_strict_mode(schema_opts.strict_mode);

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
      // If the input channel has been closed (shutdown), don't start
      // a new API request — exit the loop so teardown can proceed.
      if (input_ch_ && input_ch_->is_closed()) {
        break;
      }

      // Create a fresh response channel for each request attempt.
      // This allows cancellation (closing the channel) without affecting
      // future requests, and ensures retries get a clean channel.
      response_ch_ = std::make_shared<dripcore::channel<api::api_event>>(32);

      // Build and send API request.
      auto body = provider_->build_chat_request(model_, chat_.messages(), tools_schema, true, session_id_, sampling_params_);

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
          if (retry_count < max_retries && is_retryable_error(*evt)) {
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

          auto delta = provider_->parse_stream_delta(j);

          // Handle content
          for (const auto& mc : delta.content) {
            agent_loop_on_message_content(mc);
            accumulate_message_content(accumulated_content, mc);
          }

          // Accumulate tool calls from deltas.
          if (delta.tool_calls) {
            for (auto &tc : *delta.tool_calls) {
              auto orphaned = accumulate_tool_call(accumulated_tool_calls, std::move(tc));
              if (orphaned) {
                // The server sent text as function.name on a subsequent
                // chunk — recover it as content so it isn't lost.
                std::cerr << std::format(
                  "[agent] warning: recovered orphaned function.name text ({} chars)\n",
                  orphaned->size());
                api::message_content mc{
                  .type = "text",
                  .content = std::move(*orphaned)
                };
                agent_loop_on_message_content(mc);
                accumulate_message_content(accumulated_content, std::move(mc));
              }
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
          else if (!delta.finish_reason.empty()) {
            std::cerr << "[agent] unexpected finish reason '" << delta.finish_reason << "'" << std::endl;
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

        // Sanitize tool call arguments for storage. The assistant message
        // is sent back in the conversation history on subsequent API requests.
        // If function.arguments contains invalid JSON, OpenAI-compatible
        // providers (e.g. GreenPT) reject the entire request, causing the
        // agent to silently fail on all future messages. Replace invalid
        // JSON with an empty object so the stored message is always valid.
        auto stored_tool_calls = accumulated_tool_calls;
        for (auto& tc : stored_tool_calls) {
          try {
            (void)api::json::parse(tc.function.arguments);
          } catch (...) {
            tc.function.arguments = "{}";
          }
        }

        for (auto& tc : stored_tool_calls) {
          if (!tc.function.name.empty() && !tools_.has_tool(tc.function.name)) {
            std::cerr << std::format(
              "[agent] warning: unknown tool '{}' (length {})\n",
              tc.function.name.substr(0, 80),
              tc.function.name.size());
            if (tc.function.name.size() > 80) {
              tc.function.name = tc.function.name.substr(0, 77) + "...";
            }
          }
        }

        // Add assistant message with accumulated content and tool calls.
        chat_.add_assistant_message(
          std::move(accumulated_content),
          std::move(stored_tool_calls)
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
    auto accumulated_text = accumulate_text(accumulated_content);
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
    auto accumulated_text = accumulate_text(accumulated_content);

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

  std::optional<std::string> agent_task_base::accumulate_tool_call(
    std::vector<api::tool_call>& accumulated, api::tool_call tc)
  {
    if (!tc.id.empty())
    {
      // First chunk of a new tool call (has id, possibly name).
      accumulated.push_back(std::move(tc));
      return std::nullopt;
    }

    std::optional<std::string> orphaned_name;

    if (tc.index.has_value())
    {
      // OpenAI-compatible streaming: use index to find the matching
      // tool call, even if chunks from different calls are interleaved.
      auto it = std::find_if(accumulated.begin(), accumulated.end(),
        [&](const api::tool_call& c) { return c.index == tc.index; });
      if (it != accumulated.end())
      {
        // Per the OpenAI streaming spec, function.name is sent only in
        // the first chunk (with id). Only set if not already present,
        // to prevent contamination from malformed server responses
        // where text content bleeds into the function.name field.
        if (it->function.name.empty() && !tc.function.name.empty())
        {
          it->function.name = tc.function.name;
        }
        else if (!tc.function.name.empty())
        {
          // Name already set but server sent another name fragment —
          // this is malformed. Recover the text as content.
          orphaned_name = std::move(tc.function.name);
        }
        it->function.arguments += tc.function.arguments;
      }
    }
    else if (!accumulated.empty())
    {
      // Fallback: append to last (Mistral format, no index field).
      auto& last = accumulated.back();
      if (last.function.name.empty() && !tc.function.name.empty())
      {
        last.function.name = tc.function.name;
      }
      else if (!tc.function.name.empty())
      {
        orphaned_name = std::move(tc.function.name);
      }
      last.function.arguments += tc.function.arguments;
    }

    return orphaned_name;
  }

  auto agent_task_base::execute_tool(const api::tool_call& tc) -> dripcore::awaitable<std::string>
  {
    // Build the request before the co_await expression: constructing a
    // nested braced temporary inside the co_await triggers a GCC 13
    // internal compiler error (build_special_member_call ICE).
    api::json request{
      {
        "function", {
          {"name", tc.function.name},
          {"arguments", tc.function.arguments}
        }
      }
    };
    auto result = co_await tools_.dispatch(std::move(request));
    if (result.is_string()) {
      co_return result.get<std::string>();
    }
    co_return result.dump();
  }

  std::string agent_task_base::accumulate_text(
    const std::vector<api::message_content>& accumulated_content)
  {
    size_t total_size = 0;
    for (const auto& mc : accumulated_content) {
      if (mc.type == "text") {
        total_size += mc.content.size();
      }
    }
    std::string text;
    text.reserve(total_size);
    for (const auto& mc : accumulated_content) {
      if (mc.type == "text") {
        text += mc.content;
      }
    }
    return text;
  }

  policy_result agent_task_base::evaluate_tool_policy(
    const api::tool_call& tc, const api::json& tool_args) const
  {
    policy_context ctx{
      .tool_name = tc.function.name,
      .tool_arguments = tool_args,
      .working_directory = ""
    };
    return policy_engine_.evaluate(ctx);
  }

  api::json agent_task_base::build_tool_rejection(
    const std::string& status, const std::string& message)
  {
    return api::json{
      {"status", status},
      {"message", message}
    };
  }

  auto agent_task_base::execute_and_record_tool(
    const api::tool_call& tc) -> dripcore::awaitable<std::string>
  {
    auto res = co_await execute_tool(tc);
    chat_.add_tool_result(tc.function.name, tc.id, res);
    co_return res;
  }

  bool agent_task_base::is_retryable_error(const api::api_event& evt)
  {
    // HTTP status code errors: use the structured http_status field when
    // available (set by the HTTP coordinator in stream_response_state::finalize).
    // Retry on 503 (service unavailable), 502 (bad gateway), 429 (rate limited).
    if (evt.http_status) {
      int status = *evt.http_status;
      return status == 503 || status == 502 || status == 429;
    }

    // Connection-level errors (server closed, network error, stream reset,
    // send failure — can happen if the server closes the connection between
    // the connected() check and the actual send). These don't have an HTTP
    // status code, so we match on the error message string.
    const auto& error = evt.payload;
    if (error.find("connection closed") != std::string::npos ||
        error.find("connection error") != std::string::npos ||
        error.find("stream reset") != std::string::npos ||
        error.find("send error") != std::string::npos) {
      return true;
    }
    return false;
  }
} // namespace mistral::agent