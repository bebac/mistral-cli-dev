#include "agent/server_agent_task.h"
#include "agent/agent_system.h"
#include "api/agent_api.h"

#include <cctype>
#include <format>

namespace mistral::agent
{
  namespace
  {
    // Percent-encode a query parameter value (RFC 3986 unreserved
    // characters are kept as-is).
    std::string url_encode_query(const std::string& value)
    {
      std::string out;
      out.reserve(value.size());
      for (unsigned char c : value)
      {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        {
          out.push_back(static_cast<char>(c));
        }
        else
        {
          out += std::format("%{:02X}", c);
        }
      }
      return out;
    }
  } // namespace

  std::string server_agent_task::chat_request_path() const
  {
    // The Conversations API is required for builtin tools like
    // web_search: Chat Completions responses (what /v1/agents/completions
    // returns) don't include the search result references these tools
    // produce, so the completions endpoint rejects the connector with
    // "invalid_tools". The Conversations API returns tool_reference
    // citation chunks and connector usage instead.
    return "/" + api_version_ + "/conversations";
  }

  bool server_agent_task::request_stream() const
  {
    // Non-streaming: the full ConversationResponse arrives as one event.
    return false;
  }

  std::string server_agent_task::build_request_body(const api::json& /*tools_schema*/) const
  {
    // The server-side agent owns the model, instructions and builtin
    // tools. inputs carries the conversation text; store=false keeps the
    // conversation ephemeral (each subagent call is independent).
    api::json body;
    body["inputs"] = conversation_text();
    body["agent_id"] = server_agent_id_;
    body["stream"] = false;
    body["store"] = false;
    return body.dump();
  }

  std::string server_agent_task::conversation_text() const
  {
    std::string text;
    for (const auto& msg : chat_.messages())
    {
      if (msg.role == "system") continue;  // instructions live server-side
      for (const auto& mc : msg.content)
      {
        if (mc.type != "text" || mc.content.empty()) continue;
        if (!text.empty()) text += "\n\n";
        text += mc.content;
      }
    }
    return text;
  }

  api::stream_delta server_agent_task::parse_response_delta(const api::json& j) const
  {
    // ConversationResponse: outputs[] entries. message.output entries
    // carry the answer text plus tool_reference citation chunks.
    api::stream_delta delta;
    if (!j.contains("outputs") || !j["outputs"].is_array()) {
      return delta;
    }

    std::string text;
    std::vector<std::pair<std::string, std::string>> sources;  // (title, url)
    for (const auto& entry : j["outputs"])
    {
      const auto entry_type =
        entry.contains("type") && entry["type"].is_string()
          ? entry["type"].get<std::string>() : std::string();
      if (entry_type != "message.output") continue;
      if (!entry.contains("content")) continue;

      const auto& content = entry["content"];
      if (content.is_string())
      {
        text += content.get<std::string>();
      }
      else if (content.is_array())
      {
        for (const auto& chunk : content)
        {
          const auto chunk_type =
            chunk.contains("type") && chunk["type"].is_string()
              ? chunk["type"].get<std::string>() : std::string();
          if (chunk_type == "text")
          {
            if (chunk.contains("text") && chunk["text"].is_string()) {
              text += chunk["text"].get<std::string>();
            }
          }
          else if (chunk_type == "tool_reference")
          {
            std::string title;
            std::string url;
            if (chunk.contains("title") && chunk["title"].is_string()) {
              title = chunk["title"].get<std::string>();
            }
            if (chunk.contains("url") && chunk["url"].is_string()) {
              url = chunk["url"].get<std::string>();
            }
            if (!title.empty()) {
              // The remote agent emits one tool_reference chunk per
              // citation occurrence, so the same source can appear
              // several times; keep the first occurrence only.
              bool seen = false;
              for (const auto& [t, u] : sources) {
                if (t == title && u == url) {
                  seen = true;
                  break;
                }
              }
              if (!seen) {
                sources.emplace_back(std::move(title), std::move(url));
              }
            }
          }
        }
      }
    }

    // Append citations as a markdown source list.
    if (!sources.empty())
    {
      text += "\n\nSources:";
      for (const auto& [title, url] : sources)
      {
        if (!url.empty()) {
          text += "\n- [" + title + "](" + url + ")";
        } else {
          text += "\n- " + title;
        }
      }
    }

    if (!text.empty())
    {
      delta.content.push_back(api::message_content{
        .type = "text",
        .content = std::move(text)
      });
    }
    delta.finish_reason = "stop";
    return delta;
  }

