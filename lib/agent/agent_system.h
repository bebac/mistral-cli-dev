#ifndef MISTRAL_AGENT_SYSTEM_H
#define MISTRAL_AGENT_SYSTEM_H

#include "agent/agent_config.h"
#include "agent/agent_system_stats_task.h"
#include "agent/approval_manager.h"
#include "agent/load_command.h"
#include "agent/policy_engine.h"
#include "agent/save_command.h"
#include "agent/tool_command.h"
#include "agent/tool_registry.h"
#include "agent/ui_status.h"
#include "agent/usage_command.h"
#include "common/shutdown_signal.h"
#include "slash_command/slash_command_registry.h"
#include "tools/edit_types.h"
#include "tools/patchset_store.h"

#include <api/chat_types.h>
#include <api/types.h>
#include <dripcore/channel.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dripcore
{
  class scheduler;
}

namespace mistral::common
{
  class process_manager;
} // namespace mistral::common

namespace mistral::tools
{
  class file_tools;
  class note_tools;
#ifdef MISTRAL_ENABLE_TREE_SITTER
  class code_tools;
#endif
  class exec_tools;
  class task_tools;
  class patchset_tools;
  class research_tools;
  class ask_tools;
  class process_tools;
} // namespace mistral::tools

namespace mistral::agent
{
  // ---- Transport Endpoint ----
  // A distinct model API endpoint derived from the agent configs: one
  // entry per (host, port, api_key_env) referenced by any agent's
  // model. Agents that share an endpoint share its request channel.
  struct transport_endpoint
  {
    std::string host = "api.mistral.ai";
    int port = 443;
    std::string api_key_env; // Env var holding the API key (empty: no key required)

    bool operator==(const transport_endpoint& other) const = default;
  };

  // ---- Tool Catalog ----
  // Immutable snapshot of the registered tools, published by agent_system
  // on the scheduler thread (the only thread that mutates the global tool
  // registry, via MCP connects). Readers copy the shared_ptr, so they never
  // touch the live registry and can safely run on the UI thread.
  struct tool_catalog
  {
    std::vector<std::string> all;                                  ///< Every registered tool name.
    std::map<std::string, std::vector<std::string>> per_agent;     ///< agent id -> tool names it can call.
    std::map<std::string, std::vector<std::string>> mcp_by_server; ///< MCP server -> its tool names.
  };

  // ---- Agent System Singleton ----
  // Provides shared resources, configuration access, and model management.
  class agent_system : public tools::patchset_store
  {
  public:
    static agent_system& instance();

    // Initialize the agent system with configuration
    void initialize(
      const std::string& config_file,
      const std::string& base_dir,
      const std::string& notes_dir
    );

    // Resolve system prompt (handles file:// URLs). When the agent's
    // available tools include ask_user (or the wildcard), a brief
    // "Asking the human" introduction is appended.
    std::string resolve_system_prompt(const agent_config& agent_cfg, const std::string& model_id) const;

    // Resolve server-agent instructions (handles file:// URLs)
    std::string resolve_server_agent_instructions(const agent_config& agent_cfg) const;

    // Server-side agent id cache (local agent id -> server agent entity id)
    std::optional<std::string> get_server_agent_id(const std::string& agent_id) const;
    void set_server_agent_id(const std::string& agent_id, const std::string& server_agent_id);

    // Resolve tool policy (handles file:// URLs and JSON strings)
    json resolve_tool_policy(const agent_config& agent_cfg) const;

    // Resolve tool schema options (model defaults + agent overrides).
    // These are client-side schema generation options, not API request params.
    tool_schema_options resolve_tool_schema_options(const agent_config& agent_cfg) const;

    // Agent configuration access
    const agent_config& get_agent_config(const std::string& agent_id) const;
    bool has_agent_config(const std::string& agent_id) const;

    // Model configuration access
    const model_config& get_model_config(const std::string& model_name) const;
    bool has_model_config(const std::string& model_name) const;

    // Resolve sampling parameters (model defaults + agent overrides)
    api::sampling_params resolve_sampling_params(const agent_config& agent_cfg) const;

