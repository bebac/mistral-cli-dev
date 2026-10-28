#include "agent/agent_config.h"
#include "agent/agent_system.h"
#include "agent/tool_registry.h"
#include "tools/exec_tools.h"
#include "tools/file_tools.h"
#include "tools/note_tools.h"
#include <doctest/doctest.h>

#include <dripcore/scheduler.h>
#include <dripcore/task.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace mistral::agent
{
  namespace fs = std::filesystem;
  using json = nlohmann::ordered_json;
} // namespace mistral::agent

// ============================================================================
// Test Fixture for agent_system tests
// ============================================================================
struct AgentSystemTestFixture
{
  std::string temp_dir;
  std::string config_file;
  std::string base_dir;
  std::string notes_dir;

  AgentSystemTestFixture()
  {
    // Create a temporary directory for test files
    temp_dir = std::filesystem::temp_directory_path() / "mistral_cli_test";
    std::filesystem::create_directories(temp_dir);

    config_file = (std::filesystem::path(temp_dir) / "mistral-cli.json").string();
    base_dir = std::filesystem::path(temp_dir).string();
    notes_dir = (std::filesystem::path(temp_dir) / "notes").string();
    std::filesystem::create_directories(notes_dir);
  }

  ~AgentSystemTestFixture()
  {
    // Clean up temporary files
    try
    {
      std::filesystem::remove_all(temp_dir);
    }
    catch (...)
    {
      // Ignore cleanup errors
    }
  }

  void create_config_file(const std::string& content)
  {
    std::ofstream out(config_file);
    out << content;
  }

  void create_config_file(const nlohmann::json& content)
  {
    std::ofstream out(config_file);
    out << content.dump(2);
  }

  // Helper to create a standard config with a main agent
  nlohmann::json create_standard_config(
    const std::string& model_name = "mistral-medium",
    const std::string& model_id = "mistral-medium-latest"
  )
  {
    return {
      {"models",
       {{model_name,
         {{"provider", "mistral"},
          {"model_id", model_id},
          {"api_key_env", "MISTRAL_API_KEY"},
          {"base_url", "https://api.mistral.ai"},
          {"api_version", "v1"}}}}},
      {"agents",
       {{"main", {{"model", model_name}, {"system_prompt", "You are a helpful assistant."}, {"tool_policy", "deny"}}}}},
      {"policy", {{"default", "deny"}}}
    };
  }

  // Helper to initialize agent_system with standard config
  void initialize_agent_system_with_main_agent(
    const std::string& model_name = "mistral-medium",
    const std::string& model_id = "mistral-medium-latest"
  )
  {
    create_config_file(create_standard_config(model_name, model_id));
    auto& system = mistral::agent::agent_system::instance();
    system.initialize(config_file, base_dir, notes_dir);
  }

  // Helper to initialize agent_system with empty config (for tests that don't need config data)
  void initialize_agent_system_empty()
  {
    create_config_file(nlohmann::json({}));
    auto& system = mistral::agent::agent_system::instance();
    system.initialize(config_file, base_dir, notes_dir);
  }
};

// ============================================================================
// agent_config Tests
// ============================================================================
TEST_SUITE_BEGIN("agent_config");

TEST_CASE("model_config: default values")
{
  mistral::agent::model_config cfg;
  CHECK(cfg.provider.empty());
  CHECK(cfg.model_id.empty());
  CHECK(cfg.api_key_env.empty());
  CHECK(cfg.base_url.empty());
  CHECK(cfg.api_version == "v1");
}

TEST_CASE("agent_config: default values")
{
  mistral::agent::agent_config cfg;
  CHECK(cfg.model.empty());
  CHECK(cfg.system_prompt.empty());
  CHECK(cfg.tool_policy.empty());
  CHECK(cfg.available_tools.empty());
}

TEST_CASE("full_config: default values")
{
  mistral::agent::full_config cfg;
  CHECK(cfg.models.empty());
  CHECK(cfg.agents.empty());
  CHECK(cfg.default_policy == "deny");
}

TEST_SUITE_END;

TEST_SUITE_BEGIN("agent_system::configuration_loading");

