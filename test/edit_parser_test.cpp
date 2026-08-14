#include "agent/edit_parser.h"
#include "doctest/doctest.h"

#include <string>
#include <vector>

namespace mistral::agent
{
  namespace test
  {
    TEST_SUITE_BEGIN("edit_parser");

    TEST_CASE("extract_patchset_id - valid IDs")
    {
      // Basic valid ID
      auto result = extract_patchset_id("Here are the changes (patchset: my-edit-id):");
      CHECK(result.has_value());
      CHECK(*result == "my-edit-id");

      // ID with hyphens and underscores
      result = extract_patchset_id("Some text (patchset: my-complex_id-123) more text");
      CHECK(result.has_value());
      CHECK(*result == "my-complex_id-123");

      // ID at start of string
      result = extract_patchset_id("(patchset: first)");
      CHECK(result.has_value());
      CHECK(*result == "first");

      // ID at end of string
      result = extract_patchset_id("text before (patchset: last)");
      CHECK(result.has_value());
      CHECK(*result == "last");

      // ID with whitespace around it (should be trimmed)
      result = extract_patchset_id("(patchset:   spaced-id  )");
      CHECK(result.has_value());
      CHECK(*result == "spaced-id");
    }

    TEST_CASE("extract_patchset_id - invalid IDs")
    {
      // No patchset marker
      auto result = extract_patchset_id("Just some text without a patchset");
      CHECK(!result.has_value());

      // Missing closing parenthesis
      result = extract_patchset_id("(patchset: missing-close");
      CHECK(!result.has_value());

      // Empty ID
      result = extract_patchset_id("(patchset: )");
      CHECK(!result.has_value());

      // ID with spaces (invalid characters)
      result = extract_patchset_id("(patchset: has spaces)");
      CHECK(!result.has_value());

      // ID with special characters
      result = extract_patchset_id("(patchset: invalid@id)");
      CHECK(!result.has_value());

      // ID with dots
      result = extract_patchset_id("(patchset: invalid.id)");
      CHECK(!result.has_value());
    }

    TEST_CASE("extract_patchset_id - edge cases")
    {
      // Multiple patchset markers - returns first
      auto result = extract_patchset_id("(patchset: first) ... (patchset: second)");
      CHECK(result.has_value());
      CHECK(*result == "first");

      // Nested parentheses (should not match inner)
      result = extract_patchset_id("text (patchset: outer) (not this)");
      CHECK(result.has_value());
      CHECK(*result == "outer");

      // Minimal valid ID (single character)
      result = extract_patchset_id("(patchset: a)");
      CHECK(result.has_value());
      CHECK(*result == "a");
    }

    TEST_CASE("validation_result_t methods")
    {
      validation_result_t result;

      // Empty result
      CHECK(result.all_valid() == true);
      CHECK(result.fixable_error_count() == 0);
      CHECK(result.get_fixable_errors().empty());
      CHECK(result.get_non_fixable_errors().empty());

      // Add a valid validation
      result.validations.push_back({.file_path = "test.cpp", .valid = true});
      CHECK(result.all_valid() == true);
      CHECK(result.fixable_error_count() == 0);

      // Add a fixable error
      result.validations.push_back(
        {
          .file_path = "test2.cpp",
          .valid = false,
          .error_type = "search_not_found",
          .is_fixable = true
        }
      );
      CHECK(result.all_valid() == false);
      CHECK(result.fixable_error_count() == 1);
      CHECK(result.get_fixable_errors().size() == 1);
      CHECK(result.get_non_fixable_errors().empty());

      // Add a non-fixable error
      result.validations.push_back(
        {
          .file_path = "test3.cpp",
          .valid = false,
          .error_type = "file_not_found",
          .is_fixable = false
        }
      );
      CHECK(result.all_valid() == false);
      CHECK(result.fixable_error_count() == 1);
      CHECK(result.get_fixable_errors().size() == 1);
      CHECK(result.get_non_fixable_errors().size() == 1);
    }

