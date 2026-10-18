#include "doctest/doctest.h"

#include "agent/agent_system.h"
#include "agent/agent_task_base.h"
#include "api/agent_api.h"
#include "api/chat_types.h"
#include "api/providers/mistral_provider.h"
#include "api/types.h"

#include <dripcore/channel.h>
#include <dripcore/scheduler.h>
#include <dripcore/task.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace
{
  namespace api = mistral::api;
  namespace agent = mistral::agent;
  using json = nlohmann::ordered_json;

  // ---------------------------------------------------------------------------
  // Test fixture: temp workspace + initialized agent_system singleton
  // ---------------------------------------------------------------------------

  struct server_agent_test_fixture
  {
    std::string temp_dir;
    std::string config_file;
    std::string base_dir;
    std::string notes_dir;
    std::shared_ptr<dripcore::channel<api::api_request>> request_ch;

    server_agent_test_fixture()
    {
      temp_dir =
        (std::filesystem::temp_directory_path() / "mistral_server_agent_test").string();
      std::filesystem::remove_all(temp_dir);
      std::filesystem::create_directories(temp_dir);
      config_file = (std::filesystem::path(temp_dir) / "mistral-cli.json").string();
      base_dir = temp_dir;
      notes_dir = (std::filesystem::path(temp_dir) / "notes").string();
      std::filesystem::create_directories(notes_dir);
      request_ch = std::make_shared<dripcore::channel<api::api_request>>(8);
    }

    ~server_agent_test_fixture()
    {
      std::error_code ec;
      std::filesystem::remove_all(temp_dir, ec);
    }

    void write_config(const std::string& content)
    {
      std::ofstream out(config_file);
      out << content;
    }

    void initialize()
    {
      agent::agent_system::instance().initialize(
        config_file, base_dir, notes_dir, request_ch);
    }
  };

  const char* k_researcher_config = R"({
    "models": {
      "mistral-small": {
        "provider": "mistral",
        "model_id": "mistral-small-latest",
        "api_key_env": "MISTRAL_API_KEY",
        "base_url": "https://api.mistral.ai",
        "api_version": "v1"
      }
    },
    "agents": {
      "researcher": {
        "model": "mistral-small",
        "server_agent": {
          "name": "mistral-cli-researcher",
          "description": "test researcher",
          "instructions": "You research things.",
          "tools": [{"type": "web_search"}],
          "completion_args": {"temperature": 0.2}
        }
      }
    },
    "policy": {"default": "deny"}
  })";

  const char* k_full_config = R"({
    "models": {
      "mistral-medium": {
        "provider": "mistral",
        "model_id": "mistral-medium-latest",
        "api_key_env": "MISTRAL_API_KEY",
        "base_url": "https://api.mistral.ai",
        "api_version": "v1"
      },
      "mistral-small": {
        "provider": "mistral",
        "model_id": "mistral-small-latest",
        "api_key_env": "MISTRAL_API_KEY",
        "base_url": "https://api.mistral.ai",
        "api_version": "v1"
      }
    },
    "agents": {
      "main": {
        "model": "mistral-medium",
        "system_prompt": "You are a test agent.",
        "available_tools": ["web_search"],
        "subagent_tools": {"web_search": "researcher"},
        "tool_policy": "allow"
      },
      "researcher": {
        "model": "mistral-small",
        "server_agent": {
          "name": "mistral-cli-researcher",
          "description": "test researcher",
          "instructions": "You research things.",
          "tools": [{"type": "web_search"}]
        }
      }
    },
    "policy": {"default": "deny"}
  })";

  // ---------------------------------------------------------------------------
  // Fake HTTP layer: consumes api_requests and feeds canned responses
  // ---------------------------------------------------------------------------

  struct fake_http_options
  {
    bool agent_exists = false;        // GET /v1/agents/pages returns a match
    bool drift_metadata = false;      // existing agent has a stale config hash
    bool drift_model_metadata = false;  // existing agent was created with another model
    bool fail_create = false;         // POST /v1/agents fails
    bool fail_patch = false;          // PATCH /v1/agents/{id} fails
    bool fail_conversations = false;  // POST /v1/conversations fails
    bool plain_content = false;       // message.output content is a plain string
    // Override the message.output content chunks (defaults to the standard
    // text + tool_reference set).
    std::optional<json> conversation_content;
  };

  class fake_http_task : public dripcore::task
  {
  public:
    fake_http_task(
      std::shared_ptr<dripcore::channel<api::api_request>> request_ch,
      std::vector<api::api_request>* captured,
      fake_http_options opts = {})
      : request_ch_(std::move(request_ch))
      , captured_(captured)
      , opts_(opts)
    {}

    dripcore::awaitable<> body() override
    {
      while (true)
      {
        auto req = co_await request_ch_->recv(this);
        if (!req) break;
        captured_->push_back(*req);
        handle(*req);
      }
      co_return;
    }

  private:
    using event_ch = dripcore::channel<api::api_event>;

    static void respond_json(std::shared_ptr<event_ch> ch, const std::string& body)
    {
      ch->try_send({api::event_type::data, body});
      ch->try_send({api::event_type::done, {}});
    }

    // The config fingerprint the server-side agent would carry in its
    // metadata (computed from the initialized agent_system config).
    static std::string current_config_fingerprint(
      const std::string& model_override = "")
    {
      const auto& sys = agent::agent_system::instance();
      const auto& cfg = sys.get_agent_config("researcher");
      return agent::server_agent_config_fingerprint(
        *cfg.server_agent, sys.resolve_server_agent_instructions(cfg),
        model_override.empty()
          ? sys.resolve_model_id(cfg.model) : model_override);
    }

    void respond_conversation(std::shared_ptr<event_ch> ch)
    {
      json content;
      if (opts_.conversation_content)
      {
        content = *opts_.conversation_content;
      }
      else if (opts_.plain_content)
      {
        content = "Research findings";
      }
      else
      {
        content = json::array({
          json{{"type", "text"}, {"text", "Research"}},
          json{{"type", "tool_reference"}, {"tool", "web_search"},
               {"title", "Example Source"}, {"url", "https://example.com/article"}},
          json{{"type", "text"}, {"text", " findings"}}
        });
      }
      json response = {
        {"object", "conversation.response"},
        {"conversation_id", "conv_test123"},
        {"outputs", json::array({
          json{{"type", "tool.execution"}, {"name", "web_search"}, {"object", "entry"},
               {"created_at", "2026-01-01T00:00:00Z"}, {"completed_at", "2026-01-01T00:00:01Z"},
               {"id", "tool_exec_1"}},
          json{{"type", "message.output"}, {"content", content}, {"object", "entry"},
               {"created_at", "2026-01-01T00:00:01Z"}, {"completed_at", "2026-01-01T00:00:02Z"},
               {"id", "msg_1"}, {"agent_id", "agt_created"}, {"model", "mistral-small-latest"},
               {"role", "assistant"}}
        })},
        {"usage", json{{"prompt_tokens", 100}, {"completion_tokens", 20},
                       {"total_tokens", 120}, {"connector_tokens", 500},
                       {"connectors", json{{"web_search", 1}}}}}
      };
      respond_json(ch, response.dump());
    }

    void handle(const api::api_request& req)
    {
      auto& ch = req.response_ch;
      if (req.method == "GET" && req.path.starts_with("/v1/agents/pages"))
      {
        if (opts_.agent_exists)
        {
          // Hash the existing agent would carry in its metadata.
          std::string metadata_hash = current_config_fingerprint();
          if (opts_.drift_metadata)
          {
            metadata_hash = "stale-hash";
          }
          else if (opts_.drift_model_metadata)
          {
            // As if the agent had been created with a different model.
            metadata_hash = current_config_fingerprint("mistral-large-latest");
          }
          json agent_json = {
            {"id", "agt_existing"},
            {"name", "mistral-cli-researcher"},
            {"model", "mistral-small-latest"},
            {"version", 1},
            {"versions", json::array({1})},
            {"created_at", "2026-01-01T00:00:00Z"},
            {"updated_at", "2026-01-01T00:00:00Z"},
            {"deployment_chat", false},
            {"source", "api"},
            {"metadata", json{{
              std::string(agent::server_agent_config_hash_key),
              metadata_hash
            }}}
          };
          json page = {
            {"object", "list"},
            {"data", json::array({agent_json})},
            {"next_page_token", nullptr}
          };
          respond_json(ch, page.dump());
        }
        else
        {
          respond_json(ch, R"({"object":"list","data":[],"next_page_token":null})");
        }
      }
      else if (req.method == "POST" && req.path == "/v1/agents")
      {
        if (opts_.fail_create)
        {
          ch->try_send({api::event_type::error, "HTTP 422: validation error", 422});
          ch->try_send({api::event_type::done, {}});
        }
        else
        {
          respond_json(ch,
            R"({"id":"agt_created","name":"mistral-cli-researcher","model":"mistral-small-latest","version":1,"versions":[1],"created_at":"2026-01-01T00:00:00Z","updated_at":"2026-01-01T00:00:00Z","deployment_chat":false,"source":"api"})");
        }
      }
      else if (req.method == "PATCH" && req.path.starts_with("/v1/agents/"))
      {
        if (opts_.fail_patch)
        {
          ch->try_send({api::event_type::error,
            "HTTP 400: {\"object\":\"error\",\"message\":\"WebSearchTool connector is not supported\",\"type\":\"invalid_tools\",\"code\":\"1800\"}", 400});
          ch->try_send({api::event_type::done, {}});
        }
        else
        {
          respond_json(ch,
            R"({"id":"agt_existing","name":"mistral-cli-researcher","model":"mistral-small-latest","version":2,"versions":[1,2],"created_at":"2026-01-01T00:00:00Z","updated_at":"2026-01-01T00:00:00Z","deployment_chat":false,"source":"api"})");
        }
      }
      else if (req.method == "POST" && req.path == "/v1/conversations")
      {
        if (opts_.fail_conversations)
        {
          ch->try_send({api::event_type::error,
            "HTTP 400: {\"object\":\"error\",\"message\":\"WebSearchTool connector is not supported\",\"type\":\"invalid_tools\",\"code\":\"1800\"}", 400});
          ch->try_send({api::event_type::done, {}});
        }
        else
        {
          respond_conversation(ch);
        }
      }
      else
      {
        ch->try_send({api::event_type::error,
          "unexpected request: " + req.method + " " + req.path});
        ch->try_send({api::event_type::done, {}});
      }
    }

    std::shared_ptr<dripcore::channel<api::api_request>> request_ch_;
    std::vector<api::api_request>* captured_;
    fake_http_options opts_;
  };

  // Calls agent_system::run_subagent once (or twice for the cache test) and
  // closes the request channel so the fake HTTP task can exit.
  class subagent_caller_task : public dripcore::task
  {
  public:
    subagent_caller_task(
      std::shared_ptr<dripcore::channel<api::api_request>> request_ch,
      std::string agent_id,
      std::string message,
      std::vector<std::string>* results,
      int runs = 1)
      : request_ch_(std::move(request_ch))
      , agent_id_(std::move(agent_id))
      , message_(std::move(message))
      , results_(results)
      , runs_(runs)
    {}

    dripcore::awaitable<> body() override
    {
      for (int i = 0; i < runs_; ++i)
      {
        results_->push_back(co_await agent::agent_system::instance().run_subagent(
          *this, agent_id_, message_));
      }
      request_ch_->close();
      co_return;
    }

  private:
    std::shared_ptr<dripcore::channel<api::api_request>> request_ch_;
    std::string agent_id_;
    std::string message_;
    std::vector<std::string>* results_;
    int runs_;
  };

  // Agent task that routes a web_search tool call through execute_tool.
  class tool_routing_agent_task : public agent::agent_task_base
  {
  public:
    tool_routing_agent_task(
      const std::string& agent_id,
      std::shared_ptr<dripcore::channel<std::string>> input_ch,
      std::shared_ptr<dripcore::channel<api::api_request>> request_ch,
      std::string* result)
      : agent_task_base(agent_id, std::move(input_ch))
      , request_ch_(std::move(request_ch))
      , result_(result)
    {}

    dripcore::awaitable<> body() override
    {
      api::tool_call tc;
      tc.id = "call_1";
      tc.function.name = "web_search";
      tc.function.arguments = R"({"query":"test query"})";
      *result_ = co_await execute_tool(tc);
      request_ch_->close();
      co_return;
    }

  protected:
    void thinking_begin() override {}
    void thinking_end() override {}
    void agent_loop_on_sending_chat_request() override {}
    void agent_loop_on_message_content(const api::message_content&) override {}
    auto agent_loop_do_tool_call(const api::tool_call&)
      -> dripcore::awaitable<void> override { co_return; }
    void agent_loop_on_error(const std::string&) override {}

  private:
    std::shared_ptr<dripcore::channel<api::api_request>> request_ch_;
    std::string* result_;
  };

  // Dispatches a tool directly through the global registry (debug path).
  class dispatch_task : public dripcore::task
  {
  public:
    explicit dispatch_task(std::string* result) : result_(result) {}

    dripcore::awaitable<> body() override
    {
      json tool_call = {
        {"function", {{"name", "web_search"}, {"arguments", R"({"query":"x"})"}}}
      };
      auto res = co_await agent::agent_system::instance().global_tools().dispatch(tool_call);
      *result_ = res.dump();
      co_return;
    }

  private:
    std::string* result_;
  };
} // namespace

