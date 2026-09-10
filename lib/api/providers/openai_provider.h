#ifndef API_PROVIDERS_OPENAI_PROVIDER_H
#define API_PROVIDERS_OPENAI_PROVIDER_H

#include "api/provider.h"

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
  class openai_provider : public provider
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

    std::string_view id() const override { return "openai"; }
  };
} // namespace mistral::api

#endif // API_PROVIDERS_OPENAI_PROVIDER_H