#include "tools/note_tools.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace mistral::tools
{
  // ---- Frontmatter parsing ----

  // Trim leading/trailing whitespace.
  static std::string trim(const std::string& s)
  {
    auto start = s.find_first_not_of(" \t");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t");
    return s.substr(start, end - start + 1);
  }

  // Parse a YAML-like tag list: [tag1, tag2, tag3] or tag1, tag2
  static std::vector<std::string> parse_tag_list(const std::string& value)
  {
    std::vector<std::string> tags;
    std::string v = trim(value);

    // Strip surrounding brackets if present.
    if (v.size() >= 2 && v.front() == '[' && v.back() == ']') {
      v = v.substr(1, v.size() - 2);
    }

    std::istringstream ss(v);
    std::string item;
    while (std::getline(ss, item, ',')) {
      auto t = trim(item);
      if (!t.empty()) {
        tags.push_back(t);
      }
    }
    return tags;
  }

  parsed_note parse_frontmatter(const std::string& content)
  {
    parsed_note result;

    // Frontmatter must start with "---\n" at the very beginning.
    if (!content.starts_with("---\n") && !content.starts_with("---\r\n")) {
      result.body = content;
      return result;
    }

    // Find the closing "---".
    size_t start = content.find('\n') + 1;  // Skip past first "---\n".
    size_t end = content.find("\n---", start);

    if (end == std::string::npos) {
      // No closing --- found; treat entire content as body.
      result.body = content;
      return result;
    }

    // Parse the frontmatter block line by line.
    std::string fm_block = content.substr(start, end - start);
    std::istringstream ss(fm_block);
    std::string line;

    while (std::getline(ss, line))
    {
      // Strip \r if present.
      if (!line.empty() && line.back() == '\r') line.pop_back();

      auto colon = line.find(':');
      if (colon == std::string::npos) continue;

      std::string key = trim(line.substr(0, colon));
      std::string value = trim(line.substr(colon + 1));

      if (key == "title") {
        result.front.title = value;
      } else if (key == "tags") {
        result.front.tags = parse_tag_list(value);
      }
    }

    // Body is everything after the closing "---\n".
    size_t body_start = end + 4;  // Skip "\n---"
    if (body_start < content.size() && content[body_start] == '\n') {
      ++body_start;
    } else if (body_start + 1 < content.size() && content[body_start] == '\r') {
      body_start += 2;  // Skip \r\n
    }

    result.body = (body_start < content.size()) ? content.substr(body_start) : "";
    return result;
  }

  // ---- note_tools ----

  note_tools::note_tools(std::string notes_dir)
    : notes_dir_(std::move(notes_dir))
  {
    fs::create_directories(notes_dir_);
    engine_.set_include_base(notes_dir_);
    engine_.enable_command_expansion(false);  // No shell exec when reading notes.
  }

  fs::path note_tools::resolve_path(const std::string& path) const
  {
    fs::path p = fs::path(notes_dir_) / path;
    // Append .md if no extension.
    if (!p.has_extension()) {
      p += ".md";
    }
    return p;
  }

  std::string note_tools::list_notes(std::string folder)
  {
    fs::path base = folder.empty() ? fs::path(notes_dir_) : fs::path(notes_dir_) / folder;

    if (!fs::exists(base)) {
      return std::format("error: folder '{}' does not exist", folder);
    }

    std::ostringstream out;

    for (const auto& entry : fs::recursive_directory_iterator(base))
    {
      if (!entry.is_regular_file()) continue;

      auto ext = entry.path().extension().string();
      if (ext != ".md") continue;

      auto rel = fs::relative(entry.path(), notes_dir_);

      // Read frontmatter for title.
      std::ifstream f(entry.path());
      std::ostringstream ss;
      ss << f.rdbuf();
      auto parsed = parse_frontmatter(ss.str());

      out << rel.string();
      if (!parsed.front.title.empty()) {
        out << "  — " << parsed.front.title;
      }
      if (!parsed.front.tags.empty()) {
        out << "  [";
        for (size_t i = 0; i < parsed.front.tags.size(); ++i) {
          if (i > 0) out << ", ";
          out << parsed.front.tags[i];
        }
        out << "]";
      }
      out << "\n";
    }

    auto result = out.str();
    return result.empty() ? "no notes found" : result;
  }

  json note_tools::read_note(std::string path)
  {
    auto full = resolve_path(path);

    if (!fs::exists(full)) {
      return {{"error", std::format("note not found: {}", path)}};
    }

    std::ifstream f(full);
    if (!f) {
      return {{"error", std::format("cannot open note: {}", path)}};
    }

    std::ostringstream ss;
    ss << f.rdbuf();
    std::string raw = ss.str();

    auto parsed = parse_frontmatter(raw);

    // Resolve includes in the body via the template engine.
    std::string rendered;
    try {
      rendered = engine_.process(parsed.body);
    } catch (const std::exception& e) {
      rendered = parsed.body;  // Fallback to raw on error.
    }

    json result;
    result["path"] = fs::relative(full, notes_dir_).string();
    if (!parsed.front.title.empty()) result["title"] = parsed.front.title;
    if (!parsed.front.tags.empty()) result["tags"] = parsed.front.tags;
    result["content"] = rendered;
    return result;
  }

  json note_tools::write_note(std::string path, std::string content)
  {
    auto full = resolve_path(path);

    try {
      fs::create_directories(full.parent_path());

      std::ofstream f(full, std::ios::binary);
      if (!f) {
        return {{"error", std::format("cannot write note: {}", path)}};
      }
      f << content;
      f.close();

      return {
        {"status", "ok"},
        {"path", fs::relative(full, notes_dir_).string()}
      };
    } catch (const std::exception& e) {
      return {{"error", e.what()}};
    }
  }

  json note_tools::search_notes(std::string query, std::string tag)
  {
    if (query.empty() && tag.empty()) {
      return {{"error", "at least one of query or tag must be provided"}};
    }

    // Lowercase the query and tag for case-insensitive matching.
    std::string query_lower = query;
    std::string tag_lower = tag;
    std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);
    std::transform(tag_lower.begin(), tag_lower.end(), tag_lower.begin(), ::tolower);

    json results = json::array();

    for (const auto& entry : fs::recursive_directory_iterator(notes_dir_))
    {
      if (!entry.is_regular_file()) continue;
      if (entry.path().extension() != ".md") continue;

      std::ifstream f(entry.path());
      if (!f) continue;

      std::ostringstream ss;
      ss << f.rdbuf();
      std::string content = ss.str();

      auto parsed = parse_frontmatter(content);
      auto rel = fs::relative(entry.path(), notes_dir_).string();

      // Tag filter.
      if (!tag_lower.empty()) {
        bool has_tag = false;
        for (const auto& t : parsed.front.tags) {
          std::string t_lower = t;
          std::transform(t_lower.begin(), t_lower.end(), t_lower.begin(), ::tolower);
          if (t_lower == tag_lower) { has_tag = true; break; }
        }
        if (!has_tag) continue;
      }

      // Text search.
      json match;
      match["path"] = rel;
      if (!parsed.front.title.empty()) match["title"] = parsed.front.title;
      if (!parsed.front.tags.empty()) match["tags"] = parsed.front.tags;

      if (!query_lower.empty()) {
        // Search line by line, collect matching lines with context.
        json matching_lines = json::array();
        std::istringstream lines(content);
        std::string line;
        int line_num = 0;

        while (std::getline(lines, line))
        {
          ++line_num;
          std::string line_lower = line;
          std::transform(line_lower.begin(), line_lower.end(), line_lower.begin(), ::tolower);

          if (line_lower.find(query_lower) != std::string::npos) {
            matching_lines.push_back({
              {"line", line_num},
              {"text", trim(line)}
            });
          }
        }

        if (matching_lines.empty()) continue;  // Query didn't match.
        match["matches"] = std::move(matching_lines);
      }

      results.push_back(std::move(match));
    }

    return results;
  }

} // namespace mistral::tools