// ============================================================================
// api::builtin_tool / create_agent_request / agent_entity
// ============================================================================

TEST_SUITE_BEGIN("agent_api");

TEST_CASE("builtin_tool: serializes type and optional tool_configuration")
{
  api::builtin_tool plain{.type = "web_search"};
  json j = plain;
  CHECK(j == json({{"type", "web_search"}}));

  api::builtin_tool configured{.type = "web_search_premium"};
  configured.tool_configuration = json({{"include", json::array({"example.com"})}});
  json j2 = configured;
  CHECK(j2["type"] == "web_search_premium");
  CHECK(j2["tool_configuration"]["include"][0] == "example.com");
}

TEST_CASE("create_agent_request: serializes required and optional fields")
{
  api::create_agent_request req;
  req.model = "mistral-small-latest";
  req.name = "mistral-cli-researcher";
  req.instructions = "You research things.";
  req.description = "test researcher";
  req.tools = {api::builtin_tool{.type = "web_search"}};

  json j = req;
  CHECK(j["model"] == "mistral-small-latest");
  CHECK(j["name"] == "mistral-cli-researcher");
  CHECK(j["instructions"] == "You research things.");
  CHECK(j["description"] == "test researcher");
  REQUIRE(j["tools"].size() == 1);
  CHECK(j["tools"][0]["type"] == "web_search");
  // Optional fields are omitted when unset.
  CHECK(!j.contains("metadata"));
}