TEST_CASE_FIXTURE(AgentSystemTestFixture, "load_config_from_file: basic config")
{
  nlohmann::json config = {
    {"models",
     {{"mistral-medium",
       {{"provider", "mistral"},
        {"model_id", "mistral-medium-latest"},
        {"api_key_env", "MISTRAL_API_KEY"},
        {"base_url", "https://api.mistral.ai"},
        {"api_version", "v1"}}}}},
    {"agents",
     {{"main",
       {{"model", "mistral-medium"},
        {"system_prompt", "You are a helpful assistant."},
        {"tool_policy", "deny"},
        {"available_tools", {"ls", "read"}}}}}},
    {"policy", {{"default", "deny"}}}
  };

  create_config_file(config);

  auto& system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir);

  // Check models
  CHECK(system.has_model_config("mistral-medium"));
  const auto& model = system.get_model_config("mistral-medium");
  CHECK(model.provider == "mistral");
  CHECK(model.model_id == "mistral-medium-latest");
  CHECK(model.api_key_env == "MISTRAL_API_KEY");
  CHECK(model.base_url == "https://api.mistral.ai");
  CHECK(model.api_version == "v1");

  // Check agents
  CHECK(system.has_agent_config("main"));
  const auto& agent = system.get_agent_config("main");
  CHECK(agent.model == "mistral-medium");
  CHECK(agent.system_prompt == "You are a helpful assistant.");
  CHECK(agent.tool_policy == "deny");
  CHECK(agent.available_tools.size() == 2);
  CHECK(agent.available_tools[0] == "ls");
  CHECK(agent.available_tools[1] == "read");

  // Check default policy
  CHECK(system.get_config().default_policy == "deny");
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "load_config_from_file: missing file throws")
{
  CHECK_THROWS_AS(
    mistral::agent::agent_system::instance().initialize(
      "/nonexistent/path/mistral-cli.json", base_dir, notes_dir
    ),
    std::runtime_error
  );
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "load_config_from_file: invalid JSON throws")
{
  create_config_file(std::string("this is not valid json {"));
  CHECK_THROWS_AS(
    mistral::agent::agent_system::instance().initialize(config_file, base_dir, notes_dir), std::runtime_error
  );
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "load_config_from_file: empty config")
{
  create_config_file(nlohmann::json({}));
  auto& system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir);

  CHECK(system.get_config().models.empty());
  CHECK(system.get_config().agents.empty());
  CHECK(system.get_config().default_policy == "deny");
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "global_tools: exec tool is registered")
{
  using namespace mistral::agent;

  create_config_file(nlohmann::json({}));
  auto& system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir);

  auto& global_registry = system.global_tools();

  // Check that exec tool is registered
  CHECK(global_registry.has_tool("exec") == true);

  // Get the tool and verify it exists
  auto exec_tool = global_registry.get_tool("exec");
  REQUIRE(exec_tool != nullptr);
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "load_config_from_file: multiple models and agents")
{
  nlohmann::json config = {
    {"models",
     {{"mistral-small", {{"provider", "mistral"}, {"model_id", "mistral-small-latest"}}},
      {"gpt-4o-mini",
       {{"provider", "openai"},
        {"model_id", "gpt-4o-mini"},
        {"api_key_env", "OPENAI_API_KEY"},
        {"base_url", "https://api.openai.com"}}}}},
    {"agents",
     {{"main", {{"model", "mistral-small"}, {"tool_policy", "allow"}}},
      {"read_only",
       {{"model", "gpt-4o-mini"},
        {"system_prompt", "Read-only assistant"},
        {"tool_policy", "require_approval"},
        {"available_tools", {"read", "note_read"}}}}}},
    {"policy", {{"default", "allow"}}}
  };

  create_config_file(config);
  auto& system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir);

  // Check multiple models
  CHECK(system.has_model_config("mistral-small"));
  CHECK(system.has_model_config("gpt-4o-mini"));
  CHECK_FALSE(system.has_model_config("nonexistent"));

  // Check model resolution
  CHECK(system.resolve_model_id("mistral-small") == "mistral-small-latest");
  CHECK(system.resolve_model_id("gpt-4o-mini") == "gpt-4o-mini");
  CHECK(system.resolve_model_id("nonexistent") == "nonexistent"); // fallback

  // Check multiple agents
  CHECK(system.has_agent_config("main"));
  CHECK(system.has_agent_config("read_only"));
  CHECK_FALSE(system.has_agent_config("nonexistent"));

  // Check agent configs
  const auto& main_agent = system.get_agent_config("main");
  CHECK(main_agent.model == "mistral-small");
  CHECK(main_agent.tool_policy == "allow");

  const auto& ro_agent = system.get_agent_config("read_only");
  CHECK(ro_agent.model == "gpt-4o-mini");
  CHECK(ro_agent.system_prompt == "Read-only assistant");
  CHECK(ro_agent.tool_policy == "require_approval");
  CHECK(ro_agent.available_tools.size() == 2);
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "resolve_tool_schema_options: model default, agent override, explicit false")
{
  nlohmann::json config = {
    {"models",
     {{"strict-model", {{"provider", "mistral"}, {"model_id", "mistral-medium-latest"}, {"strict_mode", true}}},
      {"plain-model", {{"provider", "mistral"}, {"model_id", "mistral-small-latest"}}}}},
    {"agents",
     {{"inherits", {{"model", "strict-model"}}},
      {"explicit_on", {{"model", "plain-model"}, {"strict_mode", true}}},
      {"explicit_off", {{"model", "strict-model"}, {"strict_mode", false}}},
      {"plain", {{"model", "plain-model"}}}}},
    {"policy", {{"default", "deny"}}}
  };

  create_config_file(config);
  auto& system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir);

  // Model default inherited
  auto opts = system.resolve_tool_schema_options(system.get_agent_config("inherits"));
  CHECK(opts.strict_mode == true);

  // Agent-level true on a non-strict model
  opts = system.resolve_tool_schema_options(system.get_agent_config("explicit_on"));
  CHECK(opts.strict_mode == true);

  // Agent-level false overrides model-level true
  opts = system.resolve_tool_schema_options(system.get_agent_config("explicit_off"));
  CHECK(opts.strict_mode == false);

  // Default off when neither model nor agent sets it
  opts = system.resolve_tool_schema_options(system.get_agent_config("plain"));
  CHECK(opts.strict_mode == false);
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "get_agent_config: throws for missing agent")
{
  create_config_file(nlohmann::json({{"models", {}}, {"agents", {}}, {"policy", {"default", "deny"}}}));
  auto& system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir);

  CHECK_THROWS_AS(system.get_agent_config("nonexistent"), std::runtime_error);
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "get_model_config: throws for missing model")
{
  create_config_file(nlohmann::json({{"models", {}}, {"agents", {}}, {"policy", {"default", "deny"}}}));
  auto& system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir);

  CHECK_THROWS_AS(system.get_model_config("nonexistent"), std::runtime_error);
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "resolve_tool_policy: malformed policy file throws")
{
  // Policy file with invalid JSON (missing comma after an entry).
  const std::string policy_file = (std::filesystem::path(temp_dir) / "policy.json").string();
  {
    std::ofstream out(policy_file);
    out << "{\n  \"default\": \"deny\"\n  \"exec\": \"require_approval\"\n}\n";
  }

  initialize_agent_system_with_main_agent();
  auto& system = mistral::agent::agent_system::instance();

  mistral::agent::agent_config agent_cfg = system.get_agent_config("main");
  agent_cfg.tool_policy = "file://" + policy_file;

  CHECK_THROWS_AS(system.resolve_tool_policy(agent_cfg), std::exception);
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "resolve_tool_policy: valid policy file resolves")
{
  const std::string policy_file = (std::filesystem::path(temp_dir) / "policy.json").string();
  {
    std::ofstream out(policy_file);
    out << "{\"default\": \"deny\", \"exec\": \"require_approval\"}\n";
  }

  initialize_agent_system_with_main_agent();
  auto& system = mistral::agent::agent_system::instance();

  mistral::agent::agent_config agent_cfg = system.get_agent_config("main");
  agent_cfg.tool_policy = "file://" + policy_file;

  auto policy = system.resolve_tool_policy(agent_cfg);
  CHECK(policy["default"] == "deny");
  CHECK(policy["exec"] == "require_approval");
}

