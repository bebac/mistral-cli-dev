#include "knowledge/knowledge_store.h"

#include "common/string_utils.h"
#include "knowledge/code_chunker.h"
#include "knowledge/markdown_chunker.h"

#include <dripcore/channel.h>
#include <dripcore/executor.h>
#include <dripcore/sleep_awaiter.h>
#include <dripcore/task.h>

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <cstdint>
#include <iterator>
#include <memory>
#include <set>
#include <string_view>
#include <utility>

namespace fs = std::filesystem;

// sqlite-vec entry point (declared here rather than via <sqlite-vec.h>:
// without SQLITE_CORE that header pulls in <sqlite3ext.h>, which
// macro-rewrites every sqlite3_* call in this file). The amalgamation
// is compiled with SQLITE_CORE, so the symbol links directly.
struct sqlite3_api_routines;
extern "C" int sqlite3_vec_init(sqlite3* db, char** pzErrMsg, const sqlite3_api_routines* pApi);

namespace mistral::knowledge
{
  namespace
  {
    // Bumped to 4 when the workspace's own repository started being
    // stored as "workspace" rather than "". `source` is a vec0 partition
    // key, so the old rows cannot be updated in place; the version bump
    // drops and rebuilds the index instead.
    constexpr int k_schema_version = 4;

    // Source name for the workspace's own repository. Explicit rather
    // than empty, so that an empty `source` argument can mean "every
    // repository" in knowledge_store::search().
    constexpr const char* k_workspace_source = "workspace";

    // Bumped whenever chunking changes shape: stored vectors then
    // describe different text, so the index must be rebuilt. Part of the
    // index identity in ensure_schema().
    constexpr int k_chunker_version = 1;

    // How often the collector re-checks the results channel while
    // embedding requests are in flight. Small enough to be invisible
    // next to a network round trip, large enough not to busy-loop.
    constexpr auto k_embed_poll_interval = std::chrono::milliseconds(10);

    // Backstop for the whole embedding phase. Each request already has
    // its own deadline, so this only fires if a worker dies without
    // reporting, which would otherwise hang the refresh forever.
    constexpr auto k_embed_deadline = std::chrono::minutes(10);

    // Files walked between chunking-phase progress reports. A cold build
    // walks thousands of files and every report costs the frontend a
    // status snapshot, so this is deliberately coarse.
    constexpr std::size_t k_progress_every_files = 32;

    // One embedding phase's worth of work. Held by the store and by
    // every worker task, so it outlives an early return from the
    // collector: workers still running after a timeout keep a valid job
    // instead of writing into a destroyed frame.
    struct embed_job
    {
      std::vector<std::string> inputs;
      // [begin, end) ranges into inputs, one per request.
      std::vector<std::pair<std::size_t, std::size_t>> groups;
      // One vector per input, filled in as requests complete.
      std::vector<std::vector<float>> vectors;
      std::atomic<std::size_t> next{0};
    };
    using embed_job_ptr = std::shared_ptr<embed_job>;

    struct embed_outcome
    {
      std::size_t group = 0;
      std::vector<std::vector<float>> vectors;
      std::string error;
    };

    // Runs one request per turn until the job's groups are exhausted.
    // Several of these are spawned at once so the process is not idle
    // for the whole round trip; the shared counter hands out work, so a
    // slow request does not stall the others.
    class embed_worker_task : public dripcore::task
    {
    public:
      embed_worker_task(
        embedder& emb,
        embed_job_ptr job,
        std::shared_ptr<dripcore::channel<embed_outcome>> out
      )
        : embedder_(emb), job_(std::move(job)), out_(std::move(out))
      {
      }

    protected:
      dripcore::awaitable<> body() override
      {
        while (true)
        {
          const std::size_t index = job_->next.fetch_add(1);
          if (index >= job_->groups.size())
          {
            co_return;
          }

          const auto [begin, end] = job_->groups[index];
          std::vector<std::string> slice(
            job_->inputs.begin() + static_cast<std::ptrdiff_t>(begin),
            job_->inputs.begin() + static_cast<std::ptrdiff_t>(end)
          );

          embed_outcome outcome;
          outcome.group = index;
          outcome.vectors = co_await embedder_.embed_batch(std::move(slice), outcome.error);
          out_->try_send(std::move(outcome));
        }
      }

    private:
      embedder& embedder_;
      embed_job_ptr job_;
      std::shared_ptr<dripcore::channel<embed_outcome>> out_;
    };

    // RAII wrapper around sqlite3_stmt.
    class statement
    {
    public:
      statement() = default;
      ~statement()
      {
        reset();
      }

      statement(const statement&) = delete;
      statement& operator=(const statement&) = delete;

      bool prepare(sqlite3* db, const std::string& sql, std::string& error)
      {
        reset();
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK)
        {
          error = sqlite3_errmsg(db);
          return false;
        }
        return true;
      }

      sqlite3_stmt* get() const
      {
        return stmt_;
      }

      void reset()
      {
        if (stmt_)
        {
          sqlite3_finalize(stmt_);
          stmt_ = nullptr;
        }
      }

    private:
      sqlite3_stmt* stmt_ = nullptr;
    };

    bool exec(sqlite3* db, const std::string& sql, std::string& error)
    {
      char* err = nullptr;
      if (sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK)
      {
        error = err ? err : "unknown sqlite error";
        sqlite3_free(err);
        return false;
      }
      return true;
    }

    std::string column_text(sqlite3_stmt* stmt, int column)
    {
      if (const unsigned char* text = sqlite3_column_text(stmt, column))
      {
        return reinterpret_cast<const char*>(text);
      }
      return {};
    }

