#ifndef MISTRAL_KNOWLEDGE_STORE_H
#define MISTRAL_KNOWLEDGE_STORE_H

#include "knowledge/embedder.h"
#include "knowledge/knowledge_types.h"

#include <dripcore/awaitable.h>

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

struct sqlite3;

namespace mistral::knowledge
{
  // Persistent semantic search store for workspace documentation (and,
  // later, memory and external corpora). One SQLite database holds a
  // regular `chunks` metadata table plus a sqlite-vec `chunks_vec`
  // virtual table with the embeddings; corpus is a vec0 partition key,
  // so KNN queries filter by corpus natively.
  //
  // The store is single-process: one connection, used from the
  // scheduler thread. refresh_corpus() is incremental — files whose
  // content hash is unchanged are skipped.
  class knowledge_store
  {
  public:
    // The embedder is borrowed, not owned: it must outlive the store.
    // Its dim() fixes the vec0 column width and its identity() decides
    // when the stored index must be rebuilt.
    knowledge_store(knowledge_settings settings, embedder& emb);
    ~knowledge_store();

    knowledge_store(const knowledge_store&) = delete;
    knowledge_store& operator=(const knowledge_store&) = delete;

    // Open (creating if needed) the database at settings_.db_path and
    // ensure the schema. A dimension or schema-version mismatch wipes
    // the index (it is rebuilt on the next refresh). Returns false and
    // sets error on failure.
    bool open(std::string& error);

    // Open an existing database without the schema or embedder check,
    // for callers that only read what is already indexed. Never creates,
    // never drops, never rebuilds: a missing file or a mismatched schema
    // simply means the reads find nothing.
    //
    // Exists because the check in open() is destructive, and some callers
    // run in contexts where destroying the index would be a surprise —
    // building a system prompt, for one.
    bool open_read_only(std::string& error);

    bool is_open() const
    {
      return db_ != nullptr;
    }

    int embedding_dim() const
    {
      return embedder_.dim();
    }

    // Reports progress of refresh_corpus() while it runs, so a frontend
    // can show a counter instead of going quiet for the whole build.
    // Called on the scheduler thread from inside the refresh, so the
    // callback must be cheap and must not call back into the store.
    //
    // The store never reports index_phase::idle: it cannot tell whether
    // the caller will refresh again (/index loops until nothing is
    // pending), so clearing the indicator is the caller's job.
    using progress_fn = std::function<void(const index_progress&)>;

    void set_progress_callback(progress_fn fn)
    {
      progress_ = std::move(fn);
    }

    // Incrementally index corpus from root_dir: new and changed files
    // are (re)chunked and embedded, vanished files are dropped.
    // Yields to the scheduler periodically.
    //
    // kind selects both the extension list and the chunker: documents
    // use markdown_chunker over include_extensions, code uses
    // code_chunker over code_extensions.
    //
    // corpus and root_dir are taken by value on purpose: a coroutine's
    // reference parameters dangle when callers pass temporaries (the
    // body runs at the first co_await, after the temporary is gone).
    dripcore::awaitable<refresh_stats> refresh_corpus(
      std::string corpus,
      std::string root_dir,
      std::string& error,
      corpus_kind kind = corpus_kind::documents
    );

    // Semantic search over indexed chunks. An empty corpus searches
    // all corpora; an empty source searches all repositories. k <= 0
    // uses the configured default; k is clamped to [1, 100].
    //
    // query, corpus and source are taken by value for the same reason as
    // refresh_corpus's parameters.
    dripcore::awaitable<std::vector<search_hit>> search(
      std::string query,
      std::string corpus,
      std::string source,
      int k,
      std::string& error
    );

    // Chunk count per corpus.
    std::map<std::string, int> corpus_stats(std::string& error) const;

    // Recompute chunk breadcrumbs for an already-indexed corpus without
    // re-embedding. Breadcrumbs are derived data and are never sent to
    // the embedder, so a breadcrumb-only change needs no vectors. Rows
    // are matched by id in insertion order; a file whose chunk count no
    // longer matches its stored rows is skipped and needs a re-index.
    dripcore::awaitable<rechunk_stats> rechunk_breadcrumbs(
      std::string corpus,
      std::string root_dir,
      corpus_kind kind,
      std::string& error
    );

    // Chunk count per source (repository) within one corpus. The empty
    // key is the workspace's own repo.
    //
    // Scoped to a corpus on purpose: the same source name appears in both
    // corpora, so counting across them inflates every entry with
    // documentation chunks and makes search_code report a repository as
    // larger than the code it holds.
    std::map<std::string, int> source_stats(const std::string& corpus, std::string& error) const;

  private:
    bool open_impl(bool read_only, std::string& error);
    bool ensure_schema(std::string& error);

    // Embed every input, forming requests across file boundaries and
    // keeping several in flight at once. Returns one vector per input,
    // in order. On failure returns false and sets error; vectors is
    // then untouched.
    dripcore::awaitable<bool> embed_inputs(
      std::vector<std::string> inputs,
      std::vector<std::vector<float>>& vectors,
      std::string& error
    );

    // Write one file's chunks and vectors in a single transaction.
    // Serial by design: one SQLite connection cannot run concurrent
    // transactions, and the writes are local and fast next to the
    // embedding they follow. vectors holds every pending file's vectors
    // back to back; vector_offset is where this file's begin.
    dripcore::awaitable<bool> write_file(
      const std::string& corpus,
      const std::string& rel_path,
      const std::string& source,
      const std::vector<doc_chunk>& chunks,
      const std::vector<std::vector<float>>& vectors,
      std::size_t vector_offset,
      const std::string& content_hash,
      refresh_stats& stats,
      std::string& error
    );
    bool remove_file(const std::string& corpus, const std::string& rel_path, refresh_stats& stats, std::string& error);
    bool is_excluded(const std::filesystem::path& path) const;
    bool has_extension(const std::filesystem::path& path, corpus_kind kind) const;

    // Report one progress step; a no-op when no callback is set.
    void report(index_phase phase, int done, int total) const;

    knowledge_settings settings_;
    embedder& embedder_;
    sqlite3* db_ = nullptr;
    progress_fn progress_;
    // Size of the tree the current refresh is working on, filled in once
    // the walk finishes and reported with every phase. Zero outside a
    // refresh.
    int files_total_ = 0;
  };

} // namespace mistral::knowledge

#endif // MISTRAL_KNOWLEDGE_STORE_H
