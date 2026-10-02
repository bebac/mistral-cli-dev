#include "tools/code_index.h"
#include "common/embedded_resources.h"

#include <dripcore/sleep_awaiter.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <set>
#include <sstream>

// Declared in the grammar's bindings (tree-sitter-cpp.h). Declared inline
// to avoid adding a grammar-specific include path to the build.
extern "C" {
const TSLanguage* tree_sitter_cpp(void);
}

namespace fs = std::filesystem;

namespace mistral::tools
{
  namespace
  {
    using parser_ptr = std::unique_ptr<TSParser, decltype(&ts_parser_delete)>;
    using query_ptr = std::unique_ptr<TSQuery, decltype(&ts_query_delete)>;
    using cursor_ptr = std::unique_ptr<TSQueryCursor, decltype(&ts_query_cursor_delete)>;

    std::string_view node_text(TSNode node, const std::string& source)
    {
      const std::uint32_t start = ts_node_start_byte(node);
      return std::string_view(source).substr(start, ts_node_end_byte(node) - start);
    }

    // Component-wise path prefix test (fs::path::starts_with is not
    // available in all C++ standard library implementations).
    bool is_under(const fs::path& path, const fs::path& root)
    {
      if (root.empty() || path.empty()) {
        return false;
      }
      auto root_it = root.begin();
      auto path_it = path.begin();
      for (; root_it != root.end(); ++root_it, ++path_it) {
        if (path_it == path.end() || *path_it != *root_it) {
          return false;
        }
      }
      return true;
    }

    query_ptr compile_query(std::string_view source)
    {
      std::uint32_t error_offset = 0;
      TSQueryError error_type = TSQueryErrorNone;
      TSQuery* query = ts_query_new(
        tree_sitter_cpp(), source.data(), static_cast<std::uint32_t>(source.size()),
        &error_offset, &error_type);
      return query_ptr(query, ts_query_delete);
    }

    // Process-lifetime compiled queries; a TSQuery is tied to the language,
    // not to a parse, so it is safe to share across parses.
    TSQuery* tags_query()
    {
      static query_ptr query = compile_query(mistral::embedded::cpp_tags_query());
      return query.get();
    }

    TSQuery* references_query()
    {
      static query_ptr query = compile_query(mistral::embedded::cpp_references_query());
      return query.get();
    }

    // The tags query captures e.g. the function_declarator of a function,
    // not the enclosing declaration. Walk up to the outermost wrapping
    // declaration so extracted text includes the return type and body.
    TSNode enclosing_declaration(TSNode node)
    {
      static const std::vector<std::string_view> wrapping_types = {
        "function_definition", "declaration", "field_declaration", "template_declaration"
      };

      TSNode current = node;
      while (true) {
        const TSNode parent = ts_node_parent(current);
        if (ts_node_is_null(parent)) {
          break;
        }
        const std::string_view type(ts_node_type(parent));
        bool wraps = false;
        for (const auto& wrapping : wrapping_types) {
          if (type == wrapping) {
            wraps = true;
            break;
          }
        }
        if (!wraps) {
          break;
        }
        current = parent;
      }
      return current;
    }
  }

  // ---- parsed_source ----