    // Resolved model id of a model entry: the active /model selection when
    // one was made, otherwise the entry's first id.
    std::string resolve_model_id(const std::string& model_name) const;

    // Resolved model id of an agent: the active /model selection when one
    // was made, otherwise the agent's own model_id default when it names an
    // id the entry offers, otherwise the entry's first id. Tasks and
    // frontends should prefer this overload, so a per-agent default is
    // honoured.
    std::string resolve_model_id(const agent_config& agent_cfg) const;

    // Selectable model ids of a model entry (empty when the entry is
    // unknown). The first id is the default; the active one is tracked
    // per entry and returned by resolve_model_id().
    std::vector<std::string> model_ids_for(const std::string& model_name) const;

    // Switch the active model id of a model entry. Throws when the entry
    // is unknown or the id is not one of the entry's model_ids. Running
    // agent tasks learn about the switch through the [MODEL] control
    // message (see model_command.h), not through this call.
    void set_active_model_id(const std::string& model_name, const std::string& model_id);

    // Resolve the distinct transport endpoints needed by the configured
    // agents: each agent's model contributes its base_url (host, port)
    // and api_key_env; duplicates are merged. Models no agent references
    // contribute nothing.
    std::vector<transport_endpoint> resolve_transport_endpoints() const;

    // Get the full configuration
    const full_config& get_config() const
    {
      return config_;
    }

    // Tool registry
    tool_registry create_tool_registry(const std::vector<std::string>& available_tools) const;
    const tool_registry& global_tools() const
    {
      return global_tools_;
    }
    tool_registry& global_tools()
    {
      return global_tools_;
    }

    // Immutable snapshot of the registered tools. Safe to read from any
    // thread: the returned catalog is never mutated.
    std::shared_ptr<const tool_catalog> tool_catalog_snapshot() const;

    // Rebuild the snapshot. Must be called on the scheduler thread, which is
    // the only thread that mutates the global tool registry.
    void refresh_tool_catalog();

    // Tool names available to `agent_id` (empty when the agent is unknown).
    std::vector<std::string> agent_tool_names(const std::string& agent_id) const;

    // Tool names available to the current interactive agent (empty when no
    // agent is active).
    std::vector<std::string> current_agent_tool_names() const;

    // Project the tool catalog into the completion context a frontend needs:
    // MCP server names and the current agent's tool names. Safe to call from
    // any thread.
    slash_command::completion_context build_completion_context() const;

    // Channel carrying ui_status snapshots to the frontend. Created in
    // initialize(); a frontend that never reads it simply fills the buffer.
    std::shared_ptr<dripcore::channel<ui_status>> ui_status_ch() const;

    // Shutdown broadcast. Bridges register a callback that releases them, so
    // their exit does not depend on their producer closing the data channel.
    std::shared_ptr<common::shutdown_signal> shutdown_signal() const;

    // Fire the shutdown signal so every registered bridge can exit. Without
    // this a bridge stays suspended on recv() and the scheduler never
    // drains. Must be called on the scheduler thread.
    void fire_shutdown();

    // Build a ui_status snapshot and push it to the frontend. Must be
    // called on the scheduler thread: it reads mcp_manager state, which
    // only that thread mutates.
    void publish_ui_status();

    // Current interactive agent id. Registered by interactive_agent_task so
    // slash commands and completers can resolve "the agent the user is
    // talking to".
    void set_current_agent_id(const std::string& agent_id);
    std::optional<std::string> current_agent_id() const;

    // Access to global tool instances
    mistral::tools::file_tools& file_tools()
    {
      return *file_tools_;
    }
    mistral::tools::note_tools& note_tools()
    {
      return *note_tools_;
    }
#ifdef MISTRAL_ENABLE_TREE_SITTER
    mistral::tools::code_tools& code_tools()
    {
      return *code_tools_;
    }
#endif
    mistral::tools::exec_tools& exec_tools()
    {
      return *exec_tools_;
    }
    mistral::tools::task_tools& task_tools()
    {
      return *task_tools_;
    }
    mistral::tools::patchset_tools& patchset_tools()
    {
      return *patchset_tools_;
    }
    mistral::tools::research_tools& research_tools()
    {
      return *research_tools_;
    }
    mistral::tools::ask_tools& ask_tools()
    {
      return *ask_tools_;
    }
    mistral::common::process_manager& process_manager()
    {
      return *process_manager_;
    }
    mistral::tools::process_tools& process_tools()
    {
      return *process_tools_;
    }

