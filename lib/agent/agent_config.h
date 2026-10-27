#ifndef MISTRAL_AGENT_CONFIG_H
#define MISTRAL_AGENT_CONFIG_H

#include "api/agent_api.h"
#ifdef MISTRAL_ENABLE_TREE_SITTER
#include "tools/code_index.h"
#endif

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mistral::agent
{

  // ---- Model Configuration ----
  // Represents a model configuration for a specific provider.
  struct model_config
  {
    std::string provider;           // "mistral", "openai", etc.
    std::string model_id;           // Provider-specific model identifier
    std::string api_key_env;        // Environment variable for API key
    std::string base_url;           // API base URL
    std::string api_version = "v1"; // API version

    // Sampling parameters (defaults)
    std::optional<std::string> reasoning_effort; // e.g., "high", "low", "none"
    std::optional<float> temperature;            // 0.0 to 1.0
    std::optional<float> top_p;                  // 0.0 to 1.0
    std::optional<int> top_k;                    // Mistral-specific
    std::optional<int> max_tokens;               // Maximum tokens to generate
    std::optional<float> frequency_penalty;      // -2.0 to 2.0
    std::optional<float> presence_penalty;       // -2.0 to 2.0
    std::optional<int> seed;                     // Random seed for deterministic output
    std::optional<bool> safe_prompt;             // Mistral-specific
    std::optional<float> randomness;             // Mistral-specific, 0.0 to 1.0
    std::optional<bool> strict_mode;             // Strict tool schema mode (OpenAI-style)
  };

  // ---- Server-side Agent Configuration ----
  // Describes a Mistral Agents API entity (POST /v1/agents). When set on
  // an agent, its task talks to the Conversations API (POST
  // /v1/conversations) with the server-side agent_id instead of POST
  // /v1/chat/completions. The server-side agent owns the model,
  // instructions and builtin tools (e.g. web_search), which execute
  // server-side.
  struct server_agent_config
  {
    std::string name;         // Server-side agent name (required)
    std::string instructions; // Agent instructions (supports file://)
    std::string description;
    std::vector<api::builtin_tool> tools; // Builtin tools, e.g. web_search
    // CompletionArgs (temperature, top_p, random_seed, ...) applied at
    // creation time. AgentsCompletionRequest does not accept
    // temperature/top_p/seed per request.
    std::optional<api::json> completion_args;
  };

  // Metadata key under which the server-side agent's config fingerprint
  // is stored (see server_agent_config_fingerprint).
  inline constexpr std::string_view server_agent_config_hash_key = "mistral-cli-config-hash";

  // Fingerprint of a server_agent config (model, tools, resolved
  // instructions, description, completion_args) as a hex string. Stored
  // in the server-side agent's metadata so that config drift is detected
  // when an existing agent is reused by name. The model is the resolved
  // model id sent in CreateAgentRequest.model.
  std::string server_agent_config_fingerprint(
    const server_agent_config& cfg,
    const std::string& resolved_instructions,
    const std::string& model
  );

  // ---- Agent Configuration ----
  // Represents an agent configuration that references a model by name.
  struct agent_config
  {
    std::string model; // Name of model from models section
    std::string system_prompt;
    std::string tool_policy; // Can be: "deny", "allow", "require_approval", a JSON object as string, or
                             // "file://path/to/policy.json"
    std::vector<std::string> available_tools;

    // Server-side agent entity (Mistral Agents API). When set, the agent
    // task uses the Conversations API (POST /v1/conversations) instead
    // of POST /v1/chat/completions.
    std::optional<server_agent_config> server_agent;

    // Maps a tool name to a subagent (local agent id). Tool calls with
    // these names are routed to the named agent's subagent task instead
    // of being dispatched to the local tool registry.
    std::map<std::string, std::string> subagent_tools;

    // Sampling parameters (overrides model defaults)
    std::optional<std::string> reasoning_effort; // Override model default
    std::optional<float> temperature;            // Override model default
    std::optional<float> top_p;                  // Override model default
    std::optional<int> top_k;                    // Override model default
    std::optional<int> max_tokens;               // Override model default
    std::optional<float> frequency_penalty;      // Override model default
    std::optional<float> presence_penalty;       // Override model default
    std::optional<int> seed;                     // Override model default
    std::optional<bool> safe_prompt;             // Override model default
    std::optional<float> randomness;             // Override model default
    std::optional<bool> strict_mode;             // Override model default
  };

  // ---- Tool Schema Options ----
  // Client-side options controlling tool schema generation.
  // These are NOT API request parameters and are never serialized
  // into request bodies.
  struct tool_schema_options
  {
    bool strict_mode = false; // OpenAI-style strict tool schema mode
  };

  // ---- MCP Server Configuration ----
  // Describes one MCP server. Two transports are supported:
  //
  //   HTTP (Streamable HTTP):
  //     { "url": "http://127.0.0.1:6006/mcp" }
  //
  //   stdio (spawn a child process, JSON-RPC over pipes):
  //     { "command": "xerj", "args": ["mcp"],
  //       "env": { "XERJ_URL": "http://localhost:9200" } }
  //
  // An optional preflight section ensures a dependency is running
  // before the MCP server is connected:
  //
  //   "preflight": {
  //     "check_url": "http://localhost:9200",
  //     "start_command": ["xerj", "--insecure", "--data-dir", "~/xerj-data"],
  //     "post_start_command": ["xerj", "autoindex", "."]
  //   }
  //
  // Preflight flow: HTTP GET check_url; if it fails, spawn
  // start_command as a detached process, poll check_url until it
  // responds (or timeout), then run post_start_command.
  struct mcp_server_config
  {
    // HTTP transport (Streamable HTTP endpoint).
    std::string url;

    // stdio transport (spawn a child process).
    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;

    // Preflight: ensure a dependency is running before connecting.
    std::string check_url;
    std::vector<std::string> start_command;
    std::vector<std::string> post_start_command;

    // Auto-connect on startup (default false). When true, the MCP
    // manager connects this server automatically when the scheduler
    // starts, without requiring /mcp connect.
    bool auto_connect = false;

    bool is_stdio() const
    {
      return !command.empty();
    }
  };

  // ---- Full Configuration ----
  // Represents the complete configuration loaded from mistral-cli.json.
  struct full_config
  {
    std::map<std::string, model_config> models;
    std::map<std::string, agent_config> agents;
#ifdef MISTRAL_ENABLE_TREE_SITTER
    tools::code_index_settings code_index;
#endif
    std::string default_policy = "deny";
    std::map<std::string, mcp_server_config> mcp_servers;
  };

} // namespace mistral::agent

#endif // MISTRAL_AGENT_CONFIG_H