  auto server_agent_task::agent_loop() -> dripcore::awaitable<void>
  {
    if (!server_agent_resolved_)
    {
      if (!co_await ensure_server_agent())
      {
        // Surface the setup failure through the standard subagent error
        // path: agent_loop_on_error records it, agent_loop_on_completed
        // sends it to the caller (run_subagent).
        std::string error =
          "could not set up server-side agent for '" + agent_id_ + "'";
        if (!setup_error_.empty()) {
          error += ": " + setup_error_;
        }
        agent_loop_on_error(error);
        std::vector<api::message_content> no_content;
        agent_loop_on_completed(no_content);
        co_return;
      }
      server_agent_resolved_ = true;
    }
    co_await subagent_task::agent_loop();
  }

  std::string server_agent_task::config_fingerprint() const
  {
    auto& sys = agent_system::instance();
    const auto& agent_cfg = sys.get_agent_config(agent_id_);
    return server_agent_config_fingerprint(
      *agent_cfg.server_agent,
      sys.resolve_server_agent_instructions(agent_cfg),
      model_);
  }

  auto server_agent_task::ensure_server_agent() -> dripcore::awaitable<bool>
  {
    auto& sys = agent_system::instance();

    // Reuse the id resolved earlier in this session.
    if (auto cached = sys.get_server_agent_id(agent_id_))
    {
      server_agent_id_ = *cached;
      co_return true;
    }

    const auto& agent_cfg = sys.get_agent_config(agent_id_);
    if (!agent_cfg.server_agent)
    {
      co_return false;
    }

    // The Agents API is Mistral-only.
    const auto& model_cfg = sys.get_model_config(agent_cfg.model);
    if (model_cfg.provider != "mistral")
    {
      setup_error_ = "provider '" + model_cfg.provider +
        "' does not support the Mistral Agents API";
      co_return false;
    }

    // Reuse an existing server-side agent with the same name (created by
    // a previous session), updating it in place when the local config
    // drifted; otherwise create a new one.
    if (auto found = co_await find_server_agent_by_name(agent_cfg.server_agent->name))
    {
      server_agent_id_ = found->id;

      // The config fingerprint is stored in the agent's metadata at
      // creation/update time. A mismatch means the local config changed
      // (e.g. the builtin tool type or the model) — update the agent so
      // the change takes effect without deleting anything server-side.
      std::string found_fp;
      if (found->metadata && found->metadata->contains(server_agent_config_hash_key) &&
          (*found->metadata)[server_agent_config_hash_key].is_string())
      {
        found_fp = (*found->metadata)[server_agent_config_hash_key].get<std::string>();
      }
      if (found_fp != config_fingerprint())
      {
        if (!co_await update_server_agent(found->id))
        {
          co_return false;
        }
      }
    }
    else if (auto created = co_await create_server_agent())
    {
      server_agent_id_ = *created;
    }
    else
    {
      co_return false;
    }

    sys.set_server_agent_id(agent_id_, server_agent_id_);
    co_return true;
  }

  auto server_agent_task::find_server_agent_by_name(const std::string& name)
    -> dripcore::awaitable<std::optional<api::agent_entity>>
  {
    auto response_ch = std::make_shared<dripcore::channel<api::api_event>>(4);
    agent_system::instance().set_current_response_ch(response_ch);

    std::string path =
      "/" + api_version_ + "/agents/pages?name=" + url_encode_query(name);
    request_ch_->try_send(
      {.method = "GET", .path = path, .body = "", .stream = false, .response_ch = response_ch});

    std::optional<api::agent_entity> result;
    while (true)
    {
      auto evt = co_await response_ch->recv(this);
      if (!evt) break;  // Channel closed (cancelled).
      if (evt->type == api::event_type::done) break;
      if (evt->type == api::event_type::error)
      {
        // A failed lookup is treated as "not found" — creation is tried
        // next (and its error is surfaced if creation also fails).
        break;
      }

      // Non-streaming response: the full body arrives as one data event.
      try
      {
        auto j = api::json::parse(evt->payload);
        if (j.contains("data") && j["data"].is_array())
        {
          for (const auto& agent_json : j["data"])
          {
            // The name filter may be fuzzy — match exactly.
            if (agent_json.value("name", "") == name)
            {
              result = api::agent_entity_from_json(agent_json);
              break;
            }
          }
        }
      }
      catch (const std::exception&)
      {
        // Malformed page: treated as "not found".
      }
    }

    agent_system::instance().clear_current_response_ch();
    co_return result;
  }

