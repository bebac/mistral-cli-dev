#include "tools/edit_validator.h"
#include "doctest/doctest.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace mistral::tools
{
  namespace test
  {
    TEST_SUITE_BEGIN("edit_validator");

    // Helper to create a temporary file with relative path
    class temp_file
    {
    public:
      explicit temp_file(const std::string& content = "", int test_line = 0,
                         const std::string& ext = ".tmp")
      {
        // Use a relative path that's valid for the validator
        // Include test line number to avoid collisions between different test cases
        std::string filename = "test_file_";
        if (test_line > 0)
        {
          filename += std::to_string(test_line) + "_";
        }
        filename += std::to_string(next_id_++) + ext;
        path_ = filename;

        std::ofstream out(path_);
        if (!content.empty())
        {
          out << content;
        }
      }

      ~temp_file()
      {
        if (std::filesystem::exists(path_))
        {
          std::filesystem::remove(path_);
        }
      }

      std::string path() const { return path_; }

      void write(const std::string& content)
      {
        std::ofstream out(path_);
        out << content;
      }

    private:
      std::string path_;
      static inline int next_id_ = 0;
    };

    TEST_CASE("validate_edit - invalid file paths")
    {
      // Absolute path
      {
        file_edit_t edit;
        edit.file_path = "/etc/passwd";
        edit.search_text = "root:";
        edit.replace_text = "admin:";

        auto result = validate_edit(edit);
        CHECK(!result.valid);
        CHECK(result.error_type == "invalid_path");
        CHECK(result.error_message.find("/etc/passwd") != std::string::npos);
      }

      // Windows drive letter
      {
        file_edit_t edit;
        edit.file_path = "C:\\Windows\\system32";
        edit.search_text = "test";
        edit.replace_text = "test2";

        auto result = validate_edit(edit);
        CHECK(!result.valid);
        CHECK(result.error_type == "invalid_path");
      }

      // Path starting with special characters
      for (char c : {'#', '<', '>', '[', '*', '-'})
      {
        file_edit_t edit;
        edit.file_path = std::string(1, c) + "file.cpp";
        edit.search_text = "test";
        edit.replace_text = "test2";

        auto result = validate_edit(edit);
        CHECK(!result.valid);
        CHECK(result.error_type == "invalid_path");
      }

      // Path is exactly "."
      {
        file_edit_t edit;
        edit.file_path = ".";
        edit.search_text = "test";
        edit.replace_text = "test2";

        auto result = validate_edit(edit);
        CHECK(!result.valid);
        CHECK(result.error_type == "invalid_path");
      }

      // Path is exactly ".."
      {
        file_edit_t edit;
        edit.file_path = "..";
        edit.search_text = "test";
        edit.replace_text = "test2";

        auto result = validate_edit(edit);
        CHECK(!result.valid);
        CHECK(result.error_type == "invalid_path");
      }

      // Path with consecutive slashes
      {
        file_edit_t edit;
        edit.file_path = "path//to//file.cpp";
        edit.search_text = "test";
        edit.replace_text = "test2";

        auto result = validate_edit(edit);
        CHECK(!result.valid);
        CHECK(result.error_type == "invalid_path");
      }

      // Path ending with slash
      {
        file_edit_t edit;
        edit.file_path = "path/to/dir/";
        edit.search_text = "test";
        edit.replace_text = "test2";

        auto result = validate_edit(edit);
        CHECK(!result.valid);
        CHECK(result.error_type == "invalid_path");
      }

      // Empty path
      {
        file_edit_t edit;
        edit.file_path = "";
        edit.search_text = "test";
        edit.replace_text = "test2";

        auto result = validate_edit(edit);
        CHECK(!result.valid);
        CHECK(result.error_type == "invalid_path");
      }
    }

    TEST_CASE("validate_edit - patchset markers parsed as file path")
    {
      // A '~~~' fence line parsed as the file path: this happens when an
      // edit block is missing its file path line (e.g. the path was
      // omitted for a subsequent edit of the same file).
      {
        file_edit_t edit;
        edit.file_path = "~~~";
        edit.search_text = "test";
        edit.replace_text = "test2";

        auto result = validate_edit(edit);
        CHECK(!result.valid);
        CHECK(result.error_type == "missing_file_path");
        CHECK(!result.is_fixable);
        CHECK(result.error_message.find("file path") != std::string::npos);
        CHECK(result.suggestion.find("file path") != std::string::npos);
      }

      // Fence with language specifier
      {
        file_edit_t edit;
        edit.file_path = "~~~cpp";
        edit.search_text = "test";
        edit.replace_text = "test2";

        auto result = validate_edit(edit);
        CHECK(!result.valid);
        CHECK(result.error_type == "missing_file_path");
      }

      // REPLACE marker parsed as the file path
      {
        file_edit_t edit;
        edit.file_path = ">>>>>>> REPLACE";
        edit.search_text = "test";
        edit.replace_text = "test2";

        auto result = validate_edit(edit);
        CHECK(!result.valid);
        CHECK(result.error_type == "missing_file_path");
      }

      // SEARCH and SEPARATOR markers parsed as the file path
      {
        file_edit_t edit;
        edit.file_path = "<<<<<<< SEARCH";
        edit.search_text = "test";
        edit.replace_text = "test2";

        auto result = validate_edit(edit);
        CHECK(!result.valid);
        CHECK(result.error_type == "missing_file_path");
      }
      {
        file_edit_t edit;
        edit.file_path = "=======";
        edit.search_text = "test";
        edit.replace_text = "test2";

        auto result = validate_edit(edit);
        CHECK(!result.valid);
        CHECK(result.error_type == "missing_file_path");
      }
    }

    TEST_CASE("validate_edit - valid file path but file does not exist")
    {
      file_edit_t edit;
      edit.file_path = "nonexistent_file_12345.cpp";
      edit.search_text = "test";
      edit.replace_text = "test2";

      auto result = validate_edit(edit);
      CHECK(!result.valid);
      CHECK(result.error_type == "file_not_found");
      CHECK(result.error_message.find("nonexistent_file_12345.cpp") != std::string::npos);
    }

    TEST_CASE("validate_edit - new file creation")
    {
      temp_file tf("", __LINE__);
      std::string path = tf.path();

      // File doesn't exist yet
      std::filesystem::remove(path);

      file_edit_t edit;
      edit.file_path = path;
      edit.search_text = ""; // Empty search text means new file
      edit.replace_text = "new content";

      auto result = validate_edit(edit);
      CHECK(result.valid);
      CHECK(result.match_count == 0);
      CHECK(result.error_type.empty());
    }

    TEST_CASE("validate_edit - preserves patch_number")
    {
      temp_file tf("test content", __LINE__);

      file_edit_t edit;
      edit.file_path = tf.path();
      edit.search_text = "test";
      edit.replace_text = "replacement";
      edit.patch_number = 42; // Set a specific patch number

      auto result = validate_edit(edit);
      CHECK(result.valid);
      CHECK(result.patch_number == 42); // Verify it's preserved
    }

    TEST_CASE("validate_edit - new file creation but file already exists")
    {
      temp_file tf("existing content");

      file_edit_t edit;
      edit.file_path = tf.path();
      edit.search_text = ""; // Empty search text means new file
      edit.replace_text = "new content";

      auto result = validate_edit(edit);
      CHECK(!result.valid);
      CHECK(result.error_type == "file_exists");
      CHECK(result.error_message.find(tf.path()) != std::string::npos);
    }

    TEST_CASE("validate_edit - file overwrite with dot marker")
    {
      temp_file tf("existing content");

      file_edit_t edit;
      edit.file_path = tf.path();
      edit.search_text = "."; // Dot means overwrite entire file
      edit.replace_text = "new content";

      auto result = validate_edit(edit);
      CHECK(result.valid);
      CHECK(result.match_count == 0);
    }

    TEST_CASE("validate_edit - file overwrite with dot marker but file does not exist")
    {
      temp_file tf("", __LINE__);
      std::string path = tf.path();
      std::filesystem::remove(path);

      file_edit_t edit;
      edit.file_path = path;
      edit.search_text = "."; // Dot means overwrite entire file
      edit.replace_text = "new content";

      auto result = validate_edit(edit);
      CHECK(!result.valid);
      CHECK(result.error_type == "file_not_found");
    }

    TEST_CASE("validate_edit - search text not found")
    {
      temp_file tf("some content here");

      file_edit_t edit;
      edit.file_path = tf.path();
      edit.search_text = "not present";
      edit.replace_text = "replacement";

      auto result = validate_edit(edit);
      CHECK(!result.valid);
      CHECK(result.error_type == "search_not_found");
      CHECK(result.match_count == 0);
      CHECK(result.is_fixable == true);
      CHECK(!result.suggestion.empty());
    }

    TEST_CASE("validate_edit - search text found once")
    {
      temp_file tf("hello world");

      file_edit_t edit;
      edit.file_path = tf.path();
      edit.search_text = "hello";
      edit.replace_text = "goodbye";

      auto result = validate_edit(edit);
      CHECK(result.valid);
      CHECK(result.match_count == 1);
      CHECK(result.match_positions.size() == 1);
      CHECK(result.match_positions[0] == 0);
    }

    TEST_CASE("validate_edit - search text found multiple times")
    {
      temp_file tf("test test test");

      file_edit_t edit;
      edit.file_path = tf.path();
      edit.search_text = "test";
      edit.replace_text = "replacement";

      auto result = validate_edit(edit);
      CHECK(!result.valid);
      CHECK(result.error_type == "multiple_matches");
      CHECK(result.match_count == 3);
      CHECK(result.match_positions.size() == 3);
      CHECK(result.match_positions[0] == 0);
      CHECK(result.match_positions[1] == 5);
      CHECK(result.match_positions[2] == 10);
      CHECK(result.is_fixable == true);
      CHECK(!result.suggestion.empty());
    }

    TEST_CASE("validate_edit - search text with special characters")
    {
      temp_file tf("line1\nline2\nline3");

      file_edit_t edit;
      edit.file_path = tf.path();
      edit.search_text = "line2\n";
      edit.replace_text = "replacement\n";

      auto result = validate_edit(edit);
      CHECK(result.valid);
      CHECK(result.match_count == 1);
    }

    TEST_CASE("validate_edit - empty file with empty search text")
    {
      temp_file tf("");

      file_edit_t edit;
      edit.file_path = tf.path();
      edit.search_text = ""; // Empty search text means new file creation
      edit.replace_text = "new content";

      auto result = validate_edit(edit);
      // Empty search text in existing file means file already exists
      CHECK(!result.valid);
      CHECK(result.error_type == "file_exists");
      CHECK(result.error_message.find(tf.path()) != std::string::npos);
    }

    TEST_CASE("validate_edit - multiline search text")
    {
      temp_file tf("first line\nsecond line\nthird line");

      file_edit_t edit;
      edit.file_path = tf.path();
      edit.search_text = "first line\nsecond line";
      edit.replace_text = "replacement";

      auto result = validate_edit(edit);
      CHECK(result.valid);
      CHECK(result.match_count == 1);
      CHECK(result.match_positions[0] == 0);
    }

    TEST_CASE("validate_edit - file read error")
    {
      // Create a directory instead of a file
      temp_file tf("", __LINE__);
      std::string path = tf.path();
      std::filesystem::remove(path);
      std::filesystem::create_directory(path);

      file_edit_t edit;
      edit.file_path = path;
      edit.search_text = "test";
      edit.replace_text = "test2";

      auto result = validate_edit(edit);
      // On most systems, trying to read a directory as a file will fail
      // But behavior may vary, so we just check it's not valid
      CHECK(!result.valid);
    }

    TEST_CASE("validate_all_edits - all valid")
    {
      temp_file tf1("content1");
      temp_file tf2("content2");

      std::vector<file_edit_t> edits;
      edits.push_back({.file_path = tf1.path(), .search_text = "content1", .replace_text = "new1"});
      edits.push_back({.file_path = tf2.path(), .search_text = "content2", .replace_text = "new2"});

      auto result = validate_all_edits(edits);
      CHECK(result.all_valid());
      CHECK(result.validations.size() == 2);
      CHECK(result.validations[0].valid);
      CHECK(result.validations[1].valid);
      CHECK(result.fixable_error_count() == 0);
      CHECK(result.get_fixable_errors().empty());
      CHECK(result.get_non_fixable_errors().empty());
    }

    TEST_CASE("validate_all_edits - preserves patch_numbers")
    {
      temp_file tf1("content1", __LINE__);
      temp_file tf2("content2", __LINE__);

      std::vector<file_edit_t> edits;
      edits.push_back({tf1.path(), "content1", "new1", 1});
      edits.push_back({tf2.path(), "content2", "new2", 2});

      auto result = validate_all_edits(edits);
      CHECK(result.all_valid());
      REQUIRE(result.validations.size() == 2);
      CHECK(result.validations[0].patch_number == 1);
      CHECK(result.validations[1].patch_number == 2);
    }

    TEST_CASE("validate_all_edits - error results include patch_numbers")
    {
      // Create a temp file
      temp_file tf("test content", __LINE__);

      std::vector<file_edit_t> edits;
      // This will fail - search text not found
      edits.push_back({tf.path(), "not present", "replacement", 1});
      // This will succeed
      edits.push_back({tf.path(), "test", "replacement", 2});

      auto result = validate_all_edits(edits);
      CHECK(!result.all_valid());
      REQUIRE(result.validations.size() == 2);

      // First edit should have error with patch_number 1
      CHECK(result.validations[0].patch_number == 1);
      CHECK(!result.validations[0].valid);
      CHECK(result.validations[0].error_type == "search_not_found");

      // Second edit should be valid with patch_number 2
      CHECK(result.validations[1].patch_number == 2);
      CHECK(result.validations[1].valid);
    }

    TEST_CASE("validate_all_edits - mixed valid and invalid")
    {
      temp_file tf1("content1");
      temp_file tf2("content2");

      std::vector<file_edit_t> edits;
      // Valid edit
      edits.push_back({.file_path = tf1.path(), .search_text = "content1", .replace_text = "new1"});
      // Invalid: file not found
      edits.push_back({.file_path = "nonexistent.cpp", .search_text = "test", .replace_text = "test2"});
      // Invalid but fixable: search not found
      edits.push_back({.file_path = tf2.path(), .search_text = "not found", .replace_text = "new2"});

      auto result = validate_all_edits(edits);
      CHECK(!result.all_valid());
      CHECK(result.validations.size() == 3);
      CHECK(result.validations[0].valid);
      CHECK(!result.validations[1].valid);
      CHECK(!result.validations[2].valid);
      CHECK(result.fixable_error_count() == 1); // Only the search_not_found is fixable
      CHECK(result.get_fixable_errors().size() == 1);
      CHECK(result.get_non_fixable_errors().size() == 1);
    }

    TEST_CASE("validate_all_edits - empty edits")
    {
      std::vector<file_edit_t> edits;

      auto result = validate_all_edits(edits);
      CHECK(result.all_valid());
      CHECK(result.validations.empty());
      CHECK(result.fixable_error_count() == 0);
    }

    TEST_CASE("validate_all_edits - all invalid non-fixable")
    {
      std::vector<file_edit_t> edits;
      edits.push_back({.file_path = "/etc/passwd", .search_text = "test", .replace_text = "test2"});
      edits.push_back({.file_path = "C:\\Windows", .search_text = "test", .replace_text = "test2"});
      edits.push_back({.file_path = "nonexistent.cpp", .search_text = "test", .replace_text = "test2"});

      auto result = validate_all_edits(edits);
      CHECK(!result.all_valid());
      CHECK(result.validations.size() == 3);
      CHECK(result.fixable_error_count() == 0);
      CHECK(result.get_fixable_errors().empty());
      CHECK(result.get_non_fixable_errors().size() == 3);
    }

    TEST_CASE("validate_edit - suspicious patch markers in replace text")
    {
      temp_file tf("hello world");

      // Replace text contains a code fence marker (~~~) at line start
      file_edit_t edit;
      edit.file_path = tf.path();
      edit.search_text = "hello";
      edit.replace_text = "goodbye\n~~~\n<<< SEARCH\nmore stuff";

      auto result = validate_edit(edit);
      CHECK(!result.valid);
      CHECK(result.error_type == "suspicious_replace_content");
      CHECK(result.is_fixable == true);
      CHECK(!result.suggestion.empty());
    }

    TEST_CASE("validate_edit - SEARCH marker in replace text")
    {
      temp_file tf("hello world");

      file_edit_t edit;
      edit.file_path = tf.path();
      edit.search_text = "hello";
      edit.replace_text = "goodbye\n<<<<<<< SEARCH\nother_file.cpp";

      auto result = validate_edit(edit);
      CHECK(!result.valid);
      CHECK(result.error_type == "suspicious_replace_content");
    }

    TEST_CASE("validate_edit - REPLACE marker in replace text")
    {
      temp_file tf("hello world");

      file_edit_t edit;
      edit.file_path = tf.path();
      edit.search_text = "hello";
      edit.replace_text = "goodbye\n>>>>>>> REPLACE\n~~~";

      auto result = validate_edit(edit);
      CHECK(!result.valid);
      CHECK(result.error_type == "suspicious_replace_content");
    }

    TEST_CASE("validate_edit - patch markers not at line start are OK")
    {
      temp_file tf("hello world");

      // ~~~ embedded in a word (not at line start) should be fine
      file_edit_t edit;
      edit.file_path = tf.path();
      edit.search_text = "hello";
      edit.replace_text = "a~~~b";

      auto result = validate_edit(edit);
      CHECK(result.valid);
    }

    TEST_CASE("validate_edit - ~~~ with trailing content on same line is OK")
    {
      temp_file tf("hello world");

      // ~~~ at line start but followed by non-whitespace content
      // (not a standalone fence) should not trigger the check.
      file_edit_t edit;
      edit.file_path = tf.path();
      edit.search_text = "hello";
      edit.replace_text = "goodbye\n~~~this is a comment\nmore stuff";

      auto result = validate_edit(edit);
      CHECK(result.valid);
    }

    TEST_CASE("validate_edit - ~~~ with language specifier on own line is flagged")
    {
      temp_file tf("hello world");

      // ~~~cpp on its own line is a valid code fence and should be flagged.
      file_edit_t edit;
      edit.file_path = tf.path();
      edit.search_text = "hello";
      edit.replace_text = "goodbye\n~~~cpp\nsome code";

      auto result = validate_edit(edit);
      CHECK(!result.valid);
      CHECK(result.error_type == "suspicious_replace_content");
    }

    TEST_CASE("validate_edit - ~~~ with trailing whitespace on own line is flagged")
    {
      temp_file tf("hello world");

      // ~~~ followed by only whitespace then end-of-line is a fence.
      file_edit_t edit;
      edit.file_path = tf.path();
      edit.search_text = "hello";
      edit.replace_text = "goodbye\n~~~  \nmore stuff";

      auto result = validate_edit(edit);
      CHECK(!result.valid);
      CHECK(result.error_type == "suspicious_replace_content");
    }

    TEST_CASE("validate_edit - ~~~ at end of replace text is flagged")
    {
      temp_file tf("hello world");

      // ~~~ as the last line (no trailing newline) is a valid fence.
      file_edit_t edit;
      edit.file_path = tf.path();
      edit.search_text = "hello";
      edit.replace_text = "goodbye\n~~~";

      auto result = validate_edit(edit);
      CHECK(!result.valid);
      CHECK(result.error_type == "suspicious_replace_content");
    }

    TEST_CASE("validate_edit - markdown files skip patch marker check")
    {
      temp_file tf("hello world", __LINE__, ".md");

      file_edit_t edit;
      edit.file_path = tf.path();
      edit.search_text = "hello";
      edit.replace_text = "goodbye\n~~~\n<<<<<<< SEARCH\n>>>>>>> REPLACE";

      auto result = validate_edit(edit);
      CHECK(result.valid);
    }

    TEST_CASE("validate_edit - .markdown files skip patch marker check")
    {
      temp_file tf("hello world", __LINE__, ".markdown");

      file_edit_t edit;
      edit.file_path = tf.path();
      edit.search_text = "hello";
      edit.replace_text = "goodbye\n~~~\n<<<<<<< SEARCH\n>>>>>>> REPLACE";

      auto result = validate_edit(edit);
      CHECK(result.valid);
    }

    TEST_CASE("validate_edit - patch markers in indented string literals are OK")
    {
      // Simulates a legitimate edit whose replace text contains patch markers
      // inside C++ string literals (indented, not at line start).
      // This is the actual apply_suspicious_content_check function which
      // mentions ~~~, <<<<<<< SEARCH, and >>>>>>> REPLACE in its error messages.
      temp_file tf("  } // namespace\n\n  edit_validation_t validate_edit(const file_edit_t& edit)\n  {");

      file_edit_t edit;
      edit.file_path = tf.path();
      edit.search_text = "  } // namespace\n\n  edit_validation_t validate_edit(const file_edit_t& edit)\n  {";
      edit.replace_text =
        "    void apply_suspicious_content_check(const file_edit_t& edit,\n"
        "                                        edit_validation_t& result)\n"
        "    {\n"
        "      result.error_message =\n"
        "        \"Replace text contains patchset format markers (~~~, <<<<<<< SEARCH, \"\n"
        "        \"or >>>>>>> REPLACE). This usually indicates a missing >>>>>>> REPLACE \"\n"
        "        \"marker in the original patchset.\";\n"
        "    }\n"
        "  } // namespace\n\n"
        "  edit_validation_t validate_edit(const file_edit_t& edit)\n"
        "  {";

      auto result = validate_edit(edit);
      CHECK(result.valid);
    }

    TEST_CASE("validate_edit - corrupted merge with markers at line start")
    {
      // Simulates the corruption scenario: a missing >>>>>>> REPLACE marker
      // causes the next edit block's content to be merged into replace_text.
      // The merged content brings ~~~ and <<<<<<< SEARCH at line start.
      temp_file tf("  } // namespace\n\n  edit_validation_t validate_edit(const file_edit_t& edit)\n  {");

      file_edit_t edit;
      edit.file_path = tf.path();
      edit.search_text = "  } // namespace\n\n  edit_validation_t validate_edit(const file_edit_t& edit)\n  {";
      edit.replace_text =
        "    void apply_suspicious_content_check(const file_edit_t& edit,\n"
        "                                        edit_validation_t& result)\n"
        "    {\n"
        "      result.error_message =\n"
        "        \"Replace text contains patchset format markers (~~~, <<<<<<< SEARCH, \"\n"
        "        \"or >>>>>>> REPLACE.\";\n"
        "    }\n"
        "  } // namespace\n"
        "~~~\n"
        "\n"
        "lib/tools/edit_validator.cpp\n"
        "~~~\n"
        "<<<<<<< SEARCH\n"
        "  // Handle file overwrite\n"
        "=======\n"
        "  // Handle file overwrite - modified\n"
        ">>>>>>> REPLACE";

      auto result = validate_edit(edit);
      CHECK(!result.valid);
      CHECK(result.error_type == "suspicious_replace_content");
      CHECK(result.is_fixable == true);
    }

    TEST_CASE("validate_edit - new file with patch markers (non-md)")
    {
      std::string new_path = "test_new_markers_" +
        std::to_string(__LINE__) + ".cpp";
      std::filesystem::remove(new_path);

      file_edit_t edit;
      edit.file_path = new_path;
      edit.search_text = ""; // New file creation
      edit.replace_text = "#include <iostream>\n~~~\n<<<<<<< SEARCH\nold\n=======\nnew\n>>>>>>> REPLACE";

      auto result = validate_edit(edit);
      CHECK(!result.valid);
      CHECK(result.error_type == "suspicious_replace_content");

      std::filesystem::remove(new_path);
    }

    TEST_CASE("validate_all_edits - all invalid fixable")
    {
      temp_file tf1("content1");
      temp_file tf2("content2");

      std::vector<file_edit_t> edits;
      edits.push_back({.file_path = tf1.path(), .search_text = "not found1", .replace_text = "new1"});
      edits.push_back({.file_path = tf2.path(), .search_text = "not found2", .replace_text = "new2"});

      auto result = validate_all_edits(edits);
      CHECK(!result.all_valid());
      CHECK(result.validations.size() == 2);
      CHECK(result.fixable_error_count() == 2);
      CHECK(result.get_fixable_errors().size() == 2);
      CHECK(result.get_non_fixable_errors().empty());
    }

    TEST_SUITE_END();
  } // namespace test
} // namespace mistral::tools