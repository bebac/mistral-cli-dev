#ifndef MISTRAL_AGENT_SYSTEM_H
#define MISTRAL_AGENT_SYSTEM_H

#include "agent/agent_config.h"
#include "agent/tool_registry.h"
#include "agent/policy_engine.h"
#include "agent/approval_manager.h"
#include "agent/save_command.h"
#include "agent/load_command.h"
#include "agent/usage_command.h"
#include "agent/tool_command.h"
#include "agent/agent_system_stats_task.h"
#include "tools/patchset_store.h"
#include "tools/edit_types.h"
#include "slash_command/slash_command_registry.h"

#include <dripcore/channel.h>
#include <api/types.h>
#include <api/chat_types.h>

#include <string>
#include <memory>
#include <map>
#include <mutex>
#include <vector>

namespace mistral::tools
{
  class file_tools;
  class note_tools;
  class code_tools;
  class exec_tools;
  class task_tools;
  class patchset_tools;
}

namespace mistral::agent
{
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
      const std::string& notes_dir,
      std::shared_ptr<dripcore::channel<api::api_request>> request_ch
    );

    // Resolve system prompt (handles file:// URLs)
    std::string resolve_system_prompt(
        const agent_config& agent_cfg,
        const std::string& model_id) const;

    // Resolve tool policy (handles file:// URLs and JSON strings)
    json resolve_tool_policy(
        const agent_config& agent_cfg) const;

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
    std::string resolve_model_id(const std::string& model_name) const;

    // Get the full configuration
    const full_config& get_config() const { return config_; }

    // Tool registry
    tool_registry create_tool_registry(const std::vector<std::string>& available_tools) const;
    const tool_registry& global_tools() const { return global_tools_; }
    tool_registry& global_tools() { return global_tools_; }

    // Access to global tool instances
    mistral::tools::file_tools& file_tools() { return *file_tools_; }
    mistral::tools::note_tools& note_tools() { return *note_tools_; }
    mistral::tools::code_tools& code_tools() { return *code_tools_; }
    mistral::tools::exec_tools& exec_tools() { return *exec_tools_; }
    mistral::tools::task_tools& task_tools() { return *task_tools_; }
    mistral::tools::patchset_tools& patchset_tools() { return *patchset_tools_; }

    // Shared channels
    std::shared_ptr<dripcore::channel<api::api_request>> request_ch() const;
    std::shared_ptr<dripcore::channel<api::api_event>> response_ch() const;
    std::shared_ptr<dripcore::channel<stat_message>> stats_ch() const;

    // Notes directory
    const std::string& notes_dir() const { return notes_dir_; }

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
    std::optional<std::string> get_current_patchset_id() const;
    void remove_patchset(const std::string& patchset_id) override;
    void mark_applied(const std::string& patchset_id) override;
    bool has_patchset() const;

    // Slash command registry
    std::shared_ptr<slash_command::slash_command_registry> slash_commands() { return slash_command_registry_; }

    // Register built-in slash commands
    void register_slash_commands();

    // Run a subagent with a message and return its response
    dripcore::awaitable<std::string> run_subagent(
      dripcore::task& task,
      const std::string& agent_id,
      const std::string& message
    );

    // Current response channel for request cancellation support.
    // Registered by agent_task_base before each API request and
    // cleared after the request completes. The signal_watcher_task
    // closes this channel when SIGINT is received to cancel the
    // in-flight request.
    void set_current_response_ch(std::shared_ptr<dripcore::channel<api::api_event>> ch);
    void clear_current_response_ch();
    std::shared_ptr<dripcore::channel<api::api_event>> get_current_response_ch() const;

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
    std::unique_ptr<mistral::tools::code_tools> code_tools_;
    std::unique_ptr<mistral::tools::exec_tools> exec_tools_;
    std::unique_ptr<mistral::tools::task_tools> task_tools_;
    std::unique_ptr<mistral::tools::patchset_tools> patchset_tools_;

    tool_registry global_tools_;

    std::shared_ptr<dripcore::channel<api::api_request>> request_ch_;
    std::shared_ptr<dripcore::channel<api::api_event>> response_ch_;

    // Last AI response storage
    mutable std::mutex last_response_mtx_;
    std::string last_response_;

    // Stats channel
    std::shared_ptr<dripcore::channel<stat_message>> stats_ch_;

    // Usage statistics storage
    mutable std::mutex usage_stats_mtx_;
    json usage_stats_;

    // Slash command registry
    std::shared_ptr<slash_command::slash_command_registry> slash_command_registry_;

    // Patchset storage - only one at a time (the latest)
    std::optional<tools::patchset_t> current_patchset_;

    // Current response channel for request cancellation
    mutable std::mutex current_response_ch_mtx_;
    std::shared_ptr<dripcore::channel<api::api_event>> current_response_ch_;

    // Active agent input channel (for slash-command control routing)
    mutable std::mutex current_input_ch_mtx_;
    std::shared_ptr<dripcore::channel<std::string>> current_input_ch_;

    // Shutdown pipe write end (for notifying signal_watcher_task)
    int shutdown_pipe_write_fd_ = -1;
  };

} // namespace mistral::agent

#endif // MISTRAL_AGENT_SYSTEM_H
