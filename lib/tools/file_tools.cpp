#include "tools/file_tools.h"

#include <filesystem>
#include <fstream>
#include <format>
#include <sstream>

namespace fs = std::filesystem;

namespace mistral::tools
{
  json file_tools::read_file(std::string path, int line_from, int line_to)
  {
    std::ifstream file(path);
    if (!file) {
      return {{"error", std::format("cannot open file: {}", path)}};
    }

    std::string line;
    std::ostringstream content;
    int line_num = 0;
    int lines_read = 0;

    while (std::getline(file, line))
    {
      ++line_num;

      if (line_from > 0 && line_num < line_from) continue;
      if (line_to > 0 && line_num > line_to) break;

      content << line_num << "|" << line << "\n";
      ++lines_read;
    }

    return {
      {"path", path},
      {"lines_read", lines_read},
      {"content", content.str()}
    };
  }

  std::string file_tools::list_files(std::string path, bool recursive, int max_depth)
  {
    std::ostringstream out;

    try {
      fs::path base(path);

      if (!fs::exists(base)) {
        return std::format("error: path '{}' does not exist", path);
      }

      if (!fs::is_directory(base)) {
        return std::format("error: '{}' is not a directory", path);
      }

      auto append_entry = [&](const fs::directory_entry& entry, const fs::path& base_path) {
        auto rel = fs::relative(entry.path(), base_path);
        char type = 'f';
        if (entry.is_directory()) type = 'd';
        else if (entry.is_symlink()) type = 'l';

        out << type << " ";
        if (entry.is_regular_file()) {
          out << std::format("{:>8} ", entry.file_size());
        } else {
          out << "       - ";
        }
        out << rel.string() << "\n";
      };

      if (recursive) {
        for (const auto& entry : fs::recursive_directory_iterator(base)) {
          if (max_depth > 0) {
            auto rel = fs::relative(entry.path(), base);
            int depth = std::distance(rel.begin(), rel.end());
            if (depth > max_depth) continue;
          }
          append_entry(entry, base);
        }
      } else {
        for (const auto& entry : fs::directory_iterator(base)) {
          append_entry(entry, base);
        }
      }
    } catch (const std::exception& e) {
      return std::format("error: {}", e.what());
    }

    return out.str();
  }

  json file_tools::write_file(std::string path, std::string content)
  {
    try {
      // Create parent directories if needed.
      fs::path p(path);
      if (p.has_parent_path()) {
        fs::create_directories(p.parent_path());
      }

      std::ofstream file(path, std::ios::binary);
      if (!file) {
        return {{"error", std::format("cannot open file for writing: {}", path)}};
      }
      file << content;
      file.close();

      return {
        {"status", "ok"},
        {"path", path},
        {"bytes_written", static_cast<int>(content.size())}
      };
    } catch (const std::exception& e) {
      return {{"error", e.what()}};
    }
  }

} // namespace mistral::tools