TEST_SUITE_END;

TEST_SUITE_BEGIN("agent_system::backward_compatibility");

TEST_SUITE_END;

TEST_SUITE_BEGIN("agent_system::tool_registry");

TEST_CASE_FIXTURE(AgentSystemTestFixture, "global_tools: contains all expected tools")
{
  initialize_agent_system_empty();
  auto& system = mistral::agent::agent_system::instance();

  const auto& tools = system.global_tools();
  auto schema = tools.get_schema();

  // Check that all expected tools are registered
  std::vector<std::string> expected_tools = {
    "ls",
    "read",
#ifdef MISTRAL_ENABLE_TREE_SITTER
    "outline",
    "read_symbol",
    "find_symbol",
    "find_references",
    "ast_search",
#endif
    "note_list",
    "note_read",
    "note_write",
    "note_search",
    "ask_user"
  };

  for (const auto& tool_name : expected_tools)
  {
    CHECK(tools.has_tool(tool_name));
  }

  // Check schema contains all tools
  CHECK(schema.size() >= expected_tools.size());
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "global_tools: schema is valid JSON")
{
  initialize_agent_system_empty();
  auto& system = mistral::agent::agent_system::instance();

  const auto& tools = system.global_tools();
  auto schema = tools.get_schema();

  // Should be a JSON array
  CHECK(schema.is_array());
  CHECK(schema.size() > 0);

  // Each element should have the expected structure
  for (const auto& tool_schema : schema)
  {
    CHECK(tool_schema.contains("type"));
    CHECK(tool_schema["type"] == "function");
    CHECK(tool_schema.contains("function"));
    const auto& func = tool_schema["function"];
    CHECK(func.contains("name"));
    CHECK(func.contains("description"));
    CHECK(func.contains("parameters"));
  }
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "create_tool_registry: filters to allowed tools only")
{
  initialize_agent_system_empty();
  auto& system = mistral::agent::agent_system::instance();

  // Get a filtered registry
  std::vector<std::string> available_tools = {"ls", "read"};
  auto filtered_registry = system.create_tool_registry(available_tools);

  // Should only have the allowed tools
  CHECK(filtered_registry.has_tool("ls"));
  CHECK(filtered_registry.has_tool("read"));
  CHECK_FALSE(filtered_registry.has_tool("grep"));        // Should be filtered out
  CHECK_FALSE(filtered_registry.has_tool("note_list"));   // Should be filtered out
  CHECK_FALSE(filtered_registry.has_tool("note_read"));   // Should be filtered out
  CHECK_FALSE(filtered_registry.has_tool("note_write"));  // Should be filtered out
  CHECK_FALSE(filtered_registry.has_tool("note_search")); // Should be filtered out

  // Schema should only contain allowed tools
  auto schema = filtered_registry.get_schema();
  CHECK(schema.size() == 2); // Only ls and read
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "create_tool_registry: empty available_tools")
{
  initialize_agent_system_empty();
  auto& system = mistral::agent::agent_system::instance();

  std::vector<std::string> available_tools;
  auto filtered_registry = system.create_tool_registry(available_tools);

  // Empty available_tools means no tools - should be empty registry
  CHECK_FALSE(filtered_registry.has_tool("ls"));
  CHECK_FALSE(filtered_registry.has_tool("read"));
  CHECK_FALSE(filtered_registry.has_tool("grep"));
  CHECK(filtered_registry.get_schema().empty());
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "create_tool_registry: all tools wildcard")
{
  initialize_agent_system_empty();
  auto& system = mistral::agent::agent_system::instance();

  std::vector<std::string> available_tools = {"*"};
  auto filtered_registry = system.create_tool_registry(available_tools);

  // Wildcard should return all tools
  CHECK(filtered_registry.get_schema() == system.global_tools().get_schema());
  CHECK(filtered_registry.has_tool("ls"));
  CHECK(filtered_registry.has_tool("read"));
#ifdef MISTRAL_ENABLE_TREE_SITTER
  CHECK(filtered_registry.has_tool("outline"));
  CHECK(filtered_registry.has_tool("find_symbol"));
#endif
  CHECK(filtered_registry.has_tool("note_list"));
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "tool_registry: copy constructor works")
{
  initialize_agent_system_empty();
  auto& system = mistral::agent::agent_system::instance();

  const auto& global = system.global_tools();

  // Create a copy
  mistral::agent::tool_registry copy(global);

  // Check that the copy has the same tools
  CHECK(copy.get_schema() == global.get_schema());

  // Check that all tools are present
  std::vector<std::string> all_tools = {
    "ls",
    "read",
#ifdef MISTRAL_ENABLE_TREE_SITTER
    "outline",
    "read_symbol",
    "find_symbol",
    "find_references",
    "ast_search",
#endif
    "note_list",
    "note_read",
    "note_write",
    "note_search"
  };
  for (const auto& tool_name : all_tools)
  {
    CHECK(copy.has_tool(tool_name));
  }
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "tool_registry: copy assignment works")
{
  initialize_agent_system_empty();
  auto& system = mistral::agent::agent_system::instance();

  const auto& global = system.global_tools();

  // Create a new registry and assign
  mistral::agent::tool_registry copy;
  copy = global;

  // Check that the copy has the same tools
  CHECK(copy.get_schema() == global.get_schema());
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "tool_registry: copy contains all tools")
{
  initialize_agent_system_empty();
  auto& system = mistral::agent::agent_system::instance();

  const auto& global = system.global_tools();

  // Create a copy
  mistral::agent::tool_registry copy = global;

  // Verify the copy has all the same tools as global
  CHECK(copy.get_schema() == global.get_schema());
  CHECK(copy.has_tool("ls"));
  CHECK(copy.has_tool("read"));
#ifdef MISTRAL_ENABLE_TREE_SITTER
  CHECK(copy.has_tool("read_symbol"));
#endif
}

