#include <doctest/doctest.h>
#include "agent/agent_system.h"
#include "agent/agent_config.h"
#include "agent/tool_registry.h"
#include "tools/file_tools.h"
#include "tools/note_tools.h"
#include "tools/grep_tools.h"
#include "tools/exec_tools.h"

#include <dripcore/scheduler.h>
#include <dripcore/task.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <memory>

namespace mistral::agent
{
  namespace fs = std::filesystem;
  using json = nlohmann::ordered_json;
}

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

  void create_config_file(const std::string &content)
  {
    std::ofstream out(config_file);
    out << content;
  }

  void create_config_file(const nlohmann::json &content)
  {
    std::ofstream out(config_file);
    out << content.dump(2);
  }

  // Helper to create a standard config with a main agent
  nlohmann::json create_standard_config(
      const std::string& model_name = "mistral-medium",
      const std::string& model_id = "mistral-medium-latest")
  {
    return {
      {"models", {
        {model_name, {
          {"provider", "mistral"},
          {"model_id", model_id},
          {"api_key_env", "MISTRAL_API_KEY"},
          {"base_url", "https://api.mistral.ai"},
          {"api_version", "v1"}
        }}
      }},
      {"agents", {
        {"main", {
          {"model", model_name},
          {"system_prompt", "You are a helpful assistant."},
          {"tool_policy", "deny"}
        }}
      }},
      {"policy", {
        {"default", "deny"}
      }}
    };
  }

  // Helper to initialize agent_system with standard config
  void initialize_agent_system_with_main_agent(
      const std::string& model_name = "mistral-medium",
      const std::string& model_id = "mistral-medium-latest")
  {
    create_config_file(create_standard_config(model_name, model_id));
    auto &system = mistral::agent::agent_system::instance();
    system.initialize(config_file, base_dir, notes_dir, nullptr);
  }

  // Helper to initialize agent_system with empty config (for tests that don't need config data)
  void initialize_agent_system_empty()
  {
    create_config_file(nlohmann::json({}));
    auto &system = mistral::agent::agent_system::instance();
    system.initialize(config_file, base_dir, notes_dir, nullptr);
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
    {"models", {
      {"mistral-medium", {
        {"provider", "mistral"},
        {"model_id", "mistral-medium-latest"},
        {"api_key_env", "MISTRAL_API_KEY"},
        {"base_url", "https://api.mistral.ai"},
        {"api_version", "v1"}
      }}
    }},
    {"agents", {
      {"main", {
        {"model", "mistral-medium"},
        {"system_prompt", "You are a helpful assistant."},
        {"tool_policy", "deny"},
        {"available_tools", {"ls", "read"}}
      }}
    }},
    {"policy", {
      {"default", "deny"}
    }}
  };

  create_config_file(config);

  auto &system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir, nullptr);

  // Check models
  CHECK(system.has_model_config("mistral-medium"));
  const auto &model = system.get_model_config("mistral-medium");
  CHECK(model.provider == "mistral");
  CHECK(model.model_id == "mistral-medium-latest");
  CHECK(model.api_key_env == "MISTRAL_API_KEY");
  CHECK(model.base_url == "https://api.mistral.ai");
  CHECK(model.api_version == "v1");

  // Check agents
  CHECK(system.has_agent_config("main"));
  const auto &agent = system.get_agent_config("main");
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
          "/nonexistent/path/mistral-cli.json", base_dir, notes_dir, nullptr),
      std::runtime_error);
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "load_config_from_file: invalid JSON throws")
{
  create_config_file(std::string("this is not valid json {"));
  CHECK_THROWS_AS(
      mistral::agent::agent_system::instance().initialize(
          config_file, base_dir, notes_dir, nullptr),
      std::runtime_error);
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "load_config_from_file: empty config")
{
  create_config_file(nlohmann::json({}));
  auto &system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir, nullptr);

  CHECK(system.get_config().models.empty());
  CHECK(system.get_config().agents.empty());
  CHECK(system.get_config().default_policy == "deny");
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "global_tools: exec tool is registered")
{
  using namespace mistral::agent;

  create_config_file(nlohmann::json({}));
  auto &system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir, nullptr);

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
    {"models", {
      {"mistral-small", {
        {"provider", "mistral"},
        {"model_id", "mistral-small-latest"}
      }},
      {"gpt-4o-mini", {
        {"provider", "openai"},
        {"model_id", "gpt-4o-mini"},
        {"api_key_env", "OPENAI_API_KEY"},
        {"base_url", "https://api.openai.com"}
      }}
    }},
    {"agents", {
      {"main", {
        {"model", "mistral-small"},
        {"tool_policy", "allow"}
      }},
      {"read_only", {
        {"model", "gpt-4o-mini"},
        {"system_prompt", "Read-only assistant"},
        {"tool_policy", "require_approval"},
        {"available_tools", {"read", "note_read"}}
      }}
    }},
    {"policy", {
      {"default", "allow"}
    }}
  };

  create_config_file(config);
  auto &system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir, nullptr);

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
  const auto &main_agent = system.get_agent_config("main");
  CHECK(main_agent.model == "mistral-small");
  CHECK(main_agent.tool_policy == "allow");

  const auto &ro_agent = system.get_agent_config("read_only");
  CHECK(ro_agent.model == "gpt-4o-mini");
  CHECK(ro_agent.system_prompt == "Read-only assistant");
  CHECK(ro_agent.tool_policy == "require_approval");
  CHECK(ro_agent.available_tools.size() == 2);
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "get_agent_config: throws for missing agent")
{
  create_config_file(nlohmann::json({{"models", {}}, {"agents", {}}, {"policy", {"default", "deny"}}}));
  auto &system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir, nullptr);

  CHECK_THROWS_AS(system.get_agent_config("nonexistent"), std::runtime_error);
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "get_model_config: throws for missing model")
{
  create_config_file(nlohmann::json({{"models", {}}, {"agents", {}}, {"policy", {"default", "deny"}}}));
  auto &system = mistral::agent::agent_system::instance();
  system.initialize(config_file, base_dir, notes_dir, nullptr);

  CHECK_THROWS_AS(system.get_model_config("nonexistent"), std::runtime_error);
}

