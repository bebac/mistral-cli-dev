#ifndef API_PROVIDERS_MISTRAL_PROVIDER_H
#define API_PROVIDERS_MISTRAL_PROVIDER_H

#include "api/provider.h"

namespace mistral::api
{
  // Provider implementation for the Mistral API.
  //
  // Uses Mistral's content-array thinking format and supports
  // provider-specific fields: safe_prompt, randomness, prompt_cache_key.
  class mistral_provider : public provider
  {
  public:
    json serialize_message(const message& m, bool include_thinking) const override;

    std::string build_chat_request(
      const std::string& model,
      const std::vector<message>& messages,
      const json& tools_schema,
      bool stream,
      const std::optional<std::string>& prompt_cache_key,
      const sampling_params& params) const override;

    stream_delta parse_stream_delta(const json& j) const override;

    std::string_view id() const override { return "mistral"; }
  };
} // namespace mistral::api

#endif // API_PROVIDERS_MISTRAL_PROVIDER_H