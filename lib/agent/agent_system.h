#ifndef MISTRAL_AGENT_SYSTEM_H
#define MISTRAL_AGENT_SYSTEM_H

#include "agent/agent_config.h"
#include "agent/tool_registry.h"
#include "agent/policy_engine.h"
#include "agent/approval_manager.h"
#include "agent/save_command.h"
#include "console/slash_command_registry.h"

#include <dripcore/channel.h>
#include <api/mistral_types.h>

#include <string>
#include <memory>
#include <map>
#include <vector>

namespace mistral::tools
{
  class file_tools;
  class note_tools;
  class grep_tools;
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

    // Shared channels
    std::shared_ptr<dripcore::channel<api::api_request>> request_ch() const;
    std::shared_ptr<dripcore::channel<api::api_event>> response_ch() const;

    // Notes directory
    const std::string& notes_dir() const { return notes_dir_; }

    // Last AI response access
    std::string get_last_response() const;
    void set_last_response(const std::string& response);

    // Register built-in slash commands
    void register_slash_commands();

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

    tool_registry global_tools_;

    std::shared_ptr<dripcore::channel<api::api_request>> request_ch_;
    std::shared_ptr<dripcore::channel<api::api_event>> response_ch_;

    // Last AI response storage
    mutable std::mutex last_response_mtx_;
    std::string last_response_;
  };

} // namespace mistral::agent

#endif // MISTRAL_AGENT_SYSTEM_H
