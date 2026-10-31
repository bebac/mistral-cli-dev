#ifndef MISTRAL_EXEC_TOOLS_H
#define MISTRAL_EXEC_TOOLS_H

#include <dripcore/awaitable.h>
#include <nlohmann/json.hpp>

#include <optional>
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

    // Pre-check an exec tool call before it is executed (or approved).
    // Returns an error message if the command is empty, contains shell
    // syntax (execvp() is called directly with no shell), the working
    // directory does not exist, or the command does not exist (PATH
    // lookup, then <cwd>/<command> for a bare name, or - for explicit
    // paths - not an executable file). Returns std::nullopt when the
    // call looks valid.
    static std::optional<std::string> validate(
      const std::string& command,
      const std::vector<std::string>& args,
      const std::string& cwd = ""
    );
  };

} // namespace mistral::tools

#endif // MISTRAL_EXEC_TOOLS_H
