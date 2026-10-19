#ifndef MISTRAL_APPROVAL_RENDERERS_H
#define MISTRAL_APPROVAL_RENDERERS_H

namespace mistral::agent
{
  class tool_registry;

  // Register per-tool approval prompt renderers on the given registry.
  // Currently registers renderers for "patchset_apply" and "exec";
  // all other tools fall back to the generic build_approval_prompt().
  void register_approval_renderers(tool_registry& tools);
} // namespace mistral::agent

#endif // MISTRAL_APPROVAL_RENDERERS_H
