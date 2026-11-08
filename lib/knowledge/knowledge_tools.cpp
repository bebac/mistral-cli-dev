#include "knowledge/knowledge_tools.h"

#include "knowledge/hashing_embedder.h"

#include <filesystem>
#include <map>
#include <utility>

namespace fs = std::filesystem;

namespace mistral::knowledge
{
  namespace
  {
    constexpr const char* k_workspace_corpus = "workspace-docs";
    constexpr const char* k_code_corpus = "workspace-code";

    // Column the "code only:" / "docs only:" exception lists start at:
    // the two-space indent plus the label.
    constexpr std::size_t k_exception_indent = 13;

    // Comma-separated repository names, wrapped rather than emitted on
    // one long line so the block stays readable once it is in a prompt.
    // first_column is where the first name lands; indent is where a
    // wrapped continuation line starts.
    std::string format_source_list(
      const std::map<std::string, int>& sources,
      std::size_t first_column,
      std::size_t indent
    )
    {
      std::string list;
      std::size_t column = first_column;
      for (const auto& entry : sources)
      {
        const std::string& name = entry.first;
        if (!list.empty())
        {
          if (column + name.size() + 2 > 70)
          {
            list += ",\n";
            list.append(indent, ' ');
            column = indent;
          }
          else
          {
            list += ", ";
            column += 2;
          }
        }
        list += name;
        column += name.size();
      }
      return list;
    }
  }

  knowledge_tools::knowledge_tools(
    knowledge_settings settings,
    std::string workspace_root,
    std::unique_ptr<embedder> emb
  )
    : settings_(std::move(settings)), workspace_root_(std::move(workspace_root)), embedder_(std::move(emb))
  {
    if (!embedder_)
    {
      embedder_ = std::make_unique<hashing_embedder>(settings_.embedding_dim);
    }
  }

  knowledge_settings knowledge_tools::resolved_settings() const
  {
    knowledge_settings resolved = settings_;
    fs::path db_path = fs::path(resolved.db_path);
    if (db_path.is_relative())
    {
      db_path = fs::path(workspace_root_) / db_path;
    }
    resolved.db_path = db_path.lexically_normal().generic_string();
    return resolved;
  }

  bool knowledge_tools::open_store(std::string& error)
  {
    if (store_)
    {
      return true;
    }
    store_ = std::make_unique<knowledge_store>(resolved_settings(), *embedder_);
    if (!store_->open(error))
    {
      return false;
    }
    // Forward the store's phase reports to whoever is watching. The
    // indirection keeps the store independent of the UI plumbing.
    store_->set_progress_callback([this](const index_progress& progress) {
      if (progress_)
      {
        progress_(progress);
      }
    });
    return true;
  }

  void knowledge_tools::set_progress_callback(progress_fn fn)
  {
    progress_ = std::move(fn);
  }

  void knowledge_tools::report_idle()
  {
    // A session spans several refresh_corpus() calls; clearing between
    // them would blink the indicator off once per round.
    if (indexing_session_)
    {
      return;
    }
    if (progress_)
    {
      progress_(index_progress{});
    }
  }

  void knowledge_tools::begin_indexing()
  {
    indexing_session_ = true;
  }

  void knowledge_tools::end_indexing()
  {
    indexing_session_ = false;
    report_idle();
  }

  dripcore::awaitable<bool> knowledge_tools::ensure_ready(corpus_kind kind, refresh_stats& stats, std::string& error)
  {
    if (!open_store(error))
    {
      co_return false;
    }
    const char* corpus = kind == corpus_kind::code ? k_code_corpus : k_workspace_corpus;
    stats = co_await store_->refresh_corpus(corpus, workspace_root_, error, kind);
    report_idle();
    co_return error.empty();
  }

