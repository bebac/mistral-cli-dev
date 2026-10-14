#ifndef API_TYPES_H
#define API_TYPES_H

#include <dripcore/channel.h>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace mistral::api
{
  using json = nlohmann::ordered_json;

  // ---- Message content ----

  // Represents the content of a message (thinking or text).
  struct message_content
  {
    std::string type;    // "thinking" or "text"
    std::string content;  // the accumulated content
    std::optional<bool> closed;
  };

  inline void to_json(json& j, const message_content& mc)
  {
    j = {{"type", mc.type}};
    if (mc.type == "text") {
      j["text"] = mc.content;
    } else if (mc.type == "thinking") {
      // For thinking, Mistral API expects: {"type": "thinking", "thinking": [{"type": "text", "text": "..."}]}
      // Build the text_chunk object explicitly
      json text_chunk_obj = {
        {"type", "text"},
        {"text", mc.content}
      };
      j["thinking"] = json::array({text_chunk_obj});
      if (mc.closed) {
        j["closed"] = *mc.closed;
      }
    }
  }

  inline void from_json(const json& j, message_content& mc)
  {
    j.at("type").get_to(mc.type);
    if (mc.type == "text") {
      j.at("text").get_to(mc.content);
    } else if (mc.type == "thinking") {
      // Handle both string format and array format for backward compatibility
      const auto& thinking_json = j["thinking"];
      if (thinking_json.is_string()) {
        mc.content = thinking_json.get<std::string>();
      } else if (thinking_json.is_array() && !thinking_json.empty()) {
        // Extract text from the first (and should be only) text_chunk
        mc.content = thinking_json[0]["text"].get<std::string>();
      }
    }
    if (j.contains("closed") && !j["closed"].is_null()) {
      mc.closed = j["closed"].get<bool>();
    }
  }

  // ---- Tool definition (sent in request) ----

  struct tool_function
  {
    std::string name;
    std::string description;
    json parameters;  // JSON Schema object
  };

  struct tool_def
  {
    std::string type = "function";
    tool_function function;
  };

  inline void to_json(json& j, const tool_function& f)
  {
    j = {{"name", f.name}, {"description", f.description}, {"parameters", f.parameters}};
  }

  inline void to_json(json& j, const tool_def& t)
  {
    j = {{"type", t.type}, {"function", t.function}};
  }

  // ---- Tool call (in assistant response) ----

  struct tool_call_function
  {
    std::string name;
    std::string arguments;  // JSON string
  };

  struct tool_call
  {
    std::string id;
    std::string type = "function";
    tool_call_function function;
    std::optional<json> extra_content; // google, v1beta/openai reqiures this for thought_signature
    std::optional<int> index; // OpenAI-compatible streaming: identifies which tool call a chunk belongs to
  };

  inline void to_json(json& j, const tool_call_function& f)
  {
    j = {{"name", f.name}, {"arguments", f.arguments}};
  }

  inline void to_json(json& j, const tool_call& tc)
  {
    j = {{"id", tc.id}, {"type", tc.type}, {"function", tc.function}};
    if (tc.extra_content) {
      j["extra_content"] = *tc.extra_content;
    }
  }

  inline void from_json(const json& j, tool_call_function& f)
  {
    j.at("name").get_to(f.name);
    j.at("arguments").get_to(f.arguments);
  }

  inline void from_json(const json& j, tool_call& tc)
  {
    j.at("id").get_to(tc.id);
    if (j.contains("type")) j.at("type").get_to(tc.type);
    j.at("function").get_to(tc.function);
    if (j.contains("extra_content")) {
      tc.extra_content = j["extra_content"];
    }
    if (j.contains("index") && !j["index"].is_null()) {
      tc.index = j["index"].get<int>();
    }
  }

  // ---- Chat message ----

  struct message
  {
    std::string role;
    // Content is a list of message_content blocks (thinking or text).
    std::vector<message_content> content;

    // For assistant messages with tool calls.
    std::optional<std::vector<tool_call>> tool_calls;

    // For tool result messages.
    std::optional<std::string> name;          // tool name
    std::optional<std::string> tool_call_id;  // matching call id
  };

  // Primary serialization function with optional thinking inclusion
  inline json message_to_json(const message& m, bool include_thinking = false)
  {
    json j = {{"role", m.role}};

    // Content is always a vector of message_content.
    // Filter out empty text content blocks (but keep thinking blocks and closed blocks).
    std::vector<message_content> non_empty_content;
    for (const auto& mc : m.content) {
      // Keep all thinking blocks (even if empty, they might have closed flag)
      // Keep text blocks only if they have content
      if (mc.type == "thinking" || !mc.content.empty()) {
        non_empty_content.push_back(mc);
      }
    }

    if (!non_empty_content.empty()) {
      std::vector<message_content> content_to_serialize;
      for (const auto& mc : non_empty_content) {
        // Only filter thinking if include_thinking is false
        if (include_thinking || mc.type != "thinking") {
          content_to_serialize.push_back(mc);
        }
      }

      // If there's only one content block and it's text, serialize as plain string for compatibility
      if (content_to_serialize.size() == 1 && content_to_serialize[0].type == "text" && !content_to_serialize[0].closed) {
        j["content"] = content_to_serialize[0].content;
      } else if (!content_to_serialize.empty()) {
        j["content"] = content_to_serialize;
      }
      // If all content was thinking and !include_thinking, don't include content field
    }

    // Tool messages must always carry a content field. Strict servers
    // (e.g. the Mistral API) reject tool messages without one (HTTP 422),
    // e.g. when a tool returned an empty result (such as `ls` on an
    // empty directory).
    if (m.role == "tool" && !j.contains("content")) {
      j["content"] = "";
    }

    if (m.tool_calls && !m.tool_calls->empty()) {
      j["tool_calls"] = *m.tool_calls;
    }
    if (m.name) {
      j["name"] = *m.name;
    }
    if (m.tool_call_id) {
      j["tool_call_id"] = *m.tool_call_id;
    }

    return j;
  }

  // Existing to_json forwards to message_to_json. Including or excluding thinking from
  // api requests can be easily turn on or off.
  inline void to_json(json& j, const message& m)
  {
    j = message_to_json(m, true);
  }

  // Serialize a message in OpenAI-compatible format.
  // Thinking content is extracted into a top-level `reasoning_content` string
  // field instead of being included as content array blocks (Mistral format).
  // This is required for OpenAI-compatible providers (e.g. GreenPT, DeepSeek)
  // that expect reasoning as a separate field, not as content array entries.
  inline json message_to_json_openai_compatible(const message& m)
  {
    json j = {{"role", m.role}};

    // Extract thinking content and text content separately
    std::string reasoning_content;
    std::vector<message_content> text_content;

    for (const auto& mc : m.content)
    {
      if (mc.type == "thinking")
      {
        if (!reasoning_content.empty()) reasoning_content += " ";
        reasoning_content += mc.content;
      }
      else if (!mc.content.empty())
      {
        text_content.push_back(mc);
      }
    }

    // Set content
    if (!text_content.empty())
    {
      if (text_content.size() == 1 && text_content[0].type == "text")
      {
        j["content"] = text_content[0].content;
      }
      else
      {
        json content_arr = json::array();
        for (const auto& mc : text_content)
        {
          json block = {{"type", mc.type}};
          if (mc.type == "text")
          {
            block["text"] = mc.content;
          }
          content_arr.push_back(block);
        }
        j["content"] = content_arr;
      }
    }
    else
    {
      // Always emit a content field. Strict OpenAI-compatible servers
      // reject tool messages without one (e.g. a tool that returned an
      // empty result). An empty string is universally accepted for tool
      // messages; null matches what OpenAI emits for contentless
      // assistant messages.
      if (m.role == "tool") {
        j["content"] = "";
      } else {
        j["content"] = json(nullptr);
      }
    }

    // Add reasoning_content as top-level field for assistant messages
    if (m.role == "assistant" && !reasoning_content.empty())
    {
      j["reasoning_content"] = reasoning_content;
    }

    if (m.tool_calls && !m.tool_calls->empty())
    {
      j["tool_calls"] = *m.tool_calls;
    }
    if (m.name)
    {
      j["name"] = *m.name;
    }
    if (m.tool_call_id)
    {
      j["tool_call_id"] = *m.tool_call_id;
    }

    return j;
  }

  // ---- HTTP-level request/response types (for channel transport) ----

  enum class event_type { data, error, done };

  struct api_event
  {
    event_type type;
    std::string payload;  // JSON string for data, error message for error

    // HTTP status code when type == error and the error originated from an
    // HTTP response (e.g. 503, 502, 429). Absent for connection-level errors,
    // parse errors, and non-error events. Used by is_retryable_error() to
    // classify errors without fragile string matching.
    std::optional<int> http_status;
  };

  struct api_request
  {
    std::string method = "POST";
    std::string path   = "/v1/chat/completions";
    std::string body;
    bool stream = true;
    std::shared_ptr<dripcore::channel<api::api_event>> response_ch;
  };

  struct api_response
  {
    int status_code = 0;
    std::string body;
    bool done = false;

    void reset()
    {
      status_code = 0;
      body.clear();
      done = false;
    }
  };
} // namespace mistral::api

#endif // API_TYPES_H