TEST_SUITE_END;

TEST_SUITE_BEGIN("agent_system::system_prompt");

TEST_CASE_FIXTURE(AgentSystemTestFixture, "resolve_system_prompt: ask_user intro appended when tool is available")
{
  initialize_agent_system_empty();

  mistral::agent::agent_config cfg;
  cfg.system_prompt = "You are a helpful assistant.";
  cfg.available_tools = {"ls", "read", "ask_user"};

  auto& sys = mistral::agent::agent_system::instance();
  std::string prompt = sys.resolve_system_prompt(cfg, "mistral-medium-latest");

  CHECK(prompt.find("You are a helpful assistant.") != std::string::npos);
  CHECK(prompt.find("## Asking the human") != std::string::npos);
  CHECK(prompt.find("`ask_user`") != std::string::npos);
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "resolve_system_prompt: ask_user intro appended for wildcard tools")
{
  initialize_agent_system_empty();

  mistral::agent::agent_config cfg;
  cfg.system_prompt = "You are a helpful assistant.";
  cfg.available_tools = {"*"};

  auto& sys = mistral::agent::agent_system::instance();
  std::string prompt = sys.resolve_system_prompt(cfg, "mistral-medium-latest");

  CHECK(prompt.find("## Asking the human") != std::string::npos);
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "resolve_system_prompt: no ask_user intro when tool is unavailable")
{
  initialize_agent_system_empty();

  mistral::agent::agent_config cfg;
  cfg.system_prompt = "You are a helpful assistant.";
  cfg.available_tools = {"ls", "read"};

  auto& sys = mistral::agent::agent_system::instance();
  std::string prompt = sys.resolve_system_prompt(cfg, "mistral-medium-latest");

  CHECK(prompt == "You are a helpful assistant.");
}

TEST_CASE_FIXTURE(
  AgentSystemTestFixture,
  "resolve_system_prompt: ask_user intro on empty prompt has no leading blank lines"
)
{
  initialize_agent_system_empty();

  mistral::agent::agent_config cfg;
  cfg.system_prompt = "";
  cfg.available_tools = {"ask_user"};

  auto& sys = mistral::agent::agent_system::instance();
  std::string prompt = sys.resolve_system_prompt(cfg, "mistral-medium-latest");

  // No leading blank lines: the section separator comes first.
  CHECK(prompt.starts_with("---"));
  CHECK(prompt.find("## Asking the human") != std::string::npos);
}

TEST_SUITE_END;

TEST_SUITE_BEGIN("agent_system::tool_instances");

TEST_CASE_FIXTURE(AgentSystemTestFixture, "file_tools: accessible via agent_system")
{
  initialize_agent_system_empty();
  auto& system = mistral::agent::agent_system::instance();

  auto& file_tools = system.file_tools();
  // Just check that we can access it without crashing
  CHECK(&file_tools != nullptr);
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "note_tools: accessible via agent_system")
{
  initialize_agent_system_empty();
  auto& system = mistral::agent::agent_system::instance();

  auto& note_tools = system.note_tools();
  CHECK(&note_tools != nullptr);
}

TEST_SUITE_END;

TEST_SUITE_BEGIN("agent_system::channels");

TEST_CASE_FIXTURE(AgentSystemTestFixture, "response_ch: returns null if not set")
{
  initialize_agent_system_empty();
  auto& system = mistral::agent::agent_system::instance();

  CHECK(system.response_ch() == nullptr);
}

TEST_SUITE_END;

TEST_SUITE_BEGIN("agent_system::transport_endpoints");

