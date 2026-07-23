#include "agent/agent_system.h"
#include "agent/agent_config.h"
#include "tools/file_tools.h"
#include "tools/note_tools.h"
#include "tools/grep_tools.h"
#include "template/template_engine.h"

#include <api/mistral_api.h>

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
      std::shared_ptr<dripcore::channel<api::api_request>> request_ch,
      std::shared_ptr<dripcore::channel<api::api_event>> response_ch)
  {
    base_dir_ = base_dir;
    notes_dir_ = notes_dir;
    request_ch_ = std::move(request_ch);
    response_ch_ = std::move(response_ch);

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

  void agent_system::initialize_from_arguments(
      const std::string &model,
      const std::string& base_dir,
      const std::string &notes_dir,
      std::shared_ptr<dripcore::channel<api::api_request>> request_ch,
      std::shared_ptr<dripcore::channel<api::api_event>> response_ch)
  {
    base_dir_ = base_dir;
    notes_dir_ = notes_dir;
    request_ch_ = std::move(request_ch);
    response_ch_ = std::move(response_ch);

    // Initialize tool instances
    initialize_tools(notes_dir_);

    // Build global tool registry
    build_global_tool_registry();

    // Register slash commands
    register_slash_commands();
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

      std::string execute_agent(const std::string&, agent_system& sys) override
      {
        return "Current model: " + sys.resolve_model_id(sys.get_agent_config("main").model);
      }
    };

    class tools_command : public console::slash_command
    {
    public:
      std::string name() const override { return "tools"; }
      std::string description() const override { return "List available tools"; }
      execution_context context() const override { return execution_context::AGENT; }

      std::string execute_agent(const std::string&, agent_system& sys) override
      {
        auto schema = sys.global_tools().get_schema();
        std::string result = "Available tools:\n";
        for (const auto& tool : schema) {
          if (tool.contains("function") && tool["function"].contains("name")) {
            result += "  - " + tool["function"]["name"].get<std::string>() + "\n";
          }
        }
        return result;
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
        return "Exit (not yet implemented)";
      }
    };

    auto& registry = console::slash_command_registry::instance();
    registry.register_command(std::make_unique<help_command>());
    registry.register_command(std::make_unique<clear_command>());
    registry.register_command(std::make_unique<model_command>());
    registry.register_command(std::make_unique<tools_command>());
    registry.register_command(std::make_unique<exit_command>());
  }

  void agent_system::build_global_tool_registry()
  {
    // Register all available tools
    auto &file_tools = *file_tools_;
    auto &note_tools = *note_tools_;
    auto &grep_tools = *grep_tools_;

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
} // namespace mistral::agent
