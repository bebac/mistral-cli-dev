#include "agent/approval_renderers.h"

#include "agent/agent_system.h"
#include "agent/tool_registry.h"
#include "tools/edit_types.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace mistral::agent
{
  namespace
  {
    // Minimal tool instance so tests can register named tools on a
    // local registry (the renderers never invoke the tool itself).
    class dummy_tool
    {
    public:
      json noop(std::string input)
      {
        return {{"echo", input}};
      }
    };

    void register_dummy_tools(tool_registry& registry, dummy_tool& tool_inst)
    {
      registry.register_tool(
        "patchset_apply", "Apply a patchset", &tool_inst, &dummy_tool::noop, arg("patchset_id", "The patchset ID")
      );
      registry.register_tool("exec", "Execute a command", &tool_inst, &dummy_tool::noop, arg("command", "The command"));
    }

    mistral::tools::file_edit_t make_edit(
      std::string file_path,
      std::string search_text,
      std::string replace_text,
      size_t patch_number
    )
    {
      return mistral::tools::file_edit_t{
        std::move(file_path), std::move(search_text), std::move(replace_text), patch_number
      };
    }

    // Minimal fixture: initialize the agent_system singleton with an
    // empty config so the global tool registry (and its renderers) exist.
    struct ApprovalRenderersTestFixture
    {
      std::string temp_dir;
      std::string config_file;
      std::string base_dir;
      std::string notes_dir;

      ApprovalRenderersTestFixture()
      {
        temp_dir = std::filesystem::temp_directory_path() / "mistral_cli_approval_renderers_test";
        std::filesystem::create_directories(temp_dir);
        config_file = (std::filesystem::path(temp_dir) / "mistral-cli.json").string();
        base_dir = temp_dir;
        notes_dir = (std::filesystem::path(temp_dir) / "notes").string();
        std::filesystem::create_directories(notes_dir);
      }

      ~ApprovalRenderersTestFixture()
      {
        try
        {
          std::filesystem::remove_all(temp_dir);
        }
        catch (...)
        {
        }
      }

      void initialize_agent_system_empty()
      {
        {
          std::ofstream out(config_file);
          out << "{}";
        } // Close (and flush) the file before initialize() reads it
        agent_system::instance().initialize(config_file, base_dir, notes_dir, nullptr);
      }
    };
  } // namespace

  TEST_SUITE_BEGIN("approval_renderers");

  // ---- Registry hook ----

  TEST_CASE("render_approval returns nullopt for tool without renderer")
  {
    dummy_tool tool_inst;
    tool_registry registry;
    register_dummy_tools(registry, tool_inst);

    auto result = registry.render_approval("exec", json::object(), "reason");
    CHECK(!result.has_value());
  }

  TEST_CASE("render_approval returns nullopt for unknown tool")
  {
    tool_registry registry;
    auto result = registry.render_approval("nonexistent", json::object(), "reason");
    CHECK(!result.has_value());
  }

  TEST_CASE("register_approval_renderer on unknown tool is a no-op")
  {
    tool_registry registry;
    registry.register_approval_renderer("nonexistent", [](const json&, std::string_view) {
      return std::string("custom");
    });
    CHECK(!registry.render_approval("nonexistent", json::object(), "r").has_value());
  }

  TEST_CASE("render_approval returns formatted string for registered tool")
  {
    dummy_tool tool_inst;
    tool_registry registry;
    register_dummy_tools(registry, tool_inst);
    registry.register_approval_renderer("exec", [](const json& args, std::string_view reason) {
      return "custom:" + std::string(args["command"].get<std::string>()) + ":" + std::string(reason);
    });

    auto result = registry.render_approval("exec", json{{"command", "ls"}}, "why not");
    REQUIRE(result.has_value());
    CHECK(*result == "custom:ls:why not");
  }

  TEST_CASE("renderer survives registry copy construction and assignment")
  {
    dummy_tool tool_inst;
    tool_registry registry;
    register_dummy_tools(registry, tool_inst);
    registry.register_approval_renderer("exec", [](const json&, std::string_view) { return std::string("custom"); });

    tool_registry copy_ctor = registry;
    auto result = copy_ctor.render_approval("exec", json::object(), "r");
    REQUIRE(result.has_value());
    CHECK(*result == "custom");

    tool_registry copy_assign;
    copy_assign = registry;
    result = copy_assign.render_approval("exec", json::object(), "r");
    REQUIRE(result.has_value());
    CHECK(*result == "custom");
  }

  // ---- patchset_apply renderer ----

  TEST_CASE("patchset_apply renderer shows one line per edit with status letters")
  {
    auto& agent_sys = agent_system::instance();

    std::vector<mistral::tools::file_edit_t> edits;
    edits.push_back(make_edit("src/main.cpp", "old", "new", 1));    // M
    edits.push_back(make_edit("lib/utils.h", "", "a\nb\nc", 2));    // A, 3 lines
    edits.push_back(make_edit("CMakeLists.txt", ".", "x\ny\n", 3)); // O, 2 lines
    edits.push_back(make_edit("old.cpp", "gone", "", 4));           // D
    agent_sys.store_patchset("renderer-test-ps", edits);

    dummy_tool tool_inst;
    tool_registry registry;
    register_dummy_tools(registry, tool_inst);
    register_approval_renderers(registry);

    auto result =
      registry.render_approval("patchset_apply", json{{"patchset_id", "renderer-test-ps"}}, "Policy requires approval");

    // Clean up before any CHECK so a failure cannot leak state
    agent_sys.remove_patchset("renderer-test-ps");

    REQUIRE(result.has_value());
    const std::string& prompt = *result;
    CHECK(prompt.find("\n[APPROVAL REQUIRED] patchset_apply\n") != std::string::npos);
    CHECK(prompt.find("  patchset: renderer-test-ps (4 edits)\n") != std::string::npos);
    CHECK(prompt.find("    1. M src/main.cpp\n") != std::string::npos);
    CHECK(prompt.find("    2. A lib/utils.h (3 lines)\n") != std::string::npos);
    CHECK(prompt.find("    3. O CMakeLists.txt (2 lines)\n") != std::string::npos);
    CHECK(prompt.find("    4. D old.cpp\n") != std::string::npos);
    CHECK(prompt.find("\n  Reason: Policy requires approval\n") != std::string::npos);
    CHECK(prompt.find("  Approve? [y/n/r/a/?]: ") != std::string::npos);
  }

  TEST_CASE("patchset_apply renderer uses singular for a single edit")
  {
    auto& agent_sys = agent_system::instance();

    std::vector<mistral::tools::file_edit_t> edits;
    edits.push_back(make_edit("a.cpp", "x", "y", 1));
    agent_sys.store_patchset("renderer-test-single", edits);

    dummy_tool tool_inst;
    tool_registry registry;
    register_dummy_tools(registry, tool_inst);
    register_approval_renderers(registry);

    auto result = registry.render_approval("patchset_apply", json{{"patchset_id", "renderer-test-single"}}, "r");
    agent_sys.remove_patchset("renderer-test-single");

    REQUIRE(result.has_value());
    CHECK(result->find("  patchset: renderer-test-single (1 edit)\n") != std::string::npos);
  }

  TEST_CASE("patchset_apply renderer reports unknown patchset id")
  {
    auto& agent_sys = agent_system::instance();
    agent_sys.remove_patchset("renderer-test-missing");

    dummy_tool tool_inst;
    tool_registry registry;
    register_dummy_tools(registry, tool_inst);
    register_approval_renderers(registry);

    auto result = registry.render_approval("patchset_apply", json{{"patchset_id", "renderer-test-missing"}}, "r");

    REQUIRE(result.has_value());
    CHECK(result->find("  patchset: renderer-test-missing (not found)\n") != std::string::npos);
    // Still asks for approval
    CHECK(result->find("  Approve? [y/n/r/a/?]: ") != std::string::npos);
  }

  TEST_CASE("patchset_apply renderer reports empty patchset")
  {
    auto& agent_sys = agent_system::instance();
    agent_sys.store_patchset("renderer-test-empty", {});

    dummy_tool tool_inst;
    tool_registry registry;
    register_dummy_tools(registry, tool_inst);
    register_approval_renderers(registry);

    auto result = registry.render_approval("patchset_apply", json{{"patchset_id", "renderer-test-empty"}}, "r");
    agent_sys.remove_patchset("renderer-test-empty");

    REQUIRE(result.has_value());
    CHECK(result->find("  patchset: renderer-test-empty (0 edits)\n") != std::string::npos);
  }

  TEST_CASE("patchset_apply renderer truncates long patchsets")
  {
    auto& agent_sys = agent_system::instance();

    std::vector<mistral::tools::file_edit_t> edits;
    for (size_t i = 1; i <= 12; ++i)
    {
      edits.push_back(make_edit("file" + std::to_string(i) + ".cpp", "x", "y", i));
    }
    agent_sys.store_patchset("renderer-test-long", edits);

    dummy_tool tool_inst;
    tool_registry registry;
    register_dummy_tools(registry, tool_inst);
    register_approval_renderers(registry);

    auto result = registry.render_approval("patchset_apply", json{{"patchset_id", "renderer-test-long"}}, "r");
    agent_sys.remove_patchset("renderer-test-long");

    REQUIRE(result.has_value());
    const std::string& prompt = *result;
    CHECK(prompt.find("  patchset: renderer-test-long (12 edits)\n") != std::string::npos);
    // First 10 edits are listed
    CHECK(prompt.find("    1. M file1.cpp\n") != std::string::npos);
    CHECK(prompt.find("    10. M file10.cpp\n") != std::string::npos);
    // Edits 11 and 12 are not
    CHECK(prompt.find("file11.cpp") == std::string::npos);
    CHECK(prompt.find("file12.cpp") == std::string::npos);
    CHECK(prompt.find("    ... and 2 more edits\n") != std::string::npos);
  }

  // ---- exec renderer ----

  TEST_CASE("exec renderer joins command and args and shows cwd")
  {
    dummy_tool tool_inst;
    tool_registry registry;
    register_dummy_tools(registry, tool_inst);
    register_approval_renderers(registry);

    auto result = registry.render_approval(
      "exec",
      json{{"command", "git"}, {"args", json::array({"status", "-s"})}, {"cwd", "/tmp/work"}},
      "Policy requires approval"
    );

    REQUIRE(result.has_value());
    const std::string& prompt = *result;
    CHECK(prompt.find("\n[APPROVAL REQUIRED] exec\n") != std::string::npos);
    CHECK(prompt.find("  Command: git status -s\n") != std::string::npos);
    CHECK(prompt.find("  Working directory: /tmp/work\n") != std::string::npos);
    CHECK(prompt.find("\n  Reason: Policy requires approval\n") != std::string::npos);
    CHECK(prompt.find("  Approve? [y/n/r/a/?]: ") != std::string::npos);
  }

  TEST_CASE("exec renderer omits working directory when cwd is empty or absent")
  {
    dummy_tool tool_inst;
    tool_registry registry;
    register_dummy_tools(registry, tool_inst);
    register_approval_renderers(registry);

    auto result =
      registry.render_approval("exec", json{{"command", "make"}, {"args", json::array()}, {"cwd", ""}}, "r");
    REQUIRE(result.has_value());
    CHECK(result->find("  Command: make\n") != std::string::npos);
    CHECK(result->find("Working directory") == std::string::npos);

    result = registry.render_approval("exec", json{{"command", "make"}}, "r");
    REQUIRE(result.has_value());
    CHECK(result->find("  Command: make\n") != std::string::npos);
    CHECK(result->find("Working directory") == std::string::npos);
  }

  TEST_CASE("exec renderer shows the full command line without truncation")
  {
    dummy_tool tool_inst;
    tool_registry registry;
    register_dummy_tools(registry, tool_inst);
    register_approval_renderers(registry);

    std::string long_arg(100, 'x');
    auto result = registry.render_approval("exec", json{{"command", "echo"}, {"args", json::array({long_arg})}}, "r");

    REQUIRE(result.has_value());
    // The generic prompt truncates values to 60 characters; the custom
    // renderer must show the full argument.
    CHECK(result->find(long_arg) != std::string::npos);
  }

  // ---- create_tool_registry integration ----

  TEST_CASE_FIXTURE(ApprovalRenderersTestFixture, "create_tool_registry: wildcard copy preserves renderers")
  {
    initialize_agent_system_empty();
    auto& system = agent_system::instance();

    auto registry = system.create_tool_registry({"*"});

    auto exec_prompt =
      registry.render_approval("exec", json{{"command", "git"}, {"args", json::array({"status"})}}, "r");
    REQUIRE(exec_prompt.has_value());
    CHECK(exec_prompt->find("[APPROVAL REQUIRED] exec") != std::string::npos);
    CHECK(exec_prompt->find("Command: git status") != std::string::npos);

    auto ps_prompt = registry.render_approval("patchset_apply", json{{"patchset_id", "no-such-patchset"}}, "r");
    REQUIRE(ps_prompt.has_value());
    CHECK(ps_prompt->find("[APPROVAL REQUIRED] patchset_apply") != std::string::npos);
    CHECK(ps_prompt->find("(not found)") != std::string::npos);
  }

  TEST_CASE_FIXTURE(
    ApprovalRenderersTestFixture,
    "create_tool_registry: filtered registry preserves renderer for included tool"
  )
  {
    initialize_agent_system_empty();
    auto& system = agent_system::instance();

    auto registry = system.create_tool_registry({"exec"});

    auto exec_prompt = registry.render_approval("exec", json{{"command", "ls"}}, "r");
    REQUIRE(exec_prompt.has_value());
    CHECK(exec_prompt->find("Command: ls") != std::string::npos);

    // patchset_apply is filtered out: no tool, no renderer
    CHECK(!registry.has_tool("patchset_apply"));
    CHECK(!registry.render_approval("patchset_apply", json::object(), "r").has_value());
  }

  TEST_SUITE_END();
} // namespace mistral::agent
