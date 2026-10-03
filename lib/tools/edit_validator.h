#ifndef MISTRAL_EDIT_VALIDATOR_H
#define MISTRAL_EDIT_VALIDATOR_H

#include "tools/edit_types.h"

#include <string>
#include <vector>

namespace mistral::tools
{

  // Validate a single file edit (dry run - does not modify files)
  edit_validation_t validate_edit(const file_edit_t& edit);

  // Validate all file edits atomically (dry run - does not modify files)
  validation_result_t validate_all_edits(const std::vector<file_edit_t>& edits);

} // namespace mistral::tools

#endif // MISTRAL_EDIT_VALIDATOR_H