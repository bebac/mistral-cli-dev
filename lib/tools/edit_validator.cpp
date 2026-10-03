#include "tools/edit_validator.h"
#include "common/string_utils.h"

#include <cctype>
#include <exception>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace mistral::tools
{

  namespace
  {
    // Check if a file path has a markdown extension (.md or .markdown)
    bool is_markdown_file(const std::string& path)
    {
      return (path.size() >= 3 &&
              path.compare(path.size() - 3, 3, ".md") == 0) ||
             (path.size() >= 9 &&
              path.compare(path.size() - 9, 9, ".markdown") == 0);
    }

    // Check if text contains patchset format markers that would indicate
    // a parsing error (e.g., a missing >>>>>>> REPLACE marker causing
    // subsequent edit blocks to be merged into the replace text).
    // These markers are extremely unlikely in non-markdown source files.
    bool contains_patch_markers(const std::string& text)
    {
      // Check for code fence marker (~~~) on its own line (optionally
      // followed by a language specifier and whitespace, then end-of-line).
      // This avoids false positives from ~~~ embedded in longer lines.
      {
        size_t pos = 0;
        while ((pos = text.find("~~~", pos)) != std::string::npos)
        {
          if (pos == 0 || text[pos - 1] == '\n')
          {
            // Verify the rest of the line is a valid fence: optional
            // whitespace, a single alphanumeric word (language specifier),
            // and optional trailing whitespace, then end-of-line.
            size_t i = pos + 3;
            while (i < text.size() && text[i] != '\n' &&
                   std::isspace(static_cast<unsigned char>(text[i])))
              i++;
            while (i < text.size() && text[i] != '\n' &&
                   std::isalnum(static_cast<unsigned char>(text[i])))
              i++;
            while (i < text.size() && text[i] != '\n' &&
                   std::isspace(static_cast<unsigned char>(text[i])))
              i++;
            if (i >= text.size() || text[i] == '\n')
            {
              return true;
            }
          }
          pos += 3;
        }
      }

      // Check for SEARCH marker at line start
      {
        size_t pos = 0;
        while ((pos = text.find("<<<<<<< SEARCH", pos)) != std::string::npos)
        {
          if (pos == 0 || text[pos - 1] == '\n')
          {
            return true;
          }
          pos += 14;
        }
      }

      // Check for REPLACE marker at line start
      {
        size_t pos = 0;
        while ((pos = text.find(">>>>>>> REPLACE", pos)) != std::string::npos)
        {
          if (pos == 0 || text[pos - 1] == '\n')
          {
            return true;
          }
          pos += 15;
        }
      }

      return false;
    }

    // Validate a file path for safety and correctness
    bool is_valid_file_path(const std::string& path)
    {
      if (path.empty()) return false;

      // Reject absolute paths
      if (path[0] == '/') return false;

      // Reject paths starting with Windows drive letters (e.g., C:)
      if (path.size() >= 2 && path[1] == ':' && std::isalpha(path[0])) return false;

      // Reject paths that look like markdown headers or console output
      if (path[0] == '#' || path[0] == '<' || path[0] == '>' ||
          path[0] == '[' || path[0] == '*' || path[0] == '-') return false;

      // Reject if path is exactly "." or ".."
      if (path == "." || path == "..") return false;

      // Reject if path contains consecutive slashes
      if (path.find("//") != std::string::npos) return false;

      // Reject if path starts or ends with slash
      if (path.back() == '/') return false;

      return true;
    }

    // Check if a "file path" is actually a patchset format marker line.
    // This happens when an edit block is missing its file path line (e.g.
    // the path was omitted for a subsequent edit of the same file): the
    // parser then treats the next marker line as the file path.
    bool is_patchset_marker_path(const std::string& path)
    {
      return path.rfind("~~~", 0) == 0 ||      // code fence (optionally with language)
             path.rfind("<<<<<<<", 0) == 0 ||  // SEARCH marker
             path.rfind("=======", 0) == 0 ||  // SEPARATOR marker
             path.rfind(">>>>>>>", 0) == 0;    // REPLACE marker
    }

    // Apply suspicious content check to a validation result.
    // If the replace_text contains patchset format markers (and the file
    // is not markdown), marks the result as invalid.
    void apply_suspicious_content_check(const file_edit_t& edit,
                                        edit_validation_t& result)
    {
      if (result.valid && !is_markdown_file(edit.file_path) &&
          contains_patch_markers(edit.replace_text))
      {
        result.valid = false;
        result.error_type = "suspicious_replace_content";
        result.error_message =
          "Replace text contains patchset format markers (~~~, <<<<<<< SEARCH, "
          "or >>>>>>> REPLACE). This usually indicates a missing >>>>>>> REPLACE "
          "marker in the original patchset, causing subsequent edit blocks to be "
          "merged into this edit's replace text.";
        result.is_fixable = true;
        result.suggestion =
          "Check that every edit block has a >>>>>>> REPLACE marker before the "
          "closing ~~~ fence, and that each edit block is properly separated.";
      }
    }
  } // namespace

  edit_validation_t validate_edit(const file_edit_t& edit)
  {
    edit_validation_t result;
    result.file_path = edit.file_path;
    result.search_text = edit.search_text;
    result.match_count = 0;
    result.valid = false;
    result.is_fixable = false;
    result.error_type = "";
    result.error_message = "";
    result.suggestion = "";
    result.patch_number = edit.patch_number; // Preserve patch number

    // Check for patchset format markers parsed as the file path: the edit
    // block is missing its file path line, so the parser consumed the next
    // marker line as the path. Report the structural cause, not the
    // misleading "file does not exist" / "invalid path" symptoms.
    if (is_patchset_marker_path(edit.file_path))
    {
      result.error_type = "missing_file_path";
      result.error_message =
        "Edit block is missing its file path line: '" + edit.file_path +
        "' is a patchset format marker, not a file path. This usually means "
        "the edit block did not start with its file path (e.g. the path line "
        "was omitted for a subsequent edit of the same file), so the parser "
        "consumed the next marker line as the path.";
      result.suggestion =
        "Re-emit the affected edit blocks, each starting with the file path "
        "on its own line, followed by the ~~~ fence. Every edit block needs "
        "its own file path line, even when editing the same file as the "
        "previous block.";
      return result;
    }

    // Check file path validity
    if (!is_valid_file_path(edit.file_path))
    {
      result.error_type = "invalid_path";
      result.error_message = "Invalid file path: " + edit.file_path;
      return result;
    }

    // Handle new file creation (empty search text)
    if (edit.search_text.empty())
    {
      if (fs::exists(edit.file_path))
      {
        result.error_type = "file_exists";
        result.error_message = "File already exists: " + edit.file_path;
      }
      else
      {
        result.valid = true;
      }
      apply_suspicious_content_check(edit, result);
      return result;
    }

    // Handle file overwrite (search text is exactly ".")
    if (edit.search_text == ".")
    {
      if (!fs::exists(edit.file_path))
      {
        result.error_type = "file_not_found";
        result.error_message = "File does not exist: " + edit.file_path;
      }
      else
      {
        result.valid = true;
      }
      apply_suspicious_content_check(edit, result);
      return result;
    }

    // Regular edit: check file exists
    if (!fs::exists(edit.file_path))
    {
      result.error_type = "file_not_found";
      result.error_message = "File does not exist: " + edit.file_path;
      return result;
    }

    // Read file content
    std::ifstream in(edit.file_path);
    if (!in)
    {
      result.error_type = "file_read_error";
      result.error_message = "Could not open file for reading: " + edit.file_path;
      return result;
    }

    std::string content;
    try
    {
      content.assign(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
    }
    catch (const std::exception&)
    {
      // On Linux, opening a directory with std::ifstream succeeds but the
      // first read throws (e.g. "Is a directory"). Report it as a read
      // error instead of letting the exception escape.
      result.error_type = "file_read_error";
      result.error_message = "Could not read file: " + edit.file_path;
      return result;
    }

    // Find all occurrences of search_text in the target file
    size_t pos = 0;
    while ((pos = content.find(edit.search_text, pos)) != std::string::npos)
    {
      result.match_positions.push_back(pos);
      result.match_count++;
      pos += edit.search_text.length();
    }

    if (result.match_count == 0)
    {
      result.error_type = "search_not_found";
      result.error_message = "Search text not found in file";
      result.is_fixable = true;
      result.suggestion = "Check for whitespace differences, escaping errors, or line ending issues";
    }
    else if (result.match_count > 1)
    {
      result.error_type = "multiple_matches";
      result.error_message = "Search text found " + std::to_string(result.match_count) +
                               " times in file";
      result.is_fixable = true;
      result.suggestion = "Make search text more specific by including surrounding context";
    }
    else
    {
      result.valid = true;
    }

    apply_suspicious_content_check(edit, result);

    return result;
  }

  validation_result_t validate_all_edits(const std::vector<file_edit_t>& edits)
  {
    validation_result_t result;
    result.validations.reserve(edits.size());

    for (const auto& edit : edits)
    {
      result.validations.push_back(validate_edit(edit));
    }

    return result;
  }

} // namespace mistral::tools