TEST_CASE("create_agent_request: minimal request has only model and name")
{
  api::create_agent_request req;
  req.model = "mistral-small-latest";
  req.name = "researcher";

  json j = req;
  CHECK(j.size() == 2);
  CHECK(j["model"] == "mistral-small-latest");
  CHECK(j["name"] == "researcher");
}

TEST_CASE("create_agent_request: serializes completion_args")
{
  api::create_agent_request req;
  req.model = "mistral-small-latest";
  req.name = "mistral-cli-researcher";
  req.completion_args = json({{"temperature", 0.2}, {"max_tokens", 4096}});

  json j = req;
  REQUIRE(j.contains("completion_args"));
  CHECK(j["completion_args"]["temperature"] == doctest::Approx(0.2));
  CHECK(j["completion_args"]["max_tokens"] == 4096);
}

TEST_CASE("agent_entity_from_json: parses the fields mistral-cli uses")
{
  auto j = json::parse(
    R"({"id":"agt_123","name":"mistral-cli-researcher","model":"mistral-small-latest","version":3,"description":"d","instructions":"i","versions":[1,2,3],"created_at":"2026-01-01T00:00:00Z","updated_at":"2026-01-01T00:00:00Z","deployment_chat":false,"source":"api"})");
  auto entity = api::agent_entity_from_json(j);
  CHECK(entity.id == "agt_123");
  CHECK(entity.name == "mistral-cli-researcher");
  REQUIRE(entity.description.has_value());
  CHECK(*entity.description == "d");
  REQUIRE(entity.instructions.has_value());
  CHECK(*entity.instructions == "i");
  CHECK(entity.version == 3);
}

