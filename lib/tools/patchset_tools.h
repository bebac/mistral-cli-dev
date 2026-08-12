#pragma once

#include "tools/patchset_store.h"
#include "agent/edit_types.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>
#include <optional>

namespace mistral::agent
{
  using json = nlohmann::ordered_json;

  class patchset_tools
  {
  public:
    explicit patchset_tools(patchset_store &store);

    // Apply a stored patchset after validation
    // Automatically attempts correction via patchfixer subagent if validation fails with fixable errors
    json patchset_apply(std::string patchset_id);

    // Get patchset details including all numbered edits
    json patchset_get(std::string patchset_id);

    // Validate a patchset without applying (dry run)
    json patchset_validate(std::string patchset_id);

    // Update a single edit in a stored patchset
    // At least one of search_text or replace_text must be provided
    json patchset_update(
      std::string patchset_id,
      int patch_number,
      std::string file_path,
      std::optional<std::string> search_text = std::nullopt,
      std::optional<std::string> replace_text = std::nullopt
    );

    // Clear a stored patchset
    json patchset_clear(std::string patchset_id);

  private:
    // Helper methods
    json apply_validated_edits(const std::vector<file_edit_t> &edits);
    json attempt_automatic_correction(
      const std::string &patchset_id,
      const std::vector<file_edit_t> &edits,
      const validation_result_t &validation);
    std::string format_validation_errors(const validation_result_t &validation);
    std::string format_edits_for_ai(const std::vector<file_edit_t> &edits);
    json make_error_response(const std::string &error_type, const std::string &error_message);
    json make_validation_failure_response(
      const std::string &patchset_id,
      const validation_result_t &validation
    );

    patchset_store &store_;
  };
} // namespace mistral::agent
