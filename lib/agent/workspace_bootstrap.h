#ifndef MISTRAL_WORKSPACE_BOOTSTRAP_H
#define MISTRAL_WORKSPACE_BOOTSTRAP_H

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mistral::agent
{

  // Options controlling workspace bootstrap.
  struct bootstrap_options
  {
    bool enabled      = true;  // false: bootstrap is a no-op
    bool force_update = false; // true: overwrite the managed file even if locally modified
  };

  // Outcome of a bootstrap run. File names are relative to the base dir.
  struct bootstrap_result
  {
    bool ran = false;                   // false when bootstrap was disabled
    std::vector<std::string> created;   // files written because they were missing
    std::vector<std::string> updated;   // managed-file overwrites (with reason)
    std::vector<std::string> warnings;  // non-fatal notices
  };

  // Ensures the workspace directory (default .mistral-cli) exists and
  // contains the default configuration files embedded in the executable:
  //
  //   - mistral-cli.json, policy.json, sys_prompt.md:
  //       seeded only if missing, never overwritten.
  //   - sys_patchset_instructions.md:
  //       managed. Overwritten from the embedded content when the on-disk
  //       copy is an unmodified stock copy of an older version. Provenance
  //       is tracked in bootstrap.json (version + content hash). Locally
  //       modified copies are left in place with a warning unless
  //       options.force_update is set.
  bootstrap_result bootstrap_workspace(
    const std::filesystem::path& base_dir,
    const bootstrap_options& options = {});

  // FNV-1a 64-bit hash of data as a 16 character lowercase hex string.
  // Deterministic across platforms and builds (unlike std::hash).
  std::string fnv1a_64_hex(std::string_view data);

} // namespace mistral::agent

#endif // MISTRAL_WORKSPACE_BOOTSTRAP_H