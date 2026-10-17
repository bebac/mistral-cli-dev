#ifndef API_AGENT_API_H
#define API_AGENT_API_H

#include "api/types.h"

#include <optional>
#include <string>
#include <vector>

namespace mistral::api
{
  // ---- Builtin (server-side) tools ----

  // A builtin tool reference for a server-side agent entity, e.g.
  // {"type": "web_search"} or
  // {"type": "web_search_premium", "tool_configuration": {...}}.
  // See WebSearchTool / WebSearchPremiumTool in mistral-openapi.yaml.
  // Builtin tools execute server-side; the client never sees tool calls
  // for them.
  struct builtin_tool
  {
    std::string type;  // "web_search", "web_search_premium", "code_interpreter", ...
    std::optional<json> tool_configuration;
  };

  inline void to_json(json& j, const builtin_tool& t)
  {
    j = {{"type", t.type}};
    if (t.tool_configuration) {
      j["tool_configuration"] = *t.tool_configuration;
    }
  }

  // ---- CreateAgentRequest (POST /v1/agents) ----

  // Required fields per the OpenAPI spec: model, name. The endpoint has
  // additionalProperties: false — do not add fields that are not in the
  // spec.
  struct create_agent_request
  {
    std::string model;
    std::string name;
    std::optional<std::string> instructions;
    std::optional<std::string> description;
    std::vector<builtin_tool> tools;
    // CompletionArgs (temperature, top_p, random_seed, ...) applied to
    // the server-side agent at creation time. AgentsCompletionRequest
    // does not accept temperature/top_p/seed per request.
    std::optional<json> completion_args;
    std::optional<json> metadata;
  };

  inline void to_json(json& j, const create_agent_request& req)
  {
    j = {
      {"model", req.model},
      {"name", req.name},
    };
    if (req.instructions) j["instructions"] = *req.instructions;
    if (req.description) j["description"] = *req.description;
    if (!req.tools.empty()) j["tools"] = req.tools;
    if (req.completion_args) j["completion_args"] = *req.completion_args;
    if (req.metadata) j["metadata"] = *req.metadata;
  }

  // ---- Agent entity (response of POST /v1/agents, GET /v1/agents/...) ----

  // Minimal view of the Agent schema — only the fields mistral-cli uses.
  struct agent_entity
  {
    std::string id;
    std::string name;
    std::optional<std::string> description;
    std::optional<std::string> instructions;
    std::optional<json> metadata;
    int version = 0;
  };

  inline agent_entity agent_entity_from_json(const json& j)
  {
    agent_entity entity;
    if (j.contains("id") && j["id"].is_string()) {
      entity.id = j["id"].get<std::string>();
    }
    if (j.contains("name") && j["name"].is_string()) {
      entity.name = j["name"].get<std::string>();
    }
    if (j.contains("description") && j["description"].is_string()) {
      entity.description = j["description"].get<std::string>();
    }
    if (j.contains("instructions") && j["instructions"].is_string()) {
      entity.instructions = j["instructions"].get<std::string>();
    }
    if (j.contains("metadata") && j["metadata"].is_object()) {
      entity.metadata = j["metadata"];
    }
    if (j.contains("version") && j["version"].is_number_integer()) {
      entity.version = j["version"].get<int>();
    }
    return entity;
  }
} // namespace mistral::api

#endif // API_AGENT_API_H
