#include "tools/patchset_tools.h"
#include "agent/edit_validator.h"
#include "agent/edit_parser.h"
#include "agent/edit_types.h"

#include <nlohmann/json.hpp>

#include <exception>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>

namespace fs = std::filesystem;

namespace mistral::agent
{
  using json = nlohmann::ordered_json;

  patchset_tools::patchset_tools(patchset_store &store)
    : store_(store) {}

  json patchset_tools::patchset_apply(std::string patchset_id)
  {
    // 1. Retrieve patchset from agent_system
    auto patchset = store_.get_patchset(patchset_id);
    if (!patchset)
    {
      return make_error_response(
        "patchset_not_found",
        "No patchset with ID: " + patchset_id
      );
    }

    // 2. Check if already applied
    if (patchset->applied)
    {
      return make_error_response(
        "already_applied",
        "Patchset '" + patchset_id + "' has already been applied"
      );
    }

    // 3. Check for empty patchset (formatting error indicator)
    if (patchset->edits.empty())
    {
      return make_error_response(
        "no_edits",
        "Patchset '" + patchset_id + "' contains 0 edits. "
        "This may indicate a formatting error (e.g., missing >>>>>>> REPLACE "
        "markers or mismatched ~~~ code fences)."
      );
    }

    // 4. Validate all edits (dry run)
    auto validation_result = validate_all_edits(patchset->edits);

    if (validation_result.all_valid())
    {
      // 4. All valid - apply all edits
      auto result = apply_validated_edits(patchset->edits);

      // 5. Mark patchset as applied if all edits succeeded
      if (result["status"] == "success" && result["failed"].get<int>() == 0)
      {
        store_.mark_applied(patchset_id);
      }

      return result;
    }

    // 6. Validation failed
    if (validation_result.fixable_error_count() > 0)
    {
      // Try automatic correction via patchfixer subagent
      return attempt_automatic_correction(patchset_id, patchset->edits, validation_result);
    }

    // 7. Non-fixable errors - return validation failure
    return make_validation_failure_response(patchset_id, validation_result);
  }

  json patchset_tools::patchset_get(std::string patchset_id)
  {
    auto patchset = store_.get_patchset(patchset_id);
    if (!patchset)
    {
      return make_error_response(
        "patchset_not_found",
        "No patchset with ID: " + patchset_id
      );
    }

    json result;
    result["patchset_id"] = patchset->patchset_id;
    result["edit_count"] = patchset->edits.size();

    json edits_array = json::array();
    for (const auto &edit : patchset->edits)
    {
      json edit_json;
      edit_json["patch_number"] = edit.patch_number;
      edit_json["file_path"] = edit.file_path;
      edit_json["search_text"] = edit.search_text;
      edit_json["replace_text"] = edit.replace_text;
      edits_array.push_back(edit_json);
    }
    result["edits"] = edits_array;

    return result;
  }

  json patchset_tools::patchset_validate(std::string patchset_id)
  {
    auto patchset = store_.get_patchset(patchset_id);
    if (!patchset)
    {
      return make_error_response(
        "patchset_not_found",
        "No patchset with ID: " + patchset_id
      );
    }

    // Check for empty patchset (formatting error indicator)
    if (patchset->edits.empty())
    {
      return make_error_response(
        "no_edits",
        "Patchset '" + patchset_id + "' contains 0 edits. "
        "This may indicate a formatting error (e.g., missing >>>>>>> REPLACE "
        "markers or mismatched ~~~ code fences)."
      );
    }

    auto validation_result = validate_all_edits(patchset->edits);

    json result;
    result["patchset_id"] = patchset_id;
    result["all_valid"] = validation_result.all_valid();
    result["fixable_error_count"] = validation_result.fixable_error_count();

    json validations_array = json::array();
    for (const auto &v : validation_result.validations)
    {
      json val_json;
      val_json["patch_number"] = v.patch_number;
      val_json["file_path"] = v.file_path;
      val_json["valid"] = v.valid;
      if (!v.valid)
      {
        val_json["error_type"] = v.error_type;
        val_json["error_message"] = v.error_message;
        val_json["match_count"] = v.match_count;
        val_json["is_fixable"] = v.is_fixable;
        val_json["suggestion"] = v.suggestion;
      }
      validations_array.push_back(val_json);
    }
    result["validations"] = validations_array;

    return result;
  }

