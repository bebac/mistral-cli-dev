#include "agent/agent_task_base.h"
#include "agent/agent_system.h"
#include "agent/edit_parser.h"
#include "doctest/doctest.h"

#include <string>
#include <vector>

namespace mistral::agent::test
{

TEST_SUITE_BEGIN("agent_task_base_patchset_parsing");

TEST_CASE("patchset parsing - patchset with tool call")
{
    // This test verifies that when an AI response contains both
    // a patchset and a tool call, the patchset is stored before
    // the tool is executed.

    // Note: This is an integration test that requires mocking or
    // a more complex setup. For now, we test the parsing logic directly.

    // Simulate accumulated text content with patchset
    std::string accumulated_text =
        "Here are the changes (patchset: test-patchset):\n"
        "\n"
        "src/main.cpp\n"
        "~~~cpp\n"
        "<<<<<<< SEARCH\n"
        "old_code\n"
        "=======\n"
        "new_code\n"
        ">>>>>>> REPLACE\n"
        "~~~\n"
        "\n"
        "patchset_validate(patchset_id=\"test-patchset\")";

    // Parse and store
    auto parse_result = parse_edits_with_patchset(accumulated_text);
    REQUIRE(parse_result.has_value());
    CHECK(parse_result->first == "test-patchset");
    CHECK(parse_result->second.size() == 1);
    CHECK(parse_result->second[0].patch_number == 1);

    // Store in agent_system
    auto& agent_sys = agent_system::instance();
    agent_sys.remove_patchset("test-patchset"); // Clear first
    agent_sys.store_patchset(parse_result->first, parse_result->second);

    // Verify it's stored
    CHECK(agent_sys.has_patchset());
    auto current_id = agent_sys.get_current_patchset_id();
    CHECK(current_id.has_value());
    CHECK(*current_id == "test-patchset");

    // Verify we can retrieve it
    auto retrieved = agent_sys.get_patchset("test-patchset");
    REQUIRE(retrieved.has_value());
    CHECK(retrieved->edits[0].file_path == "src/main.cpp");

    // Clean up
    agent_sys.remove_patchset("test-patchset");
}

TEST_CASE("patchset parsing - multiple edits with tool call")
{
    std::string accumulated_text =
        "Here are the changes (patchset: multi-edit):\n"
        "\n"
        "src/main.cpp\n"
        "~~~cpp\n"
        "<<<<<<< SEARCH\n"
        "#include <iostream>\n"
        "=======\n"
        "#include <iostream>\n"
        "#include <vector>\n"
        ">>>>>>> REPLACE\n"
        "~~~\n"
        "\n"
        "src/utils.cpp\n"
        "~~~cpp\n"
        "<<<<<<< SEARCH\n"
        "helper()\n"
        "=======\n"
        "fixed_helper()\n"
        ">>>>>>> REPLACE\n"
        "~~~\n"
        "\n"
        "patchset_apply(patchset_id=\"multi-edit\")";

    auto parse_result = parse_edits_with_patchset(accumulated_text);
    REQUIRE(parse_result.has_value());
    CHECK(parse_result->first == "multi-edit");
    CHECK(parse_result->second.size() == 2);
    CHECK(parse_result->second[0].patch_number == 1);
    CHECK(parse_result->second[1].patch_number == 2);

    // Store and verify
    auto& agent_sys = agent_system::instance();
    agent_sys.remove_patchset("multi-edit");
    agent_sys.store_patchset(parse_result->first, parse_result->second);

    auto retrieved = agent_sys.get_patchset("multi-edit");
    REQUIRE(retrieved.has_value());
    CHECK(retrieved->edits.size() == 2);
    CHECK(retrieved->edits[0].file_path == "src/main.cpp");
    CHECK(retrieved->edits[1].file_path == "src/utils.cpp");

    // Clean up
    agent_sys.remove_patchset("multi-edit");
}

TEST_CASE("patchset parsing - no patchset in response")
{
    std::string accumulated_text =
        "This is just a regular response without any patchset.\n"
        "Here's some information for the user.\n"
        "read(path=\"src/main.cpp\")";

    auto parse_result = parse_edits_with_patchset(accumulated_text);
    CHECK(!parse_result.has_value());

    // agent_system should not have a patchset
    auto& agent_sys = agent_system::instance();
    agent_sys.remove_patchset("no-patchset");
    CHECK(!agent_sys.has_patchset());
}

TEST_CASE("patchset parsing - patchset without tool call")
{
    // When there's a patchset but no tool calls (final answer)
    std::string accumulated_text =
        "Here are the changes (patchset: final-answer):\n"
        "\n"
        "src/main.cpp\n"
        "~~~cpp\n"
        "<<<<<<< SEARCH\n"
        "old\n"
        "=======\n"
        "new\n"
        ">>>>>>> REPLACE\n"
        "~~~";

    auto parse_result = parse_edits_with_patchset(accumulated_text);
    REQUIRE(parse_result.has_value());
    CHECK(parse_result->first == "final-answer");

    // Store and verify
    auto& agent_sys = agent_system::instance();
    agent_sys.remove_patchset("final-answer");
    agent_sys.store_patchset(parse_result->first, parse_result->second);

    CHECK(agent_sys.has_patchset());
    CHECK(agent_sys.get_current_patchset_id() == "final-answer");

    // Clean up
    agent_sys.remove_patchset("final-answer");
}

TEST_SUITE_END;
} // namespace mistral::agent::test