  std::string knowledge_tools::index_summary()
  {
    // A throwaway store opened read-only. The summary must not disturb
    // the index the session is using, and must not bring an empty one
    // into being when none exists.
    knowledge_store store(resolved_settings(), *embedder_);
    std::string error;
    if (!store.open_read_only(error))
    {
      return {};
    }
    const auto code_sources = store.source_stats(k_code_corpus, error);
    if (!error.empty())
    {
      return {};
    }
    const auto doc_sources = store.source_stats(k_workspace_corpus, error);
    if (!error.empty() || (code_sources.empty() && doc_sources.empty()))
    {
      return {};
    }

    // The corpora hold different repositories: a header-only library has
    // code and no prose, a docs-only checkout the reverse. Naming both
    // lists in full costs several prompt lines to convey a one-entry
    // difference, so the union is named once and each repository that
    // only one corpus holds is called out under the tool that cannot
    // reach it. When the two corpora match, no exception line is added.
    std::map<std::string, int> all_sources = code_sources;
    for (const auto& [name, count] : doc_sources)
    {
      all_sources[name] += count;
    }

    std::map<std::string, int> code_only;
    for (const auto& [name, count] : code_sources)
    {
      if (doc_sources.find(name) == doc_sources.end())
      {
        code_only[name] = count;
      }
    }
    std::map<std::string, int> docs_only;
    for (const auto& [name, count] : doc_sources)
    {
      if (code_sources.find(name) == code_sources.end())
      {
        docs_only[name] = count;
      }
    }

    const std::string head =
      "Knowledge index — search before reading files.\n\n"
      "search_code(query, source, k) and search_docs(query, source, k):\n"
      "  source \"\" = every repository, or one of: ";
    std::string block = head + format_source_list(all_sources, head.size(), 2);
    if (!code_only.empty())
    {
      block += "\n  code only: " + format_source_list(code_only, k_exception_indent, k_exception_indent);
    }
    if (!docs_only.empty())
    {
      block += "\n  docs only: " + format_source_list(docs_only, k_exception_indent, k_exception_indent);
    }
    return block;
  }

  dripcore::awaitable<json> knowledge_tools::search_docs(std::string query, std::string source, int k)
  {
    std::string error;
    refresh_stats refresh;
    if (!co_await ensure_ready(corpus_kind::documents, refresh, error))
    {
      co_return json{{"error", error.empty() ? "knowledge store unavailable" : error}};
    }

    if (k <= 0)
    {
      k = settings_.search_max_results;
    }

    const std::vector<search_hit> hits = co_await store_->search(query, k_workspace_corpus, source, k, error);
    if (!error.empty())
    {
      co_return json{{"error", error}};
    }

    json results = json::array();
    for (const auto& hit : hits)
    {
      results.push_back(json{
        {"corpus", hit.corpus},
        {"source", hit.source},
        {"path", hit.path},
        {"breadcrumb", hit.breadcrumb},
        {"line_start", hit.line_start},
        {"line_end", hit.line_end},
        {"score", hit.score},
        {"snippet", hit.snippet},
      });
    }

    // Repositories present in the documentation corpus, so the caller can
    // discover valid values for `source` instead of guessing. The two
    // corpora hold different repositories, so this is not the list
    // search_code returns. The empty name is the workspace's own
    // repository.
    json sources = json::array();
    const auto source_counts = store_->source_stats(k_workspace_corpus, error);
    if (error.empty())
    {
      for (const auto& [name, count] : source_counts)
      {
        sources.push_back(json{{"source", name}, {"chunks", count}});
      }
    }
    error.clear();

    json result{
      {"query", query},
      {"source", source},
      {"count", results.size()},
      {"results", std::move(results)},
      {"sources", std::move(sources)},
    };
    if (refresh.files_pending > 0)
    {
      // The index is incomplete: the per-refresh budget ran out. The
      // results are valid, just drawn from a partial index, and the next
      // search indexes more.
      result["indexing"] = json{{"files_pending", refresh.files_pending}};
    }
    co_return result;
  }

