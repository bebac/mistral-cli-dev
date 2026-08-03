#include "agent/agent_system.h"
#include "agent/agent_config.h"
#include "agent/subagent_task.h"
#include "tools/file_tools.h"
#include "tools/note_tools.h"
#include "tools/grep_tools.h"
#include "tools/exec_tools.h"
#include "template/template_engine.h"

#include <api/mistral_api.h>

#include <dripcore/task.h>
#include <dripcore/awaitable.h>

#include <fstream>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace mistral::agent
{

  namespace fs = std::filesystem;
  using json = nlohmann::ordered_json;

  // ---- Singleton Instance ----

  agent_system &agent_system::instance()
  {
    static agent_system instance;
    return instance;
  }

  // ---- Initialization ----

  void agent_system::initialize(
    const std::string &config_file,
    const std::string& base_dir,
    const std::string &notes_dir,
    std::shared_ptr<dripcore::channel<api::api_request>> request_ch
  )
  {
    base_dir_ = base_dir;
    notes_dir_ = notes_dir;
    request_ch_ = std::move(request_ch);

    // Initialize stats channel
    stats_ch_ = std::make_shared<dripcore::channel<stat_message>>(32);

    // Load configuration
    config_ = load_config_from_file(config_file);

    // Initialize tool instances
    initialize_tools(notes_dir_);

    // Build global tool registry
    build_global_tool_registry();

    // Register slash commands
    register_slash_commands();
  }

  // ---- Tool Policy Resolution ----

  json agent_system::resolve_tool_policy(
      const agent_config& agent_cfg) const
  {
    std::string tool_policy = agent_cfg.tool_policy;

    // Check if tool_policy is a file:// URL
    if (tool_policy.find("file://") == 0)
    {
      // Extract path from file:// URL
      std::string file_path = tool_policy.substr(7); // Remove "file://" prefix

      try
      {
        // Check if file exists
        if (!fs::exists(file_path))
        {
          throw std::runtime_error("Tool policy file not found: " + file_path);
        }

        // Read and parse the file
        std::ifstream policy_file(file_path);
        if (!policy_file)
        {
          throw std::runtime_error("Failed to open tool policy file: " + file_path);
        }

        json policy_config = json::parse(policy_file);

        // Validate that it has at least a default field
        if (!policy_config.contains("default"))
        {
          throw std::runtime_error("Tool policy file must contain a 'default' field: " + file_path);
        }

        return policy_config;
      }
      catch (const std::exception& e)
      {
        std::cerr << "error resolving tool policy file: " << e.what() << std::endl;
        throw;
      }
    }

    // Check if tool_policy is a JSON object string
    if (!tool_policy.empty() && (tool_policy.front() == '{' || tool_policy.front() == '['))
    {
      try
      {
        json policy_config = json::parse(tool_policy);

        // If it's a simple string value wrapped in quotes, unwrap it
        if (policy_config.is_string())
        {
          std::string simple_policy = policy_config.get<std::string>();
          json result;
          result["default"] = simple_policy;
          return result;
        }

        // Validate that it has at least a default field
        if (!policy_config.contains("default"))
        {
          throw std::runtime_error("Tool policy JSON must contain a 'default' field");
        }

        return policy_config;
      }
      catch (const std::exception& e)
      {
        std::cerr << "error parsing tool policy JSON: " << e.what() << std::endl;
        throw;
      }
    }

    // Simple string policy - convert to policy engine format
    json policy_config;
    policy_config["default"] = tool_policy;
    return policy_config;
  }

  // ---- Configuration Loading ----

  full_config agent_system::load_config_from_file(const std::string &config_file) const
  {
    full_config config;

    // Check if file exists
    if (!fs::exists(config_file))
    {
      throw std::runtime_error("Configuration file not found: " + config_file);
    }

    // Read and parse JSON
    std::ifstream in(config_file);
    if (!in)
    {
      throw std::runtime_error("Failed to open configuration file: " + config_file);
    }

    json j;
    try
    {
      j = json::parse(in);
    }
    catch (const std::exception &e)
    {
      throw std::runtime_error(std::string("Failed to parse configuration file: ") + e.what());
    }

    // Parse models
    if (j.contains("models") && j["models"].is_object())
    {
      for (auto &[name, model_json] : j["models"].items())
      {
        model_config model_cfg;
        model_cfg.provider = model_json.value("provider", "mistral");
        model_cfg.model_id = model_json.value("model_id", name);
        model_cfg.api_key_env = model_json.value("api_key_env", "MISTRAL_API_KEY");
        model_cfg.base_url = model_json.value("base_url", "https://api.mistral.ai");
        model_cfg.api_version = model_json.value("api_version", "v1");

        // Parse sampling parameters
        if (model_json.contains("reasoning_effort") && !model_json["reasoning_effort"].is_null()) {
          model_cfg.reasoning_effort = model_json["reasoning_effort"].get<std::string>();
        }
        if (model_json.contains("temperature") && !model_json["temperature"].is_null()) {
          model_cfg.temperature = model_json["temperature"].get<float>();
        }
        if (model_json.contains("top_p") && !model_json["top_p"].is_null()) {
          model_cfg.top_p = model_json["top_p"].get<float>();
        }
        if (model_json.contains("top_k") && !model_json["top_k"].is_null()) {
          model_cfg.top_k = model_json["top_k"].get<int>();
        }
        if (model_json.contains("max_tokens") && !model_json["max_tokens"].is_null()) {
          model_cfg.max_tokens = model_json["max_tokens"].get<int>();
        }
        if (model_json.contains("frequency_penalty") && !model_json["frequency_penalty"].is_null()) {
          model_cfg.frequency_penalty = model_json["frequency_penalty"].get<float>();
        }
        if (model_json.contains("presence_penalty") && !model_json["presence_penalty"].is_null()) {
          model_cfg.presence_penalty = model_json["presence_penalty"].get<float>();
        }
        if (model_json.contains("seed") && !model_json["seed"].is_null()) {
          model_cfg.seed = model_json["seed"].get<int>();
        }
        if (model_json.contains("safe_prompt") && !model_json["safe_prompt"].is_null()) {
          model_cfg.safe_prompt = model_json["safe_prompt"].get<bool>();
        }
        if (model_json.contains("randomness") && !model_json["randomness"].is_null()) {
          model_cfg.randomness = model_json["randomness"].get<float>();
        }

        config.models[name] = std::move(model_cfg);
      }
    }

    // Parse agents
    if (j.contains("agents") && j["agents"].is_object())
    {
      for (auto &[name, agent_json] : j["agents"].items())
      {
        agent_config agent_cfg;
        agent_cfg.model = agent_json.value("model", "mistral-medium");
        agent_cfg.system_prompt = agent_json.value("system_prompt", "");

        // Handle tool_policy - can be string or JSON object
        if (agent_json.contains("tool_policy"))
        {
          if (agent_json["tool_policy"].is_string())
          {
            agent_cfg.tool_policy = agent_json["tool_policy"].get<std::string>();
          }
          else if (agent_json["tool_policy"].is_object())
          {
            // Convert JSON object to string
            agent_cfg.tool_policy = agent_json["tool_policy"].dump();
          }
          else
          {
            agent_cfg.tool_policy = "deny";
          }
        }
        else
        {
          agent_cfg.tool_policy = "deny";
        }

        // Handle available_tools
        if (agent_json.contains("available_tools") && agent_json["available_tools"].is_array())
        {
          for (auto &tool : agent_json["available_tools"])
          {
            agent_cfg.available_tools.push_back(tool.get<std::string>());
          }
        }

        // Parse sampling parameters (agent-level overrides)
        if (agent_json.contains("reasoning_effort") && !agent_json["reasoning_effort"].is_null()) {
          agent_cfg.reasoning_effort = agent_json["reasoning_effort"].get<std::string>();
        }
        if (agent_json.contains("temperature") && !agent_json["temperature"].is_null()) {
          agent_cfg.temperature = agent_json["temperature"].get<float>();
        }
        if (agent_json.contains("top_p") && !agent_json["top_p"].is_null()) {
          agent_cfg.top_p = agent_json["top_p"].get<float>();
        }
        if (agent_json.contains("top_k") && !agent_json["top_k"].is_null()) {
          agent_cfg.top_k = agent_json["top_k"].get<int>();
        }
        if (agent_json.contains("max_tokens") && !agent_json["max_tokens"].is_null()) {
          agent_cfg.max_tokens = agent_json["max_tokens"].get<int>();
        }
        if (agent_json.contains("frequency_penalty") && !agent_json["frequency_penalty"].is_null()) {
          agent_cfg.frequency_penalty = agent_json["frequency_penalty"].get<float>();
        }
        if (agent_json.contains("presence_penalty") && !agent_json["presence_penalty"].is_null()) {
          agent_cfg.presence_penalty = agent_json["presence_penalty"].get<float>();
        }
        if (agent_json.contains("seed") && !agent_json["seed"].is_null()) {
          agent_cfg.seed = agent_json["seed"].get<int>();
        }
        if (agent_json.contains("safe_prompt") && !agent_json["safe_prompt"].is_null()) {
          agent_cfg.safe_prompt = agent_json["safe_prompt"].get<bool>();
        }
        if (agent_json.contains("randomness") && !agent_json["randomness"].is_null()) {
          agent_cfg.randomness = agent_json["randomness"].get<float>();
        }

        config.agents[name] = std::move(agent_cfg);
      }
    }

    // Parse policy
    if (j.contains("policy") && j["policy"].is_object())
    {
      config.default_policy = j["policy"].value("default", "deny");
    }

    return config;
  }

  // ---- Tool Initialization ----

  void agent_system::initialize_tools(const std::string &notes_dir)
  {
    file_tools_ = std::make_unique<mistral::tools::file_tools>();
    note_tools_ = std::make_unique<mistral::tools::note_tools>(notes_dir);
    grep_tools_ = std::make_unique<mistral::tools::grep_tools>();
    exec_tools_ = std::make_unique<mistral::tools::exec_tools>();
  }

  void agent_system::register_slash_commands()
  {
    namespace console = mistral::console;

    // Register built-in slash commands
    class help_command : public console::slash_command
    {
    public:
      std::string name() const override { return "help"; }
      std::string description() const override { return "Show available slash commands"; }
      std::string usage() const override { return "/help [command]"; }
      execution_context context() const override { return execution_context::IMMEDIATE; }

      std::string execute_immediate(const std::string& args) override
      {
        auto& registry = console::slash_command_registry::instance();
        auto names = registry.get_command_names();

        if (args.empty()) {
          std::string result = "Available slash commands:\n";
          for (const auto& name : names) {
            const auto* cmd = registry.get_command(name);
            if (cmd) {
              result += "  /" + name;
              if (!cmd->description().empty()) {
                result += " - " + cmd->description();
              }
              result += "\n";
            }
          }
          return result;
        } else {
          const auto* cmd = registry.get_command(args);
          if (!cmd) {
            return "Unknown command: /" + args;
          }
          return "Usage: /" + cmd->usage() + "\n" + cmd->description();
        }
      }
    };

    class clear_command : public console::slash_command
    {
    public:
      std::string name() const override { return "clear"; }
      std::string description() const override { return "Clear the chat history"; }
      execution_context context() const override { return execution_context::IMMEDIATE; }

      std::string execute_immediate(const std::string&) override
      {
        return "Chat history cleared (not yet implemented)";
      }
    };

    class model_command : public console::slash_command
    {
    public:
      std::string name() const override { return "model"; }
      std::string description() const override { return "Show current model information"; }
      execution_context context() const override { return execution_context::AGENT; }

      dripcore::awaitable<std::string> execute_agent(
        dripcore::task&,
        const std::string&,
        agent_system& sys) override
      {
        co_return "Current model: " + sys.resolve_model_id(sys.get_agent_config("main").model);
      }
    };

    class tools_command : public console::slash_command
    {
    public:
      std::string name() const override { return "tools"; }
      std::string description() const override { return "List available tools"; }
      execution_context context() const override { return execution_context::AGENT; }

      dripcore::awaitable<std::string> execute_agent(
        dripcore::task&,
        const std::string&,
        agent_system& sys) override
      {
        auto schema = sys.global_tools().get_schema();
        std::string result = "Available tools:\n";
        for (const auto& tool : schema) {
          if (tool.contains("function") && tool["function"].contains("name")) {
            result += "  - " + tool["function"]["name"].get<std::string>() + "\n";
          }
        }
        co_return result;
      }
    };

    class exit_command : public console::slash_command
    {
    public:
      std::string name() const override { return "exit"; }
      std::string description() const override { return "Exit the application"; }
      execution_context context() const override { return execution_context::IMMEDIATE; }

      std::string execute_immediate(const std::string&) override
      {
        return ""; // NOT IMPLEMENTED!
      }
    };

    auto& registry = console::slash_command_registry::instance();
    registry.register_command(std::make_unique<help_command>());
    registry.register_command(std::make_unique<clear_command>());
    registry.register_command(std::make_unique<model_command>());
    registry.register_command(std::make_unique<tools_command>());
    registry.register_command(std::make_unique<exit_command>());
    registry.register_command(std::make_unique<load_command>());
    registry.register_command(std::make_unique<save_command>());
    registry.register_command(std::make_unique<usage_command>());
    registry.register_command(std::make_unique<patchset_verify_command>());
    registry.register_command(std::make_unique<tool_command>());
  }

  void agent_system::build_global_tool_registry()
  {
    // Register all available tools
    auto &file_tools = *file_tools_;
    auto &note_tools = *note_tools_;
    auto &grep_tools = *grep_tools_;
    auto &exec_tools = *exec_tools_;

    global_tools_.register_tool(
        "ls",
        "List files and directories. Returns entries with type indicator (f=file, d=directory, l=symlink), "
        "size for files, and relative path.",
        &file_tools, &mistral::tools::file_tools::list_files,
        arg("path", "Directory path to list"),
        arg("recursive", "Whether to list subdirectories recursively", false),
        arg("max_depth", "Maximum recursion depth (0 for unlimited)", 1));

    global_tools_.register_tool(
        "read",
        "Read file contents. Use line_from and line_to for specific ranges. "
        "Set both to 0 to read entire file. Lines are 1-indexed.",
        &file_tools, &mistral::tools::file_tools::read_file,
        arg("path", "File path to read"),
        arg("line_from", "First line to read (1-indexed, 0 = from beginning)", 0),
        arg("line_to", "Last line to read (inclusive, 0 = until end)", 0));

    global_tools_.register_tool(
        "grep",
        "Search for a regular expression pattern in files. Returns matching lines with filename and line number.",
        &grep_tools, &mistral::tools::grep_tools::grep,
        arg("pattern", "Regular expression pattern to search for"),
        arg("path", "Path to search (file or directory)", std::string(".")),
        arg("recursive", "Search recursively in directories", false));

    global_tools_.register_tool(
        "note_list",
        "List notes in the notes directory. Shows path, title, and tags for each note. "
        "Pass an empty string to list all notes, or a subfolder name to filter.",
        &note_tools, &mistral::tools::note_tools::list_notes,
        arg("folder", "Subfolder to list (empty for all)", std::string("")));

    global_tools_.register_tool(
        "note_read",
        "Read a note from the notes directory. Resolves {{> path}} include directives. "
        "Returns title, tags, and rendered content. The .md extension is optional.",
        &note_tools, &mistral::tools::note_tools::read_note,
        arg("path", "Note path relative to notes directory"));

    global_tools_.register_tool(
        "note_write",
        "Create or update a note. Use YAML frontmatter for title and tags:\n"
        "---\ntitle: My Note\ntags: [tag1, tag2]\n---\n"
        "Use {{> path}} to include other notes (partials). Put reusable snippets in _partials/.",
        &note_tools, &mistral::tools::note_tools::write_note,
        arg("path", "Note path relative to notes directory"),
        arg("content", "Note content (with optional frontmatter)"));

    global_tools_.register_tool(
        "note_search",
        "Search notes by text content and/or tag. At least one must be provided. "
        "Returns matching notes with path, title, tags, and matching line context.",
        &note_tools, &mistral::tools::note_tools::search_notes,
        arg("query", "Text to search for (case-insensitive)", std::string("")),
        arg("tag", "Filter by tag (case-insensitive)", std::string("")));

    global_tools_.register_tool(
        "exec",
        "Execute an external command. Uses execvp() directly - no shell involved. "
        "Returns exit code, stdout, stderr, and command info.",
        &exec_tools, &mistral::tools::exec_tools::exec,
        arg("command", "Executable name (e.g., 'git', 'cmake')"),
        arg("args", "Command arguments as array of strings"),
        arg("cwd", "Working directory (optional, defaults to current)", std::string("")),
        arg("timeout_seconds", "Timeout in seconds", 30));
  }

  // ---- Configuration Access ----

  const agent_config &agent_system::get_agent_config(const std::string &agent_id) const
  {
    auto it = config_.agents.find(agent_id);
    if (it == config_.agents.end())
    {
      throw std::runtime_error("Agent configuration not found: " + agent_id);
    }
    return it->second;
  }

  bool agent_system::has_agent_config(const std::string &agent_id) const
  {
    return config_.agents.find(agent_id) != config_.agents.end();
  }

  const model_config &agent_system::get_model_config(const std::string &model_name) const
  {
    auto it = config_.models.find(model_name);
    if (it == config_.models.end())
    {
      throw std::runtime_error("Model configuration not found: " + model_name);
    }
    return it->second;
  }

  bool agent_system::has_model_config(const std::string &model_name) const
  {
    return config_.models.find(model_name) != config_.models.end();
  }

  api::sampling_params agent_system::resolve_sampling_params(const agent_config& agent_cfg) const
  {
    api::sampling_params params;

    // Get model defaults if available
    if (has_model_config(agent_cfg.model))
    {
      const auto& model_cfg = get_model_config(agent_cfg.model);

      // Apply model defaults
      params.reasoning_effort = model_cfg.reasoning_effort.value_or("high");
      params.temperature = model_cfg.temperature.value_or(0.6f);
      if (model_cfg.top_p) params.top_p = model_cfg.top_p;
      if (model_cfg.top_k) params.top_k = model_cfg.top_k;
      if (model_cfg.max_tokens) params.max_tokens = model_cfg.max_tokens;
      if (model_cfg.frequency_penalty) params.frequency_penalty = model_cfg.frequency_penalty;
      if (model_cfg.presence_penalty) params.presence_penalty = model_cfg.presence_penalty;
      if (model_cfg.seed) params.seed = model_cfg.seed;
      if (model_cfg.safe_prompt) params.safe_prompt = model_cfg.safe_prompt;
      if (model_cfg.randomness) params.randomness = model_cfg.randomness;
    }
    else
    {
      // Default values if no model config
      params.reasoning_effort = "high";
      params.temperature = 0.6f;
    }

    // Apply agent overrides (if specified)
    if (agent_cfg.reasoning_effort) params.reasoning_effort = *agent_cfg.reasoning_effort;
    if (agent_cfg.temperature) params.temperature = *agent_cfg.temperature;
    if (agent_cfg.top_p) params.top_p = *agent_cfg.top_p;
    if (agent_cfg.top_k) params.top_k = *agent_cfg.top_k;
    if (agent_cfg.max_tokens) params.max_tokens = *agent_cfg.max_tokens;
    if (agent_cfg.frequency_penalty) params.frequency_penalty = *agent_cfg.frequency_penalty;
    if (agent_cfg.presence_penalty) params.presence_penalty = *agent_cfg.presence_penalty;
    if (agent_cfg.seed) params.seed = *agent_cfg.seed;
    if (agent_cfg.safe_prompt) params.safe_prompt = *agent_cfg.safe_prompt;
    if (agent_cfg.randomness) params.randomness = *agent_cfg.randomness;

    return params;
  }

  std::string agent_system::resolve_model_id(const std::string &model_name) const
  {
    auto it = config_.models.find(model_name);
    if (it != config_.models.end())
    {
      return it->second.model_id;
    }
    return model_name; // Return as-is if not found (backward compatibility)
  }

  // ---- Tool Registry ----

  tool_registry agent_system::create_tool_registry(const std::vector<std::string> &available_tools) const
  {
    // Handle wildcard - return copy of global registry with all tools
    if (std::find(available_tools.begin(), available_tools.end(), "*") != available_tools.end())
    {
      return global_tools_;
    }

    // Empty available_tools means no tools allowed - return empty registry
    if (available_tools.empty())
    {
      return tool_registry{};
    }

    // Filter to only include available tools that exist in global registry
    tool_registry filtered;
    for (const auto& tool_name : available_tools)
    {
      if (auto tool = global_tools_.get_tool(tool_name))
      {
        filtered.add_tool(tool_name, tool->clone());
      }
    }
    return filtered;
  }

  // ---- Channel Access ----

  std::shared_ptr<dripcore::channel<api::api_request>> agent_system::request_ch() const
  {
    return request_ch_;
  }

  std::shared_ptr<dripcore::channel<api::api_event>> agent_system::response_ch() const
  {
    return response_ch_;
  }

  std::string agent_system::get_last_response() const
  {
    std::lock_guard<std::mutex> lock(last_response_mtx_);
    return last_response_;
  }

  void agent_system::set_last_response(const std::string &response)
  {
    std::lock_guard<std::mutex> lock(last_response_mtx_);
    last_response_ = response;
  }

  // ---- Statistics ----

  std::shared_ptr<dripcore::channel<stat_message>> agent_system::stats_ch() const
  {
    return stats_ch_;
  }

  json agent_system::get_usage_stats() const
  {
    std::lock_guard<std::mutex> lock(usage_stats_mtx_);
    return usage_stats_;
  }

  void agent_system::update_usage_stats(const json& stats)
  {
    std::lock_guard<std::mutex> lock(usage_stats_mtx_);
    usage_stats_ = stats;
  }

  void agent_system::reset_usage_stats()
  {
    std::lock_guard<std::mutex> lock(usage_stats_mtx_);
    usage_stats_ = {};
  }

  std::string agent_system::resolve_system_prompt(
    const agent_config& agent_cfg,
    const std::string& model_id
  ) const
  {
    std::string system_prompt = agent_cfg.system_prompt;

    // Check if system_prompt is a file:// URL
    if (system_prompt.find("file://") == 0) {
      // Extract path from file:// URL
      std::string file_path = system_prompt.substr(7); // Remove "file://" prefix

      try {
        // Check if file exists
        if (!fs::exists(file_path)) {
          throw std::runtime_error("System prompt file not found: " + file_path);
        }

        // Use template engine to process the file with variables
        mistral::tmpl::template_engine tmpl;
        tmpl.set_include_base(base_dir_);
        tmpl.set_variable("model", model_id);
        tmpl.set_variable("notes_dir", notes_dir_);

        // Process the file as a template
        system_prompt = tmpl.process_file(file_path);
      } catch (const std::exception& e) {
        std::cerr << "error resolving system prompt file: " << e.what() << std::endl;
        throw;
      }
    }

    return system_prompt;
  }

  dripcore::awaitable<std::string> agent_system::run_subagent(
    dripcore::task& task,
    const std::string& agent_id,
    const std::string& message
  )
  {
    if (!has_agent_config(agent_id)) {
      co_return "Error: Agent '" + agent_id + "' not found in configuration";
    }

    auto input_ch = std::make_shared<dripcore::channel<std::string>>(4);
    auto output_ch = std::make_shared<dripcore::channel<std::string>>(4);

    task.spawn<subagent_task>(agent_id, input_ch, output_ch);

    input_ch->try_send(message);
    input_ch->close();

    auto response = co_await output_ch->recv();
    if (!response) {
      co_return "Error: No response from subagent " + agent_id;
    }

    co_return *response;
  }
} // namespace mistral::agent
