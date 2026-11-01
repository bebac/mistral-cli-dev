#ifndef MISTRAL_AGENT_UI_STATUS_H
#define MISTRAL_AGENT_UI_STATUS_H

#include "slash_command/command_spec.h"

#include <optional>
#include <string>
#include <vector>

namespace mistral::agent
{
  /**
   * @brief Snapshot of the state a frontend shows outside the transcript.
   *
   * Pushed by agent_system::publish_ui_status() whenever one of its inputs
   * changes, and applied on the UI thread. Every producer runs on the
   * scheduler thread, so building a snapshot needs no locking: it reads
   * mcp_manager state on the same thread that mutates it.
   *
   * A snapshot supersedes any queued one, so a dropped send is harmless.
   */
  struct ui_status
  {
    /// Names of the servers with auto_connect=true. Fixed by configure()
    /// at startup, so it never changes after the first snapshot.
    std::vector<std::string> mcp_servers;

    /// Every auto-connect server is connected.
    bool mcp_all_connected = false;

    /// connect_auto() has completed at least once.
    bool mcp_finished = false;

    /// Cumulative token count; empty until the first API response.
    std::optional<int> total_tokens;

    /// Dynamic slash command completion candidates (MCP servers and the
    /// current agent's tools).
    slash_command::completion_context completion;
  };
} // namespace mistral::agent

#endif // MISTRAL_AGENT_UI_STATUS_H