  json patchset_tools::patchset_update(
    std::string patchset_id,
    int patch_number,
    std::string file_path,
    std::optional<std::string> search_text,
    std::optional<std::string> replace_text)
  {
    auto patchset = store_.get_patchset(patchset_id);
    if (!patchset)
    {
      return make_error_response(
        "patchset_not_found",
        "No patchset with ID: " + patchset_id
      );
    }

    // Check if already applied
    if (patchset->applied)
    {
      return make_error_response(
        "already_applied",
        "Patchset '" + patchset_id + "' has already been applied"
      );
    }

    // Find the edit with the matching patch_number
    for (auto &edit : patchset->edits)
    {
      if (edit.patch_number == static_cast<size_t>(patch_number))
      {
        // Update only the provided fields
        if (!file_path.empty() && file_path != edit.file_path)
        {
          edit.file_path = file_path;
        }
        if (search_text)
        {
          edit.search_text = *search_text;
        }
        if (replace_text)
        {
          edit.replace_text = *replace_text;
        }

        // Re-store the updated patchset
        store_.store_patchset(patchset_id, patchset->edits);
        return json{
          {"status", "success"},
          {"patchset_id", patchset_id},
          {"patch_number", patch_number},
          {"file_path", edit.file_path}
        };
      }
    }

    return make_error_response(
      "edit_not_found",
      "No edit with patch_number " + std::to_string(patch_number) + " in patchset " + patchset_id
    );
  }

  json patchset_tools::patchset_remove(std::string patchset_id, int patch_number)
  {
    auto patchset = store_.get_patchset(patchset_id);
    if (!patchset)
    {
      return make_error_response(
        "patchset_not_found",
        "No patchset with ID: " + patchset_id
      );
    }

    // Check if already applied
    if (patchset->applied)
    {
      return make_error_response(
        "already_applied",
        "Patchset '" + patchset_id + "' has already been applied"
      );
    }

    // Find the edit with the matching patch_number
    auto it = std::find_if(
      patchset->edits.begin(), patchset->edits.end(),
      [&](const file_edit_t &edit) {
        return edit.patch_number == static_cast<size_t>(patch_number);
      }
    );
    if (it == patchset->edits.end())
    {
      return make_error_response(
        "edit_not_found",
        "No edit with patch_number " + std::to_string(patch_number) + " in patchset " + patchset_id
      );
    }

    // Remove the edit. Patch numbers of the remaining edits are kept
    // stable (not renumbered) so numbers reported by earlier
    // validate/get results stay valid.
    patchset->edits.erase(it);

    // Re-store the updated patchset
    store_.store_patchset(patchset_id, patchset->edits);

    json remaining = json::array();
    for (const auto &edit : patchset->edits)
    {
      remaining.push_back(edit.patch_number);
    }

    return json{
      {"status", "success"},
      {"patchset_id", patchset_id},
      {"removed_patch_number", patch_number},
      {"remaining_edit_count", patchset->edits.size()},
      {"remaining_patch_numbers", remaining}
    };
  }

  json patchset_tools::patchset_clear(std::string patchset_id)
  {
    auto patchset = store_.get_patchset(patchset_id);
    if (!patchset)
    {
      return make_error_response(
        "patchset_not_found",
        "No patchset with ID: " + patchset_id
      );
    }

    store_.remove_patchset(patchset_id);
    return json{{"status", "success"}};
  }

