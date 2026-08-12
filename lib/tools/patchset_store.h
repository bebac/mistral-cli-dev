#ifndef MISTRAL_PATCHSET_STORE_H
#define MISTRAL_PATCHSET_STORE_H

#include "agent/edit_types.h"

#include <optional>
#include <string>
#include <vector>

namespace mistral::agent
{
  // Abstract interface for patchset storage.
  // Allows patchset_tools to be tested in isolation from agent_system.
  class patchset_store
  {
  public:
    virtual ~patchset_store() = default;

    virtual void store_patchset(const std::string& patchset_id, const std::vector<file_edit_t>& edits) = 0;
    virtual std::optional<patchset_t> get_patchset(const std::string& patchset_id) const = 0;
    virtual void remove_patchset() = 0;
  };
} // namespace mistral::agent

#endif // MISTRAL_PATCHSET_STORE_H