#pragma once

#include <string>
#include <string_view>
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

  // True when a single path component matches an exclusion entry. An
  // entry ending in '*' is a prefix match, so "build*" covers build,
  // build_asan and build_debug; anything else is an exact match.
  //
  // Shared by the knowledge store and the symbol index: both walk the
  // tree and skip directories by name, and they must agree, or a
  // directory is excluded from one index and not the other.
  bool matches_name_pattern(std::string_view name, std::string_view pattern);

  // Generate a unique session ID (timestamp + random)
  std::string generate_session_id();

  // Check if a file appears to be text (not binary)
  // Uses heuristic: contains no null bytes and mostly printable characters
  bool is_text_file(const std::string& filepath);

  // FNV-1a 64-bit hash of data as a 16 character lowercase hex string.
  // Deterministic across platforms and builds (unlike std::hash).
  std::string fnv1a_64_hex(std::string_view data);
} // namespace mistral::utils
