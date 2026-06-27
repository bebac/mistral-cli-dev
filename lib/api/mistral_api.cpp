#include "api/mistral_api.h"

namespace mistral::api
{
  std::string build_chat_request(
    const std::string& model,
    const std::vector<message>& messages,
    const json& tools_schema,
    bool stream)
  {
    json body;
    body["model"]    = model;
    body["messages"]  = messages;
    body["stream"]    = stream;
    //body["frequency_penalty"] = -0.02f;
    body["reasoning_effort"] = "high";
    //body["max_tokens"] = 5000;
    //body["temperature"] = 0.9f;

    if (!tools_schema.empty()) {
      body["tools"]       = tools_schema;
      body["tool_choice"] = "auto";
    }

    return body.dump();
  }

  stream_delta parse_stream_delta(const json& j)
  {
    stream_delta result;

    if (!j.contains("choices") || j["choices"].empty()) {
      return result;
    }

    const auto& choice = j["choices"][0];

    if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
      result.finish_reason = choice["finish_reason"].get<std::string>();
    }

    if (!choice.contains("delta")) {
      return result;
    }

    const auto& delta = choice["delta"];

    if (delta.contains("content") && !delta["content"].is_null()) {
      const auto& content_json = delta["content"];

      // Handle string content (legacy or answer phase).
      if (content_json.is_string()) {
        std::string text = content_json.get<std::string>();
        if (!text.empty()) {
          message_content mc;
          mc.type = "text";
          mc.content = std::move(text);
          result.content.push_back(std::move(mc));
        }
      }
      // Handle array content (reasoning chunks).
      else if (content_json.is_array()) {
        for (const auto& chunk_json : content_json) {
          message_content mc;
          mc.type = chunk_json["type"].get<std::string>();

          if (mc.type == "thinking") {
            if (chunk_json.contains("closed") && !chunk_json["closed"].is_null()) {
              mc.closed = chunk_json["closed"].get<bool>();
            }
            // Extract thinking text - can be a string or array of text_chunks
            if (chunk_json.contains("thinking") && !chunk_json["thinking"].is_null()) {
              const auto& thinking_json = chunk_json["thinking"];
              if (thinking_json.is_string()) {
                mc.content = thinking_json.get<std::string>();
              } else if (thinking_json.is_array()) {
                // Handle array format: [{"type": "text", "text": "..."}]
                // Accumulate all text chunks into a single string
                for (const auto& text_chunk_json : thinking_json) {
                  if (text_chunk_json.contains("text") && !text_chunk_json["text"].is_null()) {
                    if (!mc.content.empty()) mc.content += " ";
                    mc.content += text_chunk_json["text"].get<std::string>();
                  }
                }
              }
            }
            // Always add thinking blocks, even if content is empty (they might have closed flag)
            result.content.push_back(std::move(mc));
          } else if (mc.type == "text") {
            if (chunk_json.contains("text") && !chunk_json["text"].is_null()) {
              mc.content = chunk_json["text"].get<std::string>();
            }
            // Only add text blocks if they have content
            if (!mc.content.empty()) {
              result.content.push_back(std::move(mc));
            }
          }
        }
      }
    }

    if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
      std::vector<tool_call> calls;
      for (const auto& tc : delta["tool_calls"]) {
        tool_call call;
        if (tc.contains("id") && !tc["id"].is_null()) {
          call.id = tc["id"].get<std::string>();
        }
        if (tc.contains("type")) {
          call.type = tc["type"].get<std::string>();
        }
        if (tc.contains("function")) {
          const auto& fn = tc["function"];
          if (fn.contains("name") && !fn["name"].is_null()) {
            call.function.name = fn["name"].get<std::string>();
          }
          if (fn.contains("arguments") && !fn["arguments"].is_null()) {
            call.function.arguments = fn["arguments"].get<std::string>();
          }
        }
        calls.push_back(std::move(call));
      }
      result.tool_calls = std::move(calls);
    }

    return result;
  }

#if 0
  chat_response parse_response(const json& j)
  {
    chat_response result;

    if (!j.contains("choices") || j["choices"].empty()) {
      return result;
    }

    const auto& choice = j["choices"][0];

    if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
      result.finish_reason = choice["finish_reason"].get<std::string>();
    }

    if (!choice.contains("message")) {
      return result;
    }

    const auto& msg = choice["message"];

    if (msg.contains("content") && !msg["content"].is_null()) {
      const auto& content_json = msg["content"];

      // Handle string content.
      if (content_json.is_string()) {
        result.content = content_json.get<std::string>();
      }
      // Handle array content (reasoning chunks).
      else if (content_json.is_array()) {
        std::vector<content_chunk> chunks;
        for (const auto& chunk_json : content_json) {
          content_chunk chunk;
          chunk.type = chunk_json["type"].get<std::string>();

          if (chunk.type == "thinking") {
            if (chunk_json.contains("closed") && !chunk_json["closed"].is_null()) {
              chunk.closed = chunk_json["closed"].get<bool>();
            }
            if (chunk_json.contains("thinking") && !chunk_json["thinking"].is_null()) {
              chunk.thinking = chunk_json["thinking"].get<std::vector<text_chunk>>();
            }
          } else if (chunk.type == "text") {
            if (chunk_json.contains("text") && !chunk_json["text"].is_null()) {
              chunk.text = chunk_json["text"].get<std::string>();
            }
          }
          chunks.push_back(std::move(chunk));
        }
        result.content = std::move(chunks);
      }
    }

    if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
      result.tool_calls = msg["tool_calls"].get<std::vector<tool_call>>();
    }

    return result;
  }
#endif

} // namespace mistral::api
