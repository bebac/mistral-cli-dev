#ifndef MISTRAL_EMBEDDED_RESOURCES_H
#define MISTRAL_EMBEDDED_RESOURCES_H

#include <string_view>

namespace mistral::embedded
{

  // Version of the managed sys_patchset_instructions.md content. Bump this
  // whenever templates/sys_patchset_instructions.md changes so that
  // workspace_bootstrap updates managed copies in existing workspaces.
  inline constexpr int patchset_instructions_version = 4;

  // Managed file: overwritten by workspace_bootstrap when the on-disk copy
  // is an unmodified stock copy of an older version.
  std::string_view sys_patchset_instructions();

  // Seeded files: written only if missing, never overwritten.
  std::string_view sys_prompt();
  std::string_view sys_prompt_researcher();
  std::string_view policy_json();
  std::string_view config_json();

  // Tree-sitter symbol queries, embedded at build time (see
  // cmake/embed_queries.cmake). Definition queries come from the grammar
  // submodules, reference queries are maintained in this repository.
  std::string_view cpp_tags_query();
  std::string_view cpp_references_query();

} // namespace mistral::embedded

#endif // MISTRAL_EMBEDDED_RESOURCES_H