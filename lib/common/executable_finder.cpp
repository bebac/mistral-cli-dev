#include "common/executable_finder.h"

#include <cstdlib>
#include <filesystem>

#include <unistd.h>

namespace mistral::utils
{
  std::string find_executable(const std::string& command)
  {
    namespace fs = std::filesystem;

    // If it's already an absolute or relative path, verify that it
    // exists and is executable so bogus paths fail fast with a clear
    // "command not found" instead of reaching execvp() (exit 127).
    if (command.find('/') != std::string::npos)
    {
      std::error_code ec;
      if (fs::exists(command, ec) && fs::is_regular_file(command, ec) && ::access(command.c_str(), X_OK) == 0)
      {
        return command;
      }
      return "";
    }

    // Search PATH
    const char* path_env = std::getenv("PATH");
    if (!path_env)
    {
      return "";
    }

    std::string path_str(path_env);
    size_t start = 0;
    while (start < path_str.size())
    {
      size_t end = path_str.find(':', start);
      std::string dir = (end == std::string::npos) ? path_str.substr(start) : path_str.substr(start, end - start);
      if (!dir.empty())
      {
        fs::path candidate = fs::path(dir) / command;
        std::error_code ec;
        if (fs::exists(candidate, ec) && fs::is_regular_file(candidate, ec) &&
            ::access(candidate.string().c_str(), X_OK) == 0)
        {
          return candidate.string();
        }
      }
      if (end == std::string::npos)
        break;
      start = end + 1;
    }

    return "";
  }
} // namespace mistral::utils
