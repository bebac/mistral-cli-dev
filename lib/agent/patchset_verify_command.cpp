#include "agent/patchset_verify_command.h"
#include "agent/agent_system.h"
#include "agent/edit_parser.h"
#include "agent/edit_validator.h"

#include <format>
#include <string>
#include <vector>
#include <dripcore/task.h>

namespace mistral::agent
{
  std::string patchset_verify_command::name() const
  {
    return "patchset_verify";
  }

  std::string patchset_verify_command::description() const
  {
    return "Verify patchset parsing from last AI response";
  }

  std::string patchset_verify_command::usage() const
  {
    return "/patchset_verify";
  }

  console::slash_command::execution_context patchset_verify_command::context() const
  {
    return execution_context::AGENT;
  }

  dripcore::awaitable<std::string> patchset_verify_command::execute_agent(
    dripcore::task& task,
    const std::string &args,
    agent::agent_system &agent_sys)
  {
    // Get the last response from agent_system
    std::string response = agent_sys.get_last_response();

    if (response.empty())
    {
      co_return "❌ Patchset verification failed:\n   - No AI response to verify. Have you sent a message yet?";
    }

    // Parse the response for patchset
    auto parse_result = parse_edits_with_patchset(response);

    if (!parse_result)
    {
      // Try to extract patchset ID to give more specific error
      auto patchset_id = extract_patchset_id(response);
      if (!patchset_id)
      {
        co_return "❌ Patchset verification failed:\n   - No patchset ID found in response";
      }

      // Check for multiple patchset IDs
      size_t first_pos = response.find("(patchset: ");
      if (first_pos != std::string::npos)
      {
        size_t second_pos = response.find("(patchset: ", first_pos + 1);
        if (second_pos != std::string::npos)
        {
          // Extract all IDs
          std::vector<std::string> all_ids;
          size_t pos = 0;
          while (true)
          {
            auto id = extract_patchset_id(response.substr(pos));
            if (!id)
              break;
            all_ids.push_back(*id);
            pos = response.find("(patchset: ", pos) + 1;
            if (pos == std::string::npos)
              break;
          }

          std::string ids_str;
          for (size_t i = 0; i < all_ids.size(); ++i)
          {
            if (i > 0)
              ids_str += ", ";
            ids_str += all_ids[i];
          }

          co_return std::format("❌ Patchset verification failed:\n   - Multiple patchset IDs found: {}", ids_str);
        }
      }

      co_return "❌ Patchset verification failed:\n   - No valid edit blocks found in response";
    }

    // Success - structural parsing passed
    const auto& [patchset_id, edits] = *parse_result;

    auto validation_result = validate_all_edits(edits);

    // Build result message
    std::string result_msg = std::format(
        "✅ Patchset structure valid (patchset: {}, {} edits)\n\n",
        patchset_id, edits.size());

    result_msg += "Initial Validation Results:\n\n";

    for (const auto& v : validation_result.validations) {
      result_msg += std::format("File: {}\n", v.file_path);
      if (v.valid) {
        result_msg += "  ✓ [VALID]\n";
      } else {
        result_msg += std::format("  ✗ [INVALID: {}]\n", v.error_type);
        result_msg += std::format("    Message: {}\n", v.error_message);
        if (!v.search_text.empty()) {
          result_msg += std::format("    Search text: \"{}\"\n", v.search_text);
        }
        result_msg += std::format("    Matches: {}\n", v.match_count);
        if (v.is_fixable) {
          result_msg += std::format("    Fixable: YES - {}\n", v.suggestion);
        } else {
          result_msg += "    Fixable: NO\n";
        }
      }
    }

#if 0
    // Format parsed edits for patcher
    std::string formatted_edits;
    for (const auto& edit : edits)
    {
      formatted_edits += std::format(
          "File: {}\n"
          "SEARCH ({} chars):\n{}\n"
          "REPLACE ({} chars):\n{}\n\n",
          edit.file_path,
          edit.search_text.length(),
          edit.search_text,
          edit.replace_text.length(),
          edit.replace_text);
    }
#endif

#if 0
    // Build prompt for patcher with original response + parsed data
    std::string prompt = std::format(
        "Verify this patchset. Follow your standard verification process.\n\n"
        "Patchset ID: {}\n"
        "Number of edits: {}\n\n"
        "=== ORIGINAL RESPONSE ===\n{}\n\n"
        "=== PARSED EDITS ===\n{}",
        patchset_id, edits.size(), response, formatted_edits);
#else
      std::string prompt = std::format(
        "Review and, if needed, fix this patchset\n\n"
        "---\n"
        "{}\n\n"
        "---\n"
        "{}\n",
        response,
        result_msg
      );
#endif

    //std::cerr << "[patcher] " << prompt << std::endl;

#if 1
    // Run patcher subagent for semantic verification
    std::string patcher_response = co_await agent_sys.run_subagent(task, "patcher", prompt);

    //co_return std::format("✅ Patchset structure valid ({} edits).\n\nPatcher verification:\n{}",
    //    edits.size(), patcher_response);
    co_return patcher_response;
#else
    co_return "<done>";
#endif
  }
} // namespace mistral::agent