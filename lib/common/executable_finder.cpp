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
        if (
          fs::exists(candidate, ec) && fs::is_regular_file(candidate, ec) &&
          ::access(candidate.string().c_str(), X_OK) == 0
        )
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

  std::string resolve_executable(const std::string& command, const std::string& cwd)
  {
    namespace fs = std::filesystem;

    std::string found = find_executable(command);

    // A bare name that is not on PATH may still be the binary a policy
    // rule pinned to a directory: try <cwd>/<command>. This only fires
    // when the caller asked for a working directory, so a bare name
    // without one keeps the plain PATH semantics.
    if (found.empty() && !cwd.empty() && command.find('/') == std::string::npos)
    {
      fs::path candidate = fs::path(cwd) / command;
      std::error_code ec;
      if (fs::exists(candidate, ec) && fs::is_regular_file(candidate, ec) &&
          ::access(candidate.string().c_str(), X_OK) == 0)
      {
        found = candidate.string();
      }
    }

    if (found.empty())
    {
      return "";
    }

    std::error_code ec;
    fs::path absolute = fs::absolute(found, ec);
    if (ec)
    {
      return found;
    }

    // Canonicalize so policy matching compares real paths - the policy
    // engine normalizes its rule paths the same way.
    fs::path resolved = fs::weakly_canonical(absolute, ec);
    if (ec)
    {
      return absolute.lexically_normal().string();
    }
    return resolved.string();
  }
} // namespace mistral::utils
