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
      result.content = delta["content"].get<std::string>();
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
      result.content = msg["content"].get<std::string>();
    }

    if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
      result.tool_calls = msg["tool_calls"].get<std::vector<tool_call>>();
    }

    return result;
  }

} // namespace mistral::api