    std::string read_file(const fs::path& path, bool& ok)
    {
      std::ifstream f(path, std::ios::in | std::ios::binary);
      if (!f)
      {
        ok = false;
        return {};
      }
      ok = true;
      return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }

    // Bind a float32 vector as a BLOB (vec0's compact binary format).
    // Text (JSON) vectors are avoided on purpose: vec0 parses them with
    // strtod, which is locale-dependent — under a comma-decimal locale
    // (e.g. da_DK, activated process-wide when isocline calls
    // setlocale(LC_ALL, "")) "0,0" parses as a single number and the
    // dimension count is corrupted.
    void bind_vector(sqlite3_stmt* stmt, int index, const std::vector<float>& vec)
    {
      sqlite3_bind_blob(
        stmt,
        index,
        vec.data(),
        static_cast<int>(vec.size() * sizeof(float)),
        SQLITE_TRANSIENT
      );
    }

    // No chunker is trusted to respect its own budget. A single line can
    // be arbitrarily long — a minified bundle in this repo has a
    // 79k-character line — and the embeddings endpoint rejects inputs
    // over 8192 tokens, which aborts the whole refresh. Split anything
    // over the ceiling; the pieces keep the original line range so a hit
    // still points at the right place.
    std::vector<doc_chunk> enforce_chunk_budget(std::vector<doc_chunk> chunks, int max_chars)
    {
      if (max_chars <= 0)
      {
        return chunks;
      }
      const std::size_t budget = static_cast<std::size_t>(max_chars);
      std::vector<doc_chunk> out;
      out.reserve(chunks.size());
      for (auto& chunk : chunks)
      {
        if (chunk.text.size() <= budget)
        {
          out.push_back(std::move(chunk));
          continue;
        }
        for (std::size_t offset = 0; offset < chunk.text.size(); offset += budget)
        {
          doc_chunk piece;
          piece.breadcrumb = chunk.breadcrumb;
          piece.line_start = chunk.line_start;
          piece.line_end = chunk.line_end;
          piece.text = chunk.text.substr(offset, budget);
          out.push_back(std::move(piece));
        }
      }
      return out;
    }

    // True when `name` is one of the configured vendor directory names.
    bool is_vendor_dir_name(const std::string& name, const std::vector<std::string>& vendor_dirs)
    {
      return std::find(vendor_dirs.begin(), vendor_dirs.end(), name) != vendor_dirs.end();
    }

    // Longest source root containing `rel`, or the workspace's own
    // repository name when no root matches. Roots are workspace-relative
    // directory paths.
    std::string source_for(const std::string& rel, const std::vector<std::string>& roots)
    {
      std::string best;
      for (const auto& root : roots)
      {
        if (
          root.size() > best.size() && rel.size() > root.size() && rel.compare(0, root.size(), root) == 0 &&
          rel[root.size()] == '/'
        )
        {
          best = root;
        }
      }
      return best.empty() ? std::string(k_workspace_source) : best;
    }

