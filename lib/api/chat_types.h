#ifndef API_CHAT_TYPES_H
#define API_CHAT_TYPES_H

#include "api/types.h"

#include <optional>
#include <string>
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

  // Parsed content from a streaming delta chunk.
  struct stream_delta
  {
    // Content is a vector of message_content blocks.
    // Each block can be "thinking" or "text" type.
    std::vector<message_content> content;
    std::optional<std::vector<tool_call>> tool_calls;  // Incremental tool calls
    std::string finish_reason;                  // "stop", "tool_calls", or empty
  };

  // Parse a non-streaming response into message content + tool calls.
  struct chat_response
  {
    std::vector<message_content> content;
    std::vector<tool_call> tool_calls;
    std::string finish_reason;
  };
} // namespace mistral::api

#endif // API_CHAT_TYPES_H