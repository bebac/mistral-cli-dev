#include "api/providers/openai_provider.h"

namespace mistral::api
{
  json openai_provider::serialize_message(const message& m, bool include_thinking) const
  {
    // OpenAI-compatible format always uses reasoning_content for thinking,
    // so include_thinking has no effect here.
    (void)include_thinking;
    return message_to_json_openai_compatible(m);
  }

  void openai_provider::apply_request_fields(
    json& body,
    bool stream,
    const std::optional<std::string>& prompt_cache_key,
    const sampling_params& params) const
  {
    (void)prompt_cache_key;
    (void)params;

    // OpenAI-compatible providers: request usage in stream
    if (stream) {
      body["stream_options"] = {{"include_usage", true}};
    }
  }

  void openai_provider::parse_delta_fields(const json& delta, stream_delta& result) const
  {
    // Handle OpenAI-compatible "reasoning_content" field (e.g. GreenPT / GLM).
    // Some providers send thinking/reasoning as a separate top-level string
    // field in the delta, rather than inside the content array.
    if (delta.contains("reasoning_content") && !delta["reasoning_content"].is_null()) {
      const auto& reasoning_json = delta["reasoning_content"];
      if (reasoning_json.is_string()) {
        std::string text = reasoning_json.get<std::string>();
        if (!text.empty()) {
          message_content mc;
          mc.type = "thinking";
          mc.content = std::move(text);
          result.content.push_back(std::move(mc));
        }
      }
    }
  }
} // namespace mistral::api