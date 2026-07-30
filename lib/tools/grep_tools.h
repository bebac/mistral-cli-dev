#ifndef MISTRAL_GREP_TOOLS_H
#define MISTRAL_GREP_TOOLS_H

#include <dripcore/awaitable.h>
#include <nlohmann/json.hpp>
#include <string>

namespace mistral::tools
{
  using json = nlohmann::ordered_json;

  class grep_tools
  {
  public:
    // Search for regex pattern in files.
    // Returns matching lines with filename and line number.
    dripcore::awaitable<json> grep(std::string pattern, std::string path, bool recursive);
  };

} // namespace mistral::tools

#endif // MISTRAL_GREP_TOOLS_H