  bool parsed_source::load(const std::string& path, std::string& error)
  {
    const fs::path file(path);
    if (!fs::exists(file)) {
      error = std::format("path '{}' does not exist", path);
      return false;
    }
    if (!symbol_index::is_supported_source(file)) {
      error = std::format(
        "unsupported file type '{}': only C/C++ sources are supported", path);
      return false;
    }

    std::ifstream in(file);
    if (!in) {
      error = std::format("cannot open '{}'", path);
      return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return load(path, buffer.str(), error);
  }

  bool parsed_source::load(
    const std::string& path, std::string source_text, std::string& error)
  {
    source_ = std::move(source_text);

    parser_ptr parser(ts_parser_new(), ts_parser_delete);
    if (!ts_parser_set_language(parser.get(), tree_sitter_cpp())) {
      error = "failed to configure parser";
      return false;
    }

    tree_ = tree_ptr(
      ts_parser_parse_string(
        parser.get(), nullptr, source_.data(),
        static_cast<std::uint32_t>(source_.size())),
      ts_tree_delete);
    if (!tree_) {
      error = std::format("failed to parse '{}'", path);
      return false;
    }

    const TSQuery* query = tags_query();
    if (query == nullptr) {
      error = "failed to compile embedded tags query";
      return false;
    }

    definitions_.clear();
    definition_nodes_.clear();
    definition_name_nodes_.clear();

    const TSNode root = ts_tree_root_node(tree_.get());

    cursor_ptr cursor(ts_query_cursor_new(), ts_query_cursor_delete);
    ts_query_cursor_exec(cursor.get(), query, root);

    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor.get(), &match)) {
      std::string kind;
      std::string name;
      std::string scope;
      TSNode definition_node{};
      TSNode name_node{};

      for (std::uint32_t i = 0; i < match.capture_count; ++i) {
        const std::uint32_t capture_index = match.captures[i].index;
        std::uint32_t length = 0;
        const char* capture = ts_query_capture_name_for_id(query, capture_index, &length);
        const std::string_view capture_name(capture, length);
        const TSNode node = match.captures[i].node;

        if (capture_name == "name") {
          name = std::string(node_text(node, source_));
          name_node = node;
        } else if (capture_name.rfind("definition.", 0) == 0) {
          kind = capture_name.substr(std::strlen("definition."));
          definition_node = node;
        } else if (capture_name == "local.scope") {
          scope = std::string(node_text(node, source_));
        }
      }

      if (name.empty() || kind.empty() || ts_node_is_null(name_node)) {
        continue;
      }

      definitions_.push_back({
        std::move(kind), std::move(name), std::move(scope),
        static_cast<int>(ts_node_start_point(definition_node).row) + 1,
        static_cast<int>(ts_node_end_point(definition_node).row) + 1
      });
      definition_nodes_.push_back(definition_node);
      definition_name_nodes_.push_back(name_node);
    }

