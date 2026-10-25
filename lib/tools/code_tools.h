#ifndef MISTRAL_CODE_TOOLS_H
#define MISTRAL_CODE_TOOLS_H

#ifdef MISTRAL_ENABLE_TREE_SITTER

#include "tools/code_index.h"

#include <dripcore/awaitable.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <string>

namespace mistral::tools
{
  using json = nlohmann::ordered_json;

  // Syntax-aware code exploration tools built on tree-sitter. Currently
  // supports C and C++ sources.
  class code_tools
  {
  public:
    explicit code_tools(code_index_settings settings = {});

    // List the symbols (classes, structs, functions, methods, types)
    // defined in a source file with their line ranges.
    dripcore::awaitable<json> outline(std::string path);

    // Extract the exact source text of a named symbol definition in a
    // file. Handles overloads by returning every matching definition.
    // The name may be qualified (Class::method) to select among
    // same-named definitions in different scopes.
    dripcore::awaitable<json> read_symbol(std::string path, std::string name);

    // Find symbol definitions by name across a directory tree. Falls
    // back to substring matching when no exact match exists. The name
    // may be qualified (Class::method or ns::Class::method).
    dripcore::awaitable<json> find_symbol(std::string name, std::string path);

    // Find usage sites (calls, type usages, member access) of a named
    // symbol across a directory tree. Definition sites are excluded.
    dripcore::awaitable<json> find_references(std::string name, std::string path);

    // Run a tree-sitter query against a file or directory tree.
    dripcore::awaitable<json> ast_search(std::string query, std::string path);

  private:
    // Walk the supported source files under root (skipping excluded
    // directories), reading each file and passing its content to
    // visit. The walk stops early when visit returns false. Yields
    // to the scheduler periodically; unreadable entries are skipped.
    dripcore::awaitable<void> for_each_source_file(
      const std::filesystem::path& root,
      const std::function<bool(
        const std::filesystem::path& file, std::string&& source)>& visit
    );

    code_index_settings settings_;
    symbol_index index_;
  };

} // namespace mistral::tools

#endif // MISTRAL_ENABLE_TREE_SITTER

#endif // MISTRAL_CODE_TOOLS_H