TEST_CASE_FIXTURE(AgentSystemTestFixture, "resolve_transport_endpoints: agents on one provider share a single endpoint")
{
  nlohmann::json config = {
    {"models",
     {{"mistral-large",
       {{"provider", "mistral"},
        {"model_id", "mistral-large-4"},
        {"api_key_env", "MISTRAL_API_KEY"},
        {"base_url", "https://api.mistral.ai"}}},
      {"mistral-small",
       {{"provider", "mistral"},
        {"model_id", "mistral-small-latest"},
        {"api_key_env", "MISTRAL_API_KEY"},
        {"base_url", "https://api.mistral.ai"}}}}},
    {"agents",
     {{"main", {{"model", "mistral-large"}}},
      {"researcher", {{"model", "mistral-small"}}}}},
    {"policy", {{"default", "deny"}}}
  };
  create_config_file(config);
  auto& system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir);

  auto endpoints = system.resolve_transport_endpoints();
  REQUIRE(endpoints.size() == 1);
  CHECK(endpoints[0].host == "api.mistral.ai");
  CHECK(endpoints[0].port == 443);
  CHECK(endpoints[0].api_key_env == "MISTRAL_API_KEY");
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "resolve_transport_endpoints: two providers yield two endpoints")
{
  nlohmann::json config = {
    {"models",
     {{"mistral-small",
       {{"provider", "mistral"},
        {"model_id", "mistral-small-latest"},
        {"api_key_env", "MISTRAL_API_KEY"},
        {"base_url", "https://api.mistral.ai"}}},
      {"greenpt-glm",
       {{"provider", "greenpt"},
        {"model_id", "glm-5.3"},
        {"api_key_env", "GREENPT_API_KEY"},
        {"base_url", "https://api.greenpt.ai"}}}}},
    {"agents",
     {{"main", {{"model", "greenpt-glm"}}},
      {"researcher", {{"model", "mistral-small"}}}}},
    {"policy", {{"default", "deny"}}}
  };
  create_config_file(config);
  auto& system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir);

  auto endpoints = system.resolve_transport_endpoints();
  REQUIRE(endpoints.size() == 2);
  // Agents are iterated in name order: main (greenpt) first, then researcher (mistral)
  CHECK(endpoints[0].host == "api.greenpt.ai");
  CHECK(endpoints[0].port == 443);
  CHECK(endpoints[0].api_key_env == "GREENPT_API_KEY");
  CHECK(endpoints[1].host == "api.mistral.ai");
  CHECK(endpoints[1].port == 443);
  CHECK(endpoints[1].api_key_env == "MISTRAL_API_KEY");
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "resolve_transport_endpoints: models no agent references are ignored")
{
  nlohmann::json config = {
    {"models",
     {{"mistral-large",
       {{"provider", "mistral"},
        {"model_id", "mistral-large-4"},
        {"api_key_env", "MISTRAL_API_KEY"},
        {"base_url", "https://api.mistral.ai"}}},
      {"greenpt-glm",
       {{"provider", "greenpt"},
        {"model_id", "glm-5.3"},
        {"api_key_env", "GREENPT_API_KEY"},
        {"base_url", "https://api.greenpt.ai"}}}}},
    {"agents",
     {{"main", {{"model", "mistral-large"}}}}},
    {"policy", {{"default", "deny"}}}
  };
  create_config_file(config);
  auto& system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir);

  auto endpoints = system.resolve_transport_endpoints();
  REQUIRE(endpoints.size() == 1);
  CHECK(endpoints[0].host == "api.mistral.ai");
  CHECK(endpoints[0].api_key_env == "MISTRAL_API_KEY");
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "resolve_transport_endpoints: explicit port in base_url is parsed")
{
  nlohmann::json config = {
    {"models",
     {{"proxy-model",
       {{"provider", "mistral"},
        {"model_id", "mistral-small-latest"},
        {"api_key_env", "MISTRAL_API_KEY"},
        {"base_url", "https://proxy.local:8443"}}}}},
    {"agents",
     {{"main", {{"model", "proxy-model"}}}}},
    {"policy", {{"default", "deny"}}}
  };
  create_config_file(config);
  auto& system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir);

  auto endpoints = system.resolve_transport_endpoints();
  REQUIRE(endpoints.size() == 1);
  CHECK(endpoints[0].host == "proxy.local");
  CHECK(endpoints[0].port == 8443);
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "resolve_transport_endpoints: missing base_url and api_key_env use the config loader defaults")
{
  nlohmann::json config = {
    {"models",
     {{"minimal-model", {{"provider", "mistral"}, {"model_id", "mistral-small-latest"}}}}},
    {"agents",
     {{"main", {{"model", "minimal-model"}}}}},
    {"policy", {{"default", "deny"}}}
  };
  create_config_file(config);
  auto& system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir);

  // The config loader defaults base_url to https://api.mistral.ai and
  // api_key_env to MISTRAL_API_KEY
  auto endpoints = system.resolve_transport_endpoints();
  REQUIRE(endpoints.size() == 1);
  CHECK(endpoints[0].host == "api.mistral.ai");
  CHECK(endpoints[0].port == 443);
  CHECK(endpoints[0].api_key_env == "MISTRAL_API_KEY");
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "resolve_transport_endpoints: explicitly empty api_key_env means no key required")
{
  nlohmann::json config = {
    {"models",
     {{"keyless-model",
       {{"provider", "mistral"},
        {"model_id", "mistral-small-latest"},
        {"api_key_env", ""},
        {"base_url", "https://api.mistral.ai"}}}}},
    {"agents",
     {{"main", {{"model", "keyless-model"}}}}},
    {"policy", {{"default", "deny"}}}
  };
  create_config_file(config);
  auto& system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir);

  auto endpoints = system.resolve_transport_endpoints();
  REQUIRE(endpoints.size() == 1);
  CHECK(endpoints[0].host == "api.mistral.ai");
  CHECK(endpoints[0].api_key_env.empty());
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "resolve_transport_endpoints: same host with different api_key_env are distinct endpoints")
{
  nlohmann::json config = {
    {"models",
     {{"model-a",
       {{"provider", "mistral"},
        {"model_id", "a"},
        {"api_key_env", "KEY_A"},
        {"base_url", "https://api.mistral.ai"}}},
      {"model-b",
       {{"provider", "mistral"},
        {"model_id", "b"},
        {"api_key_env", "KEY_B"},
        {"base_url", "https://api.mistral.ai"}}}}},
    {"agents",
     {{"agent-a", {{"model", "model-a"}}},
      {"agent-b", {{"model", "model-b"}}}}},
    {"policy", {{"default", "deny"}}}
  };
  create_config_file(config);
  auto& system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir);

  auto endpoints = system.resolve_transport_endpoints();
  REQUIRE(endpoints.size() == 2);
  CHECK(endpoints[0].api_key_env == "KEY_A");
  CHECK(endpoints[1].api_key_env == "KEY_B");
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "resolve_transport_endpoints: no agents yields no endpoints")
{
  initialize_agent_system_empty();
  auto& system = mistral::agent::agent_system::instance();
  CHECK(system.resolve_transport_endpoints().empty());
}

TEST_SUITE_END;

TEST_SUITE_BEGIN("agent_system::endpoint_channels");

