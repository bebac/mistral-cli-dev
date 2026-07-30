#ifndef MISTRAL_FILE_TOOLS_H
#define MISTRAL_FILE_TOOLS_H

#include <dripcore/awaitable.h>
#include <nlohmann/json.hpp>

#include <string>

namespace mistral::tools
{
  using json = nlohmann::ordered_json;

  class file_tools
  {
  public:
    // Read file content with optional line range.
    dripcore::awaitable<json> read_file(std::string path, int line_from, int line_to);

    // List files in a directory.
    dripcore::awaitable<std::string> list_files(std::string path, bool recursive, int max_depth);

    // Write to a file (simple overwrite for V1).
    dripcore::awaitable<json> write_file(std::string path, std::string content);
  };

} // namespace mistral::tools

#endif // MISTRAL_FILE_TOOLS_H