  auto server_agent_task::create_server_agent()
    -> dripcore::awaitable<std::optional<std::string>>
  {
    auto& sys = agent_system::instance();
    const auto& agent_cfg = sys.get_agent_config(agent_id_);
    const auto& server_cfg = *agent_cfg.server_agent;

    api::create_agent_request req;
    req.model = model_;
    req.name = server_cfg.name;
    req.description = server_cfg.description;
    req.instructions = sys.resolve_server_agent_instructions(agent_cfg);
    req.tools = server_cfg.tools;
    req.completion_args = server_cfg.completion_args;
    req.metadata = api::json{
      {std::string(server_agent_config_hash_key), config_fingerprint()}
    };

    auto response_ch = std::make_shared<dripcore::channel<api::api_event>>(4);
    agent_system::instance().set_current_response_ch(response_ch);

    std::string path = "/" + api_version_ + "/agents";
    request_ch_->try_send(
      {.method = "POST", .path = path, .body = api::json(req).dump(), .stream = false, .response_ch = response_ch});

    std::optional<std::string> result;
    while (true)
    {
      auto evt = co_await response_ch->recv(this);
      if (!evt) break;  // Channel closed (cancelled).
      if (evt->type == api::event_type::done) break;
      if (evt->type == api::event_type::error)
      {
        setup_error_ = evt->payload;
        break;
      }

      try
      {
        auto entity = api::agent_entity_from_json(api::json::parse(evt->payload));
        if (!entity.id.empty())
        {
          result = entity.id;
        }
      }
      catch (const std::exception& e)
      {
        setup_error_ = std::string("malformed create response: ") + e.what();
      }
    }

    agent_system::instance().clear_current_response_ch();
    co_return result;
  }

  auto server_agent_task::update_server_agent(const std::string& agent_id)
    -> dripcore::awaitable<bool>
  {
    auto& sys = agent_system::instance();
    const auto& agent_cfg = sys.get_agent_config(agent_id_);
    const auto& server_cfg = *agent_cfg.server_agent;

    // UpdateAgentRequest: all fields optional. The update creates a new
    // version server-side.
    api::json body;
    // UpdateAgentRequest accepts model (mistral-openapi.yaml) — include
    // it so model drift is fixed along with the rest of the config.
    body["model"] = model_;
    body["instructions"] = sys.resolve_server_agent_instructions(agent_cfg);
    body["description"] = server_cfg.description;
    body["tools"] = server_cfg.tools;
    if (server_cfg.completion_args) {
      body["completion_args"] = *server_cfg.completion_args;
    }
    body["metadata"] = api::json{
      {std::string(server_agent_config_hash_key), config_fingerprint()}
    };

    auto response_ch = std::make_shared<dripcore::channel<api::api_event>>(4);
    agent_system::instance().set_current_response_ch(response_ch);

    std::string path = "/" + api_version_ + "/agents/" + agent_id;
    request_ch_->try_send(
      {.method = "PATCH", .path = path, .body = body.dump(), .stream = false, .response_ch = response_ch});

    bool ok = false;
    while (true)
    {
      auto evt = co_await response_ch->recv(this);
      if (!evt) break;  // Channel closed (cancelled).
      if (evt->type == api::event_type::done) break;
      if (evt->type == api::event_type::error)
      {
        setup_error_ = evt->payload;
        break;
      }

      try
      {
        auto entity = api::agent_entity_from_json(api::json::parse(evt->payload));
        ok = !entity.id.empty();
      }
      catch (const std::exception& e)
      {
        setup_error_ = std::string("malformed update response: ") + e.what();
      }
    }

    agent_system::instance().clear_current_response_ch();
    co_return ok;
  }
} // namespace mistral::agent
