#ifndef MISTRAL_API_H
#define MISTRAL_API_H

#include "api/mistral_types.h"

#include <string>
#include <variant>
#include <vector>

namespace mistral::api
{
  // Build the JSON body for POST /v1/chat/completions.
  std::string build_chat_request(
    const std::string& model,
    const std::vector<message>& messages,
    const json& tools_schema,
    bool stream = true,
    const std::optional<std::string>& prompt_cache_key = std::nullopt
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

#if 0
  chat_response parse_response(const json& j);
#endif

} // namespace mistral::api

#endif // MISTRAL_API_H
