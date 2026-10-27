#ifndef MISTRAL_AGENT_TASK_BASE_H
#define MISTRAL_AGENT_TASK_BASE_H

#include "agent/agent_config.h"
#include "agent/agent_system.h"
#include "agent/approval_manager.h"
#include "agent/conversation.h"
#include "agent/policy_engine.h"
#include "agent/tool_registry.h"
#include "api/provider.h"
#include "api/provider_registry.h"
#include "api/types.h"
#include "tools/edit_parser.h"

#include <dripcore/awaitable.h>
#include <dripcore/channel.h>
#include <dripcore/task.h>
#include <dripcore/task_handle.h>

#include <cstdint>
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

    // Returns true if the error is transient and the request should be
    // retried (e.g. HTTP 503, connection closed, stream reset).
    static bool is_retryable_error(const api::api_event& evt);

  protected:
    // Virtual methods for customizing behavior
    virtual void thinking_begin() = 0;
    virtual void thinking_end() = 0;

    // Common agent loop that processes a single message
    virtual auto agent_loop() -> dripcore::awaitable<void>;

    // Request endpoint path for the agent loop.
    // Default: POST /<api_version>/chat/completions.
    virtual std::string chat_request_path() const;
    // Build the request body for the agent loop.
    // Default: a chat completion request via the provider.
    virtual std::string build_request_body(const api::json& tools_schema) const;
    // Whether the agent loop's request expects a streaming (SSE) response.
    // Default: true (chat completions). Tasks using a non-streaming
    // endpoint override this.
    virtual bool request_stream() const;
    // Parse one response data event into a stream delta.
    // Default: chat completion chunks via the provider.
    virtual api::stream_delta parse_response_delta(const api::json& j) const;
    virtual void agent_loop_on_sending_chat_request() = 0;
    virtual void agent_loop_on_message_content(const api::message_content& mc) = 0;
    virtual auto agent_loop_do_tool_call(const api::tool_call& tc) -> dripcore::awaitable<void> = 0;
    virtual void agent_loop_on_error(const std::string& error) = 0;
    virtual void agent_loop_on_completed(std::vector<api::message_content>& accumulated_content);
    virtual void agent_loop_on_cancelled()
    {
    }

    // Rebuilds the tool registry when MCP tools were registered after
    // construction (e.g. via /mcp connect), and surfaces MCP server
    // instructions in the system prompt.
    void refresh_mcp_tools();

    // Helper methods
    std::optional<std::pair<std::string, std::vector<tools::file_edit_t>>> try_parse_and_store_patchset(
      const std::vector<api::message_content>& accumulated_content
    );

    // Parse edit blocks from accumulated content and append them to an
    // ongoing (non-applied) patchset. Returns the patchset ID and full
    // edit list if edits were appended, or std::nullopt if no edits
    // were found or no ongoing patchset exists.
    std::optional<std::pair<std::string, std::vector<tools::file_edit_t>>> try_parse_and_append_edits(
      const std::vector<api::message_content>& accumulated_content
    );
    void accumulate_message_content(std::vector<api::message_content>& accumulated, api::message_content mc);
    std::optional<std::string> accumulate_tool_call(std::vector<api::tool_call>& accumulated, api::tool_call tc);
    auto execute_tool(const api::tool_call& tc) -> dripcore::awaitable<std::string>;

    // Shared tool-call helpers used by subclasses' agent_loop_do_tool_call.
    // These extract common policy/execution logic so subclasses only
    // need to handle sink output and approval flow.
    static std::string accumulate_text(const std::vector<api::message_content>& accumulated_content);
    // Structural pre-check of a tool call, run before policy evaluation
    // and approval. Returns an error message when the call is malformed
    // and cannot succeed (e.g. an exec command containing shell syntax
    // or naming a command that does not exist), or std::nullopt when
    // the call should proceed to policy evaluation.
    std::optional<std::string> precheck_tool_call(const api::tool_call& tc, const api::json& tool_args) const;
    policy_result evaluate_tool_policy(const api::tool_call& tc, const api::json& tool_args) const;
    static api::json build_tool_rejection(const std::string& status, const std::string& message);
    auto execute_and_record_tool(const api::tool_call& tc) -> dripcore::awaitable<std::string>;

    // Agent identifier
    std::string agent_id_;

    // Configuration-derived properties
    std::string model_;
    std::string api_version_;
    std::unique_ptr<api::provider> provider_;
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

    // MCP registry version at construction / last refresh
    uint64_t mcp_registry_version_ = 0;
  };
} // namespace mistral::agent

#endif // MISTRAL_AGENT_TASK_BASE_H