    return true;
  }

  bool parsed_source::has_errors() const
  {
    if (!tree_) {
      return false;
    }
    return ts_node_has_error(ts_tree_root_node(tree_.get()));
  }

  bool parsed_source::definition_extent(
    std::size_t index, std::uint32_t& start, std::uint32_t& end) const
  {
    if (index >= definition_nodes_.size()) {
      return false;
    }
    const TSNode node = enclosing_declaration(definition_nodes_[index]);
    if (ts_node_is_null(node)) {
      return false;
    }
    start = ts_node_start_byte(node);
    end = ts_node_end_byte(node);
    return true;
  }

  std::vector<parsed_source::usage_site> parsed_source::usages_of(
    const std::string& name) const
  {
    std::vector<usage_site> sites;
    if (!tree_ || name.empty()) {
      return sites;
    }

    const TSQuery* query = references_query();
    if (query == nullptr) {
      return sites;
    }

    const TSNode root = ts_tree_root_node(tree_.get());

    cursor_ptr cursor(ts_query_cursor_new(), ts_query_cursor_delete);
    ts_query_cursor_exec(cursor.get(), query, root);

    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor.get(), &match)) {
      for (std::uint32_t i = 0; i < match.capture_count; ++i) {
        const TSNode node = match.captures[i].node;
        if (node_text(node, source_) != name) {
          continue;
        }

        usage_site site;
        site.line = static_cast<int>(ts_node_start_point(node).row) + 1;
        site.start = ts_node_start_byte(node);

        // Flag definition sites so callers can exclude them.
        const std::uint32_t end = ts_node_end_byte(node);
        for (const auto& definition_name : definition_name_nodes_) {
          if (ts_node_start_byte(definition_name) == site.start &&
              ts_node_end_byte(definition_name) == end) {
            site.is_definition = true;
            break;
          }
        }

        sites.push_back(site);
      }
    }

    return sites;
  }

  // ---- symbol_index ----

  symbol_index::symbol_index(code_index_settings settings)
    : settings_(std::move(settings))
  {
  }

  bool symbol_index::is_supported_source(const fs::path& path)
  {
    static const std::vector<std::string> extensions = {
      ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".tpp"
    };
    const std::string extension = path.extension().string();
    for (const auto& supported : extensions) {
      if (extension == supported) {
        return true;
      }
    }
    return false;
  }

  bool symbol_index::is_excluded(const fs::path& path) const
  {
    for (const auto& part : path) {
      for (const auto& dir : settings_.exclude_dirs) {
        if (part == dir) {
          return true;
        }
      }
    }
    return false;
  }

  dripcore::awaitable<void> symbol_index::refresh(const std::string& root)
  {
    std::error_code ec;
    const fs::path root_abs = fs::absolute(fs::path(root), ec).lexically_normal();
    if (ec || !fs::is_directory(root_abs, ec)) {
      co_return;
    }

    std::set<std::string> seen;
    int reparsed = 0;

    try {
      for (const auto& entry : fs::recursive_directory_iterator(
             root_abs, fs::directory_options::skip_permission_denied))
      {
        std::error_code file_ec;
        if (!entry.is_regular_file(file_ec) || file_ec) {
          continue;
        }
        const fs::path path = entry.path().lexically_normal();
        if (is_excluded(path) || !is_supported_source(path)) {
          continue;
        }

        const auto mtime = fs::last_write_time(path, file_ec);
        if (file_ec) {
          continue;
        }
        const std::uint64_t size = fs::file_size(path, file_ec);
        if (file_ec) {
          continue;
        }
        const std::uint64_t mtime_stamp =
          static_cast<std::uint64_t>(mtime.time_since_epoch().count());

        const std::string key = path.string();
        const auto existing = files_.find(key);
        if (existing != files_.end() &&
            existing->second.mtime == mtime_stamp &&
            existing->second.size == size) {
          seen.insert(key);
          continue;
        }

        parsed_source parsed;
        std::string error;
        if (parsed.load(key, error)) {
          files_[key] = file_entry{mtime_stamp, size, parsed.definitions()};
          seen.insert(key);
        }

        if (++reparsed % 20 == 0) {
          // Yield to the scheduler; a cold index build can take a while
          // on large repositories.
          co_await dripcore::sleep_awaiter{
            dripcore::executor::current(), nullptr, std::chrono::milliseconds(0)};
        }
      }
    } catch (const std::exception&) {
      // Unreadable directory entries are skipped.
    }

    // Drop indexed files under this root that no longer exist.
    for (auto it = files_.begin(); it != files_.end();) {
      if (is_under(it->first, root_abs) && seen.count(it->first) == 0) {
        it = files_.erase(it);
      } else {
        ++it;
      }
    }
  }

  std::vector<symbol_index::hit> symbol_index::find(
    const std::string& root, const std::string& name) const
  {
    std::vector<hit> hits;
    std::error_code ec;
    const fs::path root_abs = fs::absolute(fs::path(root), ec).lexically_normal();

    for (const auto& [file, entry] : files_) {
      if (!is_under(file, root_abs)) {
        continue;
      }
      for (const auto& definition : entry.definitions) {
        if (definition.name == name) {
          hits.push_back({file, definition});
        }
      }
    }
    return hits;
  }

  std::vector<symbol_index::hit> symbol_index::find_substring(
    const std::string& root, const std::string& name) const
  {
    std::vector<hit> hits;
    std::error_code ec;
    const fs::path root_abs = fs::absolute(fs::path(root), ec).lexically_normal();

    for (const auto& [file, entry] : files_) {
      if (!is_under(file, root_abs)) {
        continue;
      }
      for (const auto& definition : entry.definitions) {
        if (definition.name.find(name) != std::string::npos) {
          hits.push_back({file, definition});
        }
      }
    }
    return hits;
  }

} // namespace mistral::tools