TEST_CASE("agent_entity_from_json: tolerates missing optional fields")
{
  auto j = json::parse(R"({"id":"agt_1","name":"n"})");
  auto entity = api::agent_entity_from_json(j);
  CHECK(entity.id == "agt_1");
  CHECK(entity.name == "n");
  CHECK(!entity.description.has_value());
  CHECK(!entity.instructions.has_value());
  CHECK(entity.version == 0);
}

TEST_SUITE_END;

// ============================================================================
// provider::build_agents_completion_request
// ============================================================================

TEST_SUITE_BEGIN("provider_chat_request");

TEST_CASE("chat completion request still carries model and provider fields (regression)")
{
  api::mistral_provider provider;

  api::sampling_params params;
  params.safe_prompt = true;
  params.randomness = 0.1f;

  std::vector<api::message> messages;
  api::message user;
  user.role = "user";
  user.content = {api::message_content{.type = "text", .content = "hello"}};
  messages.push_back(user);

  auto body = json::parse(provider.build_chat_request(
    "mistral-small-latest", messages, json::array(), true, "cache-key", params));

  CHECK(body["model"] == "mistral-small-latest");
  CHECK(body["stream"] == true);
  CHECK(body["safe_prompt"] == true);
  CHECK(body["randomness"] == doctest::Approx(0.1));
  CHECK(body["prompt_cache_key"] == "cache-key");
  CHECK(!body.contains("agent_id"));
}

TEST_SUITE_END;

// ============================================================================
// server_agent_config_fingerprint
// ============================================================================

TEST_SUITE_BEGIN("server_agent_config_fingerprint");

TEST_CASE("fingerprint: stable and sensitive to config changes")
{
  agent::server_agent_config cfg;
  cfg.name = "mistral-cli-researcher";
  cfg.description = "test researcher";
  cfg.tools = {api::builtin_tool{.type = "web_search"}};
  cfg.completion_args = json{{"temperature", 0.2}};

  const auto fp1 = agent::server_agent_config_fingerprint(
    cfg, "You research things.", "mistral-small-latest");
  CHECK(fp1 == agent::server_agent_config_fingerprint(
    cfg, "You research things.", "mistral-small-latest"));
  CHECK(fp1.size() == 16);

  // Sensitive to model changes.
  CHECK(agent::server_agent_config_fingerprint(
    cfg, "You research things.", "mistral-large-latest") != fp1);

  // Sensitive to tool changes.
  auto cfg2 = cfg;
  cfg2.tools = {api::builtin_tool{.type = "web_search_premium"}};
  CHECK(agent::server_agent_config_fingerprint(
    cfg2, "You research things.", "mistral-small-latest") != fp1);

  // Sensitive to instruction changes.
  CHECK(agent::server_agent_config_fingerprint(
    cfg, "Other instructions.", "mistral-small-latest") != fp1);

  // Sensitive to completion_args changes.
  auto cfg3 = cfg;
  cfg3.completion_args = json{{"temperature", 0.5}};
  CHECK(agent::server_agent_config_fingerprint(
    cfg3, "You research things.", "mistral-small-latest") != fp1);
}

TEST_SUITE_END;

// ============================================================================
// Config parsing: server_agent + subagent_tools
// ============================================================================

TEST_SUITE_BEGIN("server_agent_config");

TEST_CASE_FIXTURE(server_agent_test_fixture, "config: parses server_agent and subagent_tools")
{
  write_config(k_full_config);
  initialize();

  auto& sys = agent::agent_system::instance();

  const auto& researcher = sys.get_agent_config("researcher");
  REQUIRE(researcher.server_agent.has_value());
  CHECK(researcher.server_agent->name == "mistral-cli-researcher");
  CHECK(researcher.server_agent->description == "test researcher");
  CHECK(researcher.server_agent->instructions == "You research things.");
  REQUIRE(researcher.server_agent->tools.size() == 1);
  CHECK(researcher.server_agent->tools[0].type == "web_search");

  const auto& main_cfg = sys.get_agent_config("main");
  REQUIRE(main_cfg.subagent_tools.size() == 1);
  CHECK(main_cfg.subagent_tools.at("web_search") == "researcher");

  // Plain-string instructions pass through unchanged.
  CHECK(sys.resolve_server_agent_instructions(researcher) == "You research things.");
}

