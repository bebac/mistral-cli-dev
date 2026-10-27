#ifndef MISTRAL_EDIT_PARSER_H
#define MISTRAL_EDIT_PARSER_H

#include "tools/edit_types.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mistral::tools
{
  // Extract patchset ID from AI response containing "(patchset: <id>)".
  // Scans all "(patchset: " mentions and returns the first valid ID;
  // invalid mentions (e.g. "(patchset: `id`)" backticked in prose) are
  // skipped. Returns std::nullopt if no valid ID is found.
  std::optional<std::string> extract_patchset_id(std::string_view input);

  // Parse a single edit block from code block content
  // The content should contain markers: <<<<<<< SEARCH, =======, >>>>>>> REPLACE
  // Returns std::nullopt if parsing fails
  std::optional<file_edit_t> parse_edit_block(std::string_view file_path, std::string_view code_block_content);

  // Parse edit blocks from text without requiring a patchset ID marker.
  // Returns parsed edits or std::nullopt if no edit blocks are found.
  std::optional<std::vector<file_edit_t>> parse_edit_blocks(std::string_view input);

  // Parse AI response containing edit blocks and patchset ID
  // Returns pair of (patchset_id, vector<file_edit_t>) or std::nullopt if parsing fails
  std::optional<std::pair<std::string, std::vector<file_edit_t>>> parse_edits_with_patchset(std::string_view input);
} // namespace mistral::tools

#endif // MISTRAL_PARSER_H
