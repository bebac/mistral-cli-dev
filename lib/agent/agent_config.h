#ifndef MISTRAL_AGENT_CONFIG_H
#define MISTRAL_AGENT_CONFIG_H

#include <optional>
#include <string>
#include <vector>
#include <map>

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
    std::optional<std::string> reasoning_effort;  // e.g., "high", "low", "none"
    std::optional<float> temperature;             // 0.0 to 1.0
    std::optional<float> top_p;                   // 0.0 to 1.0
    std::optional<int> top_k;                     // Mistral-specific
    std::optional<int> max_tokens;                // Maximum tokens to generate
    std::optional<float> frequency_penalty;       // -2.0 to 2.0
    std::optional<float> presence_penalty;        // -2.0 to 2.0
    std::optional<int> seed;                      // Random seed for deterministic output
    std::optional<bool> safe_prompt;              // Mistral-specific
    std::optional<float> randomness;              // Mistral-specific, 0.0 to 1.0
    std::optional<bool> strict_mode;              // Strict tool schema mode (OpenAI-style)
  };

  // ---- Agent Configuration ----
  // Represents an agent configuration that references a model by name.
  struct agent_config
  {
    std::string model;              // Name of model from models section
    std::string system_prompt;
    std::string tool_policy;        // Can be: "deny", "allow", "require_approval", a JSON object as string, or "file://path/to/policy.json"
    std::vector<std::string> available_tools;

    // Sampling parameters (overrides model defaults)
    std::optional<std::string> reasoning_effort;  // Override model default
    std::optional<float> temperature;             // Override model default
    std::optional<float> top_p;                   // Override model default
    std::optional<int> top_k;                     // Override model default
    std::optional<int> max_tokens;                // Override model default
    std::optional<float> frequency_penalty;       // Override model default
    std::optional<float> presence_penalty;        // Override model default
    std::optional<int> seed;                      // Override model default
    std::optional<bool> safe_prompt;              // Override model default
    std::optional<float> randomness;              // Override model default
    std::optional<bool> strict_mode;              // Override model default
  };

  // ---- Tool Schema Options ----
  // Client-side options controlling tool schema generation.
  // These are NOT API request parameters and are never serialized
  // into request bodies.
  struct tool_schema_options
  {
    bool strict_mode = false;  // OpenAI-style strict tool schema mode
  };

  // ---- Full Configuration ----
  // Represents the complete configuration loaded from mistral-cli.json.
  struct full_config
  {
    std::map<std::string, model_config> models;
    std::map<std::string, agent_config> agents;
    std::string default_policy = "deny";
  };

} // namespace mistral::agent

#endif // MISTRAL_AGENT_CONFIG_H
