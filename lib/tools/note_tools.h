#ifndef MISTRAL_NOTE_TOOLS_H
#define MISTRAL_NOTE_TOOLS_H

#include "template/template_engine.h"
#include <dripcore/awaitable.h>

#include <nlohmann/json.hpp>

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace mistral::tools
{
  using json = nlohmann::ordered_json;

  // ---- Frontmatter ----

  struct note_frontmatter
  {
    std::string title;
    std::vector<std::string> tags;
  };

  // Parse YAML-like frontmatter from a note's content.
  // Returns the frontmatter and the body (content after the closing ---).
  // If no frontmatter is present, returns an empty frontmatter and the full content.
  struct parsed_note
  {
    note_frontmatter front;
    std::string body;
  };

  parsed_note parse_frontmatter(const std::string& content);

  // Collect all unique tags from all notes and write to a file.
  // Returns the path to the generated tags file, or empty string on error.
  std::string generate_note_tags_file(const std::string& notes_dir, const std::string& base_dir);

  // ---- Note tools ----

  class note_tools
  {
  public:
    explicit note_tools(std::string notes_dir);

    // List notes in the notes directory, optionally within a subfolder.
    dripcore::awaitable<std::string> list_notes(std::string folder);

    // Read a note with {{> }} includes resolved via the template engine.
    // Returns the rendered content plus frontmatter metadata.
    dripcore::awaitable<json> read_note(std::string path);

    // Create or update a note. Content is written as-is (raw, with literal
    // include directives preserved). Creates parent directories if needed.
    dripcore::awaitable<json> write_note(std::string path, std::string content);

    // Search notes by text query and/or tag. At least one must be non-empty.
    // Returns matching notes with path, title, matching lines (for text), and tags.
    dripcore::awaitable<json> search_notes(std::string query, std::string tag);

  private:
    // Resolve a note path relative to the notes directory.
    std::filesystem::path resolve_path(const std::string& path) const;

    std::string notes_dir_;
    tmpl::template_engine engine_;
  };

} // namespace mistral::tools

#endif // MISTRAL_NOTE_TOOLS_H
