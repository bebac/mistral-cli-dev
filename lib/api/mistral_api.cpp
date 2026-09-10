#include "api/mistral_api.h"

namespace mistral::api
{
  std::string build_chat_request(
    const std::string& model,
    const std::vector<message>& messages,
    const json& tools_schema,
    bool stream,
    const std::optional<std::string>& prompt_cache_key,
    const sampling_params& params,
    const std::string& provider)
  {
    json body;
    body["model"]    = model;
    body["stream"]    = stream;

    // Serialize messages based on provider format
    json messages_arr = json::array();
    for (const auto& msg : messages)
    {
      if (provider == "mistral")
      {
        messages_arr.push_back(message_to_json(msg, true));
      }
      else
      {
        messages_arr.push_back(message_to_json_openai_compatible(msg));
      }
    }
    body["messages"] = messages_arr;

    // Apply sampling parameters if provided
    if (params.reasoning_effort) {
      body["reasoning_effort"] = *params.reasoning_effort;
    }
    if (params.temperature) {
      body["temperature"] = *params.temperature;
    }
    if (params.top_p) {
      body["top_p"] = *params.top_p;
    }
    if (params.top_k) {
      body["top_k"] = *params.top_k;
    }
    if (params.max_tokens) {
      body["max_tokens"] = *params.max_tokens;
    }
    if (params.frequency_penalty) {
      body["frequency_penalty"] = *params.frequency_penalty;
    }
    if (params.presence_penalty) {
      body["presence_penalty"] = *params.presence_penalty;
    }
    if (params.seed) {
      body["seed"] = *params.seed;
    }

    // Mistral-specific fields — only sent for the Mistral provider
    if (provider == "mistral")
    {
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

    // OpenAI-compatible providers: request usage in stream
    if (provider != "mistral" && stream)
    {
      body["stream_options"] = {{"include_usage", true}};
    }

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
        if (tc.contains("extra_content"))  {
          call.extra_content = tc["extra_content"];
        }
        if (tc.contains("index") && !tc["index"].is_null()) {
          call.index = tc["index"].get<int>();
        }
        calls.push_back(std::move(call));
      }
      result.tool_calls = std::move(calls);
    }

    return result;
  }
} // namespace mistral::api
