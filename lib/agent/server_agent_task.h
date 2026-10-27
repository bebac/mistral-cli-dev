#ifndef MISTRAL_SERVER_AGENT_TASK_H
#define MISTRAL_SERVER_AGENT_TASK_H

#include "agent/subagent_task.h"

#include <optional>
#include <string>

namespace mistral::agent
{
  // Agent task backed by a server-side Mistral Agents API entity
  // (POST /v1/agents + POST /v1/conversations). The server-side
  // agent owns the model, instructions and builtin tools (e.g.
  // web_search), which execute server-side; this task only supplies the
  // conversation inputs and consumes the (non-streaming) response.
  //
  // The server-side agent entity is resolved lazily on the first turn:
  // an existing agent with the same name is reused (GET
  // /v1/agents/pages?name=...), otherwise a new one is created (POST
  // /v1/agents). The resolved id is cached in agent_system for the rest
  // of the session.
  class server_agent_task : public subagent_task
  {
  public:
    using subagent_task::subagent_task;

  protected:
    std::string chat_request_path() const override;
    std::string build_request_body(const api::json& tools_schema) const override;
    bool request_stream() const override;
    api::stream_delta parse_response_delta(const api::json& j) const override;
    auto agent_loop() -> dripcore::awaitable<void> override;

  private:
    // Concatenated text of the conversation messages (the request's
    // "inputs" field). System messages are skipped — the server-side
    // agent owns the instructions.
    std::string conversation_text() const;

    // Ensure the server-side agent entity exists. Returns true when
    // server_agent_id_ is set.
    auto ensure_server_agent() -> dripcore::awaitable<bool>;
    // Find a server-side agent by exact name. Returns the entity when found.
    auto find_server_agent_by_name(const std::string& name) -> dripcore::awaitable<std::optional<api::agent_entity>>;
    // Create the server-side agent entity. Returns its id on success.
    auto create_server_agent() -> dripcore::awaitable<std::optional<std::string>>;
    // Update an existing server-side agent after the local config drifted
    // (creates a new version server-side). Returns true on success.
    auto update_server_agent(const std::string& agent_id) -> dripcore::awaitable<bool>;
    // Fingerprint of the current server_agent config (stored in the
    // server-side agent's metadata to detect drift on reuse).
    std::string config_fingerprint() const;

    std::string server_agent_id_;
    std::string setup_error_;
    bool server_agent_resolved_ = false;
  };
} // namespace mistral::agent

#endif // MISTRAL_SERVER_AGENT_TASK_H
