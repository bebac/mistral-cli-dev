#include "agent/edit_parser.h"
#include "common/string_utils.h"

#include <cctype>
#include <algorithm>
#include <string_view>
#include <vector>

namespace mistral::agent
{
  namespace
  {
    constexpr std::string_view SEARCH_MARKER = "<<<<<<< SEARCH";
    constexpr std::string_view SEPARATOR_MARKER = "=======";
    constexpr std::string_view REPLACE_MARKER = ">>>>>>> REPLACE";

    // Find a marker in content starting from position
    // Returns the position of the marker or std::string_view::npos if not found
    size_t find_marker(std::string_view content, std::string_view marker, size_t start_pos = 0)
    {
      size_t pos = content.find(marker, start_pos);
      // Ensure the marker is at the start of a line (or at position 0)
      while (pos != std::string_view::npos)
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
      return std::string_view::npos;
    }

    // Check if there's a code fence marker at the given position
    // Edit blocks use 5 backticks to allow nested 3-backtick code blocks
    constexpr std::string_view CODE_FENCE = "`````";
    bool is_fence_marker(std::string_view content, size_t pos)
    {
      return content.compare(pos, CODE_FENCE.length(), CODE_FENCE) == 0;
    }

    // Find the last occurrence of a marker at line start, searching backwards
    // from end_pos. Returns the position or std::string_view::npos if not found.
    size_t rfind_marker(std::string_view content, std::string_view marker, size_t end_pos)
    {
      if (end_pos > content.length()) {
        end_pos = content.length();
      }
      size_t pos = content.rfind(marker, end_pos);
      while (pos != std::string_view::npos)
      {
        if (pos == 0 || content[pos - 1] == '\n')
        {
          size_t marker_end = pos + marker.length();
          if (marker_end == content.length() || content[marker_end] == '\n')
          {
            return pos;
          }
        }
        if (pos == 0) {
          break;
        }
        pos = content.rfind(marker, pos - 1);
      }
      return std::string_view::npos;
    }

    // Find the closing code fence for an edit block starting at code_block_start.
    // The closing fence is a CODE_FENCE at line start that is immediately preceded
    // (ignoring whitespace/newlines) by a REPLACE_MARKER at line start, where that
    // REPLACE_MARKER is not inside a nested CODE_FENCE block.
    // This correctly handles both nested 5-backtick fences and literal REPLACE markers
    // inside the search/replace content.
    size_t find_closing_fence(std::string_view input, size_t code_block_start)
    {
      size_t search_from = code_block_start + CODE_FENCE.length();
      while (true)
      {
        size_t fence_pos = input.find(CODE_FENCE, search_from);
        if (fence_pos == std::string_view::npos) {
          return std::string_view::npos;
        }
        // Check that the fence is at line start
        if (fence_pos == 0 || input[fence_pos - 1] == '\n')
        {
          // Search backwards for the last REPLACE marker at line start before this fence
          size_t replace_pos = rfind_marker(input, REPLACE_MARKER, fence_pos);
          if (replace_pos != std::string_view::npos && replace_pos > code_block_start)
          {
            // Check that there's only whitespace/newlines between REPLACE and the fence
            size_t after_replace = replace_pos + REPLACE_MARKER.length();
            bool only_whitespace = true;
            for (size_t i = after_replace; i < fence_pos; ++i)
            {
              if (input[i] != '\n' && input[i] != ' ' && input[i] != '\t' && input[i] != '\r')
              {
                only_whitespace = false;
                break;
              }
            }
            if (only_whitespace)
            {
              // Verify this REPLACE is not inside a nested CODE_FENCE block.
              // Count CODE_FENCE markers at line start between the opening fence and REPLACE.
              // If odd, the REPLACE is inside a nested block and should be skipped.
              int fence_depth = 0;
              size_t scan = code_block_start + CODE_FENCE.length();
              while (scan < replace_pos)
              {
                size_t nf = input.find(CODE_FENCE, scan);
                if (nf == std::string_view::npos || nf >= replace_pos) {
                  break;
                }
                if (nf == 0 || input[nf - 1] == '\n')
                {
                  fence_depth++;
                }
                scan = nf + CODE_FENCE.length();
              }
              if (fence_depth % 2 == 0)
              {
                return fence_pos;
              }
            }
          }
        }
        search_from = fence_pos + CODE_FENCE.length();
      }
    }

