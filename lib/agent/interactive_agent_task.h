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
#include "agent/compact_command.h"
#include "agent/render_sink.h"

#include "api/types.h"
#include "common/string_utils.h"

#include <dripcore/task.h>
#include <dripcore/channel.h>
#include <dripcore/task_handle.h>

#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <cctype>
#include <charconv>
#include <format>

namespace mistral::agent
{
  // Instruction for the compaction side request. The summary replaces the
  // evicted history, so it must preserve the operational state of the
  // session (goals, decisions, files, patchset IDs, open tasks), not
  // just the narrative.
  inline constexpr std::string_view compaction_instruction =
    "This conversation is being compacted to free up context space. The summary you "
    "produce will replace everything above, so it will be the only memory of the "
    "earlier conversation. Summarize the conversation so far, preserving:\n"
    "1. The user's overall goal(s) and any standing constraints or preferences they stated\n"
    "2. Decisions made and their rationale, including approaches that were tried and rejected\n"
    "3. Files that were read or edited, and any significant findings in them\n"
    "4. Any patchset IDs that were mentioned, and whether they were applied\n"
    "5. Open tasks and unresolved questions\n\n"
    "Keep it concise. Do not include large code blocks or file contents unless they "
    "are essential.";

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
      // Register the input channel so slash commands (e.g. /compact)
      // can route control messages to this task, which owns the
      // conversation.
      agent_system::instance().set_current_input_ch(input_ch_);
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

        // Compaction control message from the /compact slash command.
        if (input->starts_with(compact_request_prefix))
        {
          co_await run_compaction(input->substr(compact_request_prefix.size()));
          continue;
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
        std::string error_msg = std::string("Failed to parse tool arguments: ") + e.what();
        sink_->output_warning(error_msg);
        chat_.add_tool_result(tc.function.name, tc.id,
          build_tool_rejection("error", error_msg).dump());
        co_return;
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
    // Compact the conversation history: summarize the evicted messages
    // via a side API request and replace them with a synthetic summary
    // turn (see conversation::apply_compaction). Called from body() when
    // a /compact control message arrives, i.e. between turns.
    auto run_compaction(const std::string& args) -> dripcore::awaitable<void>
    {
      // Parse the number of recent turns to keep (from /compact [keep]).
      size_t keep_turns = default_compact_keep_turns;
      if (!args.empty())
      {
        size_t value = 0;
        auto result = std::from_chars(args.data(), args.data() + args.size(), value);
        if (result.ec == std::errc())
        {
          keep_turns = value;
        }
        else
        {
          sink_->output_warning("Invalid /compact argument, keeping the default " +
            std::to_string(default_compact_keep_turns) + " turn(s).");
        }
      }

      if (chat_.turn_count() <= keep_turns)
      {
        sink_->output_line("Nothing to compact: the conversation has " +
          std::to_string(chat_.turn_count()) + " turn(s), keeping the last " +
          std::to_string(keep_turns) + ".");
        co_return;
      }

      auto to_summarize = chat_.messages_to_compact(keep_turns);
      sink_->output_line(std::format(
        "Compacting: summarizing {} message(s), keeping the last {} turn(s)...",
        to_summarize.size(), keep_turns));

      // Preserve the full history before compaction so it can be recovered.
      chat_.save_snapshot(".mistral-cli", session_id_);

      // Side request: [system?] + evicted messages + summarization
      // instruction. No tools are offered; the response must be plain
      // text. The prefix matches the cached conversation prefix, so the
      // input tokens are mostly a prompt-cache hit.
      std::vector<api::message> request_msgs;
      const auto& history = chat_.messages();
      if (!history.empty() && history.front().role == "system")
      {
        request_msgs.push_back(history.front());
      }
      request_msgs.insert(request_msgs.end(), to_summarize.begin(), to_summarize.end());

      api::message instruction;
      instruction.role = "user";
      instruction.content = {api::message_content{
        .type = "text",
        .content = std::string(compaction_instruction)
      }};
      request_msgs.push_back(std::move(instruction));

      auto body = provider_->build_chat_request(
        model_, request_msgs, api::json::array(), true, session_id_, sampling_params_);

      response_ch_ = std::make_shared<dripcore::channel<api::api_event>>(32);
      agent_system::instance().set_current_response_ch(response_ch_);

      std::string path = "/" + api_version_ + "/chat/completions";
      request_ch_->try_send({.path = path, .body = std::move(body), .stream = true, .response_ch = response_ch_});

      // Consume the streaming response, accumulating text content.
      std::string summary;
      bool failed = false;
      while (true)
      {
        auto evt = co_await response_ch_->recv(this);
        if (!evt)
        {
          // Channel closed (cancelled) - abort without touching the history.
          failed = true;
          break;
        }
        if (evt->type == api::event_type::done)
        {
          break;
        }
        if (evt->type == api::event_type::error)
        {
          sink_->output_error("Compaction failed: " + evt->payload);
          failed = true;
          break;
        }

        try
        {
          auto j = api::json::parse(evt->payload);
          if (j.contains("usage"))
          {
            if (stats_ch_)
            {
              stats_ch_->try_send({stat_id::api_usage, j.at("usage")});
            }
          }
          auto delta = provider_->parse_stream_delta(j);
          for (const auto& mc : delta.content)
          {
            if (mc.type == "text")
            {
              summary += mc.content;
            }
          }
        }
        catch (...) {}
      }

      agent_system::instance().clear_current_response_ch();

      if (failed || summary.empty())
      {
        if (!failed)
        {
          sink_->output_error("Compaction failed: the model returned an empty summary.");
        }
        sink_->output_line("Conversation history is unchanged.");
        co_return;
      }

      chat_.apply_compaction(summary, keep_turns);
      chat_.save(".mistral-cli", session_id_);
      sink_->output_line(std::format(
        "Compacted: history is now {} message(s).", chat_.size()));
      co_return;
    }

    std::shared_ptr<render_sink> sink_;
  };
} // namespace mistral::agent

#endif // MISTRAL_INTERACTIVE_AGENT_TASK_H