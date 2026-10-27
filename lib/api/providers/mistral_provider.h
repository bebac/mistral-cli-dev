#ifndef API_PROVIDERS_MISTRAL_PROVIDER_H
#define API_PROVIDERS_MISTRAL_PROVIDER_H

#include "api/provider_base.h"

namespace mistral::api
{
  // Provider implementation for the Mistral API.
  //
  // Uses Mistral's content-array thinking format and supports
  // provider-specific fields: safe_prompt, randomness, prompt_cache_key.
  class mistral_provider : public provider_base
  {
  public:
    json serialize_message(const message& m, bool include_thinking) const override;

    std::string_view id() const override
    {
      return "mistral";
    }

  protected:
    // Add Mistral-specific request fields: safe_prompt, randomness,
    // and prompt_cache_key.
    void apply_request_fields(
      json& body,
      bool stream,
      const std::optional<std::string>& prompt_cache_key,
      const sampling_params& params
    ) const override;
  };
} // namespace mistral::api

#endif // API_PROVIDERS_MISTRAL_PROVIDER_H