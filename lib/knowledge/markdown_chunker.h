#ifndef MISTRAL_MARKDOWN_CHUNKER_H
#define MISTRAL_MARKDOWN_CHUNKER_H

#include "knowledge/knowledge_types.h"

#include <string>
#include <vector>

namespace mistral::knowledge
{
  // Splits markdown documents into embeddable chunks. Chunks follow the
  // document's ATX header structure: every chunk carries a breadcrumb
  // of its enclosing headers ("Guide > Install > Linux"), and oversized
  // sections are split at paragraph boundaries (blank lines are dropped
  // from the chunk text; line ranges still cover the full span). Fenced
  // code blocks are respected: '#' lines inside fences are not treated
  // as headers. A leading YAML frontmatter block is skipped.
  class markdown_chunker
  {
  public:
    explicit markdown_chunker(int max_chars = 1500);

    // Split source into chunks. The returned chunks have 1-indexed
    // inclusive line ranges into source.
    std::vector<doc_chunk> chunk(const std::string& source) const;

  private:
    int max_chars_;
  };

} // namespace mistral::knowledge

#endif // MISTRAL_MARKDOWN_CHUNKER_H