    // Request channel for the given agent's transport endpoint
    // (agent -> model -> endpoint -> channel). Throws if the agent or
    // its model is unknown.
    std::shared_ptr<dripcore::channel<api::api_request>> request_ch_for(const std::string& agent_id) const;

    // Fail-fast check for the endpoints the given agent can actually
    // reach: its own model's endpoint plus those of the subagents it
    // delegates to, transitively. Endpoints belonging to agents this
    // session never uses are not checked, so a config may hold an agent
    // on a second provider without that provider's key being set.
    // Throws std::runtime_error naming the endpoint, the env var and the
    // agent that needs it.
    void validate_transport_keys(const std::string& agent_id) const;

    // Spawn one http_coordinator_task per transport endpoint on the
    // scheduler. Validates API keys first (see validate_transport_keys).
    // Call once after creating the scheduler, before spawning any
    // agent task.
    void start_transport(dripcore::scheduler& sched, const std::string& agent_id);
    void stop_transport();

    // Shared channels
    std::shared_ptr<dripcore::channel<stat_message>> stats_ch() const;

    // Notes directory
    const std::string& notes_dir() const
    {
      return notes_dir_;
    }

    // Last AI response access
    std::string get_last_response() const;
    void set_last_response(const std::string& response);

    // Get current usage stats
    json get_usage_stats() const;
    void update_usage_stats(const json& stats);
    void reset_usage_stats();

    // Patchset storage - only one at a time (the latest)
    void store_patchset(const std::string& patchset_id, const std::vector<tools::file_edit_t>& edits) override;
    void append_edits(const std::string& patchset_id, const std::vector<tools::file_edit_t>& edits) override;
    std::optional<tools::patchset_t> get_patchset(const std::string& patchset_id) const override;
    std::optional<std::string> get_current_patchset_id() const override;
    void remove_patchset(const std::string& patchset_id) override;
    void mark_applied(const std::string& patchset_id) override;
    bool has_patchset() const;

    // Slash command registry
    std::shared_ptr<slash_command::slash_command_registry> slash_commands()
    {
      return slash_command_registry_;
    }

    // Register built-in slash commands
    void register_slash_commands();

    // Run a subagent with a message and return its response
    dripcore::awaitable<std::string> run_subagent(
      dripcore::task& task,
      const std::string& agent_id,
      const std::string& message
    );

    // Cancellation hook for the operation currently in flight: an API
    // response stream, or a running exec. The owning task registers a
    // closure that stops it (closing its channel, or setting its cancel
    // flag) and clears it when the operation ends; signal_watcher_task
    // invokes it when SIGINT arrives.
    //
    // The slot is a single global, so concurrent agents overwrite each
    // other's hook. That is pre-existing behaviour; the callback form at
    // least makes a per-agent registry a later, local change.
    void set_current_cancel(std::function<void()> cancel);
    void clear_current_cancel();
    // Invokes the registered hook, if any. Safe to call with nothing
    // registered, and safe to call from the signal watcher.
    void cancel_current();

    // Active agent input channel. Registered by the interactive agent
    // task at startup so slash commands (e.g. /compact) can route
    // control messages to the task that owns the conversation.
    void set_current_input_ch(std::shared_ptr<dripcore::channel<std::string>> ch);
    std::shared_ptr<dripcore::channel<std::string>> get_current_input_ch() const;

    // Shutdown pipe for notifying the signal_watcher_task to exit.
    void set_shutdown_pipe_fd(int fd);
    void signal_shutdown();

  private:
    agent_system() = default;
    ~agent_system() = default;

    // Prevent copying
    agent_system(const agent_system&) = delete;
    agent_system& operator=(const agent_system&) = delete;

    // Load configuration from file
    full_config load_config_from_file(const std::string& config_file) const;

