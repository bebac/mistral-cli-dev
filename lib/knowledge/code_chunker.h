#ifndef MISTRAL_KNOWLEDGE_CODE_CHUNKER_H
#define MISTRAL_KNOWLEDGE_CODE_CHUNKER_H

#include "knowledge/knowledge_types.h"

#include <string>
#include <vector>

namespace mistral::knowledge
{
  // Splits source code into overlapping line windows for embedding.
  //
  // No parsing. A window is a run of lines snapped to the nearest blank
  // line, carrying a breadcrumb of the enclosing declaration path
  // (`namespace foo > struct bar`) built from brace depth. That is enough
  // for a hit to be recognisable as belonging to a function, class or
  // namespace without a grammar per language.
  //
  // Declaration-aligned chunks — reusing parsed_source::definitions()
  // extents when MISTRAL_ENABLE_TREE_SITTER is on — are the planned
  // upgrade; see doc/code-search-plan.md §7.
  class code_chunker
  {
  public:
    // max_lines: target window height.
    // overlap_lines: how far consecutive windows overlap.
    // max_chars: character budget for one window. A single over-long
    //   line is emitted whole, because splitting it here would need
    //   sub-line bookkeeping; knowledge_store::enforce_chunk_budget()
    //   splits any resulting oversize chunk before embedding.
    explicit code_chunker(int max_lines = 80, int overlap_lines = 20, int max_chars = 3000);

    std::vector<doc_chunk> chunk(const std::string& content) const;

  private:
    int max_lines_;
    int overlap_lines_;
    int max_chars_;
  };

} // namespace mistral::knowledge

#endif // MISTRAL_KNOWLEDGE_CODE_CHUNKER_H