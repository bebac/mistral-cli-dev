#ifndef API_PROVIDER_REGISTRY_H
#define API_PROVIDER_REGISTRY_H

#include "api/provider.h"

#include <memory>
#include <string>

namespace mistral::api
{
  // Factory for creating provider instances from a provider identifier string.
  //
  // Maps the "provider" field from model_config to a concrete provider
  // implementation. Unknown provider strings default to the OpenAI-compatible
  // provider for backward compatibility.
  class provider_registry
  {
  public:
    // Create a provider instance from a provider identifier string.
    // Returns an OpenAI-compatible provider for unrecognized strings
    // (backward compat).
    static std::unique_ptr<provider> create(const std::string& provider_id);
  };
} // namespace mistral::api

#endif // API_PROVIDER_REGISTRY_H