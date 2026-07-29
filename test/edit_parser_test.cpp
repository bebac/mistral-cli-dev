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
        "```cpp\n"
        "<<<<<<< SEARCH\n"
        "#include <iostream>\n"
        "=======\n"
        "#include <iostream>\n"
        "#include <vector>\n"
        ">>>>>>> REPLACE\n"
        "```";

      auto result = parse_edits_with_patchset(input);
      REQUIRE(result.has_value());
      CHECK(result->first == "my-patchset-id");
      REQUIRE(result->second.size() == 1);
      CHECK(result->second[0].file_path == "src/main.cpp");
      CHECK(result->second[0].search_text == "#include <iostream>");
      CHECK(result->second[0].replace_text == "#include <iostream>\n#include <vector>");
    }

    TEST_CASE("parse_edits_with_patchset - multiple edits")
    {
      std::string input =
        "Here are the changes (patchset: multi-edit):\n"
        "\n"
        "src/main.cpp\n"
        "```cpp\n"
        "<<<<<<< SEARCH\n"
        "#include <iostream>\n"
        "=======\n"
        "#include <iostream>\n"
        "#include <vector>\n"
        ">>>>>>> REPLACE\n"
        "```\n"
        "\n"
        "lib/utils.cpp\n"
        "```cpp\n"
        "<<<<<<< SEARCH\n"
        "void foo() {}\n"
        "=======\n"
        "void foo() { return 42; }\n"
        ">>>>>>> REPLACE\n"
        "```";

      auto result = parse_edits_with_patchset(input);
      REQUIRE(result.has_value());
      CHECK(result->first == "multi-edit");
      REQUIRE(result->second.size() == 2);
      CHECK(result->second[0].file_path == "src/main.cpp");
      CHECK(result->second[1].file_path == "lib/utils.cpp");
    }

    TEST_CASE("parse_edits_with_patchset - no patchset ID")
    {
      std::string input =
        "Here are the changes:\n"
        "\n"
        "src/main.cpp\n"
        "```cpp\n"
        "<<<<<<< SEARCH\n"
        "#include <iostream>\n"
        "=======\n"
        "#include <iostream>\n"
        "#include <vector>\n"
        ">>>>>>> REPLACE\n"
        "```";

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
        "```cpp\n"
        "<<<<<<< SEARCH\n"
        "=======\n"
        "// New file content\n"
        "int main() { return 0; }\n"
        ">>>>>>> REPLACE\n"
        "```";

      auto result = parse_edits_with_patchset(input);
      REQUIRE(result.has_value());
      CHECK(result->first == "new-file");
      REQUIRE(result->second.size() == 1);
      CHECK(result->second[0].file_path == "new_file.cpp");
      CHECK(result->second[0].search_text == "");
      CHECK(result->second[0].replace_text == "// New file content\nint main() { return 0; }");
    }

    TEST_CASE("parse_edits_with_patchset - file path with spaces")
    {
      std::string input =
        "Here are the changes (patchset: spaced-path):\n"
        "\n"
        "src/my file.cpp\n"
        "```cpp\n"
        "<<<<<<< SEARCH\n"
        "old\n"
        "=======\n"
        "new\n"
        ">>>>>>> REPLACE\n"
        "```";

      auto result = parse_edits_with_patchset(input);
      REQUIRE(result.has_value());
      CHECK(result->first == "spaced-path");
      REQUIRE(result->second.size() == 1);
      CHECK(result->second[0].file_path == "src/my file.cpp");
    }

    TEST_CASE("parse_edits_with_patchset - without language specifier")
    {
      std::string input =
        "Here are the changes (patchset: no-lang):\n"
        "\n"
        "src/main.cpp\n"
        "```\n"
        "<<<<<<< SEARCH\n"
        "#include <iostream>\n"
        "=======\n"
        "#include <iostream>\n"
        "#include <vector>\n"
        ">>>>>>> REPLACE\n"
        "```";

      auto result = parse_edits_with_patchset(input);
      REQUIRE(result.has_value());
      CHECK(result->first == "no-lang");
      REQUIRE(result->second.size() == 1);
      CHECK(result->second[0].file_path == "src/main.cpp");
      CHECK(result->second[0].search_text == "#include <iostream>");
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
      CHECK(result->replace_text == "=======");
    }

    TEST_SUITE_END();
  } // namespace test
} // namespace mistral::agent
