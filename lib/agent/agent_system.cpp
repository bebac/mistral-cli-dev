#include "agent/agent_system.h"
#include "agent/agent_config.h"
#include "agent/compact_command.h"
#include "agent/subagent_task.h"
#include "tools/patchset_tools.h"
#include "tools/file_tools.h"
#include "tools/note_tools.h"
#include "tools/grep_tools.h"
#include "tools/code_tools.h"
#include "tools/exec_tools.h"
#include "tools/task_tools.h"
#include "template/template_engine.h"

#include <api/chat_types.h>

#include <dripcore/task.h>
#include <dripcore/awaitable.h>

#include <algorithm>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <unistd.h>

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
    slash_command_registry_ = std::make_shared<slash_command::slash_command_registry>();
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

        json policy_config = json::parse(policy_file, nullptr, true, true);

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
        json policy_config = json::parse(tool_policy, nullptr, true, true);

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
      j = json::parse(in, nullptr, true, true);
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
        if (model_json.contains("strict_mode") && !model_json["strict_mode"].is_null()) {
          model_cfg.strict_mode = model_json["strict_mode"].get<bool>();
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
        if (agent_json.contains("strict_mode") && !agent_json["strict_mode"].is_null()) {
          agent_cfg.strict_mode = agent_json["strict_mode"].get<bool>();
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
    code_tools_ = std::make_unique<mistral::tools::code_tools>();
    exec_tools_ = std::make_unique<mistral::tools::exec_tools>();
    task_tools_ = std::make_unique<mistral::tools::task_tools>(notes_dir);
    patchset_tools_ = std::make_unique<mistral::agent::patchset_tools>(*this);
  }

  void agent_system::register_slash_commands()
  {
    namespace sc = mistral::slash_command;

    // Register built-in slash commands
    class help_command : public sc::slash_command
    {
    public:
      explicit help_command(sc::slash_command_registry& registry)
        : registry_(registry) {}

      std::string name() const override { return "help"; }
      std::string description() const override { return "Show available slash commands"; }
      std::string usage() const override { return "/help [command]"; }

      dripcore::awaitable<std::string> execute(
        dripcore::task&,
        const std::string& args,
        agent::agent_system&) override
      {
        auto names = registry_.get_command_names();

        if (args.empty()) {
          std::string result = "Available slash commands:\n";
          for (const auto& name : names) {
            const auto* cmd = registry_.get_command(name);
            if (cmd) {
              result += "  /" + name;
              if (!cmd->description().empty()) {
                result += " - " + cmd->description();
              }
              result += "\n";
            }
          }
          co_return result;
        } else {
          const auto* cmd = registry_.get_command(args);
          if (!cmd) {
            co_return "Unknown command: /" + args;
          }
          co_return "Usage: /" + cmd->usage() + "\n" + cmd->description();
        }
      }

    private:
      sc::slash_command_registry& registry_;
    };

    class clear_command : public sc::slash_command
    {
    public:
      std::string name() const override { return "clear"; }
      std::string description() const override { return "Clear the chat history"; }

      dripcore::awaitable<std::string> execute(
        dripcore::task&,
        const std::string&,
        agent::agent_system&) override
      {
        co_return "Chat history cleared (not yet implemented)";
      }
    };

    class model_command : public sc::slash_command
    {
    public:
      std::string name() const override { return "model"; }
      std::string description() const override { return "Show current model information"; }

      dripcore::awaitable<std::string> execute(
        dripcore::task&,
        const std::string&,
        agent_system& sys) override
      {
        co_return "Current model: " + sys.resolve_model_id(sys.get_agent_config("main").model);
      }
    };

    class tools_command : public sc::slash_command
    {
    public:
      std::string name() const override { return "tools"; }
      std::string description() const override { return "List available tools"; }

      dripcore::awaitable<std::string> execute(
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

    class exit_command : public sc::slash_command
    {
    public:
      std::string name() const override { return "exit"; }
      std::string description() const override { return "Exit the application"; }

      dripcore::awaitable<std::string> execute(
        dripcore::task&,
        const std::string&,
        agent::agent_system&) override
      {
        co_return ""; // NOT IMPLEMENTED!
      }
    };

    auto& registry = *slash_command_registry_;
    registry.register_command(std::make_unique<help_command>(registry));
    registry.register_command(std::make_unique<clear_command>());
    registry.register_command(std::make_unique<model_command>());
    registry.register_command(std::make_unique<tools_command>());
    registry.register_command(std::make_unique<exit_command>());
    registry.register_command(std::make_unique<load_command>());
    registry.register_command(std::make_unique<save_command>());
    registry.register_command(std::make_unique<usage_command>());
    registry.register_command(std::make_unique<patchset_verify_command>());
    registry.register_command(std::make_unique<tool_command>());
    registry.register_command(std::make_unique<compact_command>());
  }

  void agent_system::build_global_tool_registry()
  {
    // Register all available tools
    auto& file_tools = *file_tools_;
    auto& note_tools = *note_tools_;
    auto& grep_tools = *grep_tools_;
    auto& code_tools = *code_tools_;
    auto& exec_tools = *exec_tools_;
    auto& task_tools = *task_tools_;
    auto& patchset_tools = *patchset_tools_;

    global_tools_.register_tool(
      "ls",
      "List files and directories. Returns entries with type indicator (f=file, d=directory, l=symlink), "
      "size for files, and relative path.",
      &file_tools, &mistral::tools::file_tools::list_files,
      arg("path", "Directory path to list"),
      arg("recursive", "Whether to list subdirectories recursively", false),
      arg("max_depth", "Maximum recursion depth (0 for unlimited)", 1)
    );

    global_tools_.register_tool(
      "read",
      "Read file contents. Use line_from and line_to for specific ranges. "
      "Set both to 0 to read entire file. Lines are 1-indexed.",
      &file_tools, &mistral::tools::file_tools::read_file,
      arg("path", "File path to read"),
      arg("line_from", "First line to read (1-indexed, 0 = from beginning)", 0),
      arg("line_to", "Last line to read (inclusive, 0 = until end)", 0)
    );

    global_tools_.register_tool(
      "grep",
      "Search for a regular expression pattern in files. Returns matching lines with filename and line number.",
      &grep_tools, &mistral::tools::grep_tools::grep,
      arg("pattern", "Regular expression pattern to search for"),
      arg("path", "Path to search (file or directory)", std::string(".")),
      arg("recursive", "Search recursively in directories", false)
    );

    global_tools_.register_tool(
      "outline",
      "List the symbols (classes, structs, functions, methods, types) defined in a C/C++ "
      "source file with their line ranges. Use this to see a file's structure before "
      "reading specific symbols.",
      &code_tools, &mistral::tools::code_tools::outline,
      arg("path", "C/C++ source file to outline")
    );

    global_tools_.register_tool(
      "read_symbol",
      "Extract the exact source text of a named symbol definition (class, function, "
      "method, type) from a C/C++ source file. Returns every matching definition, "
      "e.g. overloads.",
      &code_tools, &mistral::tools::code_tools::read_symbol,
      arg("path", "C/C++ source file containing the symbol"),
      arg("name", "Name of the symbol to extract")
    );

    global_tools_.register_tool(
      "note_list",
      "List notes in the notes directory. Shows path, title, and tags for each note. "
      "Pass an empty string to list all notes, or a subfolder name to filter.",
      &note_tools, &mistral::tools::note_tools::list_notes,
      arg("folder", "Subfolder to list (empty for all)", std::string(""))
    );

    global_tools_.register_tool(
      "note_read",
      "Read a note from the notes directory. Resolves {{> path}} include directives. "
      "Returns title, tags, and rendered content. The .md extension is optional.",
      &note_tools, &mistral::tools::note_tools::read_note,
      arg("path", "Note path relative to notes directory")
    );

    global_tools_.register_tool(
      "note_write",
      "Create or update a note. Use YAML frontmatter for title and tags:\n"
      "---\ntitle: My Note\ntags: [tag1, tag2]\n---\n"
      "Use {{> path}} to include other notes (partials). Put reusable snippets in _partials/.",
      &note_tools, &mistral::tools::note_tools::write_note,
      arg("path", "Note path relative to notes directory"),
      arg("content", "Note content (with optional frontmatter)")
    );

    global_tools_.register_tool(
      "note_search",
      "Search notes by text content and/or tag. At least one must be provided. "
      "Returns matching notes with path, title, tags, and matching line context.",
      &note_tools, &mistral::tools::note_tools::search_notes,
      arg("query", "Text to search for (case-insensitive)", std::string("")),
      arg("tag", "Filter by tag (case-insensitive)", std::string(""))
    );

    global_tools_.register_tool(
      "exec",
      "Execute an external command. Uses execvp() directly - no shell involved: "
      "no pipes, redirection, globbing or command substitution. The command must "
      "be a single executable name (e.g., 'git', 'cmake') and each argument a "
      "separate array element. The call is pre-checked and fails fast if the "
      "command does not exist or contains shell syntax. "
      "Returns exit code, stdout, stderr, and command info. "
      "stdout and stderr are each capped at 64 KiB; excess output is discarded "
      "and reported via the stdout_truncated/stderr_truncated flags.",
      &exec_tools, &mistral::tools::exec_tools::exec,
      arg("command", "Executable name (e.g., 'git', 'cmake')"),
      arg("args", "Command arguments as array of strings"),
      arg("cwd", "Working directory (optional, defaults to current)", std::string("")),
      arg("timeout_seconds", "Timeout in seconds", 30)
    );

    global_tools_.register_tool(
      "patchset_apply",
      "Apply a stored patchset of file edits by its ID. Automatically attempts correction via patchfixer subagent if validation fails with fixable errors.",
      &patchset_tools, &mistral::agent::patchset_tools::patchset_apply,
      arg("patchset_id", "The unique identifier of the patchset to apply")
    );

    global_tools_.register_tool(
      "patchset_get",
      "Get details of a stored patchset including all numbered edits.",
      &patchset_tools, &mistral::agent::patchset_tools::patchset_get,
      arg("patchset_id", "The unique identifier of the patchset")
    );

    global_tools_.register_tool(
      "patchset_validate",
      "Validate a stored patchset without applying (dry run). Returns detailed validation results.",
      &patchset_tools, &mistral::agent::patchset_tools::patchset_validate,
      arg("patchset_id", "The unique identifier of the patchset to validate")
    );

    global_tools_.register_tool(
      "patchset_update",
      "Update a single edit in a stored patchset. The file_path, search_text and replace_text can each be updated (empty or omitted fields keep their existing values).",
      &patchset_tools, &mistral::agent::patchset_tools::patchset_update,
      arg("patchset_id", "The unique identifier of the patchset"),
      arg("patch_number", "The 1-indexed number of the patch to update"),
      arg("file_path", "The file path for the edit (keeps existing if empty)"),
      arg("search_text", "The search text (optional, keeps existing if not provided)"),
      arg("replace_text", "The replace text (optional, keeps existing if not provided)")
    );

    global_tools_.register_tool(
      "patchset_remove",
      "Remove a single edit block from a stored patchset by its patch number. The remaining edits keep their patch numbers. Use this to drop broken or unwanted blocks without re-emitting the whole patchset.",
      &patchset_tools, &mistral::agent::patchset_tools::patchset_remove,
      arg("patchset_id", "The unique identifier of the patchset"),
      arg("patch_number", "The number of the edit to remove (as shown by patchset_get or patchset_validate)")
    );

    global_tools_.register_tool(
      "patchset_clear",
      "Clear a stored patchset.",
      &patchset_tools, &mistral::agent::patchset_tools::patchset_clear,
      arg("patchset_id", "The unique identifier of the patchset to clear")
    );

    // ---- Task tools ----

    global_tools_.register_tool(
      "task_create",
      "Create a new task for project tracking. Tasks persist across sessions. "
      "Returns the created task with a generated ID.",
      &task_tools, &mistral::tools::task_tools::create_task,
      arg("title", "Short summary of the task"),
      arg("description", "Detailed explanation (optional)", std::string("")),
      arg("priority", "Priority: low, medium, high, critical (default: medium)", std::string("medium")),
      arg("tags", "List of tags for categorisation", std::vector<std::string>{})
    );

    global_tools_.register_tool(
      "task_list",
      "List tasks with optional filters. Empty filter values mean no filter. "
      "Returns an array of task summaries (id, title, status, priority, tags).",
      &task_tools, &mistral::tools::task_tools::list_tasks,
      arg("status", "Filter by status: todo, in-progress, done, blocked, cancelled (empty for all)", std::string("")),
      arg("tag", "Filter by tag (empty for all)", std::string("")),
      arg("priority", "Filter by priority: low, medium, high, critical (empty for all)", std::string(""))
    );

    global_tools_.register_tool(
      "task_get",
      "Get full details of a task by its ID.",
      &task_tools, &mistral::tools::task_tools::get_task,
      arg("task_id", "The task ID (e.g. task-001)")
    );

    global_tools_.register_tool(
      "task_update",
      "Update a task. Pass only the fields you want to change; empty strings are ignored. "
      "Returns the updated task.",
      &task_tools, &mistral::tools::task_tools::update_task,
      arg("task_id", "The task ID to update"),
      arg("status", "New status: todo, in-progress, done, blocked, cancelled (empty = unchanged)", std::string("")),
      arg("priority", "New priority: low, medium, high, critical (empty = unchanged)", std::string("")),
      arg("title", "New title (empty = unchanged)", std::string("")),
      arg("description", "New description (empty = unchanged)", std::string(""))
    );

    global_tools_.register_tool(
      "task_search",
      "Search tasks by text query across title and description (case-insensitive). "
      "Returns matching task summaries with match location.",
      &task_tools, &mistral::tools::task_tools::search_tasks,
      arg("query", "Text to search for")
    );
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

  tool_schema_options agent_system::resolve_tool_schema_options(const agent_config& agent_cfg) const
  {
    tool_schema_options opts;

    // Model defaults
    if (has_model_config(agent_cfg.model))
    {
      const auto& model_cfg = get_model_config(agent_cfg.model);
      if (model_cfg.strict_mode) opts.strict_mode = *model_cfg.strict_mode;
    }

    // Agent overrides (if specified)
    if (agent_cfg.strict_mode) opts.strict_mode = *agent_cfg.strict_mode;

    return opts;
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

    task.spawn_local<subagent_task>(agent_id, input_ch, output_ch);

    input_ch->try_send(message);
    input_ch->close();

    auto response = co_await output_ch->recv(&task);
    if (!response) {
      co_return "Error: No response from subagent " + agent_id;
    }

    co_return *response;
  }

  // ---- Patchset Storage ----

  // Store a patchset, overwriting any previous one
  void agent_system::store_patchset(const std::string& patchset_id, const std::vector<file_edit_t>& edits)
  {
    current_patchset_ = patchset_t{patchset_id, edits};
  }

  // Append edits to an existing non-applied patchset with matching ID.
  // If no matching patchset exists, stores as a new patchset.
  void agent_system::append_edits(const std::string& patchset_id, const std::vector<file_edit_t>& edits)
  {
    if (current_patchset_ && current_patchset_->patchset_id == patchset_id && !current_patchset_->applied)
    {
      // Number after the highest existing patch number: patch numbers are
      // stable identifiers (patchset_remove keeps gaps), so size-based
      // numbering could collide with an existing number after a removal.
      size_t next_patch_number = 0;
      for (const auto& edit : current_patchset_->edits)
      {
        next_patch_number = std::max(next_patch_number, edit.patch_number);
      }
      for (const auto& edit : edits)
      {
        auto numbered_edit = edit;
        numbered_edit.patch_number = ++next_patch_number;
        current_patchset_->edits.push_back(numbered_edit);
      }
    }
    else
    {
      store_patchset(patchset_id, edits);
    }
  }

  // Retrieve a stored patchset by ID
  std::optional<patchset_t> agent_system::get_patchset(const std::string& patchset_id) const
  {
    if (current_patchset_ && current_patchset_->patchset_id == patchset_id) {
      return current_patchset_;
    }
    return std::nullopt;
  }

  // Get the ID of the currently stored patchset
  std::optional<std::string> agent_system::get_current_patchset_id() const
  {
    if (current_patchset_) {
      return current_patchset_->patchset_id;
    }
    return std::nullopt;
  }

  // Remove a stored patchset by ID
  void agent_system::remove_patchset(const std::string& patchset_id)
  {
    if (current_patchset_ && current_patchset_->patchset_id == patchset_id)
    {
      current_patchset_.reset();
    }
  }

  // Mark a patchset as applied
  void agent_system::mark_applied(const std::string& patchset_id)
  {
    if (current_patchset_ && current_patchset_->patchset_id == patchset_id)
    {
      current_patchset_->applied = true;
    }
  }

  // Check if a patchset is currently stored
  bool agent_system::has_patchset() const
  {
    return current_patchset_.has_value();
  }

  // ---- Request Cancellation Support ----

  void agent_system::set_current_response_ch(std::shared_ptr<dripcore::channel<api::api_event>> ch)
  {
    std::lock_guard<std::mutex> lock(current_response_ch_mtx_);
    current_response_ch_ = std::move(ch);
  }

  void agent_system::clear_current_response_ch()
  {
    std::lock_guard<std::mutex> lock(current_response_ch_mtx_);
    current_response_ch_.reset();
  }

  std::shared_ptr<dripcore::channel<api::api_event>> agent_system::get_current_response_ch() const
  {
    std::lock_guard<std::mutex> lock(current_response_ch_mtx_);
    return current_response_ch_;
  }

  void agent_system::set_current_input_ch(std::shared_ptr<dripcore::channel<std::string>> ch)
  {
    std::lock_guard<std::mutex> lock(current_input_ch_mtx_);
    current_input_ch_ = std::move(ch);
  }

  std::shared_ptr<dripcore::channel<std::string>> agent_system::get_current_input_ch() const
  {
    std::lock_guard<std::mutex> lock(current_input_ch_mtx_);
    return current_input_ch_;
  }

  void agent_system::set_shutdown_pipe_fd(int fd)
  {
    shutdown_pipe_write_fd_ = fd;
  }

  void agent_system::signal_shutdown()
  {
    if (shutdown_pipe_write_fd_ >= 0) {
      char buf = '0';
      ::write(shutdown_pipe_write_fd_, &buf, 1);
    }
  }
} // namespace mistral::agent
