#include "common/string_utils.h"

#include <algorithm>
#include <sstream>
#include <cctype>
#include <regex>
#include <ranges>
#include <random>
#include <fstream>

namespace mistral::utils
{
  // Trim whitespace from both ends
  std::string trim(const std::string& s)
  {
    auto is_not_space = [](unsigned char c) { return !std::isspace(c); };
    auto start = std::ranges::find_if(s, is_not_space);
    auto end = std::ranges::find_if(s | std::views::reverse, is_not_space).base();
    return (start < end) ? std::string(start, end) : std::string();
  }

  // Split string by delimiter
  std::vector<std::string> split(const std::string& s, char delimiter)
  {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream token_stream(s);
    while (std::getline(token_stream, token, delimiter)) {
      token = trim(token);
      if (!token.empty()) tokens.push_back(token);
    }
    return tokens;
  }

  // Split string by comma
  std::vector<std::string> split_comma(const std::string& s)
  {
    return split(s, ',');
  }

  // Convert to lowercase
  std::string to_lower(const std::string& s)
  {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
  }


  // Escape special regex characters
  std::string escape_regex(const std::string& s)
  {
    static const std::string special_chars = "\\.*+?^$()[]{}|";
    std::string result;
    for (char c : s) {
      if (special_chars.find(c) != std::string::npos) {
        result += '\\';
      }
      result += c;
    }
    return result;
  }

  // Convert glob pattern to regex
  std::string glob_to_regex(const std::string& pattern)
  {
    std::string regex;
    for (size_t i = 0; i < pattern.size(); ++i) {
      char c = pattern[i];
      switch (c) {
        case '*':
          if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
            // ** matches any path including slashes
            regex += ".*";
            ++i;
          } else {
            // * matches anything except slash
            regex += "[^/]*";
          }
          break;
        case '?':
          regex += '.';
          break;
        case '.':
        case '\\':
        case '^':
        case '$':
        case '+':
        case '(':
        case ')':
        case '[':
        case ']':
        case '{':
        case '}':
        case '|':
          regex += '\\';
          regex += c;
          break;
        default:
          regex += escape_regex(std::string(1, c));
          break;
      }
    }
    return "^" + regex + "$";
  }

  // Generate a unique session ID using timestamp and random number
  std::string generate_session_id()
  {
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch()).count();

    // Simple random number (not crypto-secure, but sufficient for session IDs)
    static std::random_device rd;
    static std::mt19937_64 gen(rd());

    return std::format("{:016x}{:016x}", timestamp, gen());
  }

  // Check if a file appears to be text (not binary)
  bool is_text_file(const std::string& filepath)
  {
    std::ifstream file(filepath, std::ios::binary);
    if (!file) return false;

    // Read first 4KB to check - enough to detect binary signatures
    constexpr size_t buffer_size = 4096;
    char buffer[buffer_size];
    file.read(buffer, buffer_size);
    size_t bytes_read = file.gcount();

    // Check for null bytes (strong indicator of binary)
    for (size_t i = 0; i < bytes_read; ++i) {
      if (buffer[i] == '\0') {
        return false;
      }
    }

    // Check ratio of printable/whitespace characters
    int printable = 0;
    for (size_t i = 0; i < bytes_read; ++i) {
      unsigned char c = static_cast<unsigned char>(buffer[i]);
      if (c >= 32 || c == '\n' || c == '\r' || c == '\t') {
        printable++;
      }
    }

    // If more than 90% are printable/whitespace, treat as text
    return (printable * 10) >= (bytes_read * 9);
  }

} // namespace mistral::utils