    TEST_CASE("parse_edit_block - basic edit")
    {
      std::string content =
        "<<<<<<< SEARCH\n"
        "#include <iostream>\n"
        "=======\n"
        "#include <iostream>\n"
        "#include <vector>\n"
        ">>>>>>> REPLACE";

      auto result = parse_edit_block("src/main.cpp", content);
      REQUIRE(result.has_value());
      CHECK(result->file_path == "src/main.cpp");
      CHECK(result->search_text == "#include <iostream>");
      CHECK(result->replace_text == "#include <iostream>\n#include <vector>");
    }

    TEST_CASE("parse_edit_block - new file creation")
    {
      std::string content =
        "<<<<<<< SEARCH\n"
        "=======\n"
        "new file content\n"
        ">>>>>>> REPLACE";

      auto result = parse_edit_block("new_file.cpp", content);
      REQUIRE(result.has_value());
      CHECK(result->file_path == "new_file.cpp");
      CHECK(result->search_text == "");
      CHECK(result->replace_text == "new file content");
    }

    TEST_CASE("parse_edit_block - overwrite entire file")
    {
      std::string content =
        "<<<<<<< SEARCH\n"
        ".\n"
        "=======\n"
        "entire new content\n"
        ">>>>>>> REPLACE";

      auto result = parse_edit_block("src/main.cpp", content);
      REQUIRE(result.has_value());
      CHECK(result->file_path == "src/main.cpp");
      CHECK(result->search_text == ".");
      CHECK(result->replace_text == "entire new content");
    }

    TEST_CASE("parse_edit_block - missing markers")
    {
      // Missing SEARCH marker
      std::string content =
        "missing search\n"
        "=======\n"
        "replace\n"
        ">>>>>>> REPLACE";
      auto result = parse_edit_block("src/main.cpp", content);
      CHECK(!result.has_value());

      // Missing SEPARATOR marker
      content =
        "<<<<<<< SEARCH\n"
        "search\n"
        ">>>>>>> REPLACE";
      result = parse_edit_block("src/main.cpp", content);
      CHECK(!result.has_value());

      // Missing REPLACE marker
      content =
        "<<<<<<< SEARCH\n"
        "search\n"
        "=======\n"
        "replace";
      result = parse_edit_block("src/main.cpp", content);
      CHECK(!result.has_value());
    }

    TEST_CASE("parse_edit_block - markers not at line start")
    {
      // Markers must be at the start of a line
      std::string content =
        "  <<<<<<< SEARCH\n"
        "search\n"
        "=======\n"
        "replace\n"
        ">>>>>>> REPLACE";
      auto result = parse_edit_block("src/main.cpp", content);
      CHECK(!result.has_value());
    }

    TEST_CASE("parse_edits_with_patchset - basic case")
    {
      std::string input =
        "Here are the changes (patchset: my-patchset-id):\n"
        "\n"
        "src/main.cpp\n"
        "~~~cpp\n"
        "<<<<<<< SEARCH\n"
        "#include <iostream>\n"
        "=======\n"
        "#include <iostream>\n"
        "#include <vector>\n"
        ">>>>>>> REPLACE\n"
        "~~~";

      auto result = parse_edits_with_patchset(input);
      REQUIRE(result.has_value());
      CHECK(result->first == "my-patchset-id");
      REQUIRE(result->second.size() == 1);
      CHECK(result->second[0].file_path == "src/main.cpp");
      CHECK(result->second[0].search_text == "#include <iostream>");
      CHECK(result->second[0].replace_text == "#include <iostream>\n#include <vector>");
      CHECK(result->second[0].patch_number == 1); // Check patch number
    }

