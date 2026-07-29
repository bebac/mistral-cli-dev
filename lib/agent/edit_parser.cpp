#include "agent/edit_parser.h"
#include "common/string_utils.h"

#include <cctype>
#include <algorithm>
#include <vector>

namespace mistral::agent
{
  namespace
  {
    const std::string SEARCH_MARKER = "<<<<<<< SEARCH";
    const std::string SEPARATOR_MARKER = "=======";
    const std::string REPLACE_MARKER = ">>>>>>> REPLACE";

    // Find a marker in content starting from position
    // Returns the position of the marker or std::string::npos if not found
    size_t find_marker(const std::string &content, const std::string &marker, size_t start_pos = 0)
    {
      size_t pos = content.find(marker, start_pos);
      // Ensure the marker is at the start of a line (or at position 0)
      while (pos != std::string::npos)
      {
        if (pos == 0 || content[pos - 1] == '\n')
        {
          // Check that the marker is followed by a newline or end of string
          size_t marker_end = pos + marker.length();
          if (marker_end == content.length() || content[marker_end] == '\n')
          {
            return pos;
          }
        }
        pos = content.find(marker, pos + 1);
      }
      return std::string::npos;
    }

    // Extract text between start_marker_end and end_marker_start
    // Handles empty content (for new file creation)
    std::string extract_between_markers(size_t start_pos, size_t end_pos, const std::string &content)
    {
      if (start_pos >= end_pos) {
        return "";
      }
      size_t actual_start = start_pos;
      // Skip the newline after the start marker
      if (actual_start < content.length() && content[actual_start] == '\n') {
        actual_start++;
      }
      size_t actual_end = end_pos;
      // Skip the newline before the end marker
      if (actual_end > 0 && content[actual_end - 1] == '\n') {
        actual_end--;
      }
      // Also handle the case where actual_start and actual_end are the same or adjacent
      // This happens with empty SEARCH blocks
      if (actual_start >= actual_end) {
        return "";
      }
      return content.substr(actual_start, actual_end - actual_start);
    }

  } // namespace

  std::optional<std::string> extract_patchset_id(const std::string &input)
  {
    const std::string prefix = "(patchset: ";
    size_t start = input.find(prefix);
    if (start == std::string::npos) {
      return std::nullopt;
    }
    start += prefix.length();

    size_t end = input.find(')', start);
    if (end == std::string::npos) {
      return std::nullopt;
    }

    std::string id = mistral::utils::trim(input.substr(start, end - start));

    // Validate ID format: alphanumeric, hyphens, underscores only, no spaces
    if (id.empty()) {
      return std::nullopt;
    }

    for (char c : id)
    {
      if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') {
        return std::nullopt;
      }
    }

    return id;
  }

  std::optional<file_edit_t> parse_edit_block(const std::string &file_path,
                                           const std::string &code_block_content)
  {
    // Find all markers
    size_t search_start = find_marker(code_block_content, SEARCH_MARKER);
    if (search_start == std::string::npos) {
      return std::nullopt;
    }

    size_t separator_pos = find_marker(code_block_content, SEPARATOR_MARKER, search_start);
    if (separator_pos == std::string::npos) {
      return std::nullopt;
    }

    size_t replace_start = find_marker(code_block_content, REPLACE_MARKER, separator_pos);
    if (replace_start == std::string::npos) {
      return std::nullopt;
    }

    // Extract search and replace text
    size_t search_content_start = search_start + SEARCH_MARKER.length();
    std::string search_text = extract_between_markers(
      search_content_start, separator_pos, code_block_content
    );

    size_t replace_content_start = separator_pos + SEPARATOR_MARKER.length();
    std::string replace_text = extract_between_markers(
      replace_content_start, replace_start, code_block_content
    );

    // Check for overwrite file format (SEARCH block contains only ".")
    // This is a special case: if search_text is exactly ".", it means overwrite the entire file
    bool is_overwrite = mistral::utils::trim(search_text) == ".";

    // For new file creation, search_text should be empty
    // For overwrite, search_text is "." (but we'll keep it as is for now)

    return file_edit_t{
      .file_path = file_path,
      .search_text = search_text,
      .replace_text = replace_text
    };
  }

  std::optional<std::pair<std::string, std::vector<file_edit_t>>>
  parse_edits_with_patchset(const std::string &input)
  {
    // Extract patchset ID
    auto patchset_id = extract_patchset_id(input);
    if (!patchset_id) {
      return std::nullopt;
    }

    std::vector<file_edit_t> edits;
    size_t pos = 0;
    size_t input_len = input.length();

    // Find all edit blocks
    while (pos < input_len)
    {
      // Find file path: should be on its own line, possibly with whitespace
      size_t line_start = input.find('\n', pos);
      if (line_start == std::string::npos) {
        break;
      }
      line_start++; // Move past the newline

      // Skip empty lines
      while (line_start < input_len && input[line_start] == '\n') {
        line_start++;
      }
      if (line_start >= input_len) {
        break;
      }

      // Find end of file path line
      size_t file_path_end = input.find('\n', line_start);
      if (file_path_end == std::string::npos) {
        file_path_end = input_len;
      }

      std::string file_path = mistral::utils::trim(
          input.substr(line_start, file_path_end - line_start));

      // Skip empty file paths
      if (file_path.empty())
      {
        pos = file_path_end + 1;
        continue;
      }

      // Check if next non-empty line starts with code block marker (```)
      size_t code_block_start = input.find("```", file_path_end);
      if (code_block_start == std::string::npos)
      {
        // No code block after file path, skip
        pos = file_path_end + 1;
        continue;
      }

      // Find the end of the code block (matching ```)
      // Code block can have optional language specifier: ```cpp or ```cpp\n
      size_t code_block_end = input.find("```", code_block_start + 3);
      if (code_block_end == std::string::npos)
      {
        // No closing code block marker, skip
        pos = file_path_end + 1;
        continue;
      }

      // Extract code block content (between the opening and closing ```)
      size_t content_start = code_block_start + 3; // Skip opening ```
      // Skip language specifier if present
      if (content_start < code_block_end)
      {
        size_t first_newline = input.find('\n', content_start);
        if (first_newline != std::string::npos && first_newline < code_block_end) {
          content_start = first_newline + 1;
        }
      }

      std::string code_content = input.substr(content_start, code_block_end - content_start);

      // Trim trailing newline from code content
      if (!code_content.empty() && code_content.back() == '\n') {
        code_content.pop_back();
      }

      // Parse the edit block
      if (auto edit = parse_edit_block(file_path, code_content)) {
        edits.push_back(*edit);
      }

      // Move past this code block
      pos = code_block_end + 3;
    }

    if (edits.empty()) {
      return std::nullopt;
    }

    return std::make_pair(*patchset_id, edits);
  }
} // namespace mistral::agent