    std::string utc_now_iso()
    {
      const std::time_t now = std::time(nullptr);
      std::tm tm{};
      gmtime_r(&now, &tm);
      char buf[32];
      std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
      return buf;
    }
  } // namespace

  knowledge_store::knowledge_store(knowledge_settings settings, embedder& emb)
    : settings_(std::move(settings)), embedder_(emb)
  {
  }

  knowledge_store::~knowledge_store()
  {
    if (db_)
    {
      sqlite3_close(db_);
      db_ = nullptr;
    }
  }

  bool knowledge_store::open(std::string& error)
  {
    return open_impl(false, error);
  }

  bool knowledge_store::open_read_only(std::string& error)
  {
    return open_impl(true, error);
  }

  bool knowledge_store::open_impl(bool read_only, std::string& error)
  {
    if (db_)
    {
      return true;
    }

    std::error_code ec;
    const fs::path db_path = fs::path(settings_.db_path);
    if (!read_only && db_path.has_parent_path())
    {
      fs::create_directories(db_path.parent_path(), ec);
      // A failure here surfaces as a sqlite3_open error below.
    }

    // A read-only open must not create the file: the caller wants to know
    // whether an index exists, not to bring an empty one into being.
    const int flags = read_only
      ? (SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX)
      : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX);
    if (sqlite3_open_v2(settings_.db_path.c_str(), &db_, flags, nullptr) != SQLITE_OK)
    {
      error = db_ ? sqlite3_errmsg(db_) : "cannot allocate sqlite handle";
      if (db_)
      {
        sqlite3_close(db_);
        db_ = nullptr;
      }
      return false;
    }

    sqlite3_busy_timeout(db_, 5000);

    // Register the vec0 module on this connection. The amalgamation is
    // compiled with SQLITE_CORE, so the entry point is called directly
    // (no loadable-extension machinery).
    {
      char* vec_error = nullptr;
      if (sqlite3_vec_init(db_, &vec_error, nullptr) != SQLITE_OK)
      {
        error = vec_error ? vec_error : "sqlite3_vec_init failed";
        sqlite3_free(vec_error);
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
      }
    }

    if (read_only)
    {
      // No schema check: an index built by a different version or
      // embedder is left exactly as it is, and the caller's reads decide
      // for themselves whether what they find is usable.
      return true;
    }

    return ensure_schema(error);
  }

  bool knowledge_store::ensure_schema(std::string& error)
  {
    if (!exec(db_, "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value TEXT NOT NULL)", error))
    {
      return false;
    }
    if (!exec(
          db_,
          "CREATE TABLE IF NOT EXISTS files("
          "corpus TEXT NOT NULL, path TEXT NOT NULL, content_hash TEXT NOT NULL,"
          " indexed_at TEXT NOT NULL, PRIMARY KEY (corpus, path))",
          error
        ))
    {
      return false;
    }
    auto meta_get = [&](const char* key) {
      std::string local_error;
      statement stmt;
      std::string value;
      if (stmt.prepare(db_, "SELECT value FROM meta WHERE key = ?", local_error))
      {
        sqlite3_bind_text(stmt.get(), 1, key, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt.get()) == SQLITE_ROW)
        {
          value = column_text(stmt.get(), 0);
        }
      }
      return value;
    };

    // The embedding dimension is fixed per vec0 table, and the identity
    // covers everything else that changes the meaning of a stored
    // vector: the embedder's model, dimension and dtype, plus the
    // chunker version. When any of them differs from what the index was
    // built with, wipe it; the next refresh rebuilds. Without this,
    // switching provider silently mixes vector spaces and returns
    // plausible-looking garbage.
    const std::string want_dim = std::to_string(embedder_.dim());
    const std::string want_identity = embedder_.identity() + "|chunker=" + std::to_string(k_chunker_version);

    const std::string stored_version = meta_get("schema_version");
    const std::string stored_dim = meta_get("embedding_dim");
    const std::string stored_identity = meta_get("embedder_identity");

    if (
      !stored_version.empty() &&
      (stored_version != std::to_string(k_schema_version) || stored_dim != want_dim || stored_identity != want_identity)
    )
    {
      if (!exec(db_, "DROP TABLE IF EXISTS chunks_vec", error))
      {
        return false;
      }
      // Dropped rather than emptied: the column set changes between
      // schema versions, and CREATE TABLE IF NOT EXISTS would otherwise
      // leave an old table in place.
      if (!exec(db_, "DROP TABLE IF EXISTS chunks", error))
      {
        return false;
      }
      if (!exec(db_, "DELETE FROM files", error))
      {
        return false;
      }
    }

    // Created after the wipe above, not before it: a stale database
    // drops chunks to pick up the current column set, so the table has
    // to be (re)created here. Creating it earlier would leave it missing
    // after the drop.
    if (!exec(
          db_,
          "CREATE TABLE IF NOT EXISTS chunks("
          "id INTEGER PRIMARY KEY, corpus TEXT NOT NULL, path TEXT NOT NULL,"
          " source TEXT NOT NULL DEFAULT '',"
          " breadcrumb TEXT NOT NULL DEFAULT '', line_start INTEGER NOT NULL,"
          " line_end INTEGER NOT NULL, text TEXT NOT NULL)",
          error
        ))
    {
      return false;
    }
    if (!exec(db_, "CREATE INDEX IF NOT EXISTS chunks_corpus_path ON chunks(corpus, path)", error))
    {
      return false;
    }

    // corpus and source are both vec0 partition keys: KNN queries filter
    // on them natively, so a per-repository search stays exact instead of
    // over-fetching and post-filtering. Partition keys also keep full
    // text fidelity, unlike the 12-byte text metadata columns.
    if (!exec(
          db_,
          "CREATE VIRTUAL TABLE IF NOT EXISTS chunks_vec USING vec0("
          "embedding float[" +
            want_dim +
            "] distance_metric=cosine,"
            " corpus TEXT partition key,"
            " source TEXT partition key)",
          error
        ))
    {
      return false;
    }

    statement upsert;
    if (!upsert.prepare(
          db_,
          "INSERT INTO meta(key, value) VALUES (?, ?) "
          "ON CONFLICT(key) DO UPDATE SET value = excluded.value",
          error
        ))
    {
      return false;
    }
    auto put_meta = [&](const char* key, const std::string& value) {
      sqlite3_reset(upsert.get());
      sqlite3_bind_text(upsert.get(), 1, key, -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(upsert.get(), 2, value.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_step(upsert.get());
    };
    put_meta("schema_version", std::to_string(k_schema_version));
    put_meta("embedding_dim", want_dim);
    put_meta("embedder_identity", want_identity);
    return true;
  }

  void knowledge_store::report(index_phase phase, int done, int total) const
  {
    if (progress_)
    {
      progress_(index_progress{phase, done, total, files_total_});
    }
  }

  dripcore::awaitable<refresh_stats> knowledge_store::refresh_corpus(
    std::string corpus,
    std::string root_dir,
    std::string& error,
    corpus_kind kind
  )
  {
    refresh_stats stats;
    files_total_ = 0;
    if (!db_)
    {
      error = "knowledge store is not open";
      co_return stats;
    }

    std::error_code ec;
    const fs::path root = fs::absolute(fs::path(root_dir), ec).lexically_normal();
    if (ec || !fs::is_directory(root, ec))
    {
      error = "index root is not a directory: " + root_dir;
      co_return stats;
    }

    // Collect candidate files.
    std::vector<fs::path> files;
    {
      fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
      const fs::recursive_directory_iterator end;
      while (!ec && it != end)
      {
        std::error_code entry_ec;
        if (it->is_regular_file(entry_ec) && !is_excluded(it->path()) && has_extension(it->path(), kind))
        {
          // Skip anomalous files. A generated parser table or a bundled
          // asset can be tens of megabytes and would dominate the corpus
          // without being useful to search.
          std::error_code size_ec;
          const std::uintmax_t size = fs::file_size(it->path(), size_ec);
          if (
            !size_ec && settings_.max_file_bytes > 0 &&
            size > static_cast<std::uintmax_t>(settings_.max_file_bytes)
          )
          {
            ++stats.files_too_large;
          }
          else
          {
            files.push_back(it->path());
          }
        }
        it.increment(ec);
      }
    }
    std::sort(files.begin(), files.end());
    stats.files_seen = static_cast<int>(files.size());
    // The tree's size, known from here on and constant for the rest of
    // the refresh. Every later phase reports it as context.
    files_total_ = stats.files_seen;

    // Index new and changed files, up to the per-refresh input budget.
    // With a network embedder an unbounded cold build would block the
    // turn for minutes, and search_docs refreshes before every query.
    // Search over a partial index is correct, just incomplete, and the
    // next refresh picks up where this one stopped.
    std::set<std::string> seen;
    int since_yield = 0;
    int inputs_used = 0;
    bool budget_exhausted = false;
    markdown_chunker doc_chunker(settings_.chunk_max_chars);
    code_chunker src_chunker(
      settings_.code_chunk_max_lines, settings_.code_chunk_overlap_lines, settings_.code_chunk_max_chars
    );

    // Repository boundaries. A directory holding a .git entry is a
    // submodule (a .git file) or a vendored checkout (a .git directory);
    // a direct child of a vendor directory counts too, so a plain copy
    // with no .git of its own is still separable. Files outside all of
    // them belong to the workspace's own repo, source "".
    std::vector<std::string> source_roots;
    {
      std::error_code roots_ec;
      fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, roots_ec);
      const fs::recursive_directory_iterator roots_end;
      while (!roots_ec && it != roots_end)
      {
        std::error_code entry_ec;
        if (it->is_directory(entry_ec))
        {
          const fs::path& dir = it->path();
          if (dir != root && !is_excluded(dir))
          {
            const bool has_git = fs::exists(dir / ".git", entry_ec);
            const bool under_vendor =
              is_vendor_dir_name(dir.parent_path().filename().string(), settings_.vendor_dirs);
            if (has_git || under_vendor)
            {
              std::error_code rel_ec;
              const std::string dir_rel = fs::relative(dir, root, rel_ec).lexically_normal().generic_string();
              if (!rel_ec && !dir_rel.empty())
              {
                source_roots.push_back(dir_rel);
              }
            }
          }
        }
        it.increment(roots_ec);
      }
    }

    // Phase 1 — read, hash and chunk. All local, so it is cheap; only
    // embedding costs anything and the budget is measured in chunks.
    struct pending_file
    {
      std::string rel;
      std::string source;
      std::string hash;
      std::vector<doc_chunk> chunks;
    };
    std::vector<pending_file> pending;
    std::size_t pending_chunks = 0;

    for (std::size_t i = 0; i < files.size(); ++i)
    {
      const auto& path = files[i];
      const std::string rel = fs::relative(path, root, ec).lexically_normal().generic_string();
      if (ec)
      {
        ec.clear();
        continue;
      }
      seen.insert(rel);
      const std::string source = source_for(rel, source_roots);

      bool read_ok = false;
      const std::string content = read_file(path, read_ok);
      if (!read_ok)
      {
        continue;
      }
      const std::string hash = utils::fnv1a_64_hex(content);

      statement lookup;
      if (!lookup.prepare(db_, "SELECT content_hash FROM files WHERE corpus = ? AND path = ?", error))
      {
        co_return stats;
      }
      sqlite3_bind_text(lookup.get(), 1, corpus.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(lookup.get(), 2, rel.c_str(), -1, SQLITE_TRANSIENT);
      std::string stored_hash;
      if (sqlite3_step(lookup.get()) == SQLITE_ROW)
      {
        stored_hash = column_text(lookup.get(), 0);
      }

      if (stored_hash == hash)
      {
        ++stats.files_unchanged;
        continue;
      }

      std::vector<doc_chunk> chunks = enforce_chunk_budget(
        kind == corpus_kind::code ? src_chunker.chunk(content) : doc_chunker.chunk(content),
        settings_.max_chunk_chars
      );
      const int needed = static_cast<int>(chunks.size());

      // Always index the first changed file, even when it alone exceeds
      // the budget: otherwise one large file would block the queue
      // forever and the index would never make progress.
      if (budget_exhausted || (inputs_used > 0 && inputs_used + needed > settings_.max_inputs_per_refresh))
      {
        budget_exhausted = true;
        ++stats.files_pending;
        continue;
      }

      inputs_used += needed;
      pending_chunks += chunks.size();
      pending.push_back(pending_file{rel, source, hash, std::move(chunks)});

      if (++since_yield % 8 == 0)
      {
        // Yield to the scheduler; a cold index build can take a while
        // on large documentation trees.
        co_await dripcore::sleep_awaiter{dripcore::executor::current(), nullptr, std::chrono::milliseconds(0)};
      }

      // Only reached for a file that was queued: every skip path above
      // continues past this point. That is what keeps a refresh with
      // nothing to do silent — search_docs refreshes before every query,
      // so a report on every search would flash the indicator even when
      // the index is already current.
      if ((i + 1) % k_progress_every_files == 0 || i + 1 == files.size())
      {
        report(index_phase::chunking, static_cast<int>(i + 1), stats.files_seen);
      }
    }

    if (!pending.empty())
    {
      // Phase 2 — embed. Inputs are flattened across files so requests
      // are formed by size rather than by file boundary, and several run
      // at once.
      std::vector<std::string> inputs;
      inputs.reserve(pending_chunks);
      for (const auto& file : pending)
      {
        for (const auto& chunk : file.chunks)
        {
          // Embed the breadcrumb together with the text so the vector
          // carries the location context.
          inputs.push_back(chunk.breadcrumb + "\n" + chunk.text);
        }
      }

      // pending_chunks is exact here, and this is the phase that takes
      // the wall-clock time, so the counter is worth having.
      report(index_phase::embedding, 0, static_cast<int>(pending_chunks));

      std::vector<std::vector<float>> vectors;
      if (!co_await embed_inputs(std::move(inputs), vectors, error))
      {
        // Nothing is written, so every stored content hash still
        // matches the old text and the next refresh retries these files.
        co_return stats;
      }

      // Phase 3 — write, one transaction per file, serially.
      report(index_phase::writing, 0, static_cast<int>(pending.size()));
      std::size_t offset = 0;
      int written = 0;
      for (const auto& file : pending)
      {
        if (!co_await write_file(
              corpus, file.rel, file.source, file.chunks, vectors, offset, file.hash, stats, error
            ))
        {
          co_return stats;
        }
        offset += file.chunks.size();
        report(index_phase::writing, ++written, static_cast<int>(pending.size()));
      }
    }

    // Drop indexed files that no longer exist.
    statement known;
    if (!known.prepare(db_, "SELECT path FROM files WHERE corpus = ?", error))
    {
      co_return stats;
    }
    sqlite3_bind_text(known.get(), 1, corpus.c_str(), -1, SQLITE_TRANSIENT);
    std::vector<std::string> known_paths;
    while (sqlite3_step(known.get()) == SQLITE_ROW)
    {
      known_paths.push_back(column_text(known.get(), 0));
    }
    for (const auto& path : known_paths)
    {
      if (!seen.contains(path) && !remove_file(corpus, path, stats, error))
      {
        co_return stats;
      }
    }

    co_return stats;
  }

  dripcore::awaitable<bool> knowledge_store::embed_inputs(
    std::vector<std::string> inputs,
    std::vector<std::vector<float>>& vectors,
    std::string& error
  )
  {
    if (inputs.empty())
    {
      co_return true;
    }

    // Group the inputs into requests that fit the provider's limits.
    // Groups deliberately span file boundaries: batching per file made
    // throughput proportional to the number of files rather than to the
    // size of the corpus, so a one-chunk file cost a whole round trip.
    const int max_count = std::max(1, embedder_.batch_size());
    const int max_chars = embedder_.max_batch_chars();

    auto job = std::make_shared<embed_job>();
    job->inputs = std::move(inputs);
    job->vectors.resize(job->inputs.size());

    std::size_t offset = 0;
    while (offset < job->inputs.size())
    {
      std::size_t count = 0;
      std::size_t chars = 0;
      while (offset + count < job->inputs.size() && count < static_cast<std::size_t>(max_count))
      {
        const std::size_t len = job->inputs[offset + count].size();
        if (count > 0 && max_chars > 0 && chars + len > static_cast<std::size_t>(max_chars))
        {
          break;
        }
        chars += len;
        ++count;
      }
      if (count == 0)
      {
        // A single input over the budget still goes alone; the store's
        // chunk ceiling keeps that from being oversized.
        count = 1;
      }
      job->groups.emplace_back(offset, offset + count);
      offset += count;
    }

    const std::size_t worker_count = std::min<std::size_t>(
      job->groups.size(), static_cast<std::size_t>(std::max(1, settings_.embed_concurrency))
    );

    auto results = std::make_shared<dripcore::channel<embed_outcome>>(job->groups.size() + 1);
    for (std::size_t i = 0; i < worker_count; ++i)
    {
      dripcore::executor::current()->spawn<embed_worker_task>(embedder_, job, results);
    }

    // Collect every outcome, including after a failure: the workers hold
    // a reference to the job, so it must not be dropped while any of
    // them is still running. Draining to completion also keeps the
    // first error rather than whichever happened to arrive first.
    std::string first_error;
    std::size_t received = 0;
    std::size_t inputs_done = 0;
    const auto deadline = std::chrono::steady_clock::now() + k_embed_deadline;
    while (received < job->groups.size())
    {
      if (auto outcome = results->try_recv())
      {
        ++received;
        const auto [begin, end] = job->groups[outcome->group];
        // Counted before the error branches: a failed request is still
        // work that is no longer outstanding.
        inputs_done += end - begin;
        report(index_phase::embedding, static_cast<int>(inputs_done), static_cast<int>(job->inputs.size()));
        if (!outcome->error.empty())
        {
          if (first_error.empty())
          {
            first_error = outcome->error;
          }
          continue;
        }
        if (outcome->vectors.size() != end - begin)
        {
          if (first_error.empty())
          {
            first_error = std::format(
              "embedder returned {} vectors for {} inputs", outcome->vectors.size(), end - begin
            );
          }
          continue;
        }
        for (std::size_t k = 0; k < outcome->vectors.size(); ++k)
        {
          job->vectors[begin + k] = std::move(outcome->vectors[k]);
        }
        continue;
      }

      if (std::chrono::steady_clock::now() >= deadline)
      {
        // Each request has its own deadline, so this only fires if a
        // worker died without reporting. Give up rather than hang; the
        // job stays alive through the workers' own references.
        if (first_error.empty())
        {
          first_error = std::format(
            "embedding timed out with {} of {} requests outstanding", job->groups.size() - received, job->groups.size()
          );
        }
        break;
      }

      co_await dripcore::sleep_awaiter{dripcore::executor::current(), nullptr, k_embed_poll_interval};
    }

    if (!first_error.empty())
    {
      error = first_error;
      co_return false;
    }

    vectors = std::move(job->vectors);
    co_return true;
  }

  dripcore::awaitable<bool> knowledge_store::write_file(
    const std::string& corpus,
    const std::string& rel_path,
    const std::string& source,
    const std::vector<doc_chunk>& chunks,
    const std::vector<std::vector<float>>& vectors,
    std::size_t vector_offset,
    const std::string& content_hash,
    refresh_stats& stats,
    std::string& error
  )
  {
    // Vectors arrive already computed: embedding happens in one
    // concurrent phase across all changed files, outside any
    // transaction, so a network round trip is never held inside
    // BEGIN...COMMIT.
    if (vectors.size() < vector_offset + chunks.size())
    {
      error = std::format(
        "write_file for {}: {} vectors available at offset {}, {} needed",
        rel_path,
        vectors.size(),
        vector_offset,
        chunks.size()
      );
      co_return false;
    }

    // Remove the file's previous chunks.
    statement old_ids;
    if (!old_ids.prepare(db_, "SELECT id FROM chunks WHERE corpus = ? AND path = ?", error))
    {
      co_return false;
    }
    sqlite3_bind_text(old_ids.get(), 1, corpus.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(old_ids.get(), 2, rel_path.c_str(), -1, SQLITE_TRANSIENT);
    std::vector<sqlite3_int64> old_chunk_ids;
    while (sqlite3_step(old_ids.get()) == SQLITE_ROW)
    {
      old_chunk_ids.push_back(sqlite3_column_int64(old_ids.get(), 0));
    }

    if (!exec(db_, "BEGIN", error))
    {
      co_return false;
    }
    auto rollback = [&]() {
      std::string ignored;
      exec(db_, "ROLLBACK", ignored);
    };

    statement del_vec;
    if (!del_vec.prepare(db_, "DELETE FROM chunks_vec WHERE rowid = ?", error))
    {
      rollback();
      co_return false;
    }
    statement del_chunk;
    if (!del_chunk.prepare(db_, "DELETE FROM chunks WHERE id = ?", error))
    {
      rollback();
      co_return false;
    }
    for (const auto id : old_chunk_ids)
    {
      sqlite3_reset(del_vec.get());
      sqlite3_bind_int64(del_vec.get(), 1, id);
      sqlite3_step(del_vec.get());
      sqlite3_reset(del_chunk.get());
      sqlite3_bind_int64(del_chunk.get(), 1, id);
      sqlite3_step(del_chunk.get());
    }

    statement ins_chunk;
    if (!ins_chunk.prepare(
          db_,
          "INSERT INTO chunks(corpus, path, source, breadcrumb, line_start, line_end, text)"
          " VALUES (?, ?, ?, ?, ?, ?, ?)",
          error
        ))
    {
      rollback();
      co_return false;
    }
    statement ins_vec;
    if (!ins_vec.prepare(db_, "INSERT INTO chunks_vec(rowid, embedding, corpus, source) VALUES (?, ?, ?, ?)", error))
    {
      rollback();
      co_return false;
    }

    for (std::size_t k = 0; k < chunks.size(); ++k)
    {
      const doc_chunk& chunk = chunks[k];
      sqlite3_reset(ins_chunk.get());
      sqlite3_bind_text(ins_chunk.get(), 1, corpus.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(ins_chunk.get(), 2, rel_path.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(ins_chunk.get(), 3, source.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(ins_chunk.get(), 4, chunk.breadcrumb.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int(ins_chunk.get(), 5, chunk.line_start);
      sqlite3_bind_int(ins_chunk.get(), 6, chunk.line_end);
      sqlite3_bind_text(ins_chunk.get(), 7, chunk.text.c_str(), -1, SQLITE_TRANSIENT);
      if (sqlite3_step(ins_chunk.get()) != SQLITE_DONE)
      {
        error = sqlite3_errmsg(db_);
        rollback();
        co_return false;
      }
      const sqlite3_int64 id = sqlite3_last_insert_rowid(db_);

      sqlite3_reset(ins_vec.get());
      sqlite3_bind_int64(ins_vec.get(), 1, id);
      bind_vector(ins_vec.get(), 2, vectors[vector_offset + k]);
      sqlite3_bind_text(ins_vec.get(), 3, corpus.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(ins_vec.get(), 4, source.c_str(), -1, SQLITE_TRANSIENT);
      if (sqlite3_step(ins_vec.get()) != SQLITE_DONE)
      {
        error = sqlite3_errmsg(db_);
        rollback();
        co_return false;
      }
      ++stats.chunks_written;
    }

    // Upsert the file record.
    statement upsert_file;
    if (!upsert_file.prepare(
          db_,
          "INSERT INTO files(corpus, path, content_hash, indexed_at) VALUES (?, ?, ?, ?)"
          " ON CONFLICT(corpus, path) DO UPDATE SET"
          " content_hash = excluded.content_hash, indexed_at = excluded.indexed_at",
          error
        ))
    {
      rollback();
      co_return false;
    }
    sqlite3_bind_text(upsert_file.get(), 1, corpus.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(upsert_file.get(), 2, rel_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(upsert_file.get(), 3, content_hash.c_str(), -1, SQLITE_TRANSIENT);
    const std::string now = utc_now_iso();
    sqlite3_bind_text(upsert_file.get(), 4, now.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(upsert_file.get()) != SQLITE_DONE)
    {
      error = sqlite3_errmsg(db_);
      rollback();
      co_return false;
    }

    if (!exec(db_, "COMMIT", error))
    {
      co_return false;
    }
    ++stats.files_indexed;
    co_return true;
  }

  bool knowledge_store::remove_file(
    const std::string& corpus,
    const std::string& rel_path,
    refresh_stats& stats,
    std::string& error
  )
  {
    statement ids;
    if (!ids.prepare(db_, "SELECT id FROM chunks WHERE corpus = ? AND path = ?", error))
    {
      return false;
    }
    sqlite3_bind_text(ids.get(), 1, corpus.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(ids.get(), 2, rel_path.c_str(), -1, SQLITE_TRANSIENT);
    std::vector<sqlite3_int64> chunk_ids;
    while (sqlite3_step(ids.get()) == SQLITE_ROW)
    {
      chunk_ids.push_back(sqlite3_column_int64(ids.get(), 0));
    }

    if (!exec(db_, "BEGIN", error))
    {
      return false;
    }
    auto rollback = [&]() {
      std::string ignored;
      exec(db_, "ROLLBACK", ignored);
    };

    statement del_vec;
    if (!del_vec.prepare(db_, "DELETE FROM chunks_vec WHERE rowid = ?", error))
    {
      rollback();
      return false;
    }
    statement del_chunk;
    if (!del_chunk.prepare(db_, "DELETE FROM chunks WHERE id = ?", error))
    {
      rollback();
      return false;
    }
    for (const auto id : chunk_ids)
    {
      sqlite3_reset(del_vec.get());
      sqlite3_bind_int64(del_vec.get(), 1, id);
      sqlite3_step(del_vec.get());
      sqlite3_reset(del_chunk.get());
      sqlite3_bind_int64(del_chunk.get(), 1, id);
      sqlite3_step(del_chunk.get());
    }

    statement del_file;
    if (!del_file.prepare(db_, "DELETE FROM files WHERE corpus = ? AND path = ?", error))
    {
      rollback();
      return false;
    }
    sqlite3_bind_text(del_file.get(), 1, corpus.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(del_file.get(), 2, rel_path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(del_file.get());

    if (!exec(db_, "COMMIT", error))
    {
      return false;
    }
    ++stats.files_removed;
    return true;
  }

  dripcore::awaitable<std::vector<search_hit>> knowledge_store::search(
    std::string query,
    std::string corpus,
    std::string source,
    int k,
    std::string& error
  )
  {
    std::vector<search_hit> hits;
    if (!db_)
    {
      error = "knowledge store is not open";
      co_return hits;
    }
    if (k <= 0)
    {
      k = settings_.search_max_results;
    }
    k = std::clamp(k, 1, 100);

    std::vector<std::string> inputs;
    inputs.push_back(std::move(query));
    std::vector<std::vector<float>> vectors = co_await embedder_.embed_batch(std::move(inputs), error);
    if (!error.empty())
    {
      co_return hits;
    }
    if (vectors.size() != 1)
    {
      error = std::format("embedder returned {} vectors for the query", vectors.size());
      co_return hits;
    }

    std::string sql =
      "SELECT c.corpus, c.source, c.path, c.breadcrumb, c.line_start, c.line_end,"
      " substr(c.text, 1, 300) AS snippet, v.distance AS distance"
      " FROM chunks_vec v JOIN chunks c ON c.id = v.rowid"
      " WHERE v.embedding MATCH ?1 AND v.k = ?2";
    int next_param = 3;
    if (!corpus.empty())
    {
      sql += " AND v.corpus = ?" + std::to_string(next_param++);
    }
    if (!source.empty())
    {
      sql += " AND v.source = ?" + std::to_string(next_param++);
    }
    sql += " ORDER BY v.distance";

    statement stmt;
    if (!stmt.prepare(db_, sql, error))
    {
      co_return hits;
    }
    bind_vector(stmt.get(), 1, vectors[0]);
    sqlite3_bind_int(stmt.get(), 2, k);
    int bind_index = 3;
    if (!corpus.empty())
    {
      sqlite3_bind_text(stmt.get(), bind_index++, corpus.c_str(), -1, SQLITE_TRANSIENT);
    }
    if (!source.empty())
    {
      sqlite3_bind_text(stmt.get(), bind_index++, source.c_str(), -1, SQLITE_TRANSIENT);
    }

    while (sqlite3_step(stmt.get()) == SQLITE_ROW)
    {
      search_hit hit;
      hit.corpus = column_text(stmt.get(), 0);
      hit.source = column_text(stmt.get(), 1);
      hit.path = column_text(stmt.get(), 2);
      hit.breadcrumb = column_text(stmt.get(), 3);
      hit.line_start = sqlite3_column_int(stmt.get(), 4);
      hit.line_end = sqlite3_column_int(stmt.get(), 5);
      hit.snippet = column_text(stmt.get(), 6);
      hit.distance = sqlite3_column_double(stmt.get(), 7);
      hit.score = 1.0 - hit.distance;
      hits.push_back(std::move(hit));
    }
    co_return hits;
  }

  std::map<std::string, int> knowledge_store::corpus_stats(std::string& error) const
  {
    std::map<std::string, int> stats;
    if (!db_)
    {
      error = "knowledge store is not open";
      return stats;
    }
    statement stmt;
    if (!stmt.prepare(db_, "SELECT corpus, COUNT(*) FROM chunks GROUP BY corpus", error))
    {
      return stats;
    }
    while (sqlite3_step(stmt.get()) == SQLITE_ROW)
    {
      stats[column_text(stmt.get(), 0)] = sqlite3_column_int(stmt.get(), 1);
    }
    return stats;
  }

  std::map<std::string, int> knowledge_store::source_stats(const std::string& corpus, std::string& error) const
  {
    std::map<std::string, int> stats;
    if (!db_)
    {
      error = "knowledge store is not open";
      return stats;
    }
    statement stmt;
    if (!stmt.prepare(db_, "SELECT source, COUNT(*) FROM chunks WHERE corpus = ? GROUP BY source", error))
    {
      return stats;
    }
    sqlite3_bind_text(stmt.get(), 1, corpus.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt.get()) == SQLITE_ROW)
    {
      stats[column_text(stmt.get(), 0)] = sqlite3_column_int(stmt.get(), 1);
    }
    return stats;
  }

  dripcore::awaitable<rechunk_stats> knowledge_store::rechunk_breadcrumbs(
    std::string corpus,
    std::string root_dir,
    corpus_kind kind,
    std::string& error
  )
  {
    rechunk_stats stats;
    if (!db_)
    {
      error = "knowledge store is not open";
      co_return stats;
    }

    std::error_code ec;
    const fs::path root = fs::absolute(fs::path(root_dir), ec).lexically_normal();
    if (ec)
    {
      error = std::format("cannot resolve {}: {}", root_dir, ec.message());
      co_return stats;
    }

    std::vector<std::string> paths;
    {
      statement stmt;
      if (!stmt.prepare(db_, "SELECT DISTINCT path FROM chunks WHERE corpus = ? ORDER BY path", error))
      {
        co_return stats;
      }
      sqlite3_bind_text(stmt.get(), 1, corpus.c_str(), -1, SQLITE_TRANSIENT);
      while (sqlite3_step(stmt.get()) == SQLITE_ROW)
      {
        paths.push_back(column_text(stmt.get(), 0));
      }
    }

    markdown_chunker doc_chunker(settings_.chunk_max_chars);
    code_chunker src_chunker(
      settings_.code_chunk_max_lines, settings_.code_chunk_overlap_lines, settings_.code_chunk_max_chars
    );

    statement ids;
    statement update;
    if (!ids.prepare(db_, "SELECT id FROM chunks WHERE corpus = ? AND path = ? ORDER BY id", error))
    {
      co_return stats;
    }
    if (!update.prepare(db_, "UPDATE chunks SET breadcrumb = ? WHERE id = ?", error))
    {
      co_return stats;
    }

    int since_yield = 0;
    for (const auto& rel : paths)
    {
      bool read_ok = false;
      std::string content = read_file(root / rel, read_ok);
      if (!read_ok)
      {
        // A file that vanished since it was indexed is not an error here.
        ++stats.files_skipped;
        continue;
      }

      const std::vector<doc_chunk> chunks = enforce_chunk_budget(
        kind == corpus_kind::code ? src_chunker.chunk(content) : doc_chunker.chunk(content),
        settings_.max_chunk_chars
      );

      // Rows in insertion order, which is chunk order.
      std::vector<sqlite3_int64> row_ids;
      sqlite3_reset(ids.get());
      sqlite3_clear_bindings(ids.get());
      sqlite3_bind_text(ids.get(), 1, corpus.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(ids.get(), 2, rel.c_str(), -1, SQLITE_TRANSIENT);
      while (sqlite3_step(ids.get()) == SQLITE_ROW)
      {
        row_ids.push_back(sqlite3_column_int64(ids.get(), 0));
      }

      if (row_ids.size() != chunks.size())
      {
        // Boundaries moved: patching breadcrumbs would attach them to
        // the wrong rows, so leave this file for a real re-index.
        ++stats.files_skipped;
        continue;
      }

      for (std::size_t i = 0; i < chunks.size(); ++i)
      {
        sqlite3_reset(update.get());
        sqlite3_clear_bindings(update.get());
        sqlite3_bind_text(update.get(), 1, chunks[i].breadcrumb.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(update.get(), 2, row_ids[i]);
        if (sqlite3_step(update.get()) != SQLITE_DONE)
        {
          error = std::format("updating breadcrumb for {}: {}", rel, sqlite3_errmsg(db_));
          co_return stats;
        }
        ++stats.chunks_updated;
      }
      ++stats.files_updated;

      if (++since_yield >= 8)
      {
        since_yield = 0;
        co_await dripcore::sleep_awaiter{dripcore::executor::current(), nullptr, std::chrono::milliseconds(0)};
      }
    }

    co_return stats;
  }

  bool knowledge_store::is_excluded(const fs::path& path) const
  {
    for (const auto& component : path)
    {
      const std::string name = component.string();
      for (const auto& excluded : settings_.exclude_dirs)
      {
        if (utils::matches_name_pattern(name, excluded))
        {
          return true;
        }
      }
    }
    // The final component is checked against exclude_files too: an
    // extension rule cannot tell a CMakeLists.txt from a document, and
    // the walk has already decided the file is a candidate by then.
    const std::string filename = path.filename().string();
    for (const auto& excluded : settings_.exclude_files)
    {
      if (utils::matches_name_pattern(filename, excluded))
      {
        return true;
      }
    }
    return false;
  }

  bool knowledge_store::has_extension(const fs::path& path, corpus_kind kind) const
  {
    auto lowered = [](std::string s) {
      std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
      return s;
    };
    const std::string ext = lowered(path.extension().string());
    const auto& extensions = kind == corpus_kind::code ? settings_.code_extensions : settings_.include_extensions;
    for (const auto& included : extensions)
    {
      if (ext == lowered(included))
      {
        return true;
      }
    }
    return false;
  }

} // namespace mistral::knowledge
