#include "tools/code_tools.h"

#include <dripcore/sleep_awaiter.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace fs = std::filesystem;

namespace mistral::tools
{
  namespace
  {
    // Text of a 1-indexed line of source, without the newline.
    std::string_view line_text(const std::string& source, int line)
    {
      std::size_t start = 0;
      int current = 1;
      while (start < source.size() && current < line) {
        const std::size_t newline = source.find('\n', start);
        if (newline == std::string::npos) {
          return {};
        }
        start = newline + 1;
        ++current;
      }
      std::size_t end = source.find('\n', start);
      if (end == std::string::npos) {
        end = source.size();
      }
      return std::string_view(source).substr(start, end - start);
    }

    // Path relative to the current working directory for tool output.
    std::string display_path(const std::string& file)
    {
      std::error_code ec;
      const fs::path relative = fs::relative(file, fs::current_path(ec));
      if (ec || relative.empty()) {
        return file;
      }
      return relative.string();
    }
  }

  code_tools::code_tools(code_index_settings settings)
    : settings_(std::move(settings)), index_(settings_)
  {
  }

  dripcore::awaitable<json> code_tools::outline(std::string path)
  {
    parsed_source parsed;
    std::string error;
    if (!parsed.load(path, error)) {
      co_return {{"error", error}};
    }

    json entries = json::array();
    for (const auto& definition : parsed.definitions()) {
      json entry = {
        {"kind", definition.kind},
        {"name", definition.name},
        {"line_start", definition.line_start},
        {"line_end", definition.line_end}
      };
      if (!definition.scope.empty()) {
        entry["scope"] = definition.scope;
      }
      entries.push_back(std::move(entry));
    }

    co_return {
      {"file", path},
      {"symbols", std::move(entries)},
      {"count", static_cast<int>(parsed.definitions().size())},
      {"has_errors", parsed.has_errors()}
    };
  }

  dripcore::awaitable<json> code_tools::read_symbol(std::string path, std::string name)
  {
    parsed_source parsed;
    std::string error;
    if (!parsed.load(path, error)) {
      co_return {{"error", error}};
    }

    const auto& definitions = parsed.definitions();

    json entries = json::array();
    for (std::size_t i = 0; i < definitions.size(); ++i) {
      if (definitions[i].name != name) {
        continue;
      }

      std::uint32_t start = 0;
      std::uint32_t end = 0;
      if (!parsed.definition_extent(i, start, end)) {
        continue;
      }

      entries.push_back({
        {"kind", definitions[i].kind},
        {"line_start", definitions[i].line_start},
        {"line_end", definitions[i].line_end},
        {"content", parsed.source().substr(start, end - start)}
      });
    }

    if (entries.empty()) {
      co_return {
        {"error", std::format("symbol '{}' not found in '{}'", name, path)}
      };
    }

    // Compute before moving entries into the result.
    const int count = static_cast<int>(entries.size());

    co_return {
      {"file", path},
      {"name", name},
      {"definitions", std::move(entries)},
      {"count", count}
    };
  }

  dripcore::awaitable<json> code_tools::find_symbol(std::string name, std::string path)
  {
    if (name.empty()) {
      co_return {{"error", "name must not be empty"}};
    }

    std::error_code ec;
    const fs::path root(path);
    if (!fs::exists(root, ec)) {
      co_return {{"error", std::format("path '{}' does not exist", path)}};
    }
    if (!fs::is_directory(root, ec)) {
      co_return {{"error", std::format(
        "path '{}' is not a directory; use outline or read_symbol for single files", path)}};
    }

    co_await index_.refresh(path);

    auto hits = index_.find(path, name);
    std::string match_type = "exact";
    if (hits.empty()) {
      hits = index_.find_substring(path, name);
      if (!hits.empty()) {
        match_type = "substring";
      }
    }

    std::sort(hits.begin(), hits.end(),
      [](const symbol_index::hit& a, const symbol_index::hit& b) {
        if (a.file != b.file) {
          return a.file < b.file;
        }
        return a.symbol.line_start < b.symbol.line_start;
      });

    bool truncated = false;
    if (static_cast<int>(hits.size()) > settings_.max_results) {
      hits.resize(static_cast<std::size_t>(settings_.max_results));
      truncated = true;
    }

    json matches = json::array();
    for (const auto& hit : hits) {
      matches.push_back({
        {"kind", hit.symbol.kind},
        {"name", hit.symbol.name},
        {"file", display_path(hit.file)},
        {"line_start", hit.symbol.line_start},
        {"line_end", hit.symbol.line_end}
      });
    }

    co_return {
      {"name", name},
      {"match", match_type},
      {"matches", std::move(matches)},
      {"count", static_cast<int>(hits.size())},
      {"truncated", truncated}
    };
  }

  dripcore::awaitable<json> code_tools::find_references(std::string name, std::string path)
  {
    if (name.empty()) {
      co_return {{"error", "name must not be empty"}};
    }

    std::error_code ec;
    const fs::path root(path);
    if (!fs::exists(root, ec)) {
      co_return {{"error", std::format("path '{}' does not exist", path)}};
    }
    if (!fs::is_directory(root, ec)) {
      co_return {{"error", std::format("path '{}' is not a directory", path)}};
    }

    const fs::path root_abs = fs::absolute(root, ec).lexically_normal();

    json matches = json::array();
    int count = 0;
    bool truncated = false;
    int processed = 0;

    try {
      for (const auto& entry : fs::recursive_directory_iterator(
             root_abs, fs::directory_options::skip_permission_denied))
      {
        std::error_code file_ec;
        if (!entry.is_regular_file(file_ec) || file_ec) {
          continue;
        }
        const fs::path file = entry.path().lexically_normal();
        if (index_.is_excluded(file) || !symbol_index::is_supported_source(file)) {
          continue;
        }

        std::ifstream in(file);
        if (!in) {
          continue;
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        std::string source = buffer.str();

        // Cheap textual prefilter before parsing.
        if (source.find(name) == std::string::npos) {
          continue;
        }

        parsed_source parsed;
        std::string error;
        if (!parsed.load(file.string(), std::move(source), error)) {
          continue;
        }

        for (const auto& site : parsed.usages_of(name)) {
          if (site.is_definition) {
            continue;
          }
          if (count >= settings_.max_results) {
            truncated = true;
            break;
          }
          matches.push_back({
            {"file", display_path(file.string())},
            {"line", site.line},
            {"text", std::string(line_text(parsed.source(), site.line))}
          });
          ++count;
        }

        if (truncated) {
          break;
        }

        if (++processed % 20 == 0) {
          co_await dripcore::sleep_awaiter{
            dripcore::executor::current(), nullptr, std::chrono::milliseconds(0)};
        }
      }
    } catch (const std::exception&) {
      // Unreadable directory entries are skipped.
    }

    co_return {
      {"name", name},
      {"matches", std::move(matches)},
      {"count", count},
      {"truncated", truncated}
    };
  }

} // namespace mistral::tools