  json patchset_tools::apply_validated_edits(const std::vector<file_edit_t> &edits)
  {
    json result;
    result["status"] = "success";
    result["applied"] = 0;
    result["failed"] = 0;
    result["reviewed"] = false;

    json results_array = json::array();

    for (const auto &edit : edits)
    {
      json edit_result;
      edit_result["patch_number"] = edit.patch_number;
      edit_result["file"] = edit.file_path;

      try
      {
        // For new files (empty search_text)
        if (edit.search_text.empty())
        {
          // Create parent directories if they don't exist
          auto parent_path = fs::path(edit.file_path).parent_path();
          if (!parent_path.empty())
          {
            std::error_code ec;
            fs::create_directories(parent_path, ec);
            if (ec)
            {
              edit_result["status"] = "failed";
              edit_result["error_type"] = "write_failed";
              edit_result["error"] = "Could not create directory: " + parent_path.string() +
                                     " (" + ec.message() + ")";
              results_array.push_back(edit_result);
              result["failed"] = result["failed"].get<int>() + 1;
              continue;
            }
          }

          // Create new file
          std::ofstream out(edit.file_path);
          if (!out)
          {
            edit_result["status"] = "failed";
            edit_result["error_type"] = "write_failed";
            edit_result["error"] = "Could not create file: " + edit.file_path;
            results_array.push_back(edit_result);
            result["failed"] = result["failed"].get<int>() + 1;
            continue;
          }
          out << edit.replace_text;
          out.close();
        }
        // For file overwrite (search_text is exactly ".")
        else if (edit.search_text == ".")
        {
          // Overwrite entire file
          std::ofstream out(edit.file_path);
          if (!out)
          {
            edit_result["status"] = "failed";
            edit_result["error_type"] = "write_failed";
            edit_result["error"] = "Could not overwrite file: " + edit.file_path;
            results_array.push_back(edit_result);
            result["failed"] = result["failed"].get<int>() + 1;
            continue;
          }
          out << edit.replace_text;
          out.close();
        }
        else
        {
          // Regular edit: find and replace
          std::ifstream in(edit.file_path);
          if (!in)
          {
            edit_result["status"] = "failed";
            edit_result["error_type"] = "file_read_error";
            edit_result["error"] = "Could not open file for reading: " + edit.file_path;
            results_array.push_back(edit_result);
            result["failed"] = result["failed"].get<int>() + 1;
            continue;
          }

          std::string content;
          try
          {
            content.assign(std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>());
          }
          catch (const std::exception&)
          {
            // On Linux, opening a directory with std::ifstream succeeds
            // but the first read throws (e.g. "Is a directory").
            edit_result["status"] = "failed";
            edit_result["error_type"] = "file_read_error";
            edit_result["error"] = "Could not read file: " + edit.file_path;
            results_array.push_back(edit_result);
            result["failed"] = result["failed"].get<int>() + 1;
            continue;
          }
          in.close();

          // Find the search text
          size_t pos = content.find(edit.search_text);
          if (pos == std::string::npos)
          {
            edit_result["status"] = "failed";
            edit_result["error_type"] = "search_not_found";
            edit_result["error"] = "Search text not found during application";
            results_array.push_back(edit_result);
            result["failed"] = result["failed"].get<int>() + 1;
            continue;
          }

          // Replace
          content.replace(pos, edit.search_text.length(), edit.replace_text);

          // Write back
          std::ofstream out(edit.file_path);
          if (!out)
          {
            edit_result["status"] = "failed";
            edit_result["error_type"] = "write_failed";
            edit_result["error"] = "Could not write to file: " + edit.file_path;
            results_array.push_back(edit_result);
            result["failed"] = result["failed"].get<int>() + 1;
            continue;
          }
          out << content;
          out.close();
        }

        edit_result["status"] = "applied";
        results_array.push_back(edit_result);
        result["applied"] = result["applied"].get<int>() + 1;
      }
      catch (const std::exception &e)
      {
        edit_result["status"] = "failed";
        edit_result["error_type"] = "write_failed";
        edit_result["error"] = e.what();
        results_array.push_back(edit_result);
        result["failed"] = result["failed"].get<int>() + 1;
      }
    }

    result["results"] = results_array;
    return result;
  }

