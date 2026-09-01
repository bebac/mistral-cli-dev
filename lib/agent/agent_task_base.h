#ifndef MISTRAL_AGENT_TASK_BASE_H
#define MISTRAL_AGENT_TASK_BASE_H

#include "agent/conversation.h"
#include "agent/tool_registry.h"
#include "agent/policy_engine.h"
#include "agent/approval_manager.h"
#include "agent/agent_system.h"
#include "agent/agent_config.h"
#include "agent/edit_parser.h"
#include "api/mistral_api.h"
#include "api/mistral_types.h"

#include <dripcore/task.h>
#include <dripcore/channel.h>
#include <dripcore/task_handle.h>
#include <dripcore/awaitable.h>

#include <memory>
#include <string>
#include <vector>

namespace mistral::agent
{
  class agent_task_base : public dripcore::task
  {
  public:
    agent_task_base(
      const std::string& agent_id,
      std::shared_ptr<dripcore::channel<std::string>> input_ch,
      std::shared_ptr<dripcore::channel<std::string>> output_ch = nullptr,
      std::shared_ptr<dripcore::channel<std::string>> prompt_ch = nullptr
    );

    virtual ~agent_task_base() = default;

  protected:
    // Virtual methods for customizing behavior
    virtual void thinking_begin() = 0;
    virtual void thinking_end() = 0;

    // Common agent loop that processes a single message
    virtual auto agent_loop() -> dripcore::awaitable<void>;
    virtual void agent_loop_on_sending_chat_request() = 0;
    virtual void agent_loop_on_message_content(const api::message_content& mc) = 0;
    virtual auto agent_loop_do_tool_call(const api::tool_call& tc) -> dripcore::awaitable<void> = 0;
    virtual void agent_loop_on_error(const std::string& error) = 0;
    virtual void agent_loop_on_completed(std::vector<api::message_content>& accumulated_content);
    virtual void agent_loop_on_cancelled() {}

    // Returns true if the error is transient and the request should be
    // retried (e.g. HTTP 503, connection closed, stream reset).
    static bool is_retryable_error(const std::string& error);

    // Helper methods
    std::optional<std::pair<std::string, std::vector<file_edit_t>>>
      try_parse_and_store_patchset(const std::vector<api::message_content>& accumulated_content);

    // Parse edit blocks from accumulated content and append them to an
    // ongoing (non-applied) patchset. Returns the patchset ID and full
    // edit list if edits were appended, or std::nullopt if no edits
    // were found or no ongoing patchset exists.
    std::optional<std::pair<std::string, std::vector<file_edit_t>>>
      try_parse_and_append_edits(const std::vector<api::message_content>& accumulated_content);
    void accumulate_message_content(std::vector<api::message_content>& accumulated, api::message_content mc);
    void accumulate_tool_call(std::vector<api::tool_call>& accumulated, api::tool_call tc);
    auto execute_tool(const api::tool_call& tc) -> dripcore::awaitable<std::string>;

    // Agent identifier
    std::string agent_id_;

    // Configuration-derived properties
    std::string model_;
    std::string api_version_;
    std::string system_prompt_;
    api::sampling_params sampling_params_;
    tool_registry tools_;
    policy_engine policy_engine_;
    approval_manager approval_manager_;

    // Shared state
    conversation chat_;
    std::string session_id_;

    // Channels
    std::shared_ptr<dripcore::channel<std::string>> input_ch_;
    std::shared_ptr<dripcore::channel<std::string>> output_ch_;
    std::shared_ptr<dripcore::channel<std::string>> prompt_ch_;
    std::shared_ptr<dripcore::channel<api::api_request>> request_ch_;
    std::shared_ptr<dripcore::channel<api::api_event>> response_ch_;
    std::shared_ptr<dripcore::channel<stat_message>> stats_ch_;

    bool in_thinking_ = false;
  };
} // namespace mistral::agent

#endif // MISTRAL_AGENT_TASK_BASE_H
