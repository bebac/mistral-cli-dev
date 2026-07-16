#include "tools/grep_tools.h"
#include "common/string_utils.h"

#include <filesystem>
#include <fstream>
#include <format>
#include <regex>
#include <sstream>
#include <vector>
#include <iostream>

namespace fs = std::filesystem;

namespace mistral::tools
{
  // Hardcoded list of directories to exclude from grep
  const std::vector<std::string> EXCLUDED_DIRS = {
    ".git", ".gitmodules", ".mistral-cli", ".cache", ".DS_store", ".vscode", ".vibe", "build"
  };

  bool is_excluded(const fs::path& path, const fs::path& base) {
  fs::path relative = fs::relative(path, base);
  for (const auto& part : relative) {
    for (const auto& dir : EXCLUDED_DIRS) {
      if (part == dir) {
        return true;
      }
    }
  }
  return false;
}

  json grep_tools::grep(std::string pattern, std::string path, bool recursive)
  {
    std::regex regex;
    try {
      regex = std::regex(pattern);
    } catch (const std::regex_error& e) {
      return {{"error", std::format("invalid regex pattern '{}': {}", pattern, e.what())}};
    }

    fs::path base(path);
    if (!fs::exists(base)) {
      return {{"error", std::format("path '{}' does not exist", path)}};
    }

    json results = json::array();

    auto process_file = [&](const fs::path& file_path) {
      // Skip binary files
      if (!mistral::utils::is_text_file(file_path.string())) {
        return;
      }

      std::ifstream file(file_path);
      if (!file) return;

      std::string line;
      int line_num = 0;

      while (std::getline(file, line)) {
        ++line_num;
        if (std::regex_search(line, regex)) {
          results.push_back({
            {"file", fs::relative(file_path, base).string()},
            {"line", line_num},
            {"content", line}
          });
        }
      }
    };

    try
    {
      if (fs::is_regular_file(base)) {
        process_file(base);
      }
      else if (fs::is_directory(base)) {
        if (recursive)
        {
          for (const auto &entry : fs::recursive_directory_iterator(
                   base, fs::directory_options::skip_permission_denied))
          {
            if (entry.is_regular_file() && !is_excluded(entry.path(), base)) {
              process_file(entry.path());
            }
          }
        }
        else
        {
          for (const auto &entry : fs::directory_iterator(
                   base, fs::directory_options::skip_permission_denied))
          {
            if (entry.is_regular_file() && !is_excluded(entry.path(), base)) {
              process_file(entry.path());
            }
          }
        }
      }
    }
    catch (const std::exception &e)
    {
      return {{"error", std::format("filesystem error: {}", e.what())}};
    }

    return {{"matches", results}, {"count", static_cast<int>(results.size())}};
  }

} // namespace mistral::tools
