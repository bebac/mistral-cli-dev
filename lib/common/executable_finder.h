#pragma once

#include <string>

namespace mistral::utils
{
  // Find an executable, like which(1). A bare command name (no '/') is
  // looked up in PATH; a path containing '/' is verified to exist, be a
  // regular file and be executable. Returns the resolved path, or an
  // empty string if no executable was found.
  std::string find_executable(const std::string& command);

  // Resolve an executable to an absolute path, the way exec will run it.
  // A path containing '/' is resolved against the process working
  // directory (never against a child's cwd), then verified to exist, be a
  // regular file and be executable. A bare command name is looked up in
  // PATH and, when `cwd` is non-empty, as <cwd>/<command> - so a policy
  // rule that pins a binary to a directory ("tests" in "build") can be
  // invoked by name. Returns the absolute path, or an empty string if no
  // executable was found.
  std::string resolve_executable(const std::string& command, const std::string& cwd = "");
} // namespace mistral::utils