TEST_CASE_FIXTURE(AgentSystemTestFixture, "request_ch_for: returns a channel for a configured agent")
{
  initialize_agent_system_with_main_agent();
  auto& system = mistral::agent::agent_system::instance();

  CHECK(system.request_ch_for("main") != nullptr);
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "request_ch_for: agents sharing an endpoint share its channel")
{
  nlohmann::json config = {
    {"models",
     {{"mistral-large",
       {{"provider", "mistral"},
        {"model_id", "mistral-large-4"},
        {"api_key_env", "MISTRAL_API_KEY"},
        {"base_url", "https://api.mistral.ai"}}},
      {"mistral-small",
       {{"provider", "mistral"},
        {"model_id", "mistral-small-latest"},
        {"api_key_env", "MISTRAL_API_KEY"},
        {"base_url", "https://api.mistral.ai"}}}}},
    {"agents",
     {{"main", {{"model", "mistral-large"}}},
      {"researcher", {{"model", "mistral-small"}}}}},
    {"policy", {{"default", "deny"}}}
  };
  create_config_file(config);
  auto& system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir);

  CHECK(system.request_ch_for("main") == system.request_ch_for("researcher"));
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "request_ch_for: agents on different providers get different channels")
{
  nlohmann::json config = {
    {"models",
     {{"mistral-small",
       {{"provider", "mistral"},
        {"model_id", "mistral-small-latest"},
        {"api_key_env", "MISTRAL_API_KEY"},
        {"base_url", "https://api.mistral.ai"}}},
      {"greenpt-glm",
       {{"provider", "greenpt"},
        {"model_id", "glm-5.3"},
        {"api_key_env", "GREENPT_API_KEY"},
        {"base_url", "https://api.greenpt.ai"}}}}},
    {"agents",
     {{"main", {{"model", "greenpt-glm"}}},
      {"researcher", {{"model", "mistral-small"}}}}},
    {"policy", {{"default", "deny"}}}
  };
  create_config_file(config);
  auto& system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir);

  CHECK(system.request_ch_for("main") != system.request_ch_for("researcher"));
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "request_ch_for: one channel per endpoint")
{
  nlohmann::json config = {
    {"models",
     {{"mistral-large",
       {{"provider", "mistral"},
        {"model_id", "mistral-large-4"},
        {"api_key_env", "MISTRAL_API_KEY"},
        {"base_url", "https://api.mistral.ai"}}},
      {"mistral-small",
       {{"provider", "mistral"},
        {"model_id", "mistral-small-latest"},
        {"api_key_env", "MISTRAL_API_KEY"},
        {"base_url", "https://api.mistral.ai"}}},
      {"greenpt-glm",
       {{"provider", "greenpt"},
        {"model_id", "glm-5.3"},
        {"api_key_env", "GREENPT_API_KEY"},
        {"base_url", "https://api.greenpt.ai"}}}}},
    {"agents",
     {{"main", {{"model", "mistral-large"}}},
      {"researcher", {{"model", "mistral-small"}}},
      {"helper", {{"model", "greenpt-glm"}}}}},
    {"policy", {{"default", "deny"}}}
  };
  create_config_file(config);
  auto& system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir);

  // main and researcher share the Mistral endpoint; helper is on GreenPT
  auto main_ch = system.request_ch_for("main");
  auto researcher_ch = system.request_ch_for("researcher");
  auto helper_ch = system.request_ch_for("helper");
  CHECK(main_ch == researcher_ch);
  CHECK(main_ch != helper_ch);
  CHECK(system.resolve_transport_endpoints().size() == 2);
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "request_ch_for: throws for unknown agent")
{
  initialize_agent_system_with_main_agent();
  auto& system = mistral::agent::agent_system::instance();

  CHECK_THROWS_AS(system.request_ch_for("nonexistent"), std::runtime_error);
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "request_ch_for: throws for agent with unknown model")
{
  nlohmann::json config = {
    {"models",
     {{"mistral-large",
       {{"provider", "mistral"},
        {"model_id", "mistral-large-4"},
        {"api_key_env", "MISTRAL_API_KEY"},
        {"base_url", "https://api.mistral.ai"}}}}},
    {"agents",
     {{"main", {{"model", "missing-model"}}}}},
    {"policy", {{"default", "deny"}}}
  };
  create_config_file(config);
  auto& system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir);

  CHECK_THROWS_AS(system.request_ch_for("main"), std::runtime_error);
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "request_ch_for: re-initialize rebuilds the endpoint map")
{
  nlohmann::json mistral_config = {
    {"models",
     {{"mistral-large",
       {{"provider", "mistral"},
        {"model_id", "mistral-large-4"},
        {"api_key_env", "MISTRAL_API_KEY"},
        {"base_url", "https://api.mistral.ai"}}}}},
    {"agents",
     {{"main", {{"model", "mistral-large"}}}}},
    {"policy", {{"default", "deny"}}}
  };
  create_config_file(mistral_config);
  auto& system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir);
  auto before = system.request_ch_for("main");

  nlohmann::json greenpt_config = {
    {"models",
     {{"greenpt-glm",
       {{"provider", "greenpt"},
        {"model_id", "glm-5.3"},
        {"api_key_env", "GREENPT_API_KEY"},
        {"base_url", "https://api.greenpt.ai"}}}}},
    {"agents",
     {{"main", {{"model", "greenpt-glm"}}}}},
    {"policy", {{"default", "deny"}}}
  };
  create_config_file(greenpt_config);
  system.initialize(config_file, base_dir, notes_dir);
  auto after = system.request_ch_for("main");

  CHECK(before != after);
  auto endpoints = system.resolve_transport_endpoints();
  REQUIRE(endpoints.size() == 1);
  CHECK(endpoints[0].host == "api.greenpt.ai");
}

TEST_SUITE_END;

TEST_SUITE_BEGIN("agent_system::transport_keys");

