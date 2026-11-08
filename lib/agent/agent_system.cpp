#include "agent/agent_system.h"
#include "agent/agent_config.h"
#include "agent/approval_renderers.h"
#include "agent/compact_command.h"
#include "agent/mcp_command.h"
#include "agent/model_command.h"
#include "agent/server_agent_task.h"
#include "agent/subagent_task.h"
#include "agent/workspace_bootstrap.h"
#include "common/process_manager.h"
#include "http/http_coordinator_task.h"
#include "mcp/mcp_manager.h"
#include "tools/file_tools.h"
#include "tools/note_tools.h"
#include "tools/patchset_tools.h"
#ifdef MISTRAL_ENABLE_TREE_SITTER
#include "tools/code_tools.h"
#endif
#include "template/template_engine.h"
#include "tools/ask_tools.h"
#include "tools/exec_tools.h"
#include "tools/process_tools.h"
#include "tools/research_tools.h"
#include "tools/task_tools.h"

#include <api/chat_types.h>
#include <uri.h>

#include <dripcore/awaitable.h>
#include <dripcore/scheduler.h>
#include <dripcore/task.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <unistd.h>
#include <utility>
#include <vector>

namespace mistral::agent
{

  namespace fs = std::filesystem;
  using json = nlohmann::ordered_json;

  namespace
  {
    // Derive the transport endpoint (host, port, api_key_env) from a
    // model config's base_url. An empty or unparseable base_url falls
    // back to the default Mistral endpoint.
    transport_endpoint endpoint_for_model(const model_config& model)
    {
      transport_endpoint ep;
      ep.api_key_env = model.api_key_env;
      if (!model.base_url.empty())
      {
        auto parse_result = uri::parse(model.base_url);
        if (std::holds_alternative<uri::uri_view>(parse_result))
        {
          const auto& parsed_uri = std::get<uri::uri_view>(parse_result);
          ep.host = parsed_uri.host();
          auto port_str = parsed_uri.port();
          if (!port_str.empty())
          {
            try
            {
              ep.port = std::stoi(std::string(port_str));
            }
            catch (...)
            {
              // If port parsing fails, keep the HTTPS default
            }
          }
        }
      }
      if (ep.host.empty())
      {
        ep.host = "api.mistral.ai";
        ep.port = 443;
      }
      return ep;
    }

    // Parse the sampling-related keys of a model/agent JSON block.
    // Absent or null keys stay unset (std::nullopt); unknown keys are
    // ignored.
    api::sampling_params parse_sampling_params(const json& j)
    {
      api::sampling_params p;
      if (j.contains("reasoning_effort") && !j["reasoning_effort"].is_null())
      {
        p.reasoning_effort = j["reasoning_effort"].get<std::string>();
      }
      if (j.contains("temperature") && !j["temperature"].is_null())
      {
        p.temperature = j["temperature"].get<float>();
      }
      if (j.contains("top_p") && !j["top_p"].is_null())
      {
        p.top_p = j["top_p"].get<float>();
      }
      if (j.contains("top_k") && !j["top_k"].is_null())
      {
        p.top_k = j["top_k"].get<int>();
      }
      if (j.contains("max_tokens") && !j["max_tokens"].is_null())
      {
        p.max_tokens = j["max_tokens"].get<int>();
      }
      if (j.contains("frequency_penalty") && !j["frequency_penalty"].is_null())
      {
        p.frequency_penalty = j["frequency_penalty"].get<float>();
      }
      if (j.contains("presence_penalty") && !j["presence_penalty"].is_null())
      {
        p.presence_penalty = j["presence_penalty"].get<float>();
      }
      if (j.contains("seed") && !j["seed"].is_null())
      {
        p.seed = j["seed"].get<int>();
      }
      if (j.contains("safe_prompt") && !j["safe_prompt"].is_null())
      {
        p.safe_prompt = j["safe_prompt"].get<bool>();
      }
      if (j.contains("randomness") && !j["randomness"].is_null())
      {
        p.randomness = j["randomness"].get<float>();
      }
      return p;
    }

    // Copy every set field of `overrides` onto `base`, leaving unset
    // fields untouched (agent-level sampling parameters override model
    // defaults field by field).
    void apply_sampling_overrides(api::sampling_params& base, const api::sampling_params& overrides)
    {
      if (overrides.reasoning_effort)
      {
        base.reasoning_effort = overrides.reasoning_effort;
      }
      if (overrides.temperature)
      {
        base.temperature = overrides.temperature;
      }
      if (overrides.top_p)
      {
        base.top_p = overrides.top_p;
      }
      if (overrides.top_k)
      {
        base.top_k = overrides.top_k;
      }
      if (overrides.max_tokens)
      {
        base.max_tokens = overrides.max_tokens;
      }
      if (overrides.frequency_penalty)
      {
        base.frequency_penalty = overrides.frequency_penalty;
      }
      if (overrides.presence_penalty)
      {
        base.presence_penalty = overrides.presence_penalty;
      }
      if (overrides.seed)
      {
        base.seed = overrides.seed;
      }
      if (overrides.safe_prompt)
      {
        base.safe_prompt = overrides.safe_prompt;
      }
      if (overrides.randomness)
      {
        base.randomness = overrides.randomness;
      }
    }
  } // namespace

  // ---- Singleton Instance ----

  agent_system& agent_system::instance()
  {
    static agent_system instance;
    return instance;
  }

  // ---- Initialization ----

  void agent_system::initialize(
    const std::string& config_file,
    const std::string& base_dir,
    const std::string& notes_dir
  )
  {
    base_dir_ = base_dir;
    notes_dir_ = notes_dir;

    // The server-side agent id cache is per-session: drop entries from a
    // previous initialize() call.
    {
      std::lock_guard<std::mutex> lock(server_agent_ids_mtx_);
      server_agent_ids_.clear();
    }

    // The active model selection is per-session too: a reloaded config may
    // offer different ids, so every entry starts from its default again.
    {
      std::lock_guard<std::mutex> lock(active_model_ids_mtx_);
      active_model_ids_.clear();
    }

    // Initialize stats channel
    stats_ch_ = std::make_shared<dripcore::channel<stat_message>>(32);

    // Status snapshots are coalesced: a slow frontend keeps the newest
    // few and drops the rest.
    ui_status_ch_ = std::make_shared<dripcore::channel<ui_status>>(8);

    // Shutdown broadcast. Bridges register with this instead of relying on
    // their producer to close their data channel.
    shutdown_signal_ = std::make_shared<common::shutdown_signal>();

    // Load configuration
    config_ = load_config_from_file(config_file);

    // Build one request channel per distinct transport endpoint
    endpoints_.clear();
    for (const auto& ep : resolve_transport_endpoints())
    {
      endpoints_.push_back({ep, std::make_shared<dripcore::channel<api::api_request>>(4)});
    }

    // Initialize tool instances
    initialize_tools(notes_dir_);

    // Kill background process sessions on shutdown. fire_shutdown()
    // runs on the scheduler thread (from the interactive agent task's
    // teardown), so kill_all() shares that thread with the session pump
    // tasks - no locking needed.
    shutdown_signal_->add([this] {
      if (process_manager_)
      {
        process_manager_->kill_all();
      }
    });

    // Build global tool registry
    build_global_tool_registry();

    // Configure MCP servers from the loaded configuration
    mcp::mcp_manager::instance().configure(config_.mcp_servers);

    // Register slash commands
    slash_command_registry_ = std::make_shared<slash_command::slash_command_registry>();
    register_slash_commands();

    // Publish the initial tool snapshot. It is rebuilt whenever MCP tools
    // are registered (see mcp_manager::connect).
    refresh_tool_catalog();
  }

  // ---- Tool Policy Resolution ----