  json patchset_tools::attempt_automatic_correction(
      const std::string &patchset_id,
      const std::vector<file_edit_t> &edits,
      const validation_result_t &validation)
  {
    // This is a placeholder for Phase 5 - patchfixer integration
    // For Phase 4, we return a validation failure with review_attempted flag
    // In Phase 5, this will actually invoke the patchfixer subagent

    json result;
    result["status"] = "validation_failed";
    result["patchset_id"] = patchset_id;
    result["applied"] = 0;
    result["failed"] = static_cast<int>(validation.validations.size());
    result["review_attempted"] = true;
    result["review_status"] = "not_implemented";
    result["review_error"] = "patchfixer integration not yet implemented (Phase 5)";

    // Format errors
    json errors_array = json::array();
    for (const auto &v : validation.validations)
    {
      if (!v.valid)
      {
        json error_json;
        error_json["patch_number"] = v.patch_number;
        error_json["file"] = v.file_path;
        error_json["error_type"] = v.error_type;
        error_json["error"] = v.error_message;
        if (v.error_type == "search_not_found" || v.error_type == "multiple_matches")
        {
          error_json["search_text"] = v.search_text;
          error_json["match_count"] = v.match_count;
        }
        errors_array.push_back(error_json);
      }
    }
    result["errors"] = errors_array;

    return result;
  }

  std::string patchset_tools::format_validation_errors(const validation_result_t &validation)
  {
    std::ostringstream oss;
    bool first = true;

    for (const auto &v : validation.validations)
    {
      if (!v.valid && v.is_fixable)
      {
        if (!first)
        {
          oss << "\n";
        }
        first = false;

        oss << "- Patch " << v.patch_number << ": File: " << v.file_path << "\n";
        oss << "  Error: " << v.error_type << "\n";
        oss << "  Search text: \"" << v.search_text << "\"\n";
        oss << "  Matches: " << v.match_count << " (in target file)\n";
        if (!v.suggestion.empty())
        {
          oss << "  Suggestion: " << v.suggestion << "\n";
        }
      }
    }

    return oss.str();
  }

  std::string patchset_tools::format_edits_for_ai(const std::vector<file_edit_t> &edits)
  {
    std::ostringstream oss;

    for (const auto &edit : edits)
    {
      oss << edit.file_path << "\n";

      // Determine language based on file extension
      std::string lang = "cpp";
      size_t dot_pos = edit.file_path.find_last_of('.');
      if (dot_pos != std::string::npos)
      {
        std::string ext = edit.file_path.substr(dot_pos + 1);
        if (ext == "cpp" || ext == "hpp" || ext == "hxx" || ext == "cc" || ext == "hh")
        {
          lang = "cpp";
        }
        else if (ext == "c" || ext == "h")
        {
          lang = "c";
        }
        else if (ext == "cmake")
        {
          lang = "cmake";
        }
        else if (ext == "md")
        {
          lang = "markdown";
        }
        else if (ext == "json")
        {
          lang = "json";
        }
      }

      oss << "```" << lang << "\n";
      oss << "<<<<<<< SEARCH\n";
      oss << edit.search_text;
      oss << "\n=======\n";
      oss << edit.replace_text;
      oss << "\n>>>>>>> REPLACE\n";
      oss << "```\n\n";
    }

    return oss.str();
  }

  json patchset_tools::make_error_response(const std::string &error_type, const std::string &error_message)
  {
    return json{
      {"status", "error"},
      {"error_type", error_type},
      {"error", error_message}
    };
  }

  json patchset_tools::make_validation_failure_response(
    const std::string &patchset_id,
    const validation_result_t &validation)
  {
    json result;
    result["status"] = "validation_failed";
    result["patchset_id"] = patchset_id;
    result["applied"] = 0;
    result["failed"] = static_cast<int>(validation.validations.size());
    result["review_attempted"] = false;

    json errors_array = json::array();
    for (const auto &v : validation.validations)
    {
      if (!v.valid)
      {
        json error_json;
        error_json["patch_number"] = v.patch_number;
        error_json["file"] = v.file_path;
        error_json["error_type"] = v.error_type;
        error_json["error"] = v.error_message;
        if (v.error_type == "search_not_found" || v.error_type == "multiple_matches")
        {
          error_json["search_text"] = v.search_text;
          error_json["match_count"] = v.match_count;
        }
        errors_array.push_back(error_json);
      }
    }
    result["errors"] = errors_array;

    return result;
  }
} // namespace mistral::agent