TEST_CASE_FIXTURE(AgentSystemTestFixture, "validate_transport_keys: throws when a required key is missing")
{
  nlohmann::json config = {
    {"models",
     {{"test-model",
       {{"provider", "mistral"},
        {"model_id", "test-model-latest"},
        {"api_key_env", "MISTRAL_TRANSPORT_TEST_KEY"},
        {"base_url", "https://api.mistral.ai"}}}}},
    {"agents",
     {{"main", {{"model", "test-model"}}}}},
    {"policy", {{"default", "deny"}}}
  };
  create_config_file(config);
  auto& system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir);

  unsetenv("MISTRAL_TRANSPORT_TEST_KEY");
  try
  {
    system.validate_transport_keys();
    FAIL("expected std::runtime_error");
  }
  catch (const std::runtime_error& e)
  {
    CHECK(std::string(e.what()).find("MISTRAL_TRANSPORT_TEST_KEY") != std::string::npos);
    CHECK(std::string(e.what()).find("api.mistral.ai") != std::string::npos);
  }
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "validate_transport_keys: passes when the key is set")
{
  nlohmann::json config = {
    {"models",
     {{"test-model",
       {{"provider", "mistral"},
        {"model_id", "test-model-latest"},
        {"api_key_env", "MISTRAL_TRANSPORT_TEST_KEY"},
        {"base_url", "https://api.mistral.ai"}}}}},
    {"agents",
     {{"main", {{"model", "test-model"}}}}},
    {"policy", {{"default", "deny"}}}
  };
  create_config_file(config);
  auto& system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir);

  setenv("MISTRAL_TRANSPORT_TEST_KEY", "test-key", 1);
  CHECK_NOTHROW(system.validate_transport_keys());
  unsetenv("MISTRAL_TRANSPORT_TEST_KEY");
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "validate_transport_keys: endpoint without api_key_env needs no key")
{
  nlohmann::json config = {
    {"models",
     {{"keyless-model",
       {{"provider", "mistral"},
        {"model_id", "mistral-small-latest"},
        {"api_key_env", ""},
        {"base_url", "https://api.mistral.ai"}}}}},
    {"agents",
     {{"main", {{"model", "keyless-model"}}}}},
    {"policy", {{"default", "deny"}}}
  };
  create_config_file(config);
  auto& system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir);

  CHECK_NOTHROW(system.validate_transport_keys());
}

TEST_SUITE_END;

TEST_SUITE_BEGIN("agent_system::slash_commands");

// Helper struct for testing slash commands
struct slash_command_executor : public dripcore::task
{
  slash_command_executor(
    mistral::slash_command::slash_command* cmd,
    std::string args,
    mistral::agent::agent_system& sys,
    std::string& out_result
  )
    : cmd_(cmd), args_(std::move(args)), sys_(sys), out_result_(out_result)
  {
  }

  dripcore::awaitable<> body() override
  {
    auto result = co_await cmd_->execute(*this, args_, sys_);
    out_result_ = result;
    co_return;
  }

  mistral::slash_command::slash_command* cmd_;
  std::string args_;
  mistral::agent::agent_system& sys_;
  std::string& out_result_;
};

TEST_CASE_FIXTURE(AgentSystemTestFixture, "slash_commands: registered after initialization")
{
  initialize_agent_system_empty();
  auto& system = mistral::agent::agent_system::instance();

  auto registry = system.slash_commands();
  REQUIRE(registry != nullptr);

  // Check that built-in commands are registered
  CHECK(registry->has_command("help"));
  CHECK(registry->has_command("clear"));
  CHECK(registry->has_command("model"));
  CHECK(registry->has_command("tools"));
  CHECK(registry->has_command("exit"));
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "slash_commands: help command works")
{
  initialize_agent_system_empty();
  auto& system = mistral::agent::agent_system::instance();

  auto registry = system.slash_commands();
  const auto* help_cmd = registry->get_command("help");
  REQUIRE(help_cmd != nullptr);

  std::string result;

  dripcore::scheduler sched;
  sched.spawn<slash_command_executor>(const_cast<mistral::slash_command::slash_command*>(help_cmd), "", system, result);
  sched.run();

  CHECK(!result.empty());
  CHECK(result.find("Available slash commands") != std::string::npos);
  CHECK(result.find("/help") != std::string::npos);
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "slash_commands: clear command works")
{
  initialize_agent_system_empty();
  auto& system = mistral::agent::agent_system::instance();

  auto registry = system.slash_commands();
  const auto* clear_cmd = registry->get_command("clear");
  REQUIRE(clear_cmd != nullptr);

  std::string result;

  dripcore::scheduler sched;
  sched.spawn<slash_command_executor>(
    const_cast<mistral::slash_command::slash_command*>(clear_cmd), "", system, result
  );
  sched.run();

  CHECK(!result.empty());
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "slash_commands: exit command works")
{
  initialize_agent_system_empty();
  auto& system = mistral::agent::agent_system::instance();

  auto registry = system.slash_commands();
  const auto* exit_cmd = registry->get_command("exit");
  REQUIRE(exit_cmd != nullptr);

  std::string result;

  dripcore::scheduler sched;
  sched.spawn<slash_command_executor>(const_cast<mistral::slash_command::slash_command*>(exit_cmd), "", system, result);
  sched.run();

  CHECK(result.empty());
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "slash_commands: model command executes with agent_system")
{
  initialize_agent_system_with_main_agent();

  auto& system = mistral::agent::agent_system::instance();
  auto registry = system.slash_commands();
  const auto* model_cmd = registry->get_command("model");
  REQUIRE(model_cmd != nullptr);

  std::string result;

  dripcore::scheduler sched;
  sched.spawn<slash_command_executor>(
    const_cast<mistral::slash_command::slash_command*>(model_cmd), "", system, result
  );
  sched.run();

  CHECK(!result.empty());
  CHECK(result.find("mistral-medium-latest") != std::string::npos);
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "slash_commands: tools command executes with agent_system")
{
  initialize_agent_system_with_main_agent();

  auto& system = mistral::agent::agent_system::instance();
  auto registry = system.slash_commands();
  const auto* tools_cmd = registry->get_command("tools");
  REQUIRE(tools_cmd != nullptr);

  std::string result;

  dripcore::scheduler sched;
  sched.spawn<slash_command_executor>(
    const_cast<mistral::slash_command::slash_command*>(tools_cmd), "", system, result
  );
  sched.run();

  CHECK(!result.empty());
  CHECK(result.find("Available tools") != std::string::npos);
  CHECK(result.find("ls") != std::string::npos);
}

