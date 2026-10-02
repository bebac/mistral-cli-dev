#ifndef MISTRAL_CODE_TOOLS_H
#define MISTRAL_CODE_TOOLS_H

#include <dripcore/awaitable.h>
#include <nlohmann/json.hpp>
#include <string>

namespace mistral::tools
{
  using json = nlohmann::ordered_json;

  // Syntax-aware code exploration tools built on tree-sitter. Currently
  // supports C and C++ sources.
  class code_tools
  {
  public:
    // List the symbols (classes, structs, functions, methods, types)
    // defined in a source file with their line ranges.
    dripcore::awaitable<json> outline(std::string path);

    // Extract the exact source text of a named symbol definition in a file.
    // Handles overloads by returning every matching definition.
    dripcore::awaitable<json> read_symbol(std::string path, std::string name);
  };

} // namespace mistral::tools

#endif // MISTRAL_CODE_TOOLS_H