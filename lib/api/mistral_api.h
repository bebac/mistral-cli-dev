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
    bool stream = true
  );

  // Parsed content from a streaming delta chunk.
  struct stream_delta
  {
    // Content can be:
    // - A plain string (when reasoning_effort="none" or during answer phase)
    // - A vector of content_chunk (when reasoning_effort="high" and in thinking phase)
    std::variant<std::string, std::vector<content_chunk>> content;
    std::optional<std::vector<tool_call>> tool_calls;  // Incremental tool calls
    std::string finish_reason;                  // "stop", "tool_calls", or empty
  };

  // Parse a single SSE data JSON into a stream_delta.
  stream_delta parse_stream_delta(const json& j);

  // Parse a non-streaming response into message content + tool calls.
  struct chat_response
  {
    std::variant<std::string, std::vector<content_chunk>> content;
    std::vector<tool_call> tool_calls;
    std::string finish_reason;
  };

#if 0
  chat_response parse_response(const json& j);
#endif

} // namespace mistral::api

#endif // MISTRAL_API_H
