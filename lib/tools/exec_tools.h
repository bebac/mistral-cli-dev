#ifndef MISTRAL_EXEC_TOOLS_H
#define MISTRAL_EXEC_TOOLS_H

#include <dripcore/awaitable.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace mistral::tools
{
  using json = nlohmann::ordered_json;

  class exec_tools
  {
  public:
    // Execute an external command with controlled access.
    // Uses fork() + execvp() directly - no shell involved.
    // Returns JSON with exit code, stdout, stderr, and status.
    dripcore::awaitable<json> exec(
      std::string command,
      std::vector<std::string> args,
      std::string cwd = "",
      int timeout_seconds = 30
    );

  private:
    // Find executable path using PATH lookup (like which)
    std::string find_executable(const std::string& command) const;
  };

} // namespace mistral::tools

#endif // MISTRAL_EXEC_TOOLS_H