TEST_CASE_FIXTURE(server_agent_test_fixture, "config: parses server_agent completion_args")
{
  write_config(R"({
    "models": {
      "mistral-small": {
        "provider": "mistral",
        "model_id": "mistral-small-latest",
        "api_key_env": "MISTRAL_API_KEY",
        "base_url": "https://api.mistral.ai",
        "api_version": "v1"
      }
    },
    "agents": {
      "researcher": {
        "model": "mistral-small",
        "server_agent": {
          "name": "mistral-cli-researcher",
          "tools": [{"type": "web_search"}],
          "completion_args": {"temperature": 0.2}
        }
      }
    }
  })");
  initialize();

  const auto& researcher = agent::agent_system::instance().get_agent_config("researcher");
  REQUIRE(researcher.server_agent.has_value());
  REQUIRE(researcher.server_agent->completion_args.has_value());
  CHECK((*researcher.server_agent->completion_args)["temperature"] == doctest::Approx(0.2));
}

TEST_CASE_FIXTURE(server_agent_test_fixture, "config: server_agent without a name is ignored")
{
  write_config(R"({
    "models": {
      "mistral-small": {
        "provider": "mistral",
        "model_id": "mistral-small-latest",
        "api_key_env": "MISTRAL_API_KEY",
        "base_url": "https://api.mistral.ai",
        "api_version": "v1"
      }
    },
    "agents": {
      "researcher": {
        "model": "mistral-small",
        "server_agent": {
          "instructions": "You research things.",
          "tools": [{"type": "web_search"}]
        }
      }
    }
  })");
  initialize();

  const auto& researcher = agent::agent_system::instance().get_agent_config("researcher");
  CHECK(!researcher.server_agent.has_value());
}

TEST_CASE_FIXTURE(server_agent_test_fixture, "config: agent without server_agent has none")
{
  write_config(k_full_config);
  initialize();

  const auto& main_cfg = agent::agent_system::instance().get_agent_config("main");
  CHECK(!main_cfg.server_agent.has_value());
}

TEST_CASE_FIXTURE(server_agent_test_fixture, "config: resolves file:// server-agent instructions")
{
  const std::string prompt_path = (std::filesystem::path(temp_dir) / "researcher_prompt.md").string();
  {
    std::ofstream out(prompt_path);
    out << "# Researcher\nYou research things.\n";
  }

  json config = {
    {"models", {
      {"mistral-small", {
        {"provider", "mistral"},
        {"model_id", "mistral-small-latest"},
        {"api_key_env", "MISTRAL_API_KEY"},
        {"base_url", "https://api.mistral.ai"},
        {"api_version", "v1"}
      }}
    }},
    {"agents", {
      {"researcher", {
        {"model", "mistral-small"},
        {"server_agent", {
          {"name", "mistral-cli-researcher"},
          {"instructions", "file://" + prompt_path},
          {"tools", json::array({json{{"type", "web_search"}}})}
        }}
      }}
    }}
  };
  write_config(config.dump(2));
  initialize();

  const auto& researcher = agent::agent_system::instance().get_agent_config("researcher");
  REQUIRE(researcher.server_agent.has_value());
  const auto resolved = agent::agent_system::instance().resolve_server_agent_instructions(researcher);
  CHECK(resolved == "# Researcher\nYou research things.\n");
}

TEST_SUITE_END;

// ============================================================================
// web_search tool registration
// ============================================================================

TEST_SUITE_BEGIN("web_search_tool");

TEST_CASE_FIXTURE(server_agent_test_fixture, "web_search is registered in the global tool registry")
{
  write_config(k_researcher_config);
  initialize();

  auto schema = agent::agent_system::instance().global_tools().get_schema();
  bool found = false;
  for (const auto& tool : schema)
  {
    if (tool["function"]["name"] == "web_search")
    {
      found = true;
      CHECK(tool["type"] == "function");
      CHECK(tool["function"]["parameters"]["properties"].contains("query"));
      const auto& required = tool["function"]["parameters"]["required"];
      REQUIRE(required.is_array());
      bool query_required = false;
      for (const auto& r : required)
      {
        if (r == "query") query_required = true;
      }
      CHECK(query_required);
    }
  }
  CHECK(found);
}

TEST_CASE_FIXTURE(server_agent_test_fixture, "web_search direct dispatch returns a routing hint")
{
  write_config(k_researcher_config);
  initialize();

  std::string result;
  dripcore::scheduler sched;
  sched.spawn<dispatch_task>(&result);
  sched.run();

  auto j = json::parse(result);
  CHECK(j.contains("error"));
  CHECK(j["error"].get<std::string>().find("subagent_tools") != std::string::npos);
}

TEST_SUITE_END;

// ============================================================================
// server_agent_task end-to-end (fake HTTP layer)
// ============================================================================

TEST_SUITE_BEGIN("server_agent_task");

