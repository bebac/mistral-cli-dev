#ifndef MISTRAL_API_TYPES_H
#define MISTRAL_API_TYPES_H

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace mistral::api
{
  using json = nlohmann::ordered_json;

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
  };

  inline void to_json(json& j, const tool_call_function& f)
  {
    j = {{"name", f.name}, {"arguments", f.arguments}};
  }

  inline void to_json(json& j, const tool_call& tc)
  {
    j = {{"id", tc.id}, {"type", tc.type}, {"function", tc.function}};
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
  }

  // ---- Chat message ----

  struct message
  {
    std::string role;
    std::string content;

    // For assistant messages with tool calls.
    std::optional<std::vector<tool_call>> tool_calls;

    // For tool result messages.
    std::optional<std::string> name;          // tool name
    std::optional<std::string> tool_call_id;  // matching call id
  };

  inline void to_json(json& j, const message& m)
  {
    j = {{"role", m.role}, {"content", m.content}};

    if (m.tool_calls && !m.tool_calls->empty()) {
      j["tool_calls"] = *m.tool_calls;
    }
    if (m.name) {
      j["name"] = *m.name;
    }
    if (m.tool_call_id) {
      j["tool_call_id"] = *m.tool_call_id;
    }
  }

  // ---- HTTP-level request/response types (for channel transport) ----

  struct api_request
  {
    std::string method = "POST";
    std::string path   = "/v1/chat/completions";
    std::string body;
    bool stream = true;
  };

  enum class event_type { data, error, done };

  struct api_event
  {
    event_type type;
    std::string payload;  // JSON string for data, error message for error
  };

} // namespace mistral::api

#endif // MISTRAL_API_TYPES_H