TEST_SUITE_END;

TEST_SUITE_BEGIN("agent_system::backward_compatibility");

TEST_SUITE_END;

TEST_SUITE_BEGIN("agent_system::tool_registry");

TEST_CASE_FIXTURE(AgentSystemTestFixture, "global_tools: contains all expected tools")
{
  initialize_agent_system_empty();
  auto &system = mistral::agent::agent_system::instance();

  const auto &tools = system.global_tools();
  auto schema = tools.get_schema();

  // Check that all expected tools are registered
  std::vector<std::string> expected_tools = {
      "ls", "read", "grep", "note_list", "note_read", "note_write", "note_search"};

  for (const auto &tool_name : expected_tools)
  {
    CHECK(tools.has_tool(tool_name));
  }

  // Check schema contains all tools
  CHECK(schema.size() >= expected_tools.size());
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "global_tools: schema is valid JSON")
{
  initialize_agent_system_empty();
  auto &system = mistral::agent::agent_system::instance();

  const auto &tools = system.global_tools();
  auto schema = tools.get_schema();

  // Should be a JSON array
  CHECK(schema.is_array());
  CHECK(schema.size() > 0);

  // Each element should have the expected structure
  for (const auto &tool_schema : schema)
  {
    CHECK(tool_schema.contains("type"));
    CHECK(tool_schema["type"] == "function");
    CHECK(tool_schema.contains("function"));
    const auto &func = tool_schema["function"];
    CHECK(func.contains("name"));
    CHECK(func.contains("description"));
    CHECK(func.contains("parameters"));
  }
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "create_tool_registry: filters to allowed tools only")
{
  initialize_agent_system_empty();
  auto &system = mistral::agent::agent_system::instance();

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
  auto &system = mistral::agent::agent_system::instance();

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
  auto &system = mistral::agent::agent_system::instance();

  std::vector<std::string> available_tools = {"*"};
  auto filtered_registry = system.create_tool_registry(available_tools);

  // Wildcard should return all tools
  CHECK(filtered_registry.get_schema() == system.global_tools().get_schema());
  CHECK(filtered_registry.has_tool("ls"));
  CHECK(filtered_registry.has_tool("read"));
  CHECK(filtered_registry.has_tool("grep"));
  CHECK(filtered_registry.has_tool("note_list"));
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "tool_registry: copy constructor works")
{
  initialize_agent_system_empty();
  auto &system = mistral::agent::agent_system::instance();

  const auto &global = system.global_tools();

  // Create a copy
  mistral::agent::tool_registry copy(global);

  // Check that the copy has the same tools
  CHECK(copy.get_schema() == global.get_schema());

  // Check that all tools are present
  for (const auto &tool_name : {"ls", "read", "grep", "note_list", "note_read", "note_write", "note_search"})
  {
    CHECK(copy.has_tool(tool_name));
  }
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "tool_registry: copy assignment works")
{
  initialize_agent_system_empty();
  auto &system = mistral::agent::agent_system::instance();

  const auto &global = system.global_tools();

  // Create a new registry and assign
  mistral::agent::tool_registry copy;
  copy = global;

  // Check that the copy has the same tools
  CHECK(copy.get_schema() == global.get_schema());
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "tool_registry: copy contains all tools")
{
  initialize_agent_system_empty();
  auto &system = mistral::agent::agent_system::instance();

  const auto &global = system.global_tools();

  // Create a copy
  mistral::agent::tool_registry copy = global;

  // Verify the copy has all the same tools as global
  CHECK(copy.get_schema() == global.get_schema());
  CHECK(copy.has_tool("ls"));
  CHECK(copy.has_tool("read"));
  CHECK(copy.has_tool("grep"));
}