TEST_CASE_FIXTURE(server_agent_test_fixture,
  "creates the server-side agent when not found, then completes")
{
  write_config(k_researcher_config);
  initialize();

  std::vector<api::api_request> captured;
  std::vector<std::string> results;

  dripcore::scheduler sched;
  sched.spawn<fake_http_task>(request_ch, &captured, fake_http_options{});
  sched.spawn<subagent_caller_task>(
    request_ch, "researcher", "What is the latest Mistral model?", &results);
  sched.run();

  REQUIRE(results.size() == 1);
  // Text chunks are joined; tool_reference chunks become a source list.
  CHECK(results[0] ==
    "Research findings\n\nSources:\n- [Example Source](https://example.com/article)");

  // Request sequence: find (miss) -> create -> conversation.
  REQUIRE(captured.size() == 3);

  CHECK(captured[0].method == "GET");
  CHECK(captured[0].path == "/v1/agents/pages?name=mistral-cli-researcher");
  CHECK(captured[0].stream == false);

  CHECK(captured[1].method == "POST");
  CHECK(captured[1].path == "/v1/agents");
  CHECK(captured[1].stream == false);
  auto create_body = json::parse(captured[1].body);
  CHECK(create_body["name"] == "mistral-cli-researcher");
  CHECK(create_body["model"] == "mistral-small-latest");
  CHECK(create_body["description"] == "test researcher");
  CHECK(create_body["instructions"] == "You research things.");
  REQUIRE(create_body["tools"].size() == 1);
  CHECK(create_body["tools"][0]["type"] == "web_search");
  // temperature is configured via completion_args at creation time.
  REQUIRE(create_body.contains("completion_args"));
  CHECK(create_body["completion_args"]["temperature"] == doctest::Approx(0.2));
  // The config fingerprint is stored in the agent's metadata.
  REQUIRE(create_body.contains("metadata"));
  CHECK(create_body["metadata"].contains("mistral-cli-config-hash"));

  CHECK(captured[2].method == "POST");
  CHECK(captured[2].path == "/v1/conversations");
  CHECK(captured[2].stream == false);
  auto conv_body = json::parse(captured[2].body);
  CHECK(conv_body["agent_id"] == "agt_created");
  CHECK(conv_body["inputs"] == "What is the latest Mistral model?");
  CHECK(conv_body["stream"] == false);
  CHECK(conv_body["store"] == false);
  CHECK(!conv_body.contains("model"));
  CHECK(!conv_body.contains("messages"));

  // The resolved id is cached for the session.
  auto& sys = agent::agent_system::instance();
  REQUIRE(sys.get_server_agent_id("researcher").has_value());
  CHECK(*sys.get_server_agent_id("researcher") == "agt_created");
}

TEST_CASE_FIXTURE(server_agent_test_fixture,
  "reuses an existing server-side agent by name and caches the id")
{
  write_config(k_researcher_config);
  initialize();

  std::vector<api::api_request> captured;
  std::vector<std::string> results;

  dripcore::scheduler sched;
  fake_http_options opts;
  opts.agent_exists = true;
  sched.spawn<fake_http_task>(request_ch, &captured, opts);
  // Two runs: the second must hit the session cache (no find/create).
  sched.spawn<subagent_caller_task>(
    request_ch, "researcher", "query", &results, /*runs=*/2);
  sched.run();

  REQUIRE(results.size() == 2);
  CHECK(results[0] ==
    "Research findings\n\nSources:\n- [Example Source](https://example.com/article)");
  CHECK(results[1] ==
    "Research findings\n\nSources:\n- [Example Source](https://example.com/article)");

  // First run: find (hit, matching config hash) + conversation.
  // Second run: conversation only (session cache).
  REQUIRE(captured.size() == 3);
  CHECK(captured[0].method == "GET");
  CHECK(captured[0].path.starts_with("/v1/agents/pages"));
  CHECK(captured[1].path == "/v1/conversations");
  CHECK(captured[2].path == "/v1/conversations");

  auto body1 = json::parse(captured[1].body);
  auto body2 = json::parse(captured[2].body);
  CHECK(body1["agent_id"] == "agt_existing");
  CHECK(body2["agent_id"] == "agt_existing");
}

TEST_CASE_FIXTURE(server_agent_test_fixture,
  "updates the server-side agent when the local config drifted")
{
  write_config(k_researcher_config);
  initialize();

  std::vector<api::api_request> captured;
  std::vector<std::string> results;

  dripcore::scheduler sched;
  fake_http_options opts;
  opts.agent_exists = true;
  opts.drift_metadata = true;  // existing agent has a stale config hash
  sched.spawn<fake_http_task>(request_ch, &captured, opts);
  sched.spawn<subagent_caller_task>(request_ch, "researcher", "query", &results);
  sched.run();

  REQUIRE(results.size() == 1);
  CHECK(results[0] ==
    "Research findings\n\nSources:\n- [Example Source](https://example.com/article)");

  // find (hit, stale hash) + PATCH (update in place) + conversation.
  REQUIRE(captured.size() == 3);
  CHECK(captured[0].method == "GET");
  CHECK(captured[1].method == "PATCH");
  CHECK(captured[1].path == "/v1/agents/agt_existing");
  auto patch_body = json::parse(captured[1].body);
  REQUIRE(patch_body["tools"].size() == 1);
  CHECK(patch_body["tools"][0]["type"] == "web_search");
  CHECK(patch_body["instructions"] == "You research things.");
  REQUIRE(patch_body.contains("metadata"));
  CHECK(patch_body["metadata"].contains("mistral-cli-config-hash"));
  CHECK(captured[2].path == "/v1/conversations");
  CHECK(json::parse(captured[2].body)["agent_id"] == "agt_existing");
}

