#include "agent/agent_system.h"
#include "agent/agent_config.h"
#include "tools/file_tools.h"
#include "tools/note_tools.h"
#include "tools/grep_tools.h"

#include <api/mistral_api.h>

#include <fstream>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace mistral::agent
{

  namespace fs = std::filesystem;
  using json = nlohmann::ordered_json;

  namespace
  {
    // Helper to convert tool_policy string to policy_decision
    policy_decision string_to_policy_decision(const std::string &policy_str)
    {
      if (policy_str == "allow")
      {
        return policy_decision::ALLOW;
      }
      else if (policy_str == "require_approval")
      {
        return policy_decision::REQUIRE_APPROVAL;
      }
      return policy_decision::DENY;
    }
  }

  // ---- Singleton Instance ----

  agent_system &agent_system::instance()
  {
    static agent_system instance;
    return instance;
  }

  // ---- Initialization ----

  void agent_system::initialize(
      const std::string &config_file,
      const std::string &notes_dir,
      std::shared_ptr<dripcore::channel<api::api_request>> request_ch,
      std::shared_ptr<dripcore::channel<api::api_event>> response_ch)
  {
    notes_dir_ = notes_dir;
    request_ch_ = std::move(request_ch);
    response_ch_ = std::move(response_ch);

    // Load configuration
    config_ = load_config_from_file(config_file);

    // Initialize tool instances
    initialize_tools(notes_dir_);

    // Build global tool registry
    build_global_tool_registry();

    // Set up default policy engine
    default_policy_engine_.set_default_policy(string_to_policy_decision(config_.default_policy));
  }

  void agent_system::initialize_from_arguments(
      const std::string &model,
      const std::string &system_prompt,
      const std::string &tool_policy_file,
      const std::string &tool_default_policy,
      const std::string &notes_dir,
      std::shared_ptr<dripcore::channel<api::api_request>> request_ch,
      std::shared_ptr<dripcore::channel<api::api_event>> response_ch)
  {
    notes_dir_ = notes_dir;
    request_ch_ = std::move(request_ch);
    response_ch_ = std::move(response_ch);

    // Create default configuration
    config_ = create_default_config(model, system_prompt, tool_policy_file, tool_default_policy);

    // Initialize tool instances
    initialize_tools(notes_dir_);

    // Build global tool registry
    build_global_tool_registry();

    // Set up default policy engine
    default_policy_engine_.set_default_policy(string_to_policy_decision(config_.default_policy));
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
        agent_cfg.tool_policy = agent_json.value("tool_policy", "deny");

        if (agent_json.contains("allowed_tools") && agent_json["allowed_tools"].is_array())
        {
          for (auto &tool : agent_json["allowed_tools"])
          {
            agent_cfg.allowed_tools.push_back(tool.get<std::string>());
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

  full_config agent_system::create_default_config(
      const std::string &model,
      const std::string &system_prompt,
      const std::string &tool_policy_file,
      const std::string &tool_default_policy) const
  {
    full_config config;

    // Create default model
    model_config model_cfg;
    model_cfg.provider = "mistral";
    model_cfg.model_id = model;
    model_cfg.api_key_env = "MISTRAL_API_KEY";
    model_cfg.base_url = "https://api.mistral.ai";
    model_cfg.api_version = "v1";
    config.models["default"] = std::move(model_cfg);

    // Create default agent
    agent_config agent_cfg;
    agent_cfg.model = "default";
    agent_cfg.system_prompt = system_prompt;
    agent_cfg.tool_policy = tool_default_policy;
    // All tools allowed by default in backward compatibility mode
    config.agents["main"] = std::move(agent_cfg);

    // Set default policy
    config.default_policy = tool_default_policy;

    // If tool policy file is specified, load it
    if (!tool_policy_file.empty())
    {
      try
      {
        std::ifstream policy_file(tool_policy_file);
        if (policy_file)
        {
          auto policy_config = json::parse(policy_file);
          // Extract default policy if present
          if (policy_config.contains("default"))
          {
            config.default_policy = policy_config["default"].get<std::string>();
          }
        }
      }
      catch (const std::exception &e)
      {
        std::cerr << "warning: could not load tool policy file: " << e.what() << std::endl;
      }
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

  tool_registry agent_system::create_tool_registry(const std::vector<std::string> &allowed_tools) const
  {
    // Handle wildcard - return copy of global registry with all tools
    if (std::find(allowed_tools.begin(), allowed_tools.end(), "*") != allowed_tools.end())
    {
      return global_tools_;
    }

    // Empty allowed_tools means no tools allowed - return empty registry
    if (allowed_tools.empty())
    {
      return tool_registry{};
    }

    // Filter to only include allowed tools that exist in global registry
    tool_registry filtered;
    for (const auto& tool_name : allowed_tools)
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

} // namespace mistral::agent