    // Resolve a prompt string (handles file:// URLs and templates)
    std::string resolve_prompt_string(const std::string& prompt, const std::string& model_id) const;

    // Shared by the resolve_model_id() overloads: the entry's active
    // selection, then `default_id` when the entry offers it, then the
    // entry's first id.
    std::string resolve_model_id(const std::string& model_name, const std::optional<std::string>& default_id) const;

    // Agent ids reachable from `agent_id`: the agent itself plus every
    // subagent named in its subagent_tools, transitively. Cycles are
    // tolerated; ids that name no configured agent are returned as-is.
    std::vector<std::string> reachable_agent_ids(const std::string& agent_id) const;

    // Initialize tool instances
    void initialize_tools(const std::string& notes_dir);

    // Build global tool registry
    void build_global_tool_registry();

  private:
    full_config config_;
    std::string base_dir_;
    std::string notes_dir_;

    std::unique_ptr<mistral::tools::file_tools> file_tools_;
    std::unique_ptr<mistral::tools::note_tools> note_tools_;
#ifdef MISTRAL_ENABLE_TREE_SITTER
    std::unique_ptr<mistral::tools::code_tools> code_tools_;
#endif
    std::unique_ptr<mistral::tools::exec_tools> exec_tools_;
    std::unique_ptr<mistral::tools::task_tools> task_tools_;
    std::unique_ptr<mistral::tools::patchset_tools> patchset_tools_;
    std::unique_ptr<mistral::tools::research_tools> research_tools_;
    std::unique_ptr<mistral::tools::ask_tools> ask_tools_;
    // Background process sessions (process_* tools). Plain member, not
    // a singleton, so tests can construct their own manager + tools.
    std::unique_ptr<mistral::common::process_manager> process_manager_;
    std::unique_ptr<mistral::tools::process_tools> process_tools_;

    tool_registry global_tools_;

    // One request channel per distinct transport endpoint, built in
    // initialize() from the agent configs.
    struct endpoint_entry
    {
      transport_endpoint endpoint;
      std::shared_ptr<dripcore::channel<api::api_request>> request_ch;
    };
    std::vector<endpoint_entry> endpoints_;

    // Server-side agent id cache (local agent id -> server agent entity id)
    mutable std::mutex server_agent_ids_mtx_;
    std::map<std::string, std::string> server_agent_ids_;

    // Last AI response storage
    mutable std::mutex last_response_mtx_;
    std::string last_response_;

    // Stats channel
    std::shared_ptr<dripcore::channel<stat_message>> stats_ch_;

    // UI status channel (see ui_status)
    std::shared_ptr<dripcore::channel<ui_status>> ui_status_ch_;

    // Shutdown broadcast (see shutdown_signal())
    std::shared_ptr<common::shutdown_signal> shutdown_signal_;

    // Usage statistics storage
    mutable std::mutex usage_stats_mtx_;
    json usage_stats_;

    // Slash command registry
    std::shared_ptr<slash_command::slash_command_registry> slash_command_registry_;

    // Patchset storage - only one at a time (the latest)
    std::optional<tools::patchset_t> current_patchset_;

    // Cancellation hook for the operation currently in flight
    mutable std::mutex current_cancel_mtx_;
    std::function<void()> current_cancel_;

    // Active agent input channel (for slash-command control routing)
    mutable std::mutex current_input_ch_mtx_;
    std::shared_ptr<dripcore::channel<std::string>> current_input_ch_;

    // Current interactive agent id
    mutable std::mutex current_agent_id_mtx_;
    std::optional<std::string> current_agent_id_;

    // Active model id per model entry (see resolve_model_id()). Entries
    // without an explicit selection use model_ids[0].
    mutable std::mutex active_model_ids_mtx_;
    std::map<std::string, std::string> active_model_ids_;

    // Tool catalog snapshot (see tool_catalog)
    mutable std::mutex tool_catalog_mtx_;
    std::shared_ptr<const tool_catalog> tool_catalog_;

    // Shutdown pipe write end (for notifying signal_watcher_task)
    int shutdown_pipe_write_fd_ = -1;
  };

} // namespace mistral::agent

#endif // MISTRAL_AGENT_SYSTEM_H