TEST_SUITE_END;

TEST_SUITE_BEGIN("agent_system::singleton");

TEST_CASE("instance: returns same instance")
{
  auto& instance1 = mistral::agent::agent_system::instance();
  auto& instance2 = mistral::agent::agent_system::instance();

  CHECK(&instance1 == &instance2);
}

TEST_SUITE_END;

TEST_SUITE_BEGIN("agent_system_patchset_storage");

TEST_CASE("patchset storage - store and retrieve single patchset")
{
  auto& agent_sys = mistral::agent::agent_system::instance();

  // Clear any existing patchset
  agent_sys.remove_patchset("test-patchset");
  CHECK(!agent_sys.has_patchset());

  // Create test edits
  std::vector<mistral::tools::file_edit_t> edits;
  edits.push_back({.file_path = "src/main.cpp", .search_text = "old", .replace_text = "new", .patch_number = 1});

  // Store patchset
  agent_sys.store_patchset("test-patchset", edits);

  // Verify storage
  CHECK(agent_sys.has_patchset());
  auto current_id = agent_sys.get_current_patchset_id();
  CHECK(current_id.has_value());
  CHECK(*current_id == "test-patchset");

  // Retrieve patchset
  auto retrieved = agent_sys.get_patchset("test-patchset");
  REQUIRE(retrieved.has_value());
  CHECK(retrieved->patchset_id == "test-patchset");
  CHECK(retrieved->edits.size() == 1);
  CHECK(retrieved->edits[0].file_path == "src/main.cpp");
  CHECK(retrieved->edits[0].patch_number == 1);

  // Clean up
  agent_sys.remove_patchset("test-patchset");
  CHECK(!agent_sys.has_patchset());
}

TEST_CASE("patchset storage - overwrite previous patchset")
{
  auto& agent_sys = mistral::agent::agent_system::instance();

  // Store first patchset
  std::vector<mistral::tools::file_edit_t> edits1;
  edits1.push_back({.file_path = "file1.cpp", .search_text = "a", .replace_text = "b", .patch_number = 1});
  agent_sys.store_patchset("patchset-1", edits1);

  CHECK(agent_sys.get_current_patchset_id() == "patchset-1");

  // Store second patchset - should overwrite
  std::vector<mistral::tools::file_edit_t> edits2;
  edits2.push_back({.file_path = "file2.cpp", .search_text = "x", .replace_text = "y", .patch_number = 1});
  agent_sys.store_patchset("patchset-2", edits2);

  CHECK(agent_sys.get_current_patchset_id() == "patchset-2");

  // First patchset should no longer be retrievable
  auto old_patchset = agent_sys.get_patchset("patchset-1");
  CHECK(!old_patchset.has_value());

  // Second patchset should be retrievable
  auto new_patchset = agent_sys.get_patchset("patchset-2");
  REQUIRE(new_patchset.has_value());
  CHECK(new_patchset->edits[0].file_path == "file2.cpp");

  // Clean up
  agent_sys.remove_patchset("patchset-2");
}

TEST_CASE("patchset storage - get_patchset returns nullopt for non-existent ID")
{
  auto& agent_sys = mistral::agent::agent_system::instance();

  // Clear any existing patchset
  agent_sys.remove_patchset("existing-patchset");

  // Try to get non-existent patchset
  auto result = agent_sys.get_patchset("does-not-exist");
  CHECK(!result.has_value());

  // Store a patchset
  std::vector<mistral::tools::file_edit_t> edits;
  edits.push_back({.file_path = "test.cpp", .search_text = "a", .replace_text = "b", .patch_number = 1});
  agent_sys.store_patchset("existing-patchset", edits);

  // Try to get different non-existent patchset
  result = agent_sys.get_patchset("still-does-not-exist");
  CHECK(!result.has_value());

  // Clean up
  agent_sys.remove_patchset("existing-patchset");
}

TEST_CASE("patchset storage - get_current_patchset_id returns nullopt when empty")
{
  auto& agent_sys = mistral::agent::agent_system::instance();

  // Clear any existing patchset
  agent_sys.remove_patchset("clear-empty");

  auto id = agent_sys.get_current_patchset_id();
  CHECK(!id.has_value());
}

TEST_CASE("patchset storage - multiple edits in patchset")
{
  auto& agent_sys = mistral::agent::agent_system::instance();

  // Clear any existing patchset
  agent_sys.remove_patchset("multi-edit");

  // Create patchset with multiple edits
  std::vector<mistral::tools::file_edit_t> edits;
  edits.push_back({.file_path = "src/main.cpp", .search_text = "old1", .replace_text = "new1", .patch_number = 1});
  edits.push_back({.file_path = "src/utils.cpp", .search_text = "old2", .replace_text = "new2", .patch_number = 2});
  edits.push_back({.file_path = "lib/helper.hpp", .search_text = "old3", .replace_text = "new3", .patch_number = 3});

  agent_sys.store_patchset("multi-edit", edits);

  auto retrieved = agent_sys.get_patchset("multi-edit");
  REQUIRE(retrieved.has_value());
  CHECK(retrieved->edits.size() == 3);
  CHECK(retrieved->edits[0].patch_number == 1);
  CHECK(retrieved->edits[1].patch_number == 2);
  CHECK(retrieved->edits[2].patch_number == 3);

  // Clean up
  agent_sys.remove_patchset("multi-edit");
}

TEST_SUITE_END();