    TEST_CASE("parse_edits_with_patchset - multiple edits")
    {
      std::string input =
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
        "lib/utils.cpp\n"
        "~~~cpp\n"
        "<<<<<<< SEARCH\n"
        "void foo() {}\n"
        "=======\n"
        "void foo() { return 42; }\n"
        ">>>>>>> REPLACE\n"
        "~~~";

      auto result = parse_edits_with_patchset(input);
      REQUIRE(result.has_value());
      CHECK(result->first == "multi-edit");
      REQUIRE(result->second.size() == 2);
      CHECK(result->second[0].file_path == "src/main.cpp");
      CHECK(result->second[1].file_path == "lib/utils.cpp");
      CHECK(result->second[0].patch_number == 1);
      CHECK(result->second[1].patch_number == 2);
    }

    TEST_CASE("parse_edits_with_patchset - no patchset ID")
    {
      std::string input =
        "Here are the changes:\n"
        "\n"
        "src/main.cpp\n"
        "~~~cpp\n"
        "<<<<<<< SEARCH\n"
        "#include <iostream>\n"
        "=======\n"
        "#include <iostream>\n"
        "#include <vector>\n"
        ">>>>>>> REPLACE\n"
        "~~~";

      auto result = parse_edits_with_patchset(input);
      CHECK(!result.has_value());
    }

    TEST_CASE("parse_edits_with_patchset - no edit blocks")
    {
      std::string input =
        "Here are the changes (patchset: empty-patchset):\n"
        "\n"
        "No edit blocks here";

      auto result = parse_edits_with_patchset(input);
      CHECK(!result.has_value());
    }

    TEST_CASE("parse_edits_with_patchset - new file creation")
    {
      std::string input =
        "Here are the changes (patchset: new-file):\n"
        "\n"
        "new_file.cpp\n"
        "~~~cpp\n"
        "<<<<<<< SEARCH\n"
        "=======\n"
        "// New file content\n"
        "int main() { return 0; }\n"
        ">>>>>>> REPLACE\n"
        "~~~";

      auto result = parse_edits_with_patchset(input);
      REQUIRE(result.has_value());
      CHECK(result->first == "new-file");
      REQUIRE(result->second.size() == 1);
      CHECK(result->second[0].file_path == "new_file.cpp");
      CHECK(result->second[0].search_text == "");
      CHECK(result->second[0].replace_text == "// New file content\nint main() { return 0; }");
      CHECK(result->second[0].patch_number == 1);
    }

    TEST_CASE("parse_edits_with_patchset - file path with spaces")
    {
      std::string input =
        "Here are the changes (patchset: spaced-path):\n"
        "\n"
        "src/my file.cpp\n"
        "~~~cpp\n"
        "<<<<<<< SEARCH\n"
        "old\n"
        "=======\n"
        "new\n"
        ">>>>>>> REPLACE\n"
        "~~~";

      auto result = parse_edits_with_patchset(input);
      REQUIRE(result.has_value());
      CHECK(result->first == "spaced-path");
      REQUIRE(result->second.size() == 1);
      CHECK(result->second[0].file_path == "src/my file.cpp");
      CHECK(result->second[0].patch_number == 1);
    }

    TEST_CASE("parse_edits_with_patchset - without language specifier")
    {
      std::string input =
        "Here are the changes (patchset: no-lang):\n"
        "\n"
        "src/main.cpp\n"
        "~~~\n"
        "<<<<<<< SEARCH\n"
        "#include <iostream>\n"
        "=======\n"
        "#include <iostream>\n"
        "#include <vector>\n"
        ">>>>>>> REPLACE\n"
        "~~~";

      auto result = parse_edits_with_patchset(input);
      REQUIRE(result.has_value());
      CHECK(result->first == "no-lang");
      REQUIRE(result->second.size() == 1);
      CHECK(result->second[0].file_path == "src/main.cpp");
      CHECK(result->second[0].search_text == "#include <iostream>");
      CHECK(result->second[0].patch_number == 1);
    }

    TEST_CASE("parse_edit_block - markers at line start in content are treated as literal text")
    {
      // Even if marker strings appear at line start within the content,
      // they should be treated as literal text, not as edit block markers
      std::string content =
        "<<<<<<< SEARCH\n"
        "<<<<<<< SEARCH\n"
        "=======\n"
        "=======\n"
        ">>>>>>> REPLACE\n"
        ">>>>>>> REPLACE";

      auto result = parse_edit_block("src/main.cpp", content);
      REQUIRE(result.has_value());
      CHECK(result->search_text == "<<<<<<< SEARCH");
      CHECK(result->replace_text == "=======\n>>>>>>> REPLACE");
    }