TEST_SUITE_END;

TEST_SUITE_BEGIN("agent_system::tool_instances");

TEST_CASE_FIXTURE(AgentSystemTestFixture, "file_tools: accessible via agent_system")
{
  initialize_agent_system_empty();
  auto &system = mistral::agent::agent_system::instance();

  auto &file_tools = system.file_tools();
  // Just check that we can access it without crashing
  CHECK(&file_tools != nullptr);
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "note_tools: accessible via agent_system")
{
  initialize_agent_system_empty();
  auto &system = mistral::agent::agent_system::instance();

  auto &note_tools = system.note_tools();
  CHECK(&note_tools != nullptr);
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "grep_tools: accessible via agent_system")
{
  initialize_agent_system_empty();
  auto &system = mistral::agent::agent_system::instance();

  auto &grep_tools = system.grep_tools();
  CHECK(&grep_tools != nullptr);
}

TEST_SUITE_END;

TEST_SUITE_BEGIN("agent_system::channels");

TEST_CASE_FIXTURE(AgentSystemTestFixture, "request_ch: returns null if not set")
{
  initialize_agent_system_empty();
  auto &system = mistral::agent::agent_system::instance();

  CHECK(system.request_ch() == nullptr);
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "response_ch: returns null if not set")
{
  initialize_agent_system_empty();
  auto &system = mistral::agent::agent_system::instance();

  CHECK(system.response_ch() == nullptr);
}

TEST_SUITE_END;

TEST_SUITE_BEGIN("agent_system::slash_commands");

TEST_CASE_FIXTURE(AgentSystemTestFixture, "slash_commands: registered after initialization")
{
  initialize_agent_system_empty();
  auto &system = mistral::agent::agent_system::instance();

  auto& registry = mistral::console::slash_command_registry::instance();

  // Check that built-in commands are registered
  CHECK(registry.has_command("help"));
  CHECK(registry.has_command("clear"));
  CHECK(registry.has_command("model"));
  CHECK(registry.has_command("tools"));
  CHECK(registry.has_command("exit"));
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "slash_commands: help command works")
{
  initialize_agent_system_empty();
  auto &system = mistral::agent::agent_system::instance();

  auto& registry = mistral::console::slash_command_registry::instance();
  const auto* help_cmd = registry.get_command("help");
  REQUIRE(help_cmd != nullptr);

  // Test help with no args (need const_cast because execute_immediate is non-const)
  std::string result = const_cast<mistral::console::slash_command*>(help_cmd)->execute_immediate("");
  CHECK(!result.empty());
  CHECK(result.find("Available slash commands") != std::string::npos);
  CHECK(result.find("/help") != std::string::npos);
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "slash_commands: model command requires agent context")
{
  initialize_agent_system_empty();
  auto &system = mistral::agent::agent_system::instance();

  auto& registry = mistral::console::slash_command_registry::instance();
  const auto* model_cmd = registry.get_command("model");
  REQUIRE(model_cmd != nullptr);

  // Model command should require AGENT context
  CHECK(model_cmd->context() == mistral::console::slash_command::execution_context::AGENT);
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "slash_commands: clear command is immediate")
{
  initialize_agent_system_empty();
  auto &system = mistral::agent::agent_system::instance();

  auto& registry = mistral::console::slash_command_registry::instance();
  const auto* clear_cmd = registry.get_command("clear");
  REQUIRE(clear_cmd != nullptr);

  // Clear command should be IMMEDIATE
  CHECK(clear_cmd->context() == mistral::console::slash_command::execution_context::IMMEDIATE);

  // Should be executable (need const_cast because execute_immediate is non-const)
  std::string result = const_cast<mistral::console::slash_command*>(clear_cmd)->execute_immediate("");
  CHECK(!result.empty());
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "slash_commands: exit command is immediate")
{
  initialize_agent_system_empty();
  auto &system = mistral::agent::agent_system::instance();

  auto& registry = mistral::console::slash_command_registry::instance();
  const auto* exit_cmd = registry.get_command("exit");
  REQUIRE(exit_cmd != nullptr);

  // Exit command should be IMMEDIATE
  CHECK(exit_cmd->context() == mistral::console::slash_command::execution_context::IMMEDIATE);

  // Should return empty string to signal exit (need const_cast because execute_immediate is non-const)
  std::string result = const_cast<mistral::console::slash_command*>(exit_cmd)->execute_immediate("");
  CHECK(result.empty());
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "slash_commands: registry needs_agent_execution works")
{
  initialize_agent_system_empty();
  auto &system = mistral::agent::agent_system::instance();

  auto& registry = mistral::console::slash_command_registry::instance();

  CHECK(registry.needs_agent_execution("help") == false);
  CHECK(registry.needs_agent_execution("clear") == false);
  CHECK(registry.needs_agent_execution("model") == true);
  CHECK(registry.needs_agent_execution("tools") == true);
}

