#pragma once

#include <string>

namespace mistral::utils
{
  // Find an executable, like which(1). A bare command name (no '/') is
  // looked up in PATH; a path containing '/' is verified to exist, be a
  // regular file and be executable. Returns the resolved path, or an
  // empty string if no executable was found.
  std::string find_executable(const std::string& command);
} // namespace mistral::utils
