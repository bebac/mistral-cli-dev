#include "agent/edit_validator.h"
#include "common/string_utils.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace mistral::agent
{

  namespace
  {
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

    std::string content((std::istreambuf_iterator<char>(in)),
                        std::istreambuf_iterator<char>());

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

} // namespace mistral::agent