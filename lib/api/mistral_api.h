#ifndef MISTRAL_API_H
#define MISTRAL_API_H

#include "api/mistral_types.h"

#include <string>
#include <variant>
#include <vector>

namespace mistral::api
{
  // Sampling parameters for chat completion requests
  struct sampling_params
  {
    std::optional<std::string> reasoning_effort;
    std::optional<float> temperature;
    std::optional<float> top_p;
    std::optional<int> top_k;
    std::optional<int> max_tokens;
    std::optional<float> frequency_penalty;
    std::optional<float> presence_penalty;
    std::optional<int> seed;
    std::optional<bool> safe_prompt;
    std::optional<float> randomness;
  };

  // Build the JSON body for POST /v1/chat/completions.
  std::string build_chat_request(
    const std::string& model,
    const std::vector<message>& messages,
    const json& tools_schema,
    bool stream = true,
    const std::optional<std::string>& prompt_cache_key = std::nullopt,
    const sampling_params& params = {}
  );

  // Parsed content from a streaming delta chunk.
  struct stream_delta
  {
    // Content is a vector of message_content blocks.
    // Each block can be "thinking" or "text" type.
    std::vector<message_content> content;
    std::optional<std::vector<tool_call>> tool_calls;  // Incremental tool calls
    std::string finish_reason;                  // "stop", "tool_calls", or empty
  };

  // Parse a single SSE data JSON into a stream_delta.
  stream_delta parse_stream_delta(const json& j);

  // Parse a non-streaming response into message content + tool calls.
  struct chat_response
  {
    std::vector<message_content> content;
    std::vector<tool_call> tool_calls;
    std::string finish_reason;
  };
} // namespace mistral::api

#endif // MISTRAL_API_H
