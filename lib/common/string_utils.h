#pragma once

#include <string>
#include <vector>

namespace mistral::utils
{
  // Trim whitespace from both ends
  std::string trim(const std::string& s);

  // Split string by delimiter
  std::vector<std::string> split(const std::string& s, char delimiter);

  // Split string by comma
  std::vector<std::string> split_comma(const std::string& s);

  // Convert to lowercase
  std::string to_lower(const std::string& s);

  // Escape special regex characters
  std::string escape_regex(const std::string& s);

  // Convert glob pattern to regex
  std::string glob_to_regex(const std::string& pattern);

  // Generate a unique session ID (timestamp + random)
  std::string generate_session_id();

  // Check if a file appears to be text (not binary)
  // Uses heuristic: contains no null bytes and mostly printable characters
  bool is_text_file(const std::string& filepath);
} // namespace mistral::utils