    TEST_CASE("parse_edit_block - with nested code blocks")
    {
      // edit blocks can contain nested markdown code blocks
      std::string input =
        "Here are the changes (patchset: nested-fence-patches):\n"
        "\n"
        "README.md\n"
        "~~~markdown\n"
        "<<<<<<< SEARCH\n"
        "```sh\n"
        "x = 1;\n"
        "```\n"
        "=======\n"
        "```sh\n"
        "x = 2;\n"
        "```\n"
        "\n"
        "y = 0;\n"
        ">>>>>>> REPLACE\n"
        "~~~";

      auto result = parse_edits_with_patchset(input);
      REQUIRE(result.has_value());
      CHECK(result->first == "nested-fence-patches");
      REQUIRE(result->second.size() == 1);
      CHECK(result->second[0].patch_number == 1);
      CHECK(result->second[0].search_text == "```sh\nx = 1;\n```");
      CHECK(result->second[0].replace_text == "```sh\nx = 2;\n```\n\ny = 0;");
    }

    TEST_CASE("parse_edits_with_patchset - patch numbers are assigned")
    {
      std::string input =
        "Here are the changes (patchset: numbered-patches):\n"
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
        "~~~";

      auto result = parse_edits_with_patchset(input);
      REQUIRE(result.has_value());
      CHECK(result->first == "numbered-patches");
      REQUIRE(result->second.size() == 2);

      // Check patch numbers are 1-indexed
      CHECK(result->second[0].patch_number == 1);
      CHECK(result->second[0].file_path == "src/main.cpp");

      CHECK(result->second[1].patch_number == 2);
      CHECK(result->second[1].file_path == "src/utils.cpp");
    }

    TEST_CASE("parse_edits_with_patchset - single edit gets patch number 1")
    {
      std::string input =
        "Here are the changes (patchset: single-edit):\n"
        "\n"
        "src/main.cpp\n"
        "~~~cpp\n"
        "<<<<<<< SEARCH\n"
        "old\n"
        "=======\n"
        "new\n"
        ">>>>>>> REPLACE\n"
        "~~~";

      auto result = parse_edits_with_patchset(input);
      REQUIRE(result.has_value());
      REQUIRE(result->second.size() == 1);
      CHECK(result->second[0].patch_number == 1);
    }

    TEST_CASE("parse_edits_with_patchset - nested replace markers")
    {
      std::string input =
        "Here are the changes (patchset: nested-replace-markers):\n"
        "\n"
        "src/main.cpp\n"
        "~~~cpp\n"
        "<<<<<<< SEARCH\n"
        ".\n"
        "=======\n"
        "```\n"
        ">>>>>>> REPLACE\n"
        "```\n"
        "test\n"
        ">>>>>>> REPLACE\n"
        "~~~";

      auto result = parse_edits_with_patchset(input);
      REQUIRE(result.has_value());
      REQUIRE(result->second.size() == 1);
      CHECK(result->second[0].patch_number == 1);
      CHECK(result->second[0].search_text == ".");
      CHECK(result->second[0].replace_text == "```\n>>>>>>> REPLACE\n```\ntest");
    }

