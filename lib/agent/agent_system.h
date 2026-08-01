#ifndef MISTRAL_AGENT_SYSTEM_H
#define MISTRAL_AGENT_SYSTEM_H

#include "agent/agent_config.h"
#include "agent/tool_registry.h"
#include "agent/policy_engine.h"
#include "agent/approval_manager.h"
#include "agent/save_command.h"
#include "agent/load_command.h"
#include "agent/usage_command.h"
#include "agent/patchset_verify_command.h"
#include "agent/agent_system_stats_task.h"
#include "console/slash_command_registry.h"

#include <dripcore/channel.h>
#include <api/mistral_types.h>
#include <api/mistral_api.h>

#include <string>
#include <memory>
#include <map>
#include <vector>

namespace mistral::tools
{
  class file_tools;
  class note_tools;
  class grep_tools;
  class exec_tools;
}

namespace mistral::agent
{
  // ---- Agent System Singleton ----
  // Provides shared resources, configuration access, and model management.
  class agent_system
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
    mistral::tools::grep_tools& grep_tools() { return *grep_tools_; }
    mistral::tools::exec_tools& exec_tools() { return *exec_tools_; }

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

    // Register built-in slash commands
    void register_slash_commands();

    // Run a subagent with a message and return its response
    dripcore::awaitable<std::string> run_subagent(
      dripcore::task& task,
      const std::string& agent_id,
      const std::string& message
    );

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
    std::unique_ptr<mistral::tools::grep_tools> grep_tools_;
    std::unique_ptr<mistral::tools::exec_tools> exec_tools_;

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
  };

} // namespace mistral::agent

#endif // MISTRAL_AGENT_SYSTEM_H
