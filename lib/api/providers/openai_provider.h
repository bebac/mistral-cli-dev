#ifndef API_PROVIDERS_OPENAI_PROVIDER_H
#define API_PROVIDERS_OPENAI_PROVIDER_H

#include "api/provider_base.h"

namespace mistral::api
{
  // Provider implementation for OpenAI-compatible APIs.
  //
  // Uses the reasoning_content field for thinking content instead of
  // Mistral's content-array format. Supports stream_options for usage
  // reporting in streaming responses.
  //
  // Compatible with providers such as GreenPT, DeepSeek, and other
  // OpenAI-compatible endpoints.
  class openai_provider : public provider_base
  {
  public:
    json serialize_message(const message& m, bool include_thinking) const override;

    std::string_view id() const override
    {
      return "openai";
    }

  protected:
    // Add stream_options to request usage reporting in streaming
    // responses.
    void apply_request_fields(
      json& body,
      bool stream,
      const std::optional<std::string>& prompt_cache_key,
      const sampling_params& params
    ) const override;

    // Parse the OpenAI-compatible "reasoning_content" delta field.
    void parse_delta_fields(const json& delta, stream_delta& result) const override;
  };
} // namespace mistral::api

#endif // API_PROVIDERS_OPENAI_PROVIDER_H