TEST_CASE_FIXTURE(server_agent_test_fixture,
  "updates the server-side agent when the model drifted")
{
  write_config(k_researcher_config);
  initialize();

  std::vector<api::api_request> captured;
  std::vector<std::string> results;

  dripcore::scheduler sched;
  fake_http_options opts;
  opts.agent_exists = true;
  opts.drift_model_metadata = true;  // created with a different model
  sched.spawn<fake_http_task>(request_ch, &captured, opts);
  sched.spawn<subagent_caller_task>(request_ch, "researcher", "query", &results);
  sched.run();

  REQUIRE(results.size() == 1);
  CHECK(results[0] ==
    "Research findings\n\nSources:\n- [Example Source](https://example.com/article)");

  // find (hit, model drift) + PATCH (carries the model) + conversation.
  REQUIRE(captured.size() == 3);
  CHECK(captured[0].method == "GET");
  CHECK(captured[1].method == "PATCH");
  auto patch_body = json::parse(captured[1].body);
  CHECK(patch_body["model"] == "mistral-small-latest");
  CHECK(captured[2].path == "/v1/conversations");
}

TEST_CASE_FIXTURE(server_agent_test_fixture,
  "surfaces setup errors through the subagent error path (with hint)")
{
  write_config(k_researcher_config);
  initialize();

  std::vector<api::api_request> captured;
  std::vector<std::string> results;

  dripcore::scheduler sched;
  fake_http_options opts;
  opts.agent_exists = true;
  opts.drift_metadata = true;  // triggers PATCH
  opts.fail_patch = true;
  sched.spawn<fake_http_task>(request_ch, &captured, opts);
  sched.spawn<subagent_caller_task>(request_ch, "researcher", "query", &results);
  sched.run();

  // The PATCH failure is surfaced via agent_loop_on_error ->
  // agent_loop_on_completed, including the connector hint.
  REQUIRE(results.size() == 1);
  CHECK(results[0].find("Error: could not set up server-side agent") == 0);
  CHECK(results[0].find("HTTP 400") != std::string::npos);
  CHECK(results[0].find("connector is not supported") != std::string::npos);
  CHECK(results[0].find("hint:") != std::string::npos);

  // find (hit, stale hash) + PATCH (failed) — no conversation request.
  REQUIRE(captured.size() == 2);
  CHECK(captured[0].method == "GET");
  CHECK(captured[1].method == "PATCH");
}

TEST_CASE_FIXTURE(server_agent_test_fixture,
  "parses plain string conversation content")
{
  write_config(k_researcher_config);
  initialize();

  std::vector<api::api_request> captured;
  std::vector<std::string> results;

  dripcore::scheduler sched;
  fake_http_options opts;
  opts.plain_content = true;
  sched.spawn<fake_http_task>(request_ch, &captured, opts);
  sched.spawn<subagent_caller_task>(request_ch, "researcher", "query", &results);
  sched.run();

  REQUIRE(results.size() == 1);
  CHECK(results[0] == "Research findings");
}

TEST_CASE_FIXTURE(server_agent_test_fixture,
  "reports an error when the server-side agent cannot be created")
{
  write_config(k_researcher_config);
  initialize();

  std::vector<api::api_request> captured;
  std::vector<std::string> results;

  dripcore::scheduler sched;
  fake_http_options opts;
  opts.fail_create = true;
  sched.spawn<fake_http_task>(request_ch, &captured, opts);
  sched.spawn<subagent_caller_task>(request_ch, "researcher", "query", &results);
  sched.run();

  REQUIRE(results.size() == 1);
  CHECK(results[0].find("Error: could not set up server-side agent") == 0);
  // The setup failure includes the actual server error.
  CHECK(results[0].find("HTTP 422") != std::string::npos);

  // find (miss) + create (failed) — no conversation request is made.
  REQUIRE(captured.size() == 2);
  CHECK(captured[0].method == "GET");
  CHECK(captured[1].method == "POST");
  CHECK(captured[1].path == "/v1/agents");
}

TEST_CASE_FIXTURE(server_agent_test_fixture,
  "surfaces the API error when the conversation request fails")
{
  write_config(k_researcher_config);
  initialize();

  std::vector<api::api_request> captured;
  std::vector<std::string> results;

  dripcore::scheduler sched;
  fake_http_options opts;
  opts.fail_conversations = true;
  sched.spawn<fake_http_task>(request_ch, &captured, opts);
  sched.spawn<subagent_caller_task>(request_ch, "researcher", "query", &results);
  sched.run();

  // A failed turn must surface the error instead of returning an empty
  // string (which the model cannot distinguish from a valid empty answer).
  REQUIRE(results.size() == 1);
  CHECK(results[0].find("Error: HTTP 400") == 0);
  CHECK(results[0].find("connector is not supported") != std::string::npos);
  // Connector errors get an actionable hint appended.
  CHECK(results[0].find("hint:") != std::string::npos);
}