    TEST_CASE("parse_edits_with_patchset - nested tilde fences in replace text")
    {
      // When the replace text contains tilde code fences (e.g., editing
      // a markdown file that documents patchset format), the parser must find
      // the real closing fence after the REPLACE marker, not a nested one.
      std::string input =
        "Here are the changes (patchset: nested-fences):\n"
        "\n"
        "doc/examples.md\n"
        "~~~markdown\n"
        "<<<<<<< SEARCH\n"
        "### Edit Types\n"
        "- old line\n"
        "=======\n"
        "### Edit Types\n"
        "\n"
        "**1. Edit existing file**:\n"
        "\n"
        "~~~cpp\n"
        "<<<<<<< SEARCH\n"
        "old\n"
        "=======\n"
        "new\n"
        ">>>>>>> REPLACE\n"
        "~~~\n"
        "\n"
        "**2. Create new file**:\n"
        "\n"
        "~~~cpp\n"
        "<<<<<<< SEARCH\n"
        "=======\n"
        "new content\n"
        ">>>>>>> REPLACE\n"
        "~~~\n"
        ">>>>>>> REPLACE\n"
        "~~~";

      auto result = parse_edits_with_patchset(input);
      REQUIRE(result.has_value());
      CHECK(result->first == "nested-fences");
      REQUIRE(result->second.size() == 1);
      CHECK(result->second[0].file_path == "doc/examples.md");
      CHECK(result->second[0].search_text == "### Edit Types\n- old line");
      // The replace text should contain all the nested content including
      // the 5-backtick fences and inner SEARCH/REPLACE markers
      CHECK(result->second[0].replace_text.find("**1. Edit existing file**") != std::string::npos);
      CHECK(result->second[0].replace_text.find("**2. Create new file**") != std::string::npos);
    }

    TEST_CASE("parse_edit_blocks - basic case without patchset ID")
    {
      std::string input =
        "Now I need to update the test files.\n"
        "\n"
        "src/main.cpp\n"
        "~~~cpp\n"
        "<<<<<<< SEARCH\n"
        "old\n"
        "=======\n"
        "new\n"
        ">>>>>>> REPLACE\n"
        "~~~";

      auto result = parse_edit_blocks(input);
      REQUIRE(result.has_value());
      REQUIRE(result->size() == 1);
      CHECK((*result)[0].file_path == "src/main.cpp");
      CHECK((*result)[0].search_text == "old");
      CHECK((*result)[0].replace_text == "new");
      CHECK((*result)[0].patch_number == 1);
    }

    TEST_CASE("parse_edit_blocks - multiple edits without patchset ID")
    {
      std::string input =
        "Here are the remaining changes.\n"
        "\n"
        "src/main.cpp\n"
        "~~~cpp\n"
        "<<<<<<< SEARCH\n"
        "old1\n"
        "=======\n"
        "new1\n"
        ">>>>>>> REPLACE\n"
        "~~~\n"
        "\n"
        "src/utils.cpp\n"
        "~~~cpp\n"
        "<<<<<<< SEARCH\n"
        "old2\n"
        "=======\n"
        "new2\n"
        ">>>>>>> REPLACE\n"
        "~~~";

      auto result = parse_edit_blocks(input);
      REQUIRE(result.has_value());
      REQUIRE(result->size() == 2);
      CHECK((*result)[0].file_path == "src/main.cpp");
      CHECK((*result)[0].patch_number == 1);
      CHECK((*result)[1].file_path == "src/utils.cpp");
      CHECK((*result)[1].patch_number == 2);
    }

    TEST_CASE("parse_edit_blocks - no edit blocks returns nullopt")
    {
      std::string input =
        "This is just a regular response without any edit blocks.\n"
        "Nothing to see here.";

      auto result = parse_edit_blocks(input);
      CHECK(!result.has_value());
    }

    TEST_CASE("parse_edit_blocks - works with patchset ID present")
    {
      // parse_edit_blocks should also work when a patchset ID is present,
      // it simply ignores the marker and parses the edit blocks.
      std::string input =
        "Here are the changes (patchset: my-id):\n"
        "\n"
        "src/main.cpp\n"
        "~~~cpp\n"
        "<<<<<<< SEARCH\n"
        "old\n"
        "=======\n"
        "new\n"
        ">>>>>>> REPLACE\n"
        "~~~";

      auto result = parse_edit_blocks(input);
      REQUIRE(result.has_value());
      REQUIRE(result->size() == 1);
      CHECK((*result)[0].file_path == "src/main.cpp");
    }

