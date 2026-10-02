#include "tools/code_tools.h"
#include "common/embedded_resources.h"

#include "tree_sitter/api.h"

#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

// Declared in the grammar's bindings (tree-sitter-cpp.h). Declared inline to
// avoid adding a grammar-specific include path to the build.
extern "C" {
const TSLanguage* tree_sitter_cpp(void);
}

namespace fs = std::filesystem;

namespace mistral::tools
{
  namespace
  {
    using parser_ptr = std::unique_ptr<TSParser, decltype(&ts_parser_delete)>;
    using tree_ptr = std::unique_ptr<TSTree, decltype(&ts_tree_delete)>;
    using query_ptr = std::unique_ptr<TSQuery, decltype(&ts_query_delete)>;
    using cursor_ptr = std::unique_ptr<TSQueryCursor, decltype(&ts_query_cursor_delete)>;

    bool is_supported_source(const fs::path& path)
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

    query_ptr compile_tags_query()
    {
      const std::string_view source = mistral::embedded::cpp_tags_query();
      uint32_t error_offset = 0;
      TSQueryError error_type = TSQueryErrorNone;
      TSQuery* query = ts_query_new(
        tree_sitter_cpp(), source.data(), static_cast<uint32_t>(source.size()),
        &error_offset, &error_type
      );
      return query_ptr(query, ts_query_delete);
    }

    struct symbol_info
    {
      std::string kind;
      std::string name;
      std::string scope;
      TSNode definition{};
    };

    // Run the embedded tags query and collect one entry per symbol definition.
    std::vector<symbol_info> collect_symbols(
      const TSQuery* query, TSNode root, const std::string& source)
    {
      std::vector<symbol_info> symbols;

      cursor_ptr cursor(ts_query_cursor_new(), ts_query_cursor_delete);
      ts_query_cursor_exec(cursor.get(), query, root);

      TSQueryMatch match;
      while (ts_query_cursor_next_match(cursor.get(), &match)) {
        symbol_info symbol;
        bool have_name = false;

        for (uint32_t i = 0; i < match.capture_count; ++i) {
          const uint32_t capture_index = match.captures[i].index;
          uint32_t length = 0;
          const char* capture = ts_query_capture_name_for_id(query, capture_index, &length);
          const std::string_view capture_name(capture, length);
          const TSNode node = match.captures[i].node;

          if (capture_name == "name") {
            const uint32_t start = ts_node_start_byte(node);
            symbol.name = source.substr(start, ts_node_end_byte(node) - start);
            have_name = true;
          } else if (capture_name.rfind("definition.", 0) == 0) {
            symbol.kind = capture_name.substr(std::strlen("definition."));
            symbol.definition = node;
          } else if (capture_name == "local.scope") {
            const uint32_t start = ts_node_start_byte(node);
            symbol.scope = source.substr(start, ts_node_end_byte(node) - start);
          }
        }

        if (have_name && !symbol.kind.empty()) {
          symbols.push_back(symbol);
        }
      }

      return symbols;
    }

    // The tags query captures e.g. the function_declarator of a function, not
    // the enclosing declaration. Walk up to the outermost wrapping
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

    struct parse_result
    {
      std::string error;
      std::string source;
      // unique_ptr with a function-pointer deleter has no default
      // constructor; the deleter must always be supplied.
      tree_ptr tree{nullptr, ts_tree_delete};
      query_ptr query{nullptr, ts_query_delete};
      TSNode root{};
    };

    parse_result parse_file(const std::string& path)
    {
      parse_result result;

      const fs::path file(path);
      if (!fs::exists(file)) {
        result.error = std::format("path '{}' does not exist", path);
        return result;
      }
      if (!is_supported_source(file)) {
        result.error = std::format(
          "unsupported file type '{}': only C/C++ sources are supported", path);
        return result;
      }

      std::ifstream in(file);
      if (!in) {
        result.error = std::format("cannot open '{}'", path);
        return result;
      }
      std::ostringstream buffer;
      buffer << in.rdbuf();
      result.source = buffer.str();

      parser_ptr parser(ts_parser_new(), ts_parser_delete);
      if (!ts_parser_set_language(parser.get(), tree_sitter_cpp())) {
        result.error = "failed to configure parser";
        return result;
      }

      result.tree = tree_ptr(
        ts_parser_parse_string(
          parser.get(), nullptr, result.source.data(),
          static_cast<uint32_t>(result.source.size())),
        ts_tree_delete);
      if (!result.tree) {
        result.error = std::format("failed to parse '{}'", path);
        return result;
      }

      result.query = compile_tags_query();
      if (!result.query) {
        result.error = "failed to compile embedded tags query";
        return result;
      }

      result.root = ts_tree_root_node(result.tree.get());
      return result;
    }
  }

  dripcore::awaitable<json> code_tools::outline(std::string path)
  {
    parse_result parsed = parse_file(path);
    if (!parsed.error.empty()) {
      co_return {{"error", parsed.error}};
    }

    const std::vector<symbol_info> symbols =
      collect_symbols(parsed.query.get(), parsed.root, parsed.source);

    json entries = json::array();
    for (const auto& symbol : symbols) {
      json entry = {
        {"kind", symbol.kind},
        {"name", symbol.name},
        {"line_start", ts_node_start_point(symbol.definition).row + 1},
        {"line_end", ts_node_end_point(symbol.definition).row + 1}
      };
      if (!symbol.scope.empty()) {
        entry["scope"] = symbol.scope;
      }
      entries.push_back(std::move(entry));
    }

    co_return {
      {"file", path},
      {"symbols", std::move(entries)},
      {"count", static_cast<int>(symbols.size())},
      {"has_errors", ts_node_has_error(parsed.root)}
    };
  }

  dripcore::awaitable<json> code_tools::read_symbol(std::string path, std::string name)
  {
    parse_result parsed = parse_file(path);
    if (!parsed.error.empty()) {
      co_return {{"error", parsed.error}};
    }

    const std::vector<symbol_info> symbols =
      collect_symbols(parsed.query.get(), parsed.root, parsed.source);

    json definitions = json::array();
    for (const auto& symbol : symbols) {
      if (symbol.name != name) {
        continue;
      }

      // Extract the full declaration, not just the captured declarator.
      const TSNode node = enclosing_declaration(symbol.definition);
      const uint32_t start = ts_node_start_byte(node);
      const uint32_t end = ts_node_end_byte(node);

      definitions.push_back({
        {"kind", symbol.kind},
        {"line_start", ts_node_start_point(node).row + 1},
        {"line_end", ts_node_end_point(node).row + 1},
        {"content", parsed.source.substr(start, end - start)}
      });
    }

    if (definitions.empty()) {
      co_return {
        {"error", std::format("symbol '{}' not found in '{}'", name, path)}
      };
    }

    // Compute before moving definitions into the result.
    const int count = static_cast<int>(definitions.size());

    co_return {
      {"file", path},
      {"name", name},
      {"definitions", std::move(definitions)},
      {"count", count}
    };
  }

} // namespace mistral::tools