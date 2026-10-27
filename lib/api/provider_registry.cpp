#include "api/provider_registry.h"
#include "api/providers/mistral_provider.h"
#include "api/providers/openai_provider.h"

namespace mistral::api
{
  std::unique_ptr<provider> provider_registry::create(const std::string& provider_id)
  {
    if (provider_id == "mistral")
    {
      return std::make_unique<mistral_provider>();
    }

    // All non-Mistral providers use the OpenAI-compatible format.
    // This covers "openai", "greenpt", "deepseek", and any other
    // OpenAI-compatible endpoint.
    return std::make_unique<openai_provider>();
  }
} // namespace mistral::api