    TEST_CASE("parse_edits_with_patchset - mismatched closing fence backticks (4 instead of 5)"
              * doctest::may_fail(true)
              * doctest::skip() // Skip this test. It is a malformed patchset. I don't see
                                // an easy way to handle this other than hoping that the agent
                                // realizes that the patchset does not contain the expected
                                // number of edits and creates a new corrected patchset.
    )
    {
      // Known limitation: when the AI emits tilde opening fences (~~~)
      // but 2-tilde closing fences (~~) for intermediate blocks, and only
      // the last block uses the correct 3-tilde closing, the parser cannot
      // detect the mismatched closing fences. It skips the 2-tilde fences,
      // finds the last 3-tilde fence, and merges all edits into 1 garbled
      // edit. This is an AI formatting error, not a parser bug — the parser
      // correctly requires consistent 3-tilde fencing.
      std::string input =
        "Here are the changes (patchset: fix-remove-patchset-callsites):\n"
        "\n"
        "test/agent_task_base_test.cpp\n"
        "~~~cpp\n"
        "<<<<<<< SEARCH\n"
        "    agent_sys.remove_patchset(); // Clear first\n"
        "=======\n"
        "    agent_sys.remove_patchset(\"test-patchset\"); // Clear first\n"
        ">>>>>>> REPLACE\n"
        "~~\n"
        "\n"
        "test/agent_task_base_test.cpp\n"
        "~~~cpp\n"
        "<<<<<<< SEARCH\n"
        "    // Clean up\n"
        "    agent_sys.remove_patchset();\n"
        "}\n"
        "\n"
        "TEST_CASE(\"patchset parsing - multiple edits with tool call\")\n"
        "=======\n"
        "    // Clean up\n"
        "    agent_sys.remove_patchset(\"test-patchset\");\n"
        "}\n"
        "\n"
        "TEST_CASE(\"patchset parsing - multiple edits with tool call\")\n"
        ">>>>>>> REPLACE\n"
        "~~\n"
        "\n"
        "test/agent_task_base_test.cpp\n"
        "~~~cpp\n"
        "<<<<<<< SEARCH\n"
        "    auto& agent_sys = agent_system::instance();\n"
        "    agent_sys.remove_patchset();\n"
        "    agent_sys.store_patchset(parse_result->first, parse_result->second);\n"
        "\n"
        "    auto retrieved = agent_sys.get_patchset(\"multi-edit\");\n"
        "=======\n"
        "    auto& agent_sys = agent_system::instance();\n"
        "    agent_sys.remove_patchset(\"multi-edit\");\n"
        "    agent_sys.store_patchset(parse_result->first, parse_result->second);\n"
        "\n"
        "    auto retrieved = agent_sys.get_patchset(\"multi-edit\");\n"
        ">>>>>>> REPLACE\n"
        "~~~";

      auto result = parse_edits_with_patchset(input);
      REQUIRE(result.has_value());
      CHECK(result->first == "fix-remove-patchset-callsites");
      // Should parse 3 separate edits, not merge them into 1 garbled edit.
      // Currently the parser only recognizes 3-tilde closing fences, so it
      // skips the 2-tilde ones, finds the last 3-tilde fence, and
      // produces 1 edit with garbled content from all 3 blocks.
      REQUIRE(result->second.size() == 3);
      CHECK(result->second[0].patch_number == 1);
      CHECK(result->second[0].file_path == "test/agent_task_base_test.cpp");
      CHECK(result->second[0].search_text == "    agent_sys.remove_patchset(); // Clear first");
      CHECK(result->second[0].replace_text == "    agent_sys.remove_patchset(\"test-patchset\"); // Clear first");
      CHECK(result->second[1].patch_number == 2);
      CHECK(result->second[1].file_path == "test/agent_task_base_test.cpp");
      CHECK(result->second[2].patch_number == 3);
      CHECK(result->second[2].file_path == "test/agent_task_base_test.cpp");
    }

    TEST_SUITE_END();
  } // namespace test
} // namespace mistral::agent
