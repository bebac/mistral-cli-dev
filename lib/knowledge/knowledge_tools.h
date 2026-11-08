#ifndef MISTRAL_KNOWLEDGE_TOOLS_H
#define MISTRAL_KNOWLEDGE_TOOLS_H

#include "knowledge/knowledge_store.h"
#include "knowledge/knowledge_types.h"

#include <dripcore/awaitable.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>

namespace mistral::knowledge
{
  using json = nlohmann::ordered_json;

  // Agent-facing tools over the knowledge store. The workspace
  // documentation corpus ("workspace-docs") is indexed from the
  // workspace root; the index refreshes incrementally before every
  // search, so results reflect the current files.
  class knowledge_tools
  {
  public:
    // The embedder is owned by the tools and lent to the store. A null
    // embedder falls back to the offline hashing embedder.
    knowledge_tools(knowledge_settings settings, std::string workspace_root, std::unique_ptr<embedder> emb);

    // Semantic search over indexed documentation. Returns ranked
    // chunks with path, line range, breadcrumb, score and snippet, plus
    // the repositories present in the documentation corpus so `source`
    // can be discovered rather than guessed.
    //
    // source restricts to one repository and empty searches all of them,
    // matching search_code. The corpus is fixed to the documentation
    // corpus: the tool name already selects it, and exposing it here let
    // callers reach the code corpus through the documentation tool.
    dripcore::awaitable<json> search_docs(std::string query, std::string source, int k);

    // Semantic search over indexed source code. source restricts to one
    // repository — a submodule or vendored checkout, as reported in the
    // result's `sources` list — and empty searches all of them.
    //
    // For exact identifier or string matching use grep instead: semantic
    // search is weak at that, and grep is exact and cheap.
    dripcore::awaitable<json> search_code(std::string query, std::string source, int k);

    // Re-index one workspace corpus. Incremental: only new or changed
    // files are re-embedded. Returns indexing stats.
    dripcore::awaitable<json> refresh(corpus_kind kind);

    // Re-index the workspace documentation corpus. Incremental: only
    // new or changed files are re-embedded. Returns indexing stats.
    dripcore::awaitable<json> refresh_docs();

    // Recompute breadcrumbs on both corpora without re-embedding.
    // Breadcrumbs are derived data and never reach the embedder, so this
    // is local work only.
    dripcore::awaitable<json> rechunk_breadcrumbs();

    // A short block naming the repositories in the index, for injection
    // into the system prompt. Empty when neither corpus has an index yet.
    //
    // The point is discovery: the search tools report the repository
    // names only in their own results, so without this the model has to
    // spend a call to learn what `source` accepts. The two corpora hold
    // different repositories, so a repository that only one of them
    // holds is called out under the tool that cannot reach it — an
    // unqualified single list would advertise names that return nothing.
    //
    // Reads the database through a throwaway read-only store, so it
    // cannot disturb the index the rest of the session is using.
    std::string index_summary();

    // Reports refresh progress while a corpus is being indexed. Called on
    // the scheduler thread, from inside the refresh.
    using progress_fn = std::function<void(const index_progress&)>;

    void set_progress_callback(progress_fn fn);

    // Bracket a multi-round indexing operation (/index). While a session
    // is open the indicator is not cleared between refresh_corpus()
    // calls, so it stays up for the whole build instead of blinking off
    // once per round.
    void begin_indexing();
    void end_indexing();

  private:
    // Open the store (once), resolving a relative db_path against the
    // workspace root.
    bool open_store(std::string& error);

    // Settings with db_path resolved against the workspace root.
    knowledge_settings resolved_settings() const;

    // Open the store (once) and refresh one workspace corpus. Returns
    // false on failure; stats is filled even for a partial refresh.
    dripcore::awaitable<bool> ensure_ready(corpus_kind kind, refresh_stats& stats, std::string& error);

    // Clear the progress indicator. The store reports phases but never
    // idle, so every refresh call site ends with this.
    void report_idle();

    knowledge_settings settings_;
    std::string workspace_root_;
    std::unique_ptr<embedder> embedder_;
    std::unique_ptr<knowledge_store> store_;
    progress_fn progress_;
    bool indexing_session_ = false;
  };

} // namespace mistral::knowledge

#endif // MISTRAL_KNOWLEDGE_TOOLS_H
