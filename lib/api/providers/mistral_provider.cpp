#include "api/providers/mistral_provider.h"

namespace mistral::api
{
  json mistral_provider::serialize_message(const message& m, bool include_thinking) const
  {
    return message_to_json(m, include_thinking);
  }

  void mistral_provider::apply_request_fields(
    json& body,
    bool stream,
    const std::optional<std::string>& prompt_cache_key,
    const sampling_params& params) const
  {
    (void)stream;

    // Mistral-specific fields
    if (params.safe_prompt) {
      body["safe_prompt"] = *params.safe_prompt;
    }
    if (params.randomness) {
      body["randomness"] = *params.randomness;
    }
    if (prompt_cache_key) {
      body["prompt_cache_key"] = *prompt_cache_key;
    }
  }
} // namespace mistral::api