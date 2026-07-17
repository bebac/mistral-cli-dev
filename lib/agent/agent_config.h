#ifndef MISTRAL_AGENT_CONFIG_H
#define MISTRAL_AGENT_CONFIG_H

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

    // Future: sampling parameters defaults, timeouts, etc.
  };

  // ---- Agent Configuration ----
  // Represents an agent configuration that references a model by name.
  struct agent_config
  {
    std::string model;              // Name of model from models section
    std::string system_prompt;
    std::string tool_policy;        // "deny", "allow", "require_approval"
    std::vector<std::string> allowed_tools;

    // Future: sampling parameters override, temperature, top_p, etc.
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