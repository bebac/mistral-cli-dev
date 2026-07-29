#ifndef MISTRAL_PARSER_H
#define MISTRAL_PARSER_H

#include "agent/edit_types.h"

#include <string>
#include <optional>
#include <vector>
#include <utility>

namespace mistral::agent
{
  // Extract patchset ID from AI response containing "(patchset: <id>)"
  // Returns std::nullopt if not found or malformed
  std::optional<std::string> extract_patchset_id(const std::string &input);

  // Parse a single edit block from code block content
  // The content should contain markers: <<<<<<< SEARCH, =======, >>>>>>> REPLACE
  // Returns std::nullopt if parsing fails
  std::optional<file_edit_t> parse_edit_block(
    const std::string& file_path,
    const std::string& code_block_content
  );

  // Parse AI response containing edit blocks and patchset ID
  // Returns pair of (patchset_id, vector<file_edit_t>) or std::nullopt if parsing fails
  std::optional<std::pair<std::string, std::vector<file_edit_t>>>
    parse_edits_with_patchset(const std::string& input
  );
} // namespace mistral::agent

#endif // MISTRAL_PARSER_H