  dripcore::awaitable<json> knowledge_tools::search_code(std::string query, std::string source, int k)
  {
    std::string error;
    refresh_stats refresh;
    if (!co_await ensure_ready(corpus_kind::code, refresh, error))
    {
      co_return json{{"error", error.empty() ? "knowledge store unavailable" : error}};
    }

    if (k <= 0)
    {
      k = settings_.search_max_results;
    }

    const std::vector<search_hit> hits = co_await store_->search(query, k_code_corpus, source, k, error);
    if (!error.empty())
    {
      co_return json{{"error", error}};
    }

    json results = json::array();
    for (const auto& hit : hits)
    {
      results.push_back(json{
        {"source", hit.source},
        {"path", hit.path},
        {"breadcrumb", hit.breadcrumb},
        {"line_start", hit.line_start},
        {"line_end", hit.line_end},
        {"score", hit.score},
        {"snippet", hit.snippet},
      });
    }

    // Repositories present in the index, so the caller can discover valid
    // values for `source` instead of guessing. The empty name is the
    // workspace's own repository.
    json sources = json::array();
    const auto source_counts = store_->source_stats(k_code_corpus, error);
    if (error.empty())
    {
      for (const auto& [name, count] : source_counts)
      {
        sources.push_back(json{{"source", name}, {"chunks", count}});
      }
    }
    error.clear();

    json result{
      {"query", query},
      {"source", source},
      {"count", results.size()},
      {"results", std::move(results)},
      {"sources", std::move(sources)},
    };
    if (refresh.files_pending > 0)
    {
      // The index is incomplete: the per-refresh budget ran out. The
      // results are valid, just drawn from a partial index.
      result["indexing"] = json{{"files_pending", refresh.files_pending}};
    }
    co_return result;
  }

  dripcore::awaitable<json> knowledge_tools::refresh(corpus_kind kind)
  {
    std::string error;
    if (!open_store(error))
    {
      co_return json{{"error", error}};
    }

    const char* corpus = kind == corpus_kind::code ? k_code_corpus : k_workspace_corpus;
    const refresh_stats stats = co_await store_->refresh_corpus(corpus, workspace_root_, error, kind);
    report_idle();
    if (!error.empty())
    {
      co_return json{{"error", error}};
    }

    const auto corpus_counts = store_->corpus_stats(error);
    if (!error.empty())
    {
      co_return json{{"error", error}};
    }
    json corpora = json::object();
    for (const auto& [name, count] : corpus_counts)
    {
      corpora[name] = count;
    }

    co_return json{
      {"corpus", corpus},
      {"files_seen", stats.files_seen},
      {"files_too_large", stats.files_too_large},
      {"files_indexed", stats.files_indexed},
      {"files_unchanged", stats.files_unchanged},
      {"files_removed", stats.files_removed},
      {"files_pending", stats.files_pending},
      {"chunks_written", stats.chunks_written},
      {"corpora", std::move(corpora)},
    };
  }

  dripcore::awaitable<json> knowledge_tools::rechunk_breadcrumbs()
  {
    std::string error;
    if (!open_store(error))
    {
      co_return json{{"error", error}};
    }

    json out = json::object();
    for (const auto kind : {corpus_kind::documents, corpus_kind::code})
    {
      const char* corpus = kind == corpus_kind::code ? k_code_corpus : k_workspace_corpus;
      const rechunk_stats stats = co_await store_->rechunk_breadcrumbs(corpus, workspace_root_, kind, error);
      if (!error.empty())
      {
        co_return json{{"error", error}};
      }
      out[corpus] = json{
        {"files_updated", stats.files_updated},
        {"chunks_updated", stats.chunks_updated},
        {"files_skipped", stats.files_skipped},
      };
    }
    co_return out;
  }

  dripcore::awaitable<json> knowledge_tools::refresh_docs()
  {
    co_return co_await refresh(corpus_kind::documents);
  }

} // namespace mistral::knowledge
