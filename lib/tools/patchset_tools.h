#pragma once

#include "tools/patchset_store.h"
#include "tools/edit_types.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <optional>

namespace mistral::tools
{
  using json = nlohmann::ordered_json;

  class patchset_tools
  {
  public:
    explicit patchset_tools(patchset_store &store);

    // Add a single edit to a patchset. Creates the patchset when the ID
    // is new; appends when the ID matches a stored non-applied patchset
    // (e.g. one started by emitted edit blocks). Empty search_text means
    // new file creation.
    json patchset_add(
      std::string patchset_id,
      std::string file_path,
      std::string search_text,
      std::string replace_text
    );

    // Apply a stored patchset after validation. On validation failure
    // the response lists per-edit errors; fix with patchset_update or
    // patchset_remove and apply again.
    json patchset_apply(std::string patchset_id);

    // Get patchset details including all numbered edits
    json patchset_get(std::string patchset_id);

    // Validate a patchset without applying (dry run)
    json patchset_validate(std::string patchset_id);

    // Update a single edit in a stored patchset
    // file_path, search_text and replace_text are each updated when
    // provided (empty/omitted fields keep their existing values)
    json patchset_update(
      std::string patchset_id,
      int patch_number,
      std::string file_path,
      std::optional<std::string> search_text = std::nullopt,
      std::optional<std::string> replace_text = std::nullopt
    );

    // Remove a single edit from a stored patchset by patch number
    // Remaining edits keep their patch numbers (no renumbering)
    json patchset_remove(std::string patchset_id, int patch_number);

    // Clear a stored patchset
    json patchset_clear(std::string patchset_id);

  private:
    // Helper methods
    json apply_validated_edits(const std::vector<file_edit_t> &edits);
    json attempt_automatic_correction(
      const std::string &patchset_id,
      const std::vector<file_edit_t> &edits,
      const validation_result_t &validation);
    json make_error_response(const std::string &error_type, const std::string &error_message);
    json make_validation_failure_response(
      const std::string &patchset_id,
      const validation_result_t &validation
    );

    patchset_store &store_;
  };
} // namespace mistral::tools
