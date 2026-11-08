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

    /// Resolved model id of the current agent; empty when unknown. Lets
    /// the frontend footer follow mid-session /model switches.
    std::string model_id;

    /// Live knowledge-index progress for the footer. `phase` is empty
    /// while nothing is indexing; otherwise a short word ("chunk",
    /// "embed", "write") naming the current phase, with `done` of
    /// `total` items in it.
    ///
    /// Deliberately a plain word and two integers rather than the
    /// knowledge library's own types: a frontend only needs to render
    /// it, and this header is included by code built without
    /// MISTRAL_ENABLE_KNOWLEDGE.
    struct index_progress
    {
      std::string phase;
      int done = 0;
      int total = 0;
      /// Candidate files in the tree, known once the walk finishes and 0
      /// before that. Constant for the rest of the refresh.
      int files_total = 0;
    } index;

    /// Dynamic slash command completion candidates (MCP servers and the
    /// current agent's tools).
    slash_command::completion_context completion;
  };
} // namespace mistral::agent

#endif // MISTRAL_AGENT_UI_STATUS_H
