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
    body["reasoning_effort"] = "high";
    body["max_tokens"] = 5000;
    body["temperature"] =0.5f;

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
