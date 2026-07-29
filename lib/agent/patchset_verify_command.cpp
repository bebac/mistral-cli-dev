#include "agent/patchset_verify_command.h"
#include "agent/agent_system.h"
#include "agent/edit_parser.h"

#include <format>
#include <string>
#include <vector>

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

  std::string patchset_verify_command::execute_agent(
    const std::string &args,
    agent::agent_system &agent_sys)
  {
    // Get the last response from agent_system
    std::string response = agent_sys.get_last_response();

    if (response.empty())
    {
      return "❌ Patchset verification failed:\n   - No AI response to verify. Have you sent a message yet?";
    }

    // Parse the response for patchset
    auto parse_result = parse_edits_with_patchset(response);

    if (!parse_result)
    {
      // Try to extract patchset ID to give more specific error
      auto patchset_id = extract_patchset_id(response);
      if (!patchset_id)
      {
        return "❌ Patchset verification failed:\n   - No patchset ID found in response";
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

          return std::format("❌ Patchset verification failed:\n   - Multiple patchset IDs found: {}", ids_str);
        }
      }

      return "❌ Patchset verification failed:\n   - No valid edit blocks found in response";
    }

    // Success case
    const auto& [patchset_id, edits] = *parse_result;

    std::string result = std::format("✅ Patchset verified: {}\n   - {} edit block{} parsed\n",
        patchset_id, edits.size(), edits.size() == 1 ? "" : "s");

    for (const auto& edit : edits)
    {
      result += std::format("   - {}: SEARCH ({} chars) → REPLACE ({} chars)\n",
          edit.file_path,
          edit.search_text.length(),
          edit.replace_text.length());
    }

    return result;
  }
} // namespace mistral::agent