  json agent_system::resolve_tool_policy(const agent_config& agent_cfg) const
  {
    std::string tool_policy = agent_cfg.tool_policy;

    // Check if tool_policy is a file:// URL
    if (tool_policy.find("file://") == 0)
    {
      // Extract path from file:// URL
      std::string file_path = tool_policy.substr(7); // Remove "file://" prefix

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

    // Check if tool_policy is a JSON object string
    if (!tool_policy.empty() && (tool_policy.front() == '{' || tool_policy.front() == '['))
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

    // Simple string policy - convert to policy engine format
    json policy_config;
    policy_config["default"] = tool_policy;
    return policy_config;
  }

  // ---- Configuration Loading ----

  full_config agent_system::load_config_from_file(const std::string& config_file) const
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
    catch (const std::exception& e)
    {
      throw std::runtime_error(std::string("Failed to parse configuration file: ") + e.what());
    }

    // Parse models
    if (j.contains("models") && j["models"].is_object())
    {
      for (auto& [name, model_json] : j["models"].items())
      {
        model_config model_cfg;
        model_cfg.provider = model_json.value("provider", "mistral");
        // model_id accepts a string or an array of strings (see
        // model_config::model_ids). Missing: fall back to the entry name.
        if (model_json.contains("model_id") && model_json["model_id"].is_array())
        {
          for (const auto& id : model_json["model_id"])
          {
            if (id.is_string() && !id.get<std::string>().empty())
            {
              model_cfg.model_ids.push_back(id.get<std::string>());
            }
          }
        }
        else if (model_json.contains("model_id") && model_json["model_id"].is_string())
        {
          model_cfg.model_ids.push_back(model_json["model_id"].get<std::string>());
        }
        if (model_cfg.model_ids.empty())
        {
          model_cfg.model_ids.push_back(name);
        }
        model_cfg.api_key_env = model_json.value("api_key_env", "MISTRAL_API_KEY");
        model_cfg.base_url = model_json.value("base_url", "https://api.mistral.ai");
        model_cfg.api_version = model_json.value("api_version", "v1");

        // Parse sampling parameters
        model_cfg.sampling = parse_sampling_params(model_json);
        if (model_json.contains("strict_mode") && !model_json["strict_mode"].is_null())
        {
          model_cfg.strict_mode = model_json["strict_mode"].get<bool>();
        }

        config.models[name] = std::move(model_cfg);
      }
    }

    // Parse agents
    if (j.contains("agents") && j["agents"].is_object())
    {
      for (auto& [name, agent_json] : j["agents"].items())
      {
        agent_config agent_cfg;
        agent_cfg.model = agent_json.value("model", "mistral-medium");
        agent_cfg.system_prompt = agent_json.value("system_prompt", "");

        // Optional default model id within the entry (see
        // agent_config::model_id). The models section is parsed first, so
        // the id is validated here: a typo must fail at load time instead
        // of silently falling back to the entry's first id.
        if (agent_json.contains("model_id") && agent_json["model_id"].is_string())
        {
          const auto default_id = agent_json["model_id"].get<std::string>();
          if (!default_id.empty())
          {
            auto model_it = config.models.find(agent_cfg.model);
            if (model_it == config.models.end())
            {
              throw std::runtime_error(
                "agent '" + name + "' sets model_id but its model entry '" + agent_cfg.model + "' is not configured"
              );
            }
            const auto& ids = model_it->second.model_ids;
            if (std::find(ids.begin(), ids.end(), default_id) == ids.end())
            {
              throw std::runtime_error(
                "agent '" + name + "' sets model_id '" + default_id + "', which model entry '" + agent_cfg.model +
                "' does not offer"
              );
            }
            agent_cfg.model_id = default_id;
          }
        }

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
          for (auto& tool : agent_json["available_tools"])
          {
            agent_cfg.available_tools.push_back(tool.get<std::string>());
          }
        }

        // Parse sampling parameters (agent-level overrides)
        agent_cfg.sampling = parse_sampling_params(agent_json);
        if (agent_json.contains("strict_mode") && !agent_json["strict_mode"].is_null())
        {
          agent_cfg.strict_mode = agent_json["strict_mode"].get<bool>();
        }

        // Parse server_agent (Mistral Agents API entity). A block without
        // a name is ignored (the agent falls back to chat completions).
        if (agent_json.contains("server_agent") && agent_json["server_agent"].is_object())
        {
          const auto& server_json = agent_json["server_agent"];
          server_agent_config server_cfg;
          server_cfg.name = server_json.value("name", "");
          server_cfg.instructions = server_json.value("instructions", "");
          server_cfg.description = server_json.value("description", "");
          if (server_json.contains("tools") && server_json["tools"].is_array())
          {
            for (const auto& tool_json : server_json["tools"])
            {
              api::builtin_tool tool;
              tool.type = tool_json.value("type", "");
              if (tool_json.contains("tool_configuration") && !tool_json["tool_configuration"].is_null())
              {
                tool.tool_configuration = tool_json["tool_configuration"];
              }
              server_cfg.tools.push_back(std::move(tool));
            }
          }
          if (server_json.contains("completion_args") && server_json["completion_args"].is_object())
          {
            server_cfg.completion_args = server_json["completion_args"];
          }
          if (!server_cfg.name.empty())
          {
            agent_cfg.server_agent = std::move(server_cfg);
          }
        }

        // Parse subagent_tools (tool name -> subagent local agent id)
        if (agent_json.contains("subagent_tools") && agent_json["subagent_tools"].is_object())
        {
          for (auto& [tool_name, sub_id] : agent_json["subagent_tools"].items())
          {
            if (sub_id.is_string())
            {
              agent_cfg.subagent_tools[tool_name] = sub_id.get<std::string>();
            }
          }
        }

        config.agents[name] = std::move(agent_cfg);
      }
    }

    // Parse MCP servers. Two config shapes:
    //   "<name>": "<url>"                          (HTTP, shorthand)
    //   "<name>": { "url": "..." }                 (HTTP)
    //   "<name>": { "command": "...", "args": [...], "env": {...} }  (stdio)
    // Optional "preflight": { "check_url", "start_command", "post_start_command" }
    // Optional "enabled": false drops the entry entirely — equivalent to
    // it not being present: no auto-connect, not listed by /mcp, and
    // template feature flags (e.g. xerj_enabled) stay false.
    if (j.contains("mcpServers") && j["mcpServers"].is_object())
    {
      for (auto& [name, server_json] : j["mcpServers"].items())
      {
        // "enabled": false — skip the entry as if it were not configured.
        if (server_json.is_object() && server_json.contains("enabled") &&
            server_json["enabled"].is_boolean() && !server_json["enabled"].get<bool>())
        {
          continue;
        }

        agent::mcp_server_config cfg;

        if (server_json.is_string())
        {
          cfg.url = server_json.get<std::string>();
        }
        else if (server_json.is_object())
        {
          if (server_json.contains("url") && server_json["url"].is_string())
          {
            cfg.url = server_json["url"].get<std::string>();
          }
          if (server_json.contains("command") && server_json["command"].is_string())
          {
            cfg.command = server_json["command"].get<std::string>();
          }
          if (server_json.contains("args") && server_json["args"].is_array())
          {
            for (const auto& a : server_json["args"])
            {
              if (a.is_string())
                cfg.args.push_back(a.get<std::string>());
            }
          }
          if (server_json.contains("env") && server_json["env"].is_object())
          {
            for (auto& [k, v] : server_json["env"].items())
            {
              if (v.is_string())
                cfg.env[k] = v.get<std::string>();
            }
          }
          if (server_json.contains("preflight") && server_json["preflight"].is_object())
          {
            const auto& pf = server_json["preflight"];
            if (pf.contains("check_url") && pf["check_url"].is_string())
              cfg.check_url = pf["check_url"].get<std::string>();
            if (pf.contains("start_command") && pf["start_command"].is_array())
            {
              for (const auto& c : pf["start_command"])
                if (c.is_string())
                  cfg.start_command.push_back(c.get<std::string>());
            }
            if (pf.contains("post_start_command") && pf["post_start_command"].is_array())
            {
              for (const auto& c : pf["post_start_command"])
                if (c.is_string())
                  cfg.post_start_command.push_back(c.get<std::string>());
            }
          }
          if (server_json.contains("auto_connect") && server_json["auto_connect"].is_boolean())
          {
            cfg.auto_connect = server_json["auto_connect"].get<bool>();
          }
        }

        if (!cfg.url.empty() || !cfg.command.empty())
        {
          config.mcp_servers[name] = std::move(cfg);
        }
      }
    }

#ifdef MISTRAL_ENABLE_TREE_SITTER
    // Parse code index configuration
    if (j.contains("code_index") && j["code_index"].is_object())
    {
      if (j["code_index"].contains("exclude_dirs") && j["code_index"]["exclude_dirs"].is_array())
      {
        config.code_index.exclude_dirs = j["code_index"]["exclude_dirs"].get<std::vector<std::string>>();
      }
      if (j["code_index"].contains("max_results") && j["code_index"]["max_results"].is_number())
      {
        config.code_index.max_results = j["code_index"]["max_results"].get<int>();
      }
    }
#endif // MISTRAL_ENABLE_TREE_SITTER

    // Parse policy
    if (j.contains("policy") && j["policy"].is_object())
    {
      config.default_policy = j["policy"].value("default", "deny");
    }

    return config;
  }

  // ---- Tool Initialization ----

  void agent_system::initialize_tools(const std::string& notes_dir)
  {
    file_tools_ = std::make_unique<mistral::tools::file_tools>();
    note_tools_ = std::make_unique<mistral::tools::note_tools>(notes_dir);
#ifdef MISTRAL_ENABLE_TREE_SITTER
    code_tools_ = std::make_unique<mistral::tools::code_tools>(config_.code_index);
#endif
    exec_tools_ = std::make_unique<mistral::tools::exec_tools>();
    task_tools_ = std::make_unique<mistral::tools::task_tools>(notes_dir);
    patchset_tools_ = std::make_unique<mistral::tools::patchset_tools>(*this);
    research_tools_ = std::make_unique<mistral::tools::research_tools>();
    ask_tools_ = std::make_unique<mistral::tools::ask_tools>();
    process_manager_ = std::make_unique<mistral::common::process_manager>();
    process_tools_ = std::make_unique<mistral::tools::process_tools>(*process_manager_);
  }

  void agent_system::register_slash_commands()
  {
    namespace sc = mistral::slash_command;

    // Register built-in slash commands
    class help_command : public sc::slash_command
    {
    public:
      explicit help_command(sc::slash_command_registry& registry) : registry_(registry)
      {
      }

      std::string name() const override
      {
        return "help";
      }
      std::string description() const override
      {
        return "Show available slash commands";
      }
      sc::command_spec spec() const override
      {
        sc::arg_spec command;
        command.name = "command";
        command.description = "slash command to describe";
        command.source = sc::candidate_source::command_names;

        sc::command_spec out;
        out.name = name();
        out.summary = description();
        out.args.push_back(std::move(command));
        return out;
      }

      dripcore::awaitable<std::string> execute(dripcore::task&, const std::string& args, agent::agent_system&) override
      {
        auto names = registry_.get_command_names();

        if (args.empty())
        {
          std::string result = "Available slash commands:\n";
          for (const auto& name : names)
          {
            const auto* cmd = registry_.get_command(name);
            if (cmd)
            {
              result += "  /" + name;
              if (!cmd->description().empty())
              {
                result += " - " + cmd->description();
              }
              result += "\n";
            }
          }
          co_return result;
        }
        else
        {
          const auto* cmd = registry_.get_command(args);
          if (!cmd)
          {
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
      std::string name() const override
      {
        return "clear";
      }
      std::string description() const override
      {
        return "Clear the chat history";
      }

      dripcore::awaitable<std::string> execute(dripcore::task&, const std::string&, agent::agent_system&) override
      {
        co_return "Clearing the chat history is not yet implemented";
      }
    };

    class tools_command : public sc::slash_command
    {
    public:
      std::string name() const override
      {
        return "tools";
      }
      std::string description() const override
      {
        return "List the tools available to the current agent";
      }

      sc::command_spec spec() const override
      {
        sc::arg_spec all;
        all.name = "all";
        all.description = "list every registered tool";
        all.source = sc::candidate_source::literal;
        all.literals = {{"all", "list every registered tool"}};

        sc::command_spec out;
        out.name = name();
        out.summary = description();
        out.args.push_back(std::move(all));
        return out;
      }

      dripcore::awaitable<std::string> execute(dripcore::task&, const std::string& args, agent_system& sys) override
      {
        const sc::invocation inv = spec().parse(args);
        if (!inv.ok())
        {
          co_return "Error: " + inv.error + "\nusage: /" + usage();
        }

        const bool show_all = !inv.args.empty() && inv.args[0] == "all";
        const auto catalog = sys.tool_catalog_snapshot();
        const auto agent_id = sys.current_agent_id();

        // Tools the current agent can call (empty when no agent is active).
        const std::vector<std::string> agent_tools = agent_id ? sys.agent_tool_names(*agent_id) : std::vector<std::string>{};

        if (show_all)
        {
          std::string result = "all registered tools";
          if (catalog)
          {
            result += " (" + std::to_string(catalog->all.size()) + ")";
          }
          result += ":\n";
          if (!catalog || catalog->all.empty())
          {
            co_return result + "  (none)\n";
          }
          for (const auto& name : catalog->all)
          {
            const bool available = std::find(agent_tools.begin(), agent_tools.end(), name) != agent_tools.end();
            result += "  - " + name;
            if (agent_id && !available)
            {
              result += "  (not available to this agent)";
            }
            result += "\n";
          }
          co_return result;
        }

        if (!agent_id)
        {
          co_return "No active agent; use /tools all to list every registered tool";
        }

        std::string result =
          "tools available to '" + *agent_id + "' (" + std::to_string(agent_tools.size()) + "):\n";
        if (agent_tools.empty())
        {
          co_return result + "  (none)\n";
        }
        for (const auto& name : agent_tools)
        {
          result += "  - " + name + "\n";
        }
        co_return result;
      }
    };

    class exit_command : public sc::slash_command
    {
    public:
      std::string name() const override
      {
        return "exit";
      }
      std::string description() const override
      {
        return "Exit the application";
      }

      dripcore::awaitable<std::string> execute(dripcore::task&, const std::string&, agent::agent_system&) override
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
    registry.register_command(std::make_unique<tool_command>());
    registry.register_command(std::make_unique<compact_command>());
    registry.register_command(std::make_unique<mcp_command>());
  }

  void agent_system::build_global_tool_registry()
  {
    // Register all available tools
    auto& file_tools = *file_tools_;
    auto& note_tools = *note_tools_;
#ifdef MISTRAL_ENABLE_TREE_SITTER
    auto& code_tools = *code_tools_;
#endif
    auto& exec_tools = *exec_tools_;
    auto& process_tools = *process_tools_;
    auto& task_tools = *task_tools_;
    auto& patchset_tools = *patchset_tools_;
    auto& research_tools = *research_tools_;
    auto& ask_tools = *ask_tools_;

    global_tools_.register_tool(
      "ls",
      "List files and directories. Returns entries with type indicator (f=file, d=directory, l=symlink), "
      "size for files, and relative path.",
      &file_tools,
      &mistral::tools::file_tools::list_files,
      arg("path", "Directory path to list"),
      arg("recursive", "Whether to list subdirectories recursively", false),
      arg("max_depth", "Maximum recursion depth (0 for unlimited)", 1)
    );

    global_tools_.register_tool(
      "read",
      "Read file contents. Use line_from and line_to for specific ranges. "
      "Set both to 0 to read entire file. Lines are 1-indexed.",
      &file_tools,
      &mistral::tools::file_tools::read_file,
      arg("path", "File path to read"),
      arg("line_from", "First line to read (1-indexed, 0 = from beginning)", 0),
      arg("line_to", "Last line to read (inclusive, 0 = until end)", 0)
    );

#ifdef MISTRAL_ENABLE_TREE_SITTER
    global_tools_.register_tool(
      "outline",
      "List the symbols (classes, structs, functions, methods, types) defined in a C/C++ "
      "source file with their line ranges. Use this to see a file's structure before "
      "reading specific symbols.",
      &code_tools,
      &mistral::tools::code_tools::outline,
      arg("path", "C/C++ source file to outline")
    );

    global_tools_.register_tool(
      "read_symbol",
      "Extract the exact source text of a named symbol definition (class, function, "
      "method, type) from a C/C++ source file. Returns every matching definition, "
      "e.g. overloads. Qualify the name (Class::method) to pick one method among "
      "same-named definitions; each result reports its scope.",
      &code_tools,
      &mistral::tools::code_tools::read_symbol,
      arg("path", "C/C++ source file containing the symbol"),
      arg("name", "Name of the symbol to extract; may be qualified (Class::method)")
    );

    global_tools_.register_tool(
      "find_symbol",
      "Find symbol definitions (classes, structs, functions, methods, types) by name in a "
      "directory tree or in a single file. Falls back to substring matching when no exact "
      "match exists. Returns file, kind, scope, line range and the matched line's text for "
      "each definition. Names may be qualified (Class::method or ns::Class::method) to "
      "narrow same-named symbols. A zero result includes a \"hint\" saying whether the "
      "symbol is absent, defined elsewhere, or that no C/C++ files were found.",
      &code_tools,
      &mistral::tools::code_tools::find_symbol,
      arg("name", "Symbol name to find; may be qualified (Class::method)"),
      arg("path", "File or directory to search (workspace root by default)", std::string("."))
    );

    global_tools_.register_tool(
      "find_references",
      "Find where a named symbol is defined and used (calls, type usages, member access) in a "
      "directory tree or in a single file. Definition sites are included by default and "
      "flagged \"is_definition\": true; pass include_definitions=false for usages only. "
      "Matches by name without type resolution, so same-named symbols in unrelated scopes "
      "also match; use read_symbol to confirm which overload a hit belongs to.",
      &code_tools,
      &mistral::tools::code_tools::find_references,
      arg("name", "Symbol name to find references for"),
      arg("path", "File or directory to search (workspace root by default)", std::string(".")),
      arg("include_definitions", "Include definition sites, flagged \"is_definition\": true", true)
    );

    global_tools_.register_tool(
      "ast_search",
      "Search a C/C++ file or directory tree with a tree-sitter query (scheme-like syntax). "
      "Example: ((call_expression function: (identifier) @fn) (#eq? @fn \"foo\")) finds all "
      "calls to foo. Each match reports its captures with text and line. Common node types: "
      "function_definition, declaration, class_specifier, call_expression, field_expression, "
      "identifier, type_identifier, field_identifier. Predicates: #eq?, #not-eq?, #match?, "
      "#not-match?, #any-eq?, #any-match?, #is?, #is-not?.",
      &code_tools,
      &mistral::tools::code_tools::ast_search,
      arg("query", "Tree-sitter query to run"),
      arg("path", "File or directory to search (workspace root by default)", std::string("."))
    );
#endif // MISTRAL_ENABLE_TREE_SITTER

    global_tools_.register_tool(
      "note_list",
      "List notes in the notes directory. Shows path, title, and tags for each note. "
      "Pass an empty string to list all notes, or a subfolder name to filter.",
      &note_tools,
      &mistral::tools::note_tools::list_notes,
      arg("folder", "Subfolder to list (empty for all)", std::string(""))
    );

    global_tools_.register_tool(
      "note_read",
      "Read a note from the notes directory. Resolves {{> path}} include directives. "
      "Returns title, tags, and rendered content. The .md extension is optional.",
      &note_tools,
      &mistral::tools::note_tools::read_note,
      arg("path", "Note path relative to notes directory")
    );

    global_tools_.register_tool(
      "note_write",
      "Create or update a note. Use YAML frontmatter for title and tags:\n"
      "---\ntitle: My Note\ntags: [tag1, tag2]\n---\n"
      "Use {{> path}} to include other notes (partials). Put reusable snippets in _partials/.",
      &note_tools,
      &mistral::tools::note_tools::write_note,
      arg("path", "Note path relative to notes directory"),
      arg("content", "Note content (with optional frontmatter)")
    );

    global_tools_.register_tool(
      "note_search",
      "Search notes by text content and/or tag. At least one must be provided. "
      "Returns matching notes with path, title, tags, and matching line context.",
      &note_tools,
      &mistral::tools::note_tools::search_notes,
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
      &exec_tools,
      &mistral::tools::exec_tools::exec,
      arg("command", "Executable name (e.g., 'git', 'cmake')"),
      arg("args", "Command arguments as array of strings (optional, defaults to an empty array)", std::vector<std::string>{}),
      arg("cwd", "Working directory (optional, defaults to current)", std::string("")),
      arg("timeout_seconds", "Timeout in seconds", 30)
    );

    global_tools_.register_tool(
      "process_start",
      "Start a long-running background process (dev server, REPL like lldb/gdb, ...) and return "
      "immediately with a session id - unlike exec, the process keeps running after the call. "
      "Uses execvp() directly - no shell: the command must be a single executable name and each "
      "argument a separate array element. stdin/stdout/stderr are piped; the process runs in its "
      "own process group and is killed when the CLI exits. Drive it with process_write (stdin), "
      "process_read (output), process_kill (stop), process_list (status). "
      "When wait_for (a regex) is non-empty, the call blocks until the process output matches it, "
      "the process exits, or wait_timeout_seconds elapse (default 60) - e.g. wait for a dev server "
      "to print its ready line; the result's ready flag says whether it matched. "
      "env is a list of KEY=VALUE strings applied over the inherited environment. "
      "name optionally sets the session id (letters, digits, '.', '_', '-'; must be unique).",
      &process_tools,
      &mistral::tools::process_tools::process_start,
      arg("command", "Executable name (e.g., 'npm', 'lldb')"),
      arg("args", "Arguments as array of strings (optional, defaults to an empty array)", std::vector<std::string>{}),
      arg("cwd", "Working directory (optional, defaults to current)", std::string("")),
      arg("env", "Environment overrides as KEY=VALUE strings (optional, defaults to none)", std::vector<std::string>{}),
      arg("name", "Session id to use instead of an auto-generated one (optional)", std::string("")),
      arg("wait_for", "Regex to wait for in the process output before returning (optional, empty = return immediately)", std::string("")),
      arg("wait_timeout_seconds", "Max seconds to wait for wait_for (default 60 when wait_for is set)", 0)
    );

    global_tools_.register_tool(
      "process_write",
      "Write input verbatim to the stdin of a background process session started with process_start. "
      "Include a trailing newline for line-based REPLs (lldb, gdb, python). Returns the number of "
      "bytes written; fails when the process has exited or the session is unknown.",
      &process_tools,
      &mistral::tools::process_tools::process_write,
      arg("id", "Session id returned by process_start"),
      arg("input", "Text to write to the process's stdin, verbatim")
    );

    global_tools_.register_tool(
      "process_read",
      "Read output produced by a background process session since byte offset `since` "
      "(0 = from the start; pass next_offset from the previous read to get only new output). "
      "stdout and stderr are merged. Each call returns at most max_bytes (default 65536); the "
      "session retains the last 256 KiB, so very old output is dropped and reported via truncated. "
      "Returns output, next_offset, total_bytes, running/exited and exit_code.",
      &process_tools,
      &mistral::tools::process_tools::process_read,
      arg("id", "Session id returned by process_start"),
      arg("since", "Byte offset to read from (use next_offset of the previous read)", int64_t(0)),
      arg("max_bytes", "Maximum output bytes to return in this call", 64 * 1024)
    );

    global_tools_.register_tool(
      "process_list",
      "List all background process sessions: id, name, command, args, pid, running/exited, "
      "exit_code, total output bytes and uptime in seconds.",
      &process_tools,
      &mistral::tools::process_tools::process_list
    );

    global_tools_.register_tool(
      "process_kill",
      "Kill a background process session: SIGKILL to the whole process group, then reap. "
      "The session stays listed (exited, with exit_code) and its remaining output stays "
      "readable via process_read.",
      &process_tools,
      &mistral::tools::process_tools::process_kill,
      arg("id", "Session id returned by process_start")
    );

    global_tools_.register_tool(
      "patchset_add",
      "Add a single edit to a stored patchset. Creates the patchset if the ID is new; "
      "appends if the ID matches a stored non-applied patchset. Empty search_text means "
      "new file creation; empty replace_text deletes the search text. Tool-based "
      "alternative to emitting edit blocks in response text - use one channel per "
      "patchset, never both. Only one patchset can be stored at a time: creating a new "
      "ID while a different non-applied patchset is stored fails - apply it or discard "
      "it with patchset_clear first. After adding all edits, validate with "
      "patchset_validate, then apply with patchset_apply.",
      &patchset_tools,
      &mistral::tools::patchset_tools::patchset_add,
      arg("patchset_id", "The unique identifier of the patchset (alphanumeric, hyphens, underscores)"),
      arg("file_path", "The file path for the edit, relative to the workspace root"),
      arg("search_text", "The exact text to find (empty string to create a new file)"),
      arg("replace_text", "The replacement text (empty string to delete the search text)")
    );

    global_tools_.register_tool(
      "patchset_apply",
      "Apply a stored patchset of file edits by its ID. If validation fails, the response lists per-edit errors; fix "
      "them with patchset_update or patchset_remove, then apply again.",
      &patchset_tools,
      &mistral::tools::patchset_tools::patchset_apply,
      arg("patchset_id", "The unique identifier of the patchset to apply")
    );

    global_tools_.register_tool(
      "patchset_get",
      "Get details of a stored patchset including all numbered edits.",
      &patchset_tools,
      &mistral::tools::patchset_tools::patchset_get,
      arg("patchset_id", "The unique identifier of the patchset")
    );

    global_tools_.register_tool(
      "patchset_validate",
      "Validate a stored patchset without applying (dry run). Returns detailed validation results.",
      &patchset_tools,
      &mistral::tools::patchset_tools::patchset_validate,
      arg("patchset_id", "The unique identifier of the patchset to validate")
    );

    global_tools_.register_tool(
      "patchset_update",
      "Update a single edit in a stored patchset. file_path keeps the existing value when empty. "
      "search_text and replace_text keep the existing value when omitted or null; "
      "pass an explicit empty string to clear the field.",
      &patchset_tools,
      &mistral::tools::patchset_tools::patchset_update,
      arg("patchset_id", "The unique identifier of the patchset"),
      arg("patch_number", "The 1-indexed number of the patch to update"),
      arg("file_path", "The file path for the edit (keeps existing if empty)"),
      arg("search_text", "The search text (omit or null to keep existing; empty string clears it)"),
      arg("replace_text", "The replace text (omit or null to keep existing; empty string clears it)")
    );

    global_tools_.register_tool(
      "patchset_remove",
      "Remove a single edit block from a stored patchset by its patch number. The remaining edits keep their patch "
      "numbers. Use this to drop broken or unwanted blocks without re-emitting the whole patchset.",
      &patchset_tools,
      &mistral::tools::patchset_tools::patchset_remove,
      arg("patchset_id", "The unique identifier of the patchset"),
      arg("patch_number", "The number of the edit to remove (as shown by patchset_get or patchset_validate)")
    );

    global_tools_.register_tool(
      "patchset_clear",
      "Clear a stored patchset.",
      &patchset_tools,
      &mistral::tools::patchset_tools::patchset_clear,
      arg("patchset_id", "The unique identifier of the patchset to clear")
    );

    // ---- Task tools ----

    global_tools_.register_tool(
      "task_create",
      "Create a new task for project tracking. Tasks persist across sessions. "
      "Returns the created task with a generated ID.",
      &task_tools,
      &mistral::tools::task_tools::create_task,
      arg("title", "Short summary of the task"),
      arg("description", "Detailed explanation (optional)", std::string("")),
      arg("priority", "Priority: low, medium, high, critical (default: medium)", std::string("medium")),
      arg("tags", "List of tags for categorisation", std::vector<std::string>{})
    );

    global_tools_.register_tool(
      "task_list",
      "List tasks with optional filters. Empty filter values mean no filter. "
      "Returns an array of task summaries (id, title, status, priority, tags).",
      &task_tools,
      &mistral::tools::task_tools::list_tasks,
      arg("status", "Filter by status: todo, in-progress, done, blocked, cancelled (empty for all)", std::string("")),
      arg("tag", "Filter by tag (empty for all)", std::string("")),
      arg("priority", "Filter by priority: low, medium, high, critical (empty for all)", std::string(""))
    );

    global_tools_.register_tool(
      "task_get",
      "Get full details of a task by its ID.",
      &task_tools,
      &mistral::tools::task_tools::get_task,
      arg("task_id", "The task ID (e.g. task-001)")
    );

    global_tools_.register_tool(
      "task_update",
      "Update a task. Pass only the fields you want to change; empty strings are ignored. "
      "Returns the updated task.",
      &task_tools,
      &mistral::tools::task_tools::update_task,
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
      &task_tools,
      &mistral::tools::task_tools::search_tasks,
      arg("query", "Text to search for")
    );

    // ---- Research tools ----

    // web_search is executed by routing to a server-side researcher agent
    // (see agent_config::subagent_tools and agent_task_base::execute_tool);
    // the registered implementation only serves direct/debug invocation.
    global_tools_.register_tool(
      "web_search",
      "Search the web for current information. Delegates to a research "
      "subagent that can search the web and returns a concise summary of "
      "findings with sources. Use this for questions about recent events, "
      "current software versions, or any facts you are unsure about.",
      &research_tools,
      &mistral::tools::research_tools::web_search,
      arg("query", "The search query or research question")
    );

    // ---- Ask tools ----

    global_tools_.register_tool(
      "ask_user",
      "Ask the human a question when you reach a decision point that changes "
      "the direction of the work - an unforeseen design choice, conflicting "
      "requirements, or a trade-off you cannot resolve from the codebase or "
      "conversation. The agent pauses until the human answers; the answer is "
      "returned as the tool result. Only ask when the decision genuinely "
      "changes direction: batch related questions into one call, include a "
      "recommendation so the human can confirm with a short reply, and never "
      "ask for things you can decide yourself. Do not use this for status "
      "updates or progress reports. The result contains the human's raw "
      "answer; when the answer is a bare option number, matched_option "
      "holds the resolved option text - prefer matched_option over "
      "interpreting the raw answer.",
      &ask_tools,
      &mistral::tools::ask_tools::ask_user,
      arg("question", "The decision that needs a human answer, phrased as a clear question"),
      arg("context", "What you discovered that forces this decision (1-3 sentences)", std::string("")),
      arg(
        "options",
        "The viable options, one per entry, each with a short label and its trade-off",
        std::vector<std::string>{}
      ),
      arg(
        "recommendation",
        "Your recommended option and why. Lets the human confirm with a one-word reply",
        std::string("")
      )
    );

    // Register custom approval prompt renderers
    register_approval_renderers(global_tools_);
  }

  // ---- Configuration Access ----

  const agent_config& agent_system::get_agent_config(const std::string& agent_id) const
  {
    auto it = config_.agents.find(agent_id);
    if (it == config_.agents.end())
    {
      throw std::runtime_error("Agent configuration not found: " + agent_id);
    }
    return it->second;
  }

  bool agent_system::has_agent_config(const std::string& agent_id) const
  {
    return config_.agents.contains(agent_id);
  }

  const model_config& agent_system::get_model_config(const std::string& model_name) const
  {
    auto it = config_.models.find(model_name);
    if (it == config_.models.end())
    {
      throw std::runtime_error("Model configuration not found: " + model_name);
    }
    return it->second;
  }

  bool agent_system::has_model_config(const std::string& model_name) const
  {
    return config_.models.contains(model_name);
  }

  api::sampling_params agent_system::resolve_sampling_params(const agent_config& agent_cfg) const
  {
    api::sampling_params params;

    // Apply model defaults if available
    if (has_model_config(agent_cfg.model))
    {
      params = get_model_config(agent_cfg.model).sampling;
    }

    // Built-in defaults for unset reasoning effort / temperature
    if (!params.reasoning_effort)
    {
      params.reasoning_effort = "high";
    }
    if (!params.temperature)
    {
      params.temperature = 0.6f;
    }

    // Apply agent overrides (if specified)
    apply_sampling_overrides(params, agent_cfg.sampling);

    return params;
  }

  tool_schema_options agent_system::resolve_tool_schema_options(const agent_config& agent_cfg) const
  {
    tool_schema_options opts;

    // Model defaults
    if (has_model_config(agent_cfg.model))
    {
      const auto& model_cfg = get_model_config(agent_cfg.model);
      if (model_cfg.strict_mode)
        opts.strict_mode = *model_cfg.strict_mode;
    }

    // Agent overrides (if specified)
    if (agent_cfg.strict_mode)
      opts.strict_mode = *agent_cfg.strict_mode;

    return opts;
  }

  std::string agent_system::resolve_model_id(const std::string& model_name) const
  {
    return resolve_model_id(model_name, std::nullopt);
  }

  std::string agent_system::resolve_model_id(const agent_config& agent_cfg) const
  {
    return resolve_model_id(agent_cfg.model, agent_cfg.model_id);
  }

  std::string agent_system::resolve_model_id(
    const std::string& model_name,
    const std::optional<std::string>& default_id
  ) const
  {
    auto it = config_.models.find(model_name);
    if (it == config_.models.end() || it->second.model_ids.empty())
    {
      // Unknown entry: return the name as-is (backward compatibility).
      // Parsing guarantees at least one id, but a hand-built model_config
      // must never reach model_ids.front().
      return model_name;
    }

    const auto& ids = it->second.model_ids;
    {
      std::lock_guard lock(active_model_ids_mtx_);
      if (auto active = active_model_ids_.find(model_name); active != active_model_ids_.end())
      {
        // The selection is validated by set_active_model_id(); re-check
        // anyway in case the config was reloaded underneath.
        if (std::find(ids.begin(), ids.end(), active->second) != ids.end())
        {
          return active->second;
        }
      }
    }

    // The agent's own default, when it names an id the entry offers. The
    // loader validates it; re-check for hand-built configs.
    if (default_id && std::find(ids.begin(), ids.end(), *default_id) != ids.end())
    {
      return *default_id;
    }
    return ids.front();
  }

  std::vector<std::string> agent_system::model_ids_for(const std::string& model_name) const
  {
    auto it = config_.models.find(model_name);
    if (it == config_.models.end())
    {
      return {};
    }
    return it->second.model_ids;
  }

  void agent_system::set_active_model_id(const std::string& model_name, const std::string& model_id)
  {
    auto it = config_.models.find(model_name);
    if (it == config_.models.end())
    {
      throw std::runtime_error("Model configuration not found: " + model_name);
    }
    const auto& ids = it->second.model_ids;
    if (std::find(ids.begin(), ids.end(), model_id) == ids.end())
    {
      throw std::runtime_error("Model '" + model_name + "' does not offer model id '" + model_id + "'");
    }

    {
      std::lock_guard lock(active_model_ids_mtx_);
      active_model_ids_[model_name] = model_id;
    }

    // A server-side agent owns its model, and its entity id is cached for
    // the session (see server_agent_task::ensure_server_agent). Drop the
    // cached ids of the agents using this entry, so the next call re-checks
    // the config fingerprint and patches the entity's model instead of
    // silently keeping the one it was created with.
    {
      std::lock_guard lock(server_agent_ids_mtx_);
      for (const auto& [agent_id, agent_cfg] : config_.agents)
      {
        if (agent_cfg.model == model_name)
        {
          server_agent_ids_.erase(agent_id);
        }
      }
    }
  }

  std::vector<transport_endpoint> agent_system::resolve_transport_endpoints() const
  {
    std::vector<transport_endpoint> endpoints;

    for (const auto& [agent_id, agent_cfg] : config_.agents)
    {
      (void)agent_id;
      auto model_it = config_.models.find(agent_cfg.model);
      if (model_it == config_.models.end())
      {
        // Unknown model: get_model_config() reports this when the agent is used
        continue;
      }

      const auto ep = endpoint_for_model(model_it->second);
      if (std::find(endpoints.begin(), endpoints.end(), ep) == endpoints.end())
      {
        endpoints.push_back(ep);
      }
    }

    return endpoints;
  }

  // ---- Tool Registry ----

  tool_registry agent_system::create_tool_registry(const std::vector<std::string>& available_tools) const
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
      // "mcp:<server>" pulls in all tools of one MCP server (or of
      // all connected servers with "mcp:*"); the tools must already
      // be registered via /mcp connect.
      if (tool_name.starts_with("mcp:"))
      {
        for (const auto& mcp_name : mcp::mcp_manager::instance().tool_names(tool_name.substr(4)))
        {
          if (auto tool = global_tools_.get_tool(mcp_name))
          {
            filtered.add_tool(mcp_name, tool->clone());
          }
        }
        continue;
      }
      if (auto tool = global_tools_.get_tool(tool_name))
      {
        filtered.add_tool(tool_name, tool->clone());
      }
    }
    return filtered;
  }

  // ---- Channel Access ----

  std::shared_ptr<dripcore::channel<api::api_request>> agent_system::request_ch_for(const std::string& agent_id) const
  {
    const auto& agent_cfg = get_agent_config(agent_id);
    const auto ep = endpoint_for_model(get_model_config(agent_cfg.model));

    const auto it = std::find_if(endpoints_.begin(), endpoints_.end(), [&ep](const endpoint_entry& entry) {
      return entry.endpoint == ep;
    });
    if (it == endpoints_.end())
    {
      throw std::runtime_error("no transport endpoint channel for agent '" + agent_id + "'");
    }
    return it->request_ch;
  }

  std::vector<std::string> agent_system::reachable_agent_ids(const std::string& agent_id) const
  {
    std::vector<std::string> reachable;
    std::vector<std::string> pending{agent_id};

    while (!pending.empty())
    {
      std::string current = std::move(pending.back());
      pending.pop_back();

      if (std::find(reachable.begin(), reachable.end(), current) != reachable.end())
      {
        continue; // Already seen: also terminates a subagent cycle.
      }
      reachable.push_back(current);

      const auto it = config_.agents.find(current);
      if (it == config_.agents.end())
      {
        continue; // Unknown id: reported when the agent is actually used.
      }
      for (const auto& [tool_name, subagent_id] : it->second.subagent_tools)
      {
        (void)tool_name;
        pending.push_back(subagent_id);
      }
    }

    return reachable;
  }

  void agent_system::validate_transport_keys(const std::string& agent_id) const
  {
    // Only the endpoints this session can reach are checked. A config may
    // list an agent on a second provider that is never selected (e.g.
    // -a main with a greenpt agent present), and demanding its key would
    // block a session that never talks to it.
    for (const auto& id : reachable_agent_ids(agent_id))
    {
      const auto agent_it = config_.agents.find(id);
      if (agent_it == config_.agents.end())
      {
        continue;
      }
      const auto model_it = config_.models.find(agent_it->second.model);
      if (model_it == config_.models.end())
      {
        // Unknown model: get_model_config() reports this when the agent is used.
        continue;
      }

      const auto ep = endpoint_for_model(model_it->second);
      if (ep.api_key_env.empty())
      {
        continue; // Endpoint requires no key
      }
      if (std::getenv(ep.api_key_env.c_str()) == nullptr)
      {
        throw std::runtime_error(
          "endpoint " + ep.host + ":" + std::to_string(ep.port) + " requires " + ep.api_key_env +
          " (environment variable not set, needed by agent '" + id + "')"
        );
      }
    }
  }

  void agent_system::start_transport(dripcore::scheduler& sched, const std::string& agent_id)
  {
    validate_transport_keys(agent_id);

    for (const auto& entry : endpoints_)
    {
      // An endpoint may belong to an agent this session never uses, in
      // which case its key can be unset. Assigning getenv()'s nullptr to
      // a std::string is undefined behaviour, so check it.
      std::string api_key;
      if (!entry.endpoint.api_key_env.empty())
      {
        if (const char* key = std::getenv(entry.endpoint.api_key_env.c_str()); key != nullptr)
        {
          api_key = key;
        }
      }
      sched.spawn<net::http_coordinator_task>(entry.endpoint.host, entry.endpoint.port, api_key, entry.request_ch);
    }
  }

  void agent_system::stop_transport()
  {
    for (const auto& entry : endpoints_)
    {
      if (entry.request_ch)
      {
        entry.request_ch->close();
      }
    }
  }

  std::string agent_system::get_last_response() const
  {
    std::lock_guard<std::mutex> lock(last_response_mtx_);
    return last_response_;
  }

  void agent_system::set_last_response(const std::string& response)
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

  std::string agent_system::resolve_prompt_string(const std::string& prompt, const std::string& model_id) const
  {
    // Check if prompt is a file:// URL
    if (prompt.find("file://") != 0)
    {
      return prompt;
    }

    // Extract path from file:// URL
    std::string file_path = prompt.substr(7); // Remove "file://" prefix

    try
    {
      // Check if file exists
      if (!fs::exists(file_path))
      {
        throw std::runtime_error("Prompt file not found: " + file_path);
      }

      // Use template engine to process the file with variables
      mistral::tmpl::template_engine tmpl;
      tmpl.set_include_base(base_dir_);
      tmpl.set_variable("model", model_id);
      tmpl.set_variable("notes_dir", notes_dir_);

      // Set feature flags for conditional includes ({{> file if=var}}).
      // xerj_enabled: true when the xerj MCP server is configured and
      // enabled ("enabled": false entries are dropped at load time).
      tmpl.set_variable("xerj_enabled", config_.mcp_servers.contains("xerj") ? "true" : "false");
#ifdef MISTRAL_ENABLE_TREE_SITTER
      // tree_sitter_enabled: true in builds with the tree-sitter code tools.
      tmpl.set_variable("tree_sitter_enabled", "true");
#else
      tmpl.set_variable("tree_sitter_enabled", "false");
#endif

      // Process the file as a template
      return tmpl.process_file(file_path);
    }
    catch (const std::exception& e)
    {
      std::cerr << "error resolving prompt file: " << e.what() << std::endl;
      throw;
    }
  }

  std::string agent_system::resolve_system_prompt(const agent_config& agent_cfg, const std::string& model_id) const
  {
    std::string prompt = resolve_prompt_string(agent_cfg.system_prompt, model_id);

    // When the agent can ask the human, introduce the capability briefly
    // in the system prompt: behavioral guidance lives there, and the
    // tool description alone is easy to overlook. Conditional on the
    // tool being available, so agents without ask_user never see a
    // dangling reference.
    const auto& tools = agent_cfg.available_tools;
    const bool ask_user_available = std::find(tools.begin(), tools.end(), "*") != tools.end() ||
                                    std::find(tools.begin(), tools.end(), "ask_user") != tools.end();
    if (ask_user_available)
    {
      if (!prompt.empty())
        prompt += "\n\n";
      prompt += "---\n\n## Asking the human\n\n"
                "You can ask the human directly with the `ask_user` tool. When "
                "you reach a decision that changes the direction of the work — "
                "an unforeseen design choice, conflicting requirements, or a "
                "trade-off you cannot resolve from the codebase or conversation "
                "— ask instead of guessing or silently picking a default. The "
                "tool blocks until the human answers; the answer returns as the "
                "tool result. See the tool description for when and how to ask.";
    }
    return prompt;
  }

  std::string agent_system::resolve_server_agent_instructions(const agent_config& agent_cfg) const
  {
    if (!agent_cfg.server_agent || agent_cfg.server_agent->instructions.empty())
    {
      return "";
    }
    return resolve_prompt_string(agent_cfg.server_agent->instructions, resolve_model_id(agent_cfg));
  }

  std::string server_agent_config_fingerprint(
    const server_agent_config& cfg,
    const std::string& resolved_instructions,
    const std::string& model
  )
  {
    api::json canonical;
    canonical["model"] = model;
    canonical["tools"] = cfg.tools;
    canonical["instructions"] = resolved_instructions;
    canonical["description"] = cfg.description;
    if (cfg.completion_args)
    {
      canonical["completion_args"] = *cfg.completion_args;
    }
    return fnv1a_64_hex(canonical.dump());
  }

  dripcore::awaitable<std::string> agent_system::run_subagent(
    dripcore::task& task,
    const std::string& agent_id,
    const std::string& message
  )
  {
    if (!has_agent_config(agent_id))
    {
      co_return "Error: Agent '" + agent_id + "' not found in configuration";
    }

    auto input_ch = std::make_shared<dripcore::channel<std::string>>(4);
    auto output_ch = std::make_shared<dripcore::channel<std::string>>(4);

    // Agents with a server_agent block run on the Mistral Agents API
    // (POST /v1/conversations); all others use chat completions.
    if (get_agent_config(agent_id).server_agent)
    {
      task.spawn_local<server_agent_task>(agent_id, input_ch, output_ch);
    }
    else
    {
      task.spawn_local<subagent_task>(agent_id, input_ch, output_ch);
    }

    input_ch->try_send(message);

    auto response = co_await output_ch->recv(&task);
    // Close the input channel only after the response: the agent loop
    // checks is_closed() before starting a request, so closing earlier
    // would abort the turn before any API request is made.
    input_ch->close();
    if (!response)
    {
      co_return "Error: No response from subagent " + agent_id;
    }

    co_return *response;
  }

  // ---- Server-side Agent Id Cache ----

  std::optional<std::string> agent_system::get_server_agent_id(const std::string& agent_id) const
  {
    std::lock_guard<std::mutex> lock(server_agent_ids_mtx_);
    auto it = server_agent_ids_.find(agent_id);
    if (it == server_agent_ids_.end())
    {
      return std::nullopt;
    }
    return it->second;
  }

  void agent_system::set_server_agent_id(const std::string& agent_id, const std::string& server_agent_id)
  {
    std::lock_guard<std::mutex> lock(server_agent_ids_mtx_);
    server_agent_ids_[agent_id] = server_agent_id;
  }

  // ---- Patchset Storage ----

  // Store a patchset, overwriting any previous one
  void agent_system::store_patchset(const std::string& patchset_id, const std::vector<tools::file_edit_t>& edits)
  {
    current_patchset_ = tools::patchset_t{patchset_id, edits};
  }

  // Append edits to an existing non-applied patchset with matching ID.
  // If no matching patchset exists, stores as a new patchset.
  void agent_system::append_edits(const std::string& patchset_id, const std::vector<tools::file_edit_t>& edits)
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
  std::optional<tools::patchset_t> agent_system::get_patchset(const std::string& patchset_id) const
  {
    if (current_patchset_ && current_patchset_->patchset_id == patchset_id)
    {
      return current_patchset_;
    }
    return std::nullopt;
  }

  // Get the ID of the currently stored patchset
  std::optional<std::string> agent_system::get_current_patchset_id() const
  {
    if (current_patchset_)
    {
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

  void agent_system::set_current_cancel(std::function<void()> cancel)
  {
    std::lock_guard<std::mutex> lock(current_cancel_mtx_);
    current_cancel_ = std::move(cancel);
  }

  void agent_system::clear_current_cancel()
  {
    std::lock_guard<std::mutex> lock(current_cancel_mtx_);
    current_cancel_ = nullptr;
  }

  void agent_system::cancel_current()
  {
    // Copy under the lock and invoke outside it. The callback closes a
    // channel, which wakes tasks that may register a new hook, so holding
    // the lock across the call would deadlock.
    std::function<void()> cancel;
    {
      std::lock_guard<std::mutex> lock(current_cancel_mtx_);
      cancel = current_cancel_;
    }
    if (cancel)
    {
      cancel();
    }
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

  // ---- Tool catalog ----

  namespace
  {
    // Resolve an agent's available_tools entries against a catalog. Mirrors
    // create_tool_registry() but only collects names, so it can run without
    // cloning tool instances.
    std::vector<std::string> resolve_tool_names(
      const std::vector<std::string>& available_tools,
      const tool_catalog& catalog
    )
    {
      if (std::find(available_tools.begin(), available_tools.end(), "*") != available_tools.end())
      {
        return catalog.all;
      }

      std::vector<std::string> names;
      for (const auto& entry : available_tools)
      {
        // "mcp:<server>" pulls in all tools of one MCP server (or of all
        // connected servers with "mcp:*").
        if (entry.starts_with("mcp:"))
        {
          const std::string server = entry.substr(4);
          if (server == "*")
          {
            for (const auto& [name, tools] : catalog.mcp_by_server)
            {
              names.insert(names.end(), tools.begin(), tools.end());
            }
          }
          else if (auto it = catalog.mcp_by_server.find(server); it != catalog.mcp_by_server.end())
          {
            names.insert(names.end(), it->second.begin(), it->second.end());
          }
          continue;
        }

        if (std::find(catalog.all.begin(), catalog.all.end(), entry) != catalog.all.end())
        {
          names.push_back(entry);
        }
      }

      std::sort(names.begin(), names.end());
      names.erase(std::unique(names.begin(), names.end()), names.end());
      return names;
    }
  } // namespace

  void agent_system::set_current_agent_id(const std::string& agent_id)
  {
    std::lock_guard<std::mutex> lock(current_agent_id_mtx_);
    current_agent_id_ = agent_id;
  }

  std::optional<std::string> agent_system::current_agent_id() const
  {
    std::lock_guard<std::mutex> lock(current_agent_id_mtx_);
    return current_agent_id_;
  }

  std::shared_ptr<const tool_catalog> agent_system::tool_catalog_snapshot() const
  {
    std::lock_guard<std::mutex> lock(tool_catalog_mtx_);
    return tool_catalog_;
  }

  void agent_system::refresh_tool_catalog()
  {
    auto catalog = std::make_shared<tool_catalog>();

    // Global registry names (std::map iteration order is sorted).
    for (const auto& [name, tool] : global_tools_.get_tools_map())
    {
      catalog->all.push_back(name);
    }

    // Snapshot the MCP server -> tool mapping so readers never touch the
    // manager's live state.
    auto& mcp_mgr = mcp::mcp_manager::instance();
    for (const auto& server : mcp_mgr.server_names())
    {
      catalog->mcp_by_server[server] = mcp_mgr.tool_names(server);
    }

    // Resolve each configured agent's available_tools.
    for (const auto& [agent_id, agent_cfg] : config_.agents)
    {
      catalog->per_agent[agent_id] = resolve_tool_names(agent_cfg.available_tools, *catalog);
    }

    std::lock_guard<std::mutex> lock(tool_catalog_mtx_);
    tool_catalog_ = std::move(catalog);
  }

  std::vector<std::string> agent_system::agent_tool_names(const std::string& agent_id) const
  {
    const auto catalog = tool_catalog_snapshot();
    if (!catalog)
    {
      return {};
    }
    const auto it = catalog->per_agent.find(agent_id);
    return it != catalog->per_agent.end() ? it->second : std::vector<std::string>{};
  }

  std::vector<std::string> agent_system::current_agent_tool_names() const
  {
    const auto agent_id = current_agent_id();
    if (!agent_id)
    {
      return {};
    }
    return agent_tool_names(*agent_id);
  }

  slash_command::completion_context agent_system::build_completion_context() const
  {
    slash_command::completion_context context;

    const auto catalog = tool_catalog_snapshot();
    if (!catalog)
    {
      return context;
    }

    for (const auto& [server, tools] : catalog->mcp_by_server)
    {
      context.mcp_servers.push_back({server, ""});
    }

    if (const auto agent_id = current_agent_id())
    {
      const auto it = catalog->per_agent.find(*agent_id);
      if (it != catalog->per_agent.end())
      {
        for (const auto& name : it->second)
        {
          context.agent_tools.push_back({name, ""});
        }
      }

      // The current agent's selectable model ids (/model completion). The
      // active one is marked, so the popup answers "which model am I on"
      // without having to run the command.
      if (has_agent_config(*agent_id))
      {
        const auto& agent_cfg = get_agent_config(*agent_id);
        const std::string active = resolve_model_id(agent_cfg);
        for (const auto& id : model_ids_for(agent_cfg.model))
        {
          context.model_ids.push_back({id, id == active ? "active" : ""});
        }
      }
    }
    return context;
  }

  std::shared_ptr<dripcore::channel<ui_status>> agent_system::ui_status_ch() const
  {
    return ui_status_ch_;
  }

  std::shared_ptr<common::shutdown_signal> agent_system::shutdown_signal() const
  {
    return shutdown_signal_;
  }

  void agent_system::fire_shutdown()
  {
    if (shutdown_signal_)
    {
      shutdown_signal_->fire();
    }
  }

  void agent_system::publish_ui_status()
  {
    if (!ui_status_ch_)
    {
      return;
    }

    ui_status status;
    status.completion = build_completion_context();

    // Resolved model of the current agent, so the frontend footer can
    // follow mid-session /model switches.
    if (const auto agent_id = current_agent_id(); agent_id && has_agent_config(*agent_id))
    {
      status.model_id = resolve_model_id(get_agent_config(*agent_id));
    }

    auto& mcp_mgr = mcp::mcp_manager::instance();
    status.mcp_servers = mcp_mgr.auto_connect_servers();
    if (!status.mcp_servers.empty())
    {
      status.mcp_all_connected = mcp_mgr.auto_connect_all_connected();
      status.mcp_finished = mcp_mgr.auto_connect_finished();
    }

    // The usage object is whatever the API returned, so a non-numeric
    // total_tokens must not throw out of the stats task's coroutine.
    const json stats = get_usage_stats();
    const auto tokens = stats.find("total_tokens");
    if (tokens != stats.end() && tokens->is_number())
    {
      status.total_tokens = tokens->get<int>();
    }

    // A full snapshot supersedes any queued one, so a dropped send is fine.
    ui_status_ch_->try_send(std::move(status));
  }

  void agent_system::set_shutdown_pipe_fd(int fd)
  {
    shutdown_pipe_write_fd_ = fd;
  }

  void agent_system::signal_shutdown()
  {
    if (shutdown_pipe_write_fd_ >= 0)
    {
      // A lost wakeup leaves the signal watcher running, so retry when a
      // signal interrupts the write. Any other failure means the reader has
      // closed the pipe: we are already shutting down.
      char buf = '0';
      while (::write(shutdown_pipe_write_fd_, &buf, 1) < 0 && errno == EINTR)
      {
      }
    }
  }
} // namespace mistral::agent