// Helper struct for testing slash commands with AGENT context
struct slash_command_executor : public dripcore::task {
  slash_command_executor(
      mistral::console::slash_command* cmd,
      dripcore::task& parent_task,
      const std::string& args,
      mistral::agent::agent_system& sys,
      std::string& out_result)
    : cmd_(cmd), parent_task_(parent_task), args_(args), sys_(sys), out_result_(out_result) {}

  dripcore::task_handle execute() {
    auto result = co_await cmd_->execute_agent(parent_task_, args_, sys_);
    out_result_ = result;
    co_return;
  }

  mistral::console::slash_command* cmd_;
  dripcore::task& parent_task_;
  const std::string& args_;
  mistral::agent::agent_system& sys_;
  std::string& out_result_;
};

struct slash_command_parent_task : public dripcore::task {
  dripcore::task_handle execute() { co_return; }
};

TEST_CASE_FIXTURE(AgentSystemTestFixture, "slash_commands: model command executes with agent_system")
{
  initialize_agent_system_with_main_agent();

  auto& registry = mistral::console::slash_command_registry::instance();
  const auto* model_cmd = registry.get_command("model");
  REQUIRE(model_cmd != nullptr);

  slash_command_parent_task parent;
  auto &system = mistral::agent::agent_system::instance();
  std::string result;

  dripcore::scheduler sched;
  sched.spawn<slash_command_executor>(
      const_cast<mistral::console::slash_command*>(model_cmd),
      parent,
      "",
      system,
      result);
  sched.run();

  CHECK(!result.empty());
  CHECK(result.find("mistral-medium-latest") != std::string::npos);
}

TEST_CASE_FIXTURE(AgentSystemTestFixture, "slash_commands: tools command executes with agent_system")
{
  initialize_agent_system_with_main_agent();

  auto& registry = mistral::console::slash_command_registry::instance();
  const auto* tools_cmd = registry.get_command("tools");
  REQUIRE(tools_cmd != nullptr);

  slash_command_parent_task parent;
  auto &system = mistral::agent::agent_system::instance();
  std::string result;

  dripcore::scheduler sched;
  sched.spawn<slash_command_executor>(
      const_cast<mistral::console::slash_command*>(tools_cmd),
      parent,
      "",
      system,
      result);
  sched.run();

  CHECK(!result.empty());
  CHECK(result.find("Available tools") != std::string::npos);
  CHECK(result.find("ls") != std::string::npos);
}

TEST_SUITE_END;

TEST_SUITE_BEGIN("agent_system::singleton");

TEST_CASE("instance: returns same instance")
{
  auto &instance1 = mistral::agent::agent_system::instance();
  auto &instance2 = mistral::agent::agent_system::instance();

  CHECK(&instance1 == &instance2);
}

TEST_SUITE_END;
