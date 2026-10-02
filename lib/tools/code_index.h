#ifndef MISTRAL_CODE_INDEX_H
#define MISTRAL_CODE_INDEX_H

#include <dripcore/awaitable.h>
#include "tree_sitter/api.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mistral::tools
{
  // ---- Settings ----
  // Configurable via the "code_index" section of the workspace config.
  struct code_index_settings
  {
    // Directory names excluded from indexing and reference search.
    // Matched against every path component.
    std::vector<std::string> exclude_dirs = {
      ".git", ".mistral-cli", ".cache", ".vscode", ".vibe", "build",
      "node_modules", "__pycache__"
    };

    // Maximum number of results returned by find_symbol / find_references.
    int max_results = 200;
  };

  // ---- Symbol definition ----
  struct symbol_definition
  {
    std::string kind;    // "class", "function", "method", "type"
    std::string name;
    std::string scope;   // non-empty for qualified out-of-line definitions
    int line_start = 0;  // 1-indexed, inclusive
    int line_end = 0;    // 1-indexed, inclusive
  };

  class compiled_query;

  // ---- Parsed source ----
  // Owns the parse tree of a C/C++ source file and provides symbol
  // extraction on top of the embedded tree-sitter queries.
  class parsed_source
  {
  public:
    using tree_ptr = std::unique_ptr<TSTree, decltype(&ts_tree_delete)>;

    // Parse the file at path and extract symbol definitions. Returns
    // false and sets error on failure.
    bool load(const std::string& path, std::string& error);

    // As above, but for source text that was already read by the caller
    // (path is only used for error messages).
    bool load(const std::string& path, std::string source_text, std::string& error);

    const std::string& source() const { return source_; }
    const std::vector<symbol_definition>& definitions() const { return definitions_; }

    // True if the parse produced ERROR nodes (e.g. macros or generated
    // code the grammar cannot fully understand).
    bool has_errors() const;

    // Byte range of the enclosing declaration of definition `index`,
    // including return type and body. Returns false if unavailable.
    bool definition_extent(std::size_t index, std::uint32_t& start, std::uint32_t& end) const;

    struct usage_site
    {
      int line = 0;             // 1-indexed
      std::uint32_t start = 0;  // byte offset of the identifier
      bool is_definition = false;
    };

    // All syntactic occurrences of name as an identifier (calls, type
    // usages, member access; never comments, strings or preprocessor).
    // Definition sites are flagged so callers can exclude them.
    std::vector<usage_site> usages_of(const std::string& name) const;

    struct query_capture
    {
      std::string name;
      std::string text;
      int line = 0;  // 1-indexed
    };

    struct query_match
    {
      int line_start = 0;
      int line_end = 0;
      std::vector<query_capture> captures;
    };

    // Run a compiled query against the parsed tree. Matches whose
    // predicates fail are excluded.
    std::vector<query_match> search(const compiled_query& query) const;

  private:
    std::string source_;
    tree_ptr tree_{nullptr, ts_tree_delete};
    std::vector<symbol_definition> definitions_;
    std::vector<TSNode> definition_nodes_{};
    std::vector<TSNode> definition_name_nodes_{};
  };

  // ---- Compiled query ----
  // A compiled tree-sitter query for the C++ grammar.
  class compiled_query
  {
  public:
    using query_ptr = std::unique_ptr<TSQuery, decltype(&ts_query_delete)>;

    // Compile query_source. Returns false and sets error (including the
    // byte offset of the problem) on failure. Queries without captures
    // or with unsupported predicates are rejected.
    bool compile(const std::string& query_source, std::string& error);

    const TSQuery* get() const { return query_.get(); }

  private:
    query_ptr query_{nullptr, ts_query_delete};
  };

  // ---- Symbol index ----
  // In-memory index of symbol definitions under a directory tree.
  //
  // Design notes for a future disk-based index:
  // - file_entry (stamp + definitions) is plain serializable data; a
  //   disk index would persist it (e.g. under .mistral-cli/cache/) keyed
  //   by a version stamp of (grammar, embedded query, schema version) so
  //   it can be invalidated when the binary changes.
  // - refresh() is incremental: files whose mtime and size are unchanged
  //   are skipped, so a disk index only needs to store the same stamps.
  class symbol_index
  {
  public:
    explicit symbol_index(code_index_settings settings);

    // Supported C/C++ source file test (by extension).
    static bool is_supported_source(const std::filesystem::path& path);

    // True if any path component matches an excluded directory name.
    bool is_excluded(const std::filesystem::path& path) const;

    // Incrementally (re)index supported files under root. Yields to
    // the scheduler periodically. Safe to call repeatedly.
    dripcore::awaitable<void> refresh(const std::string& root);

    struct hit
    {
      std::string file;  // absolute path
      symbol_definition symbol;
    };

    // Definitions under root whose name equals name.
    std::vector<hit> find(const std::string& root, const std::string& name) const;

    // Definitions under root whose name contains name.
    std::vector<hit> find_substring(const std::string& root, const std::string& name) const;

  private:
    struct file_entry
    {
      std::uint64_t mtime = 0;
      std::uint64_t size = 0;
      std::vector<symbol_definition> definitions;
    };

    code_index_settings settings_;
    std::map<std::string, file_entry> files_;  // absolute path -> entry
  };

} // namespace mistral::tools

#endif // MISTRAL_CODE_INDEX_H