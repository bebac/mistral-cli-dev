#ifndef MISTRAL_EDIT_TYPES_H
#define MISTRAL_EDIT_TYPES_H

#include <string>
#include <vector>
#include <optional>

namespace mistral::agent
{
  // Represents a single atomic file edit operation
  struct file_edit_t
  {
    std::string file_path;
    std::string search_text; // Empty for new file creation
    std::string replace_text;
    size_t patch_number = 0; // 1-indexed position in patchset
  };

  // Result of validating a single file_edit_t
  struct edit_validation_t
  {
    std::string file_path;
    bool valid = false;
    std::string error_type; // "file_not_found", "search_not_found", "multiple_matches", "format_error", "invalid_path", "file_exists"
    std::string error_message;
    std::string search_text;
    size_t match_count = 0; // How many times search_text appears in target file
    std::vector<size_t> match_positions;
    bool is_fixable = false; // Can patchfixer fix this?
    std::string suggestion;  // Hint for patchfixer
    size_t patch_number = 0; // for error reporting
  };

  // Patchset container struct
  struct patchset_t
  {
    std::string patchset_id;
    std::vector<file_edit_t> edits;
    bool applied = false;
  };

  // Result of validating multiple file_edit_t objects
  struct validation_result_t
  {
    std::vector<edit_validation_t> validations;

    bool all_valid() const
    {
      return std::all_of(
        validations.begin(),
        validations.end(),
        [](const edit_validation_t &ev) {
          return ev.valid;
        }
      );
    }

    int fixable_error_count() const
    {
      return static_cast<int>(std::count_if(
        validations.begin(),
        validations.end(),
        [](const edit_validation_t &ev) {
          return !ev.valid && ev.is_fixable;
        })
      );
    }

    std::vector<edit_validation_t> get_fixable_errors() const
    {
      std::vector<edit_validation_t> result;
      std::copy_if(
        validations.begin(),
        validations.end(),
        std::back_inserter(result),
        [](const edit_validation_t &ev) {
          return !ev.valid && ev.is_fixable;
        }
      );
      return result;
    }

    std::vector<edit_validation_t> get_non_fixable_errors() const
    {
      std::vector<edit_validation_t> result;
      std::copy_if(
        validations.begin(),
        validations.end(),
        std::back_inserter(result),
        [](const edit_validation_t &ev) {
          return !ev.valid && !ev.is_fixable;
        }
      );
      return result;
    }
  };
} // namespace mistral::agent

#endif // MISTRAL_EDIT_TYPES_H