    // Extract text between start_marker_end and end_marker_start
    // Handles empty content (for new file creation)
    std::string extract_between_markers(size_t start_pos, size_t end_pos, std::string_view content)
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
      return std::string(content.substr(actual_start, actual_end - actual_start));
    }

  } // namespace

  std::optional<std::string> extract_patchset_id(std::string_view input)
  {
    constexpr std::string_view prefix = "(patchset: ";
    size_t start = input.find(prefix);
    if (start == std::string_view::npos) {
      return std::nullopt;
    }
    start += prefix.length();

    size_t end = input.find(')', start);
    if (end == std::string_view::npos) {
      return std::nullopt;
    }

    std::string id = mistral::utils::trim(std::string(input.substr(start, end - start)));

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

  std::optional<file_edit_t> parse_edit_block(std::string_view file_path,
                                           std::string_view code_block_content)
  {
    // Find all markers
    size_t search_start = find_marker(code_block_content, SEARCH_MARKER);
    if (search_start == std::string_view::npos) {
      return std::nullopt;
    }

    size_t separator_pos = find_marker(code_block_content, SEPARATOR_MARKER, search_start);
    if (separator_pos == std::string_view::npos) {
      return std::nullopt;
    }

    size_t replace_start = code_block_content.rfind(REPLACE_MARKER);
    if (replace_start == std::string_view::npos) {
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
      .file_path = std::string(file_path),
      .search_text = search_text,
      .replace_text = replace_text
    };
  }

  std::optional<std::vector<file_edit_t>>
  parse_edit_blocks(std::string_view input)
  {
    std::vector<file_edit_t> edits;
    size_t pos = 0;
    size_t input_len = input.length();

    // Find all edit blocks
    while (pos < input_len)
    {
      // Find file path: should be on its own line, possibly with whitespace
      size_t line_start = input.find('\n', pos);
      if (line_start == std::string_view::npos) {
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
      if (file_path_end == std::string_view::npos) {
        file_path_end = input_len;
      }

      std::string file_path = mistral::utils::trim(
          std::string(input.substr(line_start, file_path_end - line_start)));

      // Skip empty file paths
      if (file_path.empty())
      {
        pos = file_path_end + 1;
        continue;
      }

      // Check if next non-empty line starts with code block marker
      // Must be on the immediate next line (or after skipping only whitespace)
      size_t search_start = file_path_end;
      // Skip any newlines and whitespace between file path and code block
      while (search_start < input_len && (input[search_start] == '\n' || input[search_start] == ' ' || input[search_start] == '\t')) {
        search_start++;
      }
      if (!is_fence_marker(input, search_start)) {
        // No code block immediately after file path, skip
        pos = file_path_end + 1;
        continue;
      }
      size_t code_block_start = search_start;

      // Find the closing code fence for this edit block.
      // This handles nested code fences inside the search/replace content
      // (e.g., when editing a markdown file that itself documents patchset format).
      size_t code_block_end = find_closing_fence(input, code_block_start);
      if (code_block_end == std::string_view::npos)
      {
        // No closing code block marker found, skip
        pos = file_path_end + 1;
        continue;
      }

      // Extract code block content (between the opening and closing fence)
      size_t content_start = code_block_start + CODE_FENCE.length(); // Skip opening fence
      // Skip language specifier if present
      if (content_start < code_block_end)
      {
        size_t first_newline = input.find('\n', content_start);
        if (first_newline != std::string_view::npos && first_newline < code_block_end) {
          content_start = first_newline + 1;
        }
      }

      std::string_view code_content = input.substr(content_start, code_block_end - content_start);

      // Trim trailing newline from code content
      if (!code_content.empty() && code_content.back() == '\n') {
        code_content = code_content.substr(0, code_content.length() - 1);
      }

      // Parse the edit block
      if (auto edit = parse_edit_block(file_path, code_content)) {
        // Assign patch number (1-indexed)
        edit->patch_number = edits.size() + 1;
        edits.push_back(*edit);
      }

      // Move past this code block
      pos = code_block_end + CODE_FENCE.length();
    }

    if (edits.empty()) {
      return std::nullopt;
    }

    return edits;
  }

  std::optional<std::pair<std::string, std::vector<file_edit_t>>>
  parse_edits_with_patchset(std::string_view input)
  {
    // Extract patchset ID
    auto patchset_id = extract_patchset_id(input);
    if (!patchset_id) {
      return std::nullopt;
    }

    auto edits = parse_edit_blocks(input);
    if (!edits) {
      return std::nullopt;
    }

    return std::make_pair(*patchset_id, std::move(*edits));
  }
} // namespace mistral::agent