TEST_CASE_FIXTURE(server_agent_test_fixture,
  "routes a web_search tool call to the researcher subagent")
{
  write_config(k_full_config);
  initialize();

  std::vector<api::api_request> captured;
  std::string result;
  auto input_ch = std::make_shared<dripcore::channel<std::string>>(1);

  dripcore::scheduler sched;
  sched.spawn<fake_http_task>(request_ch, &captured, fake_http_options{});
  sched.spawn<tool_routing_agent_task>("main", input_ch, request_ch, &result);
  sched.run();

  CHECK(result ==
    "Research findings\n\nSources:\n- [Example Source](https://example.com/article)");

  // find (miss) + create + conversation.
  REQUIRE(captured.size() == 3);
  CHECK(captured[2].path == "/v1/conversations");
  auto conv_body = json::parse(captured[2].body);
  CHECK(conv_body["agent_id"] == "agt_created");
  // The tool's "query" argument becomes the conversation inputs.
  CHECK(conv_body["inputs"] == "test query");
}

TEST_CASE_FIXTURE(server_agent_test_fixture,
  "renders sources without urls as bare titles and lists all sources")
{
  write_config(k_researcher_config);
  initialize();

  fake_http_options opts;
  opts.conversation_content = json::array({
    json{{"type", "text"}, {"text", "Answer"}},
    json{{"type", "tool_reference"}, {"title", "With URL"},
         {"url", "https://example.com/a"}},
    json{{"type", "tool_reference"}, {"title", "Without URL"}}
  });

  std::vector<api::api_request> captured;
  std::vector<std::string> results;

  dripcore::scheduler sched;
  sched.spawn<fake_http_task>(request_ch, &captured, opts);
  sched.spawn<subagent_caller_task>(request_ch, "researcher", "query", &results);
  sched.run();

  REQUIRE(results.size() == 1);
  CHECK(results[0] ==
    "Answer\n\nSources:\n- [With URL](https://example.com/a)\n- Without URL");
}

TEST_CASE_FIXTURE(server_agent_test_fixture,
  "deduplicates repeated tool_reference citations in the source list")
{
  write_config(k_researcher_config);
  initialize();

  fake_http_options opts;
  opts.conversation_content = json::array({
    json{{"type", "text"}, {"text", "Answer"}},
    json{{"type", "tool_reference"}, {"title", "Same Source"},
         {"url", "https://example.com/a"}},
    json{{"type", "tool_reference"}, {"title", "Same Source"},
         {"url", "https://example.com/a"}},
    json{{"type", "tool_reference"}, {"title", "Other Source"},
         {"url", "https://example.com/b"}},
    json{{"type", "tool_reference"}, {"title", "Same Source"},
         {"url", "https://example.com/a"}}
  });

  std::vector<api::api_request> captured;
  std::vector<std::string> results;

  dripcore::scheduler sched;
  sched.spawn<fake_http_task>(request_ch, &captured, opts);
  sched.spawn<subagent_caller_task>(request_ch, "researcher", "query", &results);
  sched.run();

  REQUIRE(results.size() == 1);
  CHECK(results[0] ==
    "Answer\n\nSources:\n- [Same Source](https://example.com/a)"
    "\n- [Other Source](https://example.com/b)");
}

TEST_CASE_FIXTURE(server_agent_test_fixture,
  "rejects a non-mistral provider before making any request")
{
  write_config(R"({
    "models": {
      "other": {
        "provider": "openai",
        "model_id": "gpt-test",
        "api_key_env": "OPENAI_API_KEY",
        "base_url": "https://api.example.com",
        "api_version": "v1"
      }
    },
    "agents": {
      "researcher": {
        "model": "other",
        "server_agent": {
          "name": "mistral-cli-researcher",
          "instructions": "You research things.",
          "tools": [{"type": "web_search"}]
        }
      }
    }
  })");
  initialize();

  std::vector<api::api_request> captured;
  std::vector<std::string> results;

  dripcore::scheduler sched;
  sched.spawn<fake_http_task>(request_ch, &captured, fake_http_options{});
  sched.spawn<subagent_caller_task>(request_ch, "researcher", "query", &results);
  sched.run();

  REQUIRE(results.size() == 1);
  CHECK(results[0].find("Error: could not set up server-side agent") == 0);
  CHECK(results[0].find(
    "provider 'openai' does not support the Mistral Agents API")
    != std::string::npos);
  // The provider check happens before any HTTP request.
  CHECK(captured.empty());
}

TEST_CASE_FIXTURE(server_agent_test_fixture,
  "percent-encodes the server agent name in the find request")
{
  write_config(R"({
    "models": {
      "mistral-small": {
        "provider": "mistral",
        "model_id": "mistral-small-latest",
        "api_key_env": "MISTRAL_API_KEY",
        "base_url": "https://api.mistral.ai",
        "api_version": "v1"
      }
    },
    "agents": {
      "researcher": {
        "model": "mistral-small",
        "server_agent": {
          "name": "researcher & co",
          "instructions": "You research things.",
          "tools": [{"type": "web_search"}]
        }
      }
    }
  })");
  initialize();

  std::vector<api::api_request> captured;
  std::vector<std::string> results;

  dripcore::scheduler sched;
  sched.spawn<fake_http_task>(request_ch, &captured, fake_http_options{});
  sched.spawn<subagent_caller_task>(request_ch, "researcher", "query", &results);
  sched.run();

  REQUIRE(results.size() == 1);
  // find (miss) + create + conversation.
  REQUIRE(captured.size() == 3);
  CHECK(captured[0].method == "GET");
  CHECK(captured[0].path == "/v1/agents/pages?name=researcher%20%26%20co");
}

TEST_SUITE_END;
