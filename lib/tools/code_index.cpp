#include "tools/code_index.h"
#include "common/embedded_resources.h"

#include <dripcore/sleep_awaiter.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <regex>
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
    // Empty components in root are skipped so paths with trailing
    // slashes (e.g. "." or "lib/") compare correctly.
    bool is_under(const fs::path& path, const fs::path& root)
    {
      if (root.empty() || path.empty()) {
        return false;
      }
      auto root_it = root.begin();
      auto path_it = path.begin();
      while (root_it != root.end()) {
        // Skip empty components in root (e.g. from trailing slashes).
        if (root_it->empty()) {
          ++root_it;
          continue;
        }
        if (path_it == path.end() || *path_it != *root_it) {
          return false;
        }
        ++root_it;
        ++path_it;
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

    // Names of the enclosing namespace/class/struct/union definitions
    // of node, outermost first ("ns::Outer::Inner"). Anonymous scopes
    // are skipped. Walking the parse tree (instead of extending the
    // tags query) keeps the query simple and handles any nesting.
    std::vector<std::string> enclosing_scope_parts(
      TSNode node, const std::string& source)
    {
      std::vector<std::string> parts;  // collected innermost-first
      for (TSNode current = ts_node_parent(node);
           !ts_node_is_null(current);
           current = ts_node_parent(current))
      {
        const std::string_view type(ts_node_type(current));
        if (type != "namespace_definition" && type != "class_specifier" &&
            type != "struct_specifier" && type != "union_specifier")
        {
          continue;
        }
        const TSNode name = ts_node_child_by_field_name(
          current, "name", static_cast<std::uint32_t>(std::strlen("name")));
        if (ts_node_is_null(name)) {
          continue;  // Anonymous namespace or union.
        }
        parts.push_back(std::string(node_text(name, source)));
      }
      std::reverse(parts.begin(), parts.end());
      return parts;
    }

    // Format a query compile failure with the offending snippet.
    std::string query_error(TSQueryError type, std::uint32_t offset, const std::string& source)
    {
      const char* reason = "invalid query";
      switch (type) {
        case TSQueryErrorSyntax: reason = "syntax error"; break;
        case TSQueryErrorNodeType: reason = "unknown node type"; break;
        case TSQueryErrorField: reason = "unknown field"; break;
        case TSQueryErrorCapture: reason = "invalid capture"; break;
        case TSQueryErrorStructure: reason = "invalid pattern structure"; break;
        case TSQueryErrorLanguage: reason = "language error"; break;
        default: break;
      }

      if (offset >= source.size()) {
        return std::format("query error: {} at offset {}", reason, offset);
      }
      const std::string_view snippet = std::string_view(source).substr(offset, 30);
      return std::format("query error: {} at offset {} near '{}'", reason, offset, snippet);
    }

    // ---- Predicate evaluation ----
    // The tree-sitter core does not evaluate predicates; the consumer
    // must. We support the common operators and reject everything else.

    struct predicate_arg
    {
      bool is_capture = false;
      std::uint32_t capture_index = 0;
      std::string value;
    };

    struct predicate
    {
      std::string op;
      std::vector<predicate_arg> args;
    };

    bool is_supported_predicate(const std::string& op)
    {
      static const std::vector<std::string_view> supported = {
        "#eq?", "#not-eq?", "#any-eq?", "#not-any-eq?",
        "#match?", "#not-match?", "#any-match?", "#not-any-match?",
        "#is?", "#is-not?"
      };
      for (const auto& candidate : supported) {
        if (op == candidate) {
          return true;
        }
      }
      return false;
    }

    // Split the predicate steps of a pattern into predicates.
    std::vector<predicate> parse_predicates(const TSQuery* query, std::uint32_t pattern_index)
    {
      std::vector<predicate> predicates;

      std::uint32_t step_count = 0;
      const TSQueryPredicateStep* steps =
        ts_query_predicates_for_pattern(query, pattern_index, &step_count);

      std::optional<predicate> current;
      for (std::uint32_t i = 0; i < step_count; ++i) {
        const TSQueryPredicateStep& step = steps[i];

        if (step.type == TSQueryPredicateStepTypeDone) {
          if (current) {
            predicates.push_back(std::move(*current));
            current.reset();
          }
          continue;
        }

        if (!current) {
          current = predicate{};
        }

        if (step.type == TSQueryPredicateStepTypeString) {
          std::uint32_t length = 0;
          const char* value = ts_query_string_value_for_id(query, step.value_id, &length);
          if (current->op.empty()) {
            current->op.assign(value, length);
            // The core stores the operator without its leading '#'
            // (the character is consumed while parsing).
            if (!current->op.empty() && current->op[0] != '#') {
              current->op.insert(current->op.begin(), '#');
            }
          } else {
            current->args.push_back({false, 0, std::string(value, length)});
          }
        } else {
          // Capture argument; resolved against the match at evaluation.
          current->args.push_back({true, step.value_id, {}});
        }
      }

      return predicates;
    }

    // Resolve a predicate argument to text. Returns false if the
    // argument is a capture that is not part of this match.
    bool arg_text(
      const predicate_arg& arg, const TSQueryMatch& match, const std::string& source,
      std::string& out)
    {
      if (!arg.is_capture) {
        out = arg.value;
        return true;
      }
      for (std::uint32_t i = 0; i < match.capture_count; ++i) {
        if (match.captures[i].index == arg.capture_index) {
          out = std::string(node_text(match.captures[i].node, source));
          return true;
        }
      }
      return false;
    }

    // Node type of a capture argument (for #is? / #is-not?).
    bool arg_node_type(const predicate_arg& arg, const TSQueryMatch& match, std::string& out)
    {
      if (!arg.is_capture) {
        return false;
      }
      for (std::uint32_t i = 0; i < match.capture_count; ++i) {
        if (match.captures[i].index == arg.capture_index) {
          out = ts_node_type(match.captures[i].node);
          return true;
        }
      }
      return false;
    }

    bool regex_matches(const std::string& pattern, const std::string& text)
    {
      try {
        return std::regex_search(text, std::regex(pattern));
      } catch (const std::regex_error&) {
        return false;
      }
    }

    // Evaluate all predicates of the match's pattern.
    bool predicates_hold(
      const TSQuery* query, const TSQueryMatch& match, const std::string& source)
    {
      for (const auto& pred : parse_predicates(query, match.pattern_index)) {
        if (pred.op == "#set!") {
          continue;  // Directive: pattern property, no effect on matching.
        }
        if (pred.args.size() < 2) {
          return false;
        }

        if (pred.op == "#eq?" || pred.op == "#not-eq?") {
          std::string a;
          std::string b;
          if (!arg_text(pred.args[0], match, source, a) ||
              !arg_text(pred.args[1], match, source, b)) {
            return false;
          }
          const bool equal = a == b;
          if (pred.op == "#eq?" && !equal) return false;
          if (pred.op == "#not-eq?" && equal) return false;
        } else if (pred.op == "#any-eq?" || pred.op == "#not-any-eq?") {
          std::string a;
          if (!arg_text(pred.args[0], match, source, a)) {
            return false;
          }
          bool any = false;
          for (std::size_t i = 1; i < pred.args.size(); ++i) {
            std::string b;
            if (arg_text(pred.args[i], match, source, b) && a == b) {
              any = true;
              break;
            }
          }
          if (pred.op == "#any-eq?" && !any) return false;
          if (pred.op == "#not-any-eq?" && any) return false;
        } else if (pred.op == "#match?" || pred.op == "#not-match?") {
          // Convention is (#match? @capture "pattern"); accept the
          // reversed argument order as well.
          std::string text;
          std::string pattern;
          bool ok = false;
          if (pred.args[1].is_capture && !pred.args[0].is_capture) {
            ok = arg_text(pred.args[1], match, source, text) &&
                 arg_text(pred.args[0], match, source, pattern);
          } else {
            ok = arg_text(pred.args[0], match, source, text) &&
                 arg_text(pred.args[1], match, source, pattern);
          }
          if (!ok) {
            return false;
          }
          const bool m = regex_matches(pattern, text);
          if (pred.op == "#match?" && !m) return false;
          if (pred.op == "#not-match?" && m) return false;
        } else if (pred.op == "#any-match?" || pred.op == "#not-any-match?") {
          std::string text;
          if (!arg_text(pred.args[0], match, source, text)) {
            return false;
          }
          bool any = false;
          for (std::size_t i = 1; i < pred.args.size(); ++i) {
            std::string pattern;
            if (arg_text(pred.args[i], match, source, pattern) &&
                regex_matches(pattern, text)) {
              any = true;
              break;
            }
          }
          if (pred.op == "#any-match?" && !any) return false;
          if (pred.op == "#not-any-match?" && any) return false;
        } else if (pred.op == "#is?" || pred.op == "#is-not?") {
          std::string type;
          if (!arg_node_type(pred.args[0], match, type)) {
            return false;
          }
          std::string expected;
          if (!arg_text(pred.args[1], match, source, expected)) {
            return false;
          }
          const bool is = type == expected;
          if (pred.op == "#is?" && !is) return false;
          if (pred.op == "#is-not?" && is) return false;
        } else {
          // Unknown predicate; compile() rejects these up front.
          return false;
        }
      }

      return true;
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
        } else if (capture_name.starts_with("definition.")) {
          kind = capture_name.substr(std::strlen("definition."));
          definition_node = node;
        } else if (capture_name == "local.scope") {
          scope = std::string(node_text(node, source_));
        }
      }

      if (name.empty() || kind.empty() || ts_node_is_null(name_node)) {
        continue;
      }

      // In-class member functions are declared with a field_identifier;
      // label them "method" like the out-of-line qualified definitions
      // the tags query already captures as definition.method.
      if (kind == "function" &&
          std::string_view(ts_node_type(name_node)) == "field_identifier")
      {
        kind = "method";
      }

      // Scope: enclosing namespaces/classes from the parse tree, plus
      // the qualified scope of out-of-line definitions (Class::method).
      std::vector<std::string> scope_parts =
        enclosing_scope_parts(definition_node, source_);
      if (!scope.empty()) {
        scope_parts.push_back(std::move(scope));
      }
      std::string combined_scope;
      for (const auto& part : scope_parts) {
        if (!combined_scope.empty()) {
          combined_scope += "::";
        }
        combined_scope += part;
      }

      definitions_.push_back({
        std::move(kind), std::move(name), std::move(combined_scope),
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

  // ---- qualified_name ----

  namespace
  {
    std::string trim(const std::string& text)
    {
      const std::size_t start = text.find_first_not_of(" \t");
      if (start == std::string::npos) {
        return {};
      }
      const std::size_t end = text.find_last_not_of(" \t");
      return text.substr(start, end - start + 1);
    }

    // Split "A::B" into {"A", "B"}; parts are whitespace-trimmed.
    std::vector<std::string> split_scope(const std::string& scope)
    {
      std::vector<std::string> parts;
      std::size_t start = 0;
      while (true) {
        const std::size_t sep = scope.find("::", start);
        if (sep == std::string::npos) {
          parts.push_back(trim(scope.substr(start)));
          return parts;
        }
        parts.push_back(trim(scope.substr(start, sep - start)));
        start = sep + 2;
      }
    }
  }

  qualified_name qualified_name::parse(const std::string& qualified)
  {
    qualified_name result;
    const std::vector<std::string> parts = split_scope(qualified);
    if (parts.empty()) {
      return result;
    }
    result.name = parts.back();
    result.scope.assign(parts.begin(), parts.end() - 1);
    return result;
  }

  bool qualified_name::matches(const symbol_definition& definition) const
  {
    if (definition.name != name) {
      return false;
    }
    if (scope.empty()) {
      return true;
    }
    // The requested scope must be a suffix of the definition's scope, so
    // "Class::method" also matches a definition scoped "ns::Class".
    const std::vector<std::string> parts = split_scope(definition.scope);
    if (parts.size() < scope.size()) {
      return false;
    }
    return std::equal(scope.begin(), scope.end(), parts.end() - scope.size());
  }

  // ---- compiled_query ----

  bool compiled_query::compile(const std::string& query_source, std::string& error)
  {
    std::uint32_t error_offset = 0;
    TSQueryError error_type = TSQueryErrorNone;
    TSQuery* compiled = ts_query_new(
      tree_sitter_cpp(), query_source.data(),
      static_cast<std::uint32_t>(query_source.size()), &error_offset, &error_type);

    if (compiled == nullptr || error_type != TSQueryErrorNone) {
      if (compiled != nullptr) {
        ts_query_delete(compiled);
      }
      error = query_error(error_type, error_offset, query_source);
      return false;
    }

    query_.reset(compiled);

    // Without captures a match carries no position information.
    if (ts_query_capture_count(query_.get()) == 0) {
      error = "query must capture at least one node (e.g. (function_definition) @fn)";
      return false;
    }

    // Reject predicates we cannot evaluate: the core does not evaluate
    // predicates, and silently ignoring them would produce wrong results.
    const std::uint32_t pattern_count = ts_query_pattern_count(query_.get());
    for (std::uint32_t i = 0; i < pattern_count; ++i) {
      for (const auto& pred : parse_predicates(query_.get(), i)) {
        if (pred.op == "#set!") {
          continue;  // Directive: pattern property, safe to ignore.
        }
        if (!is_supported_predicate(pred.op)) {
          error = std::format(
            "unsupported predicate '{}'; supported: #eq?, #not-eq?, #any-eq?, "
            "#not-any-eq?, #match?, #not-match?, #any-match?, #not-any-match?, "
            "#is?, #is-not?", pred.op);
          return false;
        }
      }
    }

    return true;
  }

  // ---- parsed_source: query search ----

  std::vector<parsed_source::query_match> parsed_source::search(
    const compiled_query& query) const
  {
    std::vector<query_match> results;
    if (!tree_) {
      return results;
    }

    const TSQuery* q = query.get();
    if (q == nullptr) {
      return results;
    }

    cursor_ptr cursor(ts_query_cursor_new(), ts_query_cursor_delete);
    ts_query_cursor_exec(cursor.get(), q, ts_tree_root_node(tree_.get()));

    TSQueryMatch match;
    while (ts_query_cursor_next_match(cursor.get(), &match)) {
      if (match.capture_count == 0) {
        continue;
      }
      if (!predicates_hold(q, match, source_)) {
        continue;
      }

      query_match result;
      result.line_start = std::numeric_limits<int>::max();
      result.line_end = 0;

      for (std::uint32_t i = 0; i < match.capture_count; ++i) {
        const TSNode node = match.captures[i].node;
        const int line = static_cast<int>(ts_node_start_point(node).row) + 1;

        std::uint32_t length = 0;
        const char* capture = ts_query_capture_name_for_id(q, match.captures[i].index, &length);

        query_capture entry;
        entry.name.assign(capture, length);
        entry.text = std::string(node_text(node, source_));
        entry.line = line;
        result.captures.push_back(std::move(entry));

        result.line_start = std::min(result.line_start, line);
        result.line_end = std::max(
          result.line_end, static_cast<int>(ts_node_end_point(node).row) + 1);
      }

      results.push_back(std::move(result));
    }

    return results;
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
      if (is_under(it->first, root_abs) && !seen.contains(it->first)) {
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
    const qualified_name requested = qualified_name::parse(name);

    for (const auto& [file, entry] : files_) {
      if (!is_under(file, root_abs)) {
        continue;
      }
      for (const auto& definition : entry.definitions) {
        if (requested.matches(definition)) {
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
    // Substring fallback matches the bare name only; the scope of a
    // qualified request is ignored so typos still find something.
    const std::string bare = qualified_name::parse(name).name;

    for (const auto& [file, entry] : files_) {
      if (!is_under(file, root_abs)) {
        continue;
      }
      for (const auto& definition : entry.definitions) {
        if (definition.name.find(bare) != std::string::npos) {
          hits.push_back({file, definition});
        }
      }
    }
    return hits;
  }

} // namespace mistral::tools