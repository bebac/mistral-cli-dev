#ifndef API_PROVIDER_H
#define API_PROVIDER_H

#include "api/chat_types.h"
#include "api/types.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mistral::api
{
  // Abstract interface for provider-specific API behavior.
  //
  // Each provider implementation encapsulates the differences in:
  //   - Message serialization (e.g. Mistral content-array thinking vs
  //     OpenAI-compatible reasoning_content field)
  //   - Request body construction (provider-specific fields like
  //     safe_prompt, randomness, stream_options)
  //   - Stream delta parsing (e.g. content array vs reasoning_content)
  //
  // Concrete implementations are created via the provider_registry and
  // used by agent_task_base to build requests and parse responses.
  class provider
  {
  public:
    virtual ~provider() = default;

    // Serialize a single message into provider-specific JSON format.
    // include_thinking controls whether thinking content blocks are
    // included in the serialized output.
    virtual json serialize_message(const message& m, bool include_thinking) const = 0;

    // Build the full JSON request body for a chat completion request.
    // Returns the serialized JSON as a string.
    virtual std::string build_chat_request(
      const std::string& model,
      const std::vector<message>& messages,
      const json& tools_schema,
      bool stream,
      const std::optional<std::string>& prompt_cache_key,
      const sampling_params& params
    ) const = 0;

    // Parse a single SSE data JSON chunk into a stream_delta.
    virtual stream_delta parse_stream_delta(const json& j) const = 0;

    // Provider identifier (e.g. "mistral", "openai").
    virtual std::string_view id() const = 0;
  };
} // namespace mistral::api

#endif // API_PROVIDER_H
