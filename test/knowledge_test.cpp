#include <doctest/doctest.h>
#ifdef MISTRAL_ENABLE_KNOWLEDGE

#include "knowledge/hashing_embedder.h"
#include "knowledge/code_chunker.h"
#include "knowledge/knowledge_store.h"
#include "knowledge/knowledge_tools.h"
#include "knowledge/markdown_chunker.h"

#include <dripcore/executor.h>
#include <dripcore/scheduler.h>
#include <dripcore/sleep_awaiter.h>
#include <dripcore/task.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace
{
  // ---- Test helpers ----

  struct temp_dir
  {
    fs::path path;

    temp_dir()
    {
      static int counter = 0;
      path = fs::temp_directory_path() / ("mistral_knowledge_test_" + std::to_string(++counter));
      fs::remove_all(path);
      fs::create_directories(path);
    }

    ~temp_dir()
    {
      fs::remove_all(path);
    }

    void write(const std::string& rel, const std::string& content)
    {
      const auto p = path / rel;
      fs::create_directories(p.parent_path());
      std::ofstream f(p, std::ios::binary);
      f << content;
    }
  };

  // Extract the value type from an awaitable.
  template <typename T> struct awaitable_value
  {
    using type = T;
  };

  template <typename T> struct awaitable_value<dripcore::awaitable<T>>
  {
    using type = T;
  };

  template <typename T> using awaitable_value_t = typename awaitable_value<T>::type;

  // Result holder that can be set from a task.
  template <typename T> class async_result
  {
  public:
    void set(T value)
    {
      value_ = std::move(value);
      completed_ = true;
    }
    T get() const
    {
      return value_;
    }
    bool completed() const
    {
      return completed_;
    }

  private:
    T value_{};
    bool completed_ = false;
  };

  // Run an awaitable-returning callable on a scheduler and return its result.
  template <typename Func> auto run_async(Func&& func)
  {
    using func_result = decltype(func());
    using result_type = awaitable_value_t<func_result>;

    async_result<result_type> result_holder;

    class async_task : public dripcore::task
    {
    public:
      async_task(Func&& f, async_result<result_type>& holder) : func_(std::move(f)), holder_(holder)
      {
      }

      dripcore::awaitable<> body() override
      {
        holder_.set(co_await func_());
        co_return;
      }

    private:
      Func func_;
      async_result<result_type>& holder_;
    };

    dripcore::scheduler scheduler;
    scheduler.spawn<async_task>(std::forward<Func>(func), result_holder);
    scheduler.run();

    REQUIRE(result_holder.completed());
    return result_holder.get();
  }

  double cosine_similarity(const std::vector<float>& a, const std::vector<float>& b)
  {
    double dot = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
      dot += static_cast<double>(a[i]) * b[i];
    }
    return dot;
  }

  double l2_norm(const std::vector<float>& v)
  {
    double sum = 0.0;
    for (const float x : v)
    {
      sum += static_cast<double>(x) * x;
    }
    return std::sqrt(sum);
  }

  int chunk_count(mistral::knowledge::knowledge_store& store, const std::string& corpus)
  {
    std::string error;
    const auto stats = store.corpus_stats(error);
    const auto it = stats.find(corpus);
    return it == stats.end() ? 0 : it->second;
  }

  // Embedder with a controllable identity and failure mode, so the
  // store's rebuild-on-identity-change and failure semantics can be
  // tested without a network.
  class test_embedder : public mistral::knowledge::embedder
  {
  public:
    explicit test_embedder(std::string identity, int dim = 32) : identity_(std::move(identity)), dim_(dim)
    {
    }

    int dim() const override
    {
      return dim_;
    }

    std::string identity() const override
    {
      return identity_;
    }

    int batch_size() const override
    {
      return batch_size_;
    }

    int max_batch_chars() const override
    {
      return max_batch_chars_;
    }

    dripcore::awaitable<std::vector<std::vector<float>>>
      embed_batch(std::vector<std::string> inputs, std::string& error) override
    {
      std::vector<std::vector<float>> out;

      // Observations for the pipeline tests: how many requests were
      // issued, and how many were ever in flight at the same time.
      ++request_count;
      inputs_seen += inputs.size();
      ++in_flight_;
      if (in_flight_ > max_in_flight)
      {
        max_in_flight = in_flight_;
      }

      if (delay_.count() > 0)
      {
        // Suspend so sibling requests can overlap. Without this the
        // pipeline would look sequential even when it is not.
        co_await dripcore::sleep_awaiter{dripcore::executor::current(), nullptr, delay_};
      }

      if (fail_)
      {
        error = "test embedder failure";
        --in_flight_;
        co_return out;
      }
      if (inputs.empty())
      {
        error = "embed_batch called with no inputs";
        --in_flight_;
        co_return out;
      }
      for (const auto& text : inputs)
      {
        std::vector<float> vec(static_cast<std::size_t>(dim_), 0.0f);
        for (std::size_t i = 0; i < text.size() && i < vec.size(); ++i)
        {
          vec[i] = static_cast<float>(static_cast<unsigned char>(text[i]));
        }
        out.push_back(std::move(vec));
      }
      --in_flight_;
      co_return out;
    }

    void set_fail(bool fail)
    {
      fail_ = fail;
    }

    void set_batch_size(int size)
    {
      batch_size_ = size;
    }

    void set_max_batch_chars(int chars)
    {
      max_batch_chars_ = chars;
    }

    void set_delay(std::chrono::milliseconds delay)
    {
      delay_ = delay;
    }

    int request_count = 0;
    std::size_t inputs_seen = 0;
    int max_in_flight = 0;

  private:
    std::string identity_;
    int dim_;
    bool fail_ = false;
    int batch_size_ = 4;
    int max_batch_chars_ = 0;
    std::chrono::milliseconds delay_{0};
    int in_flight_ = 0;
  };
} // namespace

// ---- Hashing embedder ----

TEST_CASE("hashing_embedder: dimension and determinism")
{
  mistral::knowledge::hashing_embedder embedder(384);
  const auto a = embedder.embed("coroutines co_await co_return suspend resume");
  const auto b = embedder.embed("coroutines co_await co_return suspend resume");
  REQUIRE(a.size() == 384);
  CHECK(a == b);
}

TEST_CASE("hashing_embedder: normalized, similar texts are closer")
{
  mistral::knowledge::hashing_embedder embedder(384);
  const auto coroutines = embedder.embed("C++20 coroutines suspend resume function co_await co_return");
  const auto concepts = embedder.embed("C++20 concepts template requirements typename constraint");
  const auto query = embedder.embed("suspend and resume a coroutine function");

  CHECK(std::abs(l2_norm(coroutines) - 1.0) < 1e-4);
  CHECK(cosine_similarity(query, coroutines) > cosine_similarity(query, concepts));
}

TEST_CASE("hashing_embedder: empty input yields the zero vector")
{
  mistral::knowledge::hashing_embedder embedder(64);
  const auto v = embedder.embed("   ");
  REQUIRE(v.size() == 64);
  for (const float x : v)
  {
    CHECK(x == 0.0f);
  }
}

// ---- Markdown chunker ----

TEST_CASE("markdown_chunker: breadcrumbs and line ranges")
{
  const std::string doc =
    "# Guide\n"
    "\n"
    "Intro text.\n"
    "\n"
    "## Install\n"
    "\n"
    "Install steps.\n"
    "\n"
    "### Linux\n"
    "\n"
    "Linux steps.\n";

  mistral::knowledge::markdown_chunker chunker;
  const auto chunks = chunker.chunk(doc);
  REQUIRE(chunks.size() == 3);

  CHECK(chunks[0].breadcrumb == "Guide");
  CHECK(chunks[0].line_start == 1);
  CHECK(chunks[0].line_end == 3);

  CHECK(chunks[1].breadcrumb == "Guide > Install");
  CHECK(chunks[1].line_start == 5);
  CHECK(chunks[1].line_end == 7);

  CHECK(chunks[2].breadcrumb == "Guide > Install > Linux");
  CHECK(chunks[2].line_start == 9);
  CHECK(chunks[2].line_end == 11);
}

TEST_CASE("markdown_chunker: '#' inside fenced code blocks is not a header")
{
  const std::string doc =
    "# Real\n"
    "\n"
    "Text.\n"
    "\n"
    "```c++\n"
    "#include <vector>\n"
    "# not a header\n"
    "```\n";

  mistral::knowledge::markdown_chunker chunker;
  const auto chunks = chunker.chunk(doc);
  REQUIRE(chunks.size() == 1);
  CHECK(chunks[0].breadcrumb == "Real");
  CHECK(chunks[0].line_start == 1);
  CHECK(chunks[0].line_end == 8);
}

TEST_CASE("markdown_chunker: oversized sections split at paragraph boundaries")
{
  std::string doc = "# Big\n\n";
  for (int i = 0; i < 20; ++i)
  {
    doc += "Paragraph number " + std::to_string(i) + " with some words to fill space.\n\n";
  }

  mistral::knowledge::markdown_chunker chunker(200);
  const auto chunks = chunker.chunk(doc);
  REQUIRE(chunks.size() > 1);
  for (const auto& chunk : chunks)
  {
    CHECK(chunk.text.size() <= 200);
    CHECK(chunk.breadcrumb == "Big");
  }
  // Line ranges are ordered and non-overlapping.
  for (std::size_t i = 1; i < chunks.size(); ++i)
  {
    CHECK(chunks[i].line_start > chunks[i - 1].line_start);
    CHECK(chunks[i].line_start > chunks[i - 1].line_end);
  }
}

TEST_CASE("markdown_chunker: leading frontmatter is skipped")
{
  const std::string doc =
    "---\n"
    "title: Note\n"
    "tags: [a, b]\n"
    "---\n"
    "# Title\n"
    "\n"
    "Body.\n";

  mistral::knowledge::markdown_chunker chunker;
  const auto chunks = chunker.chunk(doc);
  REQUIRE(chunks.size() == 1);
  CHECK(chunks[0].breadcrumb == "Title");
  CHECK(chunks[0].line_start == 5);
  CHECK(chunks[0].text.find("title: Note") == std::string::npos);
}

TEST_CASE("markdown_chunker: empty document yields no chunks")
{
  mistral::knowledge::markdown_chunker chunker;
  CHECK(chunker.chunk("").empty());
  CHECK(chunker.chunk("\n\n  \n").empty());
}

// ---- Code chunker ----

TEST_CASE("code_chunker: empty content yields no chunks")
{
  mistral::knowledge::code_chunker chunker;
  CHECK(chunker.chunk("").empty());
  CHECK(chunker.chunk("\n\n\n").empty());
}

TEST_CASE("code_chunker: a short file is one chunk")
{
  mistral::knowledge::code_chunker chunker;
  const std::string src =
    "int add(int a, int b)\n"
    "{\n"
    "  return a + b;\n"
    "}\n";

  const auto chunks = chunker.chunk(src);
  REQUIRE(chunks.size() == 1);
  CHECK(chunks[0].line_start == 1);
  CHECK(chunks[0].line_end == 4);
  CHECK(chunks[0].text.find("return a + b") != std::string::npos);
}

TEST_CASE("code_chunker: long files split into overlapping windows with no gaps")
{
  std::string src;
  for (int i = 1; i <= 200; ++i)
  {
    src += "line " + std::to_string(i) + "\n";
  }

  mistral::knowledge::code_chunker chunker(80, 20);
  const auto chunks = chunker.chunk(src);
  REQUIRE(chunks.size() >= 3);

  CHECK(chunks.front().line_start == 1);
  CHECK(chunks.back().line_end == 200);
  for (std::size_t i = 1; i < chunks.size(); ++i)
  {
    // No gap between consecutive windows, and forward progress.
    CHECK(chunks[i].line_start <= chunks[i - 1].line_end + 1);
    CHECK(chunks[i].line_start > chunks[i - 1].line_start);
  }
}

TEST_CASE("code_chunker: breadcrumb names the enclosing declaration")
{
  std::string src = "namespace demo\n{\n";
  for (int i = 0; i < 100; ++i)
  {
    src += "  int filler" + std::to_string(i) + " = 0;\n";
  }
  src += "}\n";

  mistral::knowledge::code_chunker chunker(40, 10);
  const auto chunks = chunker.chunk(src);
  REQUIRE(chunks.size() >= 2);

  // The first window opens on the namespace line itself, so it has no
  // breadcrumb; later windows inherit it.
  CHECK(chunks[0].breadcrumb.empty());
  CHECK(chunks[1].breadcrumb.find("namespace demo") != std::string::npos);
}

TEST_CASE("code_chunker: the character budget bounds a window")
{
  std::string src;
  for (int i = 0; i < 40; ++i)
  {
    src += std::string(200, 'x') + "\n";
  }

  mistral::knowledge::code_chunker chunker(40, 0, 1000);
  const auto chunks = chunker.chunk(src);
  REQUIRE(chunks.size() > 1);

  for (const auto& chunk : chunks)
  {
    CHECK(chunk.text.size() <= 1000);
  }
  // Trimming for the budget must not drop the tail of the file.
  CHECK(chunks.back().line_end == 40);
}

// ---- Knowledge store ----

TEST_CASE("knowledge_store: index and semantic search")
{
  temp_dir docs;
  docs.write(
    "coroutines.md",
    "# Coroutines\n"
    "\n"
    "A coroutine is a function that can suspend its execution and resume later.\n"
    "Use co_await to suspend and co_return to finish.\n"
    "\n"
    "## Awaitables\n"
    "\n"
    "An awaitable provides await_suspend, await_resume and await_ready.\n"
  );
  docs.write(
    "concepts.md",
    "# Concepts\n"
    "\n"
    "A concept is a named set of template requirements. Use requires clauses.\n"
    "Concepts constrain template parameters with typename and type requirements.\n"
  );

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (docs.path / "knowledge.db").string();
  settings.include_extensions = {".md"};
  settings.exclude_dirs = {".git"};

  mistral::knowledge::hashing_embedder embedder(settings.embedding_dim);
  mistral::knowledge::knowledge_store store(settings, embedder);
  std::string error;
  REQUIRE(store.open(error));

  std::string refresh_error;
  const auto stats = run_async([&]() {
    return store.refresh_corpus("workspace-docs", docs.path.string(), refresh_error);
  });
  CHECK(refresh_error.empty());
  CHECK(stats.files_seen == 2);
  CHECK(stats.files_indexed == 2);
  CHECK(stats.chunks_written >= 2);

  // A coroutines query finds the coroutines document first.
  std::string search_error;
  const auto hits = run_async([&]() {
    return store.search("suspend and resume a coroutine function", "", "", 5, search_error);
  });
  CHECK(search_error.empty());
  REQUIRE(!hits.empty());
  CHECK(hits[0].path == "coroutines.md");
  CHECK(hits[0].corpus == "workspace-docs");
  CHECK(hits[0].line_start > 0);
  CHECK(hits[0].score > 0.0);
  CHECK(hits[0].score <= 1.0 + 1e-4);

  // A concepts query finds the concepts document first.
  const auto hits2 = run_async([&]() {
    return store.search("template requirements and constraints", "", "", 5, search_error);
  });
  CHECK(search_error.empty());
  REQUIRE(!hits2.empty());
  CHECK(hits2[0].path == "concepts.md");
}

TEST_CASE("knowledge_store: corpus filter restricts results")
{
  temp_dir docs;
  docs.write("a/alpha.md", "# Alpha\n\nAlpha document about coroutines and co_await.\n");
  docs.write("b/beta.md", "# Beta\n\nBeta document about coroutines and co_await.\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (docs.path / "knowledge.db").string();

  mistral::knowledge::hashing_embedder embedder(settings.embedding_dim);
  mistral::knowledge::knowledge_store store(settings, embedder);
  std::string error;
  REQUIRE(store.open(error));

  std::string refresh_error;
  run_async([&]() {
    return store.refresh_corpus("corpus-a", (docs.path / "a").string(), refresh_error);
  });
  run_async([&]() {
    return store.refresh_corpus("corpus-b", (docs.path / "b").string(), refresh_error);
  });
  CHECK(refresh_error.empty());

  std::string search_error;
  const auto filtered = run_async([&]() {
    return store.search("coroutines co_await", "corpus-a", "", 5, search_error);
  });
  CHECK(search_error.empty());
  REQUIRE(!filtered.empty());
  for (const auto& hit : filtered)
  {
    CHECK(hit.corpus == "corpus-a");
  }

  const auto all = run_async([&]() {
    return store.search("coroutines co_await", "", "", 5, search_error);
  });
  CHECK(all.size() >= 2);
}

TEST_CASE("knowledge_store: incremental refresh and file removal")
{
  temp_dir docs;
  docs.write("one.md", "# One\n\nFirst version of the document about coroutines.\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (docs.path / "knowledge.db").string();

  mistral::knowledge::hashing_embedder embedder(settings.embedding_dim);
  mistral::knowledge::knowledge_store store(settings, embedder);
  std::string error;
  REQUIRE(store.open(error));

  std::string refresh_error;
  auto stats = run_async([&]() {
    return store.refresh_corpus("docs", docs.path.string(), refresh_error);
  });
  CHECK(refresh_error.empty());
  CHECK(stats.files_indexed == 1);
  const int chunks_after_first = chunk_count(store, "docs");
  CHECK(chunks_after_first >= 1);

  // Second refresh: nothing changed.
  stats = run_async([&]() {
    return store.refresh_corpus("docs", docs.path.string(), refresh_error);
  });
  CHECK(stats.files_seen == 1);
  CHECK(stats.files_unchanged == 1);
  CHECK(stats.files_indexed == 0);
  CHECK(chunk_count(store, "docs") == chunks_after_first);

  // Modify the file: re-indexed, old chunks replaced.
  docs.write("one.md", "# One\n\nSecond version about templates and concepts.\n");
  stats = run_async([&]() {
    return store.refresh_corpus("docs", docs.path.string(), refresh_error);
  });
  CHECK(stats.files_indexed == 1);
  CHECK(chunk_count(store, "docs") == chunks_after_first);

  std::string search_error;
  const auto hits = run_async([&]() {
    return store.search("templates and concepts", "", "", 5, search_error);
  });
  REQUIRE(!hits.empty());
  CHECK(hits[0].snippet.find("Second version") != std::string::npos);

  // Delete the file: its chunks are removed.
  fs::remove(docs.path / "one.md");
  stats = run_async([&]() {
    return store.refresh_corpus("docs", docs.path.string(), refresh_error);
  });
  CHECK(stats.files_removed == 1);
  CHECK(chunk_count(store, "docs") == 0);
}

TEST_CASE("knowledge_store: excluded directories are not indexed")
{
  temp_dir docs;
  docs.write("keep.md", "# Keep\n\nKeep this document.\n");
  docs.write("build/skip.md", "# Skip\n\nSkip this document.\n");
  docs.write(".git/hidden.md", "# Hidden\n\nHidden document.\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (docs.path / "knowledge.db").string();

  mistral::knowledge::hashing_embedder embedder(settings.embedding_dim);
  mistral::knowledge::knowledge_store store(settings, embedder);
  std::string error;
  REQUIRE(store.open(error));

  std::string refresh_error;
  const auto stats = run_async([&]() {
    return store.refresh_corpus("docs", docs.path.string(), refresh_error);
  });
  CHECK(refresh_error.empty());
  CHECK(stats.files_seen == 1);
  CHECK(stats.files_indexed == 1);
}

TEST_CASE("knowledge_store: wildcard exclusions drop whole build trees")
{
  temp_dir docs;
  docs.write("keep.md", "# Keep\n\nKeep this document.\n");
  docs.write("build/skip.md", "# Skip\n\nBuild output.\n");
  docs.write("build_asan/skip.md", "# Skip\n\nBuild output.\n");
  docs.write("build_debug/skip.md", "# Skip\n\nBuild output.\n");
  docs.write("cmake-build-debug/skip.md", "# Skip\n\nBuild output.\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (docs.path / "knowledge.db").string();

  // The default list carries "build*" and "cmake-build-*".
  mistral::knowledge::hashing_embedder embedder(settings.embedding_dim);
  mistral::knowledge::knowledge_store store(settings, embedder);
  std::string error;
  REQUIRE(store.open(error));

  std::string refresh_error;
  const auto stats = run_async([&]() {
    return store.refresh_corpus("docs", docs.path.string(), refresh_error);
  });
  CHECK(refresh_error.empty());
  CHECK(stats.files_seen == 1);
  CHECK(stats.files_indexed == 1);
}

TEST_CASE("knowledge_store: newly excluded files are purged on the next refresh")
{
  temp_dir docs;
  docs.write("keep.md", "# Keep\n\nKeep this document.\n");
  docs.write("build_asan/skip.md", "# Skip\n\nBuild output.\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (docs.path / "knowledge.db").string();
  // Index everything first, as a pre-fix index would have.
  settings.exclude_dirs = {};

  {
    mistral::knowledge::hashing_embedder embedder(settings.embedding_dim);
    mistral::knowledge::knowledge_store store(settings, embedder);
    std::string error;
    REQUIRE(store.open(error));
    std::string refresh_error;
    const auto stats = run_async([&]() {
      return store.refresh_corpus("docs", docs.path.string(), refresh_error);
    });
    CHECK(refresh_error.empty());
    CHECK(stats.files_indexed == 2);
    CHECK(chunk_count(store, "docs") == 2);
  }

  // Tightening the exclusion must drop the already-indexed rows, not
  // just stop indexing new ones.
  settings.exclude_dirs = {"build*"};
  {
    mistral::knowledge::hashing_embedder embedder(settings.embedding_dim);
    mistral::knowledge::knowledge_store store(settings, embedder);
    std::string error;
    REQUIRE(store.open(error));
    std::string refresh_error;
    const auto stats = run_async([&]() {
      return store.refresh_corpus("docs", docs.path.string(), refresh_error);
    });
    CHECK(refresh_error.empty());
    CHECK(stats.files_removed == 1);
    CHECK(chunk_count(store, "docs") == 1);
  }
}

TEST_CASE("knowledge_store: exclude_files drops files by name, not extension")
{
  temp_dir docs;
  docs.write("guide.md", "# Guide\n\nReal documentation.\n");
  docs.write("CMakeLists.txt", "add_library(thing STATIC thing.cpp)\n");
  docs.write("sub/CMakeLists.txt", "add_subdirectory(other)\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (docs.path / "knowledge.db").string();
  settings.exclude_dirs = {};

  {
    mistral::knowledge::hashing_embedder embedder(settings.embedding_dim);
    mistral::knowledge::knowledge_store store(settings, embedder);
    std::string error;
    REQUIRE(store.open(error));
    std::string refresh_error;
    const auto stats = run_async([&]() {
      return store.refresh_corpus("docs", docs.path.string(), refresh_error);
    });
    CHECK(refresh_error.empty());
    // .txt is a document extension, so both CMakeLists.txt files land in
    // the document corpus until a file-name rule excludes them.
    CHECK(stats.files_indexed == 3);
  }

  settings.exclude_files = {"CMakeLists.txt"};
  {
    mistral::knowledge::hashing_embedder embedder(settings.embedding_dim);
    mistral::knowledge::knowledge_store store(settings, embedder);
    std::string error;
    REQUIRE(store.open(error));
    std::string refresh_error;
    const auto stats = run_async([&]() {
      return store.refresh_corpus("docs", docs.path.string(), refresh_error);
    });
    CHECK(refresh_error.empty());
    CHECK(stats.files_removed == 2);
    CHECK(chunk_count(store, "docs") == 1);
  }
}

TEST_CASE("knowledge_store: vendored repositories are indexed as separate sources")
{
  temp_dir docs;
  docs.write("own.md", "# Own\n\nWorkspace document about coroutines.\n");
  // A submodule gitlink: a .git *file*, as git writes for submodules.
  docs.write("vendor/lib/.git", "gitdir: ../../.git/modules/lib\n");
  docs.write("vendor/lib/vendored.md", "# Vendored\n\nVendored document about coroutines.\n");
  // A plain copy with no .git of its own, under a vendor directory.
  docs.write("third_party/other/other.md", "# Other\n\nThird party document about coroutines.\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (docs.path / "knowledge.db").string();

  mistral::knowledge::hashing_embedder embedder(settings.embedding_dim);
  mistral::knowledge::knowledge_store store(settings, embedder);
  std::string error;
  REQUIRE(store.open(error));

  std::string refresh_error;
  const auto stats = run_async([&]() {
    return store.refresh_corpus("docs", docs.path.string(), refresh_error);
  });
  CHECK(refresh_error.empty());
  // The .git gitlink is excluded; the three documents are indexed.
  CHECK(stats.files_indexed == 3);

  const auto sources = store.source_stats("docs", error);
  CHECK(sources.at("workspace") == 1);
  CHECK(sources.at("vendor/lib") == 1);
  CHECK(sources.at("third_party/other") == 1);

  // Filtering by source is exact, not a post-filter over a fixed k.
  std::string search_error;
  const auto vendored = run_async([&]() {
    return store.search("coroutines", "", "vendor/lib", 5, search_error);
  });
  CHECK(search_error.empty());
  REQUIRE(vendored.size() == 1);
  CHECK(vendored[0].source == "vendor/lib");
  CHECK(vendored[0].path == "vendor/lib/vendored.md");

  // No filter sees every source.
  const auto all = run_async([&]() {
    return store.search("coroutines", "", "", 5, search_error);
  });
  CHECK(all.size() == 3);
}

TEST_CASE("knowledge_store: dimension change rebuilds the index")
{
  temp_dir docs;
  docs.write("one.md", "# One\n\nDocument about coroutines.\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (docs.path / "knowledge.db").string();
  settings.embedding_dim = 64;
  {
    mistral::knowledge::hashing_embedder embedder(settings.embedding_dim);
    mistral::knowledge::knowledge_store store(settings, embedder);
    std::string error;
    REQUIRE(store.open(error));
    std::string refresh_error;
    run_async([&]() {
      return store.refresh_corpus("docs", docs.path.string(), refresh_error);
    });
    std::string search_error;
    CHECK(!run_async([&]() { return store.search("coroutines", "", "", 5, search_error); }).empty());
  }

  // Reopen with a different dimension: the old index is wiped and the
  // store keeps working (rebuilt on the next refresh).
  settings.embedding_dim = 128;
  mistral::knowledge::hashing_embedder embedder(settings.embedding_dim);
  mistral::knowledge::knowledge_store store(settings, embedder);
  std::string error;
  REQUIRE(store.open(error));
  std::string refresh_error;
  const auto stats = run_async([&]() {
    return store.refresh_corpus("docs", docs.path.string(), refresh_error);
  });
  CHECK(refresh_error.empty());
  CHECK(stats.files_indexed == 1);
  std::string search_error;
  const auto hits = run_async([&]() { return store.search("coroutines", "", "", 5, search_error); });
  CHECK(search_error.empty());
  REQUIRE(!hits.empty());
  CHECK(hits[0].path == "one.md");
}

// ---- Identity, refresh budget and failure semantics ----

TEST_CASE("knowledge_store: embedder identity change rebuilds the index")
{
  temp_dir docs;
  docs.write("one.md", "# One\n\nDocument about coroutines.\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (docs.path / "knowledge.db").string();

  {
    test_embedder embedder("test/v1", 32);
    mistral::knowledge::knowledge_store store(settings, embedder);
    std::string error;
    REQUIRE(store.open(error));
    std::string refresh_error;
    const auto stats = run_async([&]() {
      return store.refresh_corpus("docs", docs.path.string(), refresh_error);
    });
    CHECK(refresh_error.empty());
    CHECK(stats.files_indexed == 1);
  }

  // Same dimension, different identity: the stored vectors mean
  // something else, so the index must be wiped and rebuilt rather than
  // silently compared against vectors from another space.
  {
    test_embedder embedder("test/v2", 32);
    mistral::knowledge::knowledge_store store(settings, embedder);
    std::string error;
    REQUIRE(store.open(error));
    std::string refresh_error;
    const auto stats = run_async([&]() {
      return store.refresh_corpus("docs", docs.path.string(), refresh_error);
    });
    CHECK(refresh_error.empty());
    CHECK(stats.files_indexed == 1);
    CHECK(stats.files_unchanged == 0);
  }
}

TEST_CASE("knowledge_store: refresh stops at the input budget and reports pending files")
{
  temp_dir docs;
  docs.write("a.md", "# A\n\nAlpha document.\n");
  docs.write("b.md", "# B\n\nBeta document.\n");
  docs.write("c.md", "# C\n\nGamma document.\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (docs.path / "knowledge.db").string();
  settings.max_inputs_per_refresh = 1;

  mistral::knowledge::hashing_embedder embedder(settings.embedding_dim);
  mistral::knowledge::knowledge_store store(settings, embedder);
  std::string error;
  REQUIRE(store.open(error));

  std::string refresh_error;
  const auto first = run_async([&]() {
    return store.refresh_corpus("docs", docs.path.string(), refresh_error);
  });
  CHECK(refresh_error.empty());
  CHECK(first.files_indexed == 1);
  CHECK(first.files_pending == 2);

  // The next refresh picks up where the first stopped.
  const auto second = run_async([&]() {
    return store.refresh_corpus("docs", docs.path.string(), refresh_error);
  });
  CHECK(refresh_error.empty());
  CHECK(second.files_indexed == 1);
  CHECK(second.files_pending == 1);
}

TEST_CASE("knowledge_store: an embed failure leaves the index unchanged")
{
  temp_dir docs;
  docs.write("one.md", "# One\n\nFirst version about coroutines.\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (docs.path / "knowledge.db").string();

  test_embedder embedder("test/fail", 32);
  mistral::knowledge::knowledge_store store(settings, embedder);
  std::string error;
  REQUIRE(store.open(error));

  std::string refresh_error;
  const auto first = run_async([&]() {
    return store.refresh_corpus("docs", docs.path.string(), refresh_error);
  });
  CHECK(refresh_error.empty());
  CHECK(first.files_indexed == 1);
  const int chunks_before = chunk_count(store, "docs");
  REQUIRE(chunks_before >= 1);

  // Change the file, then fail to embed it. Nothing may be written: the
  // stored content hash must stay unchanged so the next refresh retries.
  docs.write("one.md", "# One\n\nSecond version about templates.\n");
  embedder.set_fail(true);
  const auto failed = run_async([&]() {
    return store.refresh_corpus("docs", docs.path.string(), refresh_error);
  });
  CHECK_FALSE(refresh_error.empty());
  CHECK(failed.files_indexed == 0);
  CHECK(chunk_count(store, "docs") == chunks_before);

  // Recover: the file is retried and re-indexed.
  embedder.set_fail(false);
  refresh_error.clear();
  const auto retried = run_async([&]() {
    return store.refresh_corpus("docs", docs.path.string(), refresh_error);
  });
  CHECK(refresh_error.empty());
  CHECK(retried.files_indexed == 1);
}

TEST_CASE("knowledge_store: the code corpus uses the code chunker and its own extensions")
{
  temp_dir docs;
  docs.write("main.cpp", "int main()\n{\n  return 0;\n}\n");
  docs.write("guide.md", "# Guide\n\nA document.\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (docs.path / "knowledge.db").string();

  mistral::knowledge::hashing_embedder embedder(settings.embedding_dim);
  mistral::knowledge::knowledge_store store(settings, embedder);
  std::string error;
  REQUIRE(store.open(error));

  // Documents corpus takes only the .md file.
  std::string refresh_error;
  const auto doc_stats = run_async([&]() {
    return store.refresh_corpus("workspace-docs", docs.path.string(), refresh_error);
  });
  CHECK(refresh_error.empty());
  CHECK(doc_stats.files_seen == 1);
  CHECK(doc_stats.files_indexed == 1);

  // Code corpus takes only the .cpp file, into its own partition.
  const auto code_stats = run_async([&]() {
    return store.refresh_corpus(
      "workspace-code", docs.path.string(), refresh_error, mistral::knowledge::corpus_kind::code
    );
  });
  CHECK(refresh_error.empty());
  CHECK(code_stats.files_seen == 1);
  CHECK(code_stats.files_indexed == 1);

  const auto corpora = store.corpus_stats(error);
  CHECK(corpora.at("workspace-docs") == 1);
  CHECK(corpora.at("workspace-code") == 1);

  // The two corpora do not leak into each other.
  std::string search_error;
  const auto code_hits = run_async([&]() {
    return store.search("main", "workspace-code", "", 5, search_error);
  });
  CHECK(search_error.empty());
  REQUIRE(code_hits.size() == 1);
  CHECK(code_hits[0].path == "main.cpp");
}

// ---- Knowledge tools ----

TEST_CASE("knowledge_tools: refresh_docs and search_docs")
{
  temp_dir docs;
  docs.write("guide.md", "# Guide\n\nHow to build the project with cmake and make.\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = "cache/knowledge.db"; // relative: resolved against the workspace root
  mistral::knowledge::knowledge_tools tools(settings, docs.path.string(), nullptr);

  const auto refresh = run_async([&]() { return tools.refresh_docs(); });
  CHECK(refresh.contains("files_indexed"));
  CHECK(refresh["files_indexed"].get<int>() == 1);
  CHECK(refresh["corpora"].contains("workspace-docs"));
  CHECK(refresh["corpora"]["workspace-docs"].get<int>() >= 1);

  const auto result = run_async([&]() {
    return tools.search_docs("how do I build the project", "", 5);
  });
  CHECK(result.contains("results"));
  REQUIRE(result["results"].size() >= 1);
  const auto& first = result["results"][0];
  CHECK(first["path"].get<std::string>() == "guide.md");
  CHECK(first.contains("line_start"));
  CHECK(first.contains("line_end"));
  CHECK(first.contains("score"));
  CHECK(first.contains("snippet"));
}

TEST_CASE("knowledge_tools: search_docs stays in the document corpus and filters by source")
{
  temp_dir root;
  root.write("guide.md", "# Guide\n\nHow to build the project with cmake.\n");
  root.write("vendor/lib/notes.md", "# Notes\n\nHow to build the vendored library.\n");
  root.write("main.cpp", "// how to build the project with cmake\nint main() { return 0; }\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (root.path / "knowledge.db").string();
  mistral::knowledge::knowledge_tools tools(settings, root.path.string(), nullptr);

  // Index the code corpus too, so the test can prove the documentation
  // tool does not reach into it. The comment in main.cpp matches the
  // query closely enough to surface if it did.
  run_async([&]() { return tools.refresh(mistral::knowledge::corpus_kind::code); });

  const auto all = run_async([&]() { return tools.search_docs("how to build with cmake", "", 10); });
  REQUIRE(all.contains("results"));
  REQUIRE(all["count"].get<int>() >= 1);
  for (const auto& hit : all["results"])
  {
    CHECK(hit["corpus"].get<std::string>() == "workspace-docs");
  }

  // The workspace's own repository has a name, so it can be asked for.
  const auto workspace = run_async([&]() {
    return tools.search_docs("how to build with cmake", "workspace", 10);
  });
  REQUIRE(workspace["count"].get<int>() >= 1);
  for (const auto& hit : workspace["results"])
  {
    CHECK(hit["source"].get<std::string>() == "workspace");
  }

  const auto vendored = run_async([&]() {
    return tools.search_docs("how to build with cmake", "vendor/lib", 10);
  });
  REQUIRE(vendored["count"].get<int>() == 1);
  CHECK(vendored["results"][0]["path"].get<std::string>() == "vendor/lib/notes.md");
}

TEST_CASE("knowledge_tools: search_docs reports the documentation corpus sources")
{
  temp_dir root;
  root.write("guide.md", "# Guide\n\nHow to build the project with cmake.\n");
  root.write("vendor/lib/notes.md", "# Notes\n\nHow to build the vendored library.\n");
  // Code only: this repository is in the index, but it holds no
  // documentation, so advertising it to search_docs would send the caller
  // to an empty result set.
  root.write("vendor/code-only/impl.cpp", "int impl() { return 0; }\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (root.path / "knowledge.db").string();
  mistral::knowledge::knowledge_tools tools(settings, root.path.string(), nullptr);

  run_async([&]() { return tools.refresh(mistral::knowledge::corpus_kind::code); });

  const auto result = run_async([&]() { return tools.search_docs("how to build", "", 5); });

  // Both repositories that hold documentation are reported, so the caller
  // can discover valid values for `source` instead of guessing.
  REQUIRE(result["sources"].is_array());
  bool saw_workspace = false;
  bool saw_vendored = false;
  bool saw_code_only = false;
  for (const auto& entry : result["sources"])
  {
    const auto name = entry["source"].get<std::string>();
    CHECK(entry["chunks"].get<int>() >= 1);
    if (name == "workspace")
    {
      saw_workspace = true;
    }
    if (name == "vendor/lib")
    {
      saw_vendored = true;
    }
    if (name == "vendor/code-only")
    {
      saw_code_only = true;
    }
  }
  CHECK(saw_workspace);
  CHECK(saw_vendored);
  // The code corpus holds it; the documentation corpus does not.
  CHECK_FALSE(saw_code_only);
}

TEST_CASE("knowledge_tools: index_summary calls out repositories only one corpus holds")
{
  temp_dir root;
  root.write("guide.md", "# Guide\n\nHow to build the project.\n");
  // In both corpora: named in the shared list, never in an exception.
  root.write("vendor/lib/notes.md", "# Notes\n\nHow to build the vendored library.\n");
  root.write("vendor/lib/impl.cpp", "int impl() { return 0; }\n");
  // Code only: search_docs has nothing here, so the block must say so.
  root.write("vendor/code-only/impl.cpp", "int impl() { return 0; }\n");
  // Docs only: the reverse, for search_code.
  root.write("vendor/docs-only/readme.md", "# Readme\n\nHow to build the docs-only library.\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (root.path / "knowledge.db").string();
  mistral::knowledge::knowledge_tools tools(settings, root.path.string(), nullptr);

  run_async([&]() { return tools.refresh(mistral::knowledge::corpus_kind::code); });
  run_async([&]() { return tools.refresh_docs(); });

  const std::string summary = tools.index_summary();
  REQUIRE(!summary.empty());

  // Every repository is named once, in the shared list.
  CHECK(summary.find("vendor/code-only") != std::string::npos);
  CHECK(summary.find("vendor/docs-only") != std::string::npos);

  // The asymmetric ones are called out under the tool that cannot reach
  // them; a repository in both corpora is in neither exception list.
  const auto code_only_at = summary.find("code only: ");
  const auto docs_only_at = summary.find("docs only: ");
  REQUIRE(code_only_at != std::string::npos);
  REQUIRE(docs_only_at != std::string::npos);
  REQUIRE(code_only_at < docs_only_at);

  const std::string code_only_part = summary.substr(code_only_at, docs_only_at - code_only_at);
  const std::string docs_only_part = summary.substr(docs_only_at);
  CHECK(code_only_part.find("vendor/code-only") != std::string::npos);
  CHECK(code_only_part.find("vendor/lib") == std::string::npos);
  CHECK(code_only_part.find("vendor/docs-only") == std::string::npos);
  CHECK(docs_only_part.find("vendor/docs-only") != std::string::npos);
  CHECK(docs_only_part.find("vendor/lib") == std::string::npos);
}

TEST_CASE("knowledge_tools: index_summary adds no exception line when both corpora match")
{
  temp_dir root;
  root.write("guide.md", "# Guide\n\nHow to build the project.\n");
  root.write("main.cpp", "int main() { return 0; }\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (root.path / "knowledge.db").string();
  mistral::knowledge::knowledge_tools tools(settings, root.path.string(), nullptr);

  run_async([&]() { return tools.refresh(mistral::knowledge::corpus_kind::code); });
  run_async([&]() { return tools.refresh_docs(); });

  const std::string summary = tools.index_summary();
  REQUIRE(!summary.empty());
  // Both corpora hold exactly the workspace's own repository, so the
  // block stays the single shared list it has always been.
  CHECK(summary.find("code only: ") == std::string::npos);
  CHECK(summary.find("docs only: ") == std::string::npos);
  CHECK(summary.find("or one of: workspace") != std::string::npos);
}

TEST_CASE("knowledge_tools: index_summary names the indexed repositories")
{
  temp_dir root;
  root.write("main.cpp", "int main() { return 0; }\n");
  root.write("vendor/lib/impl.cpp", "int impl() { return 0; }\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (root.path / "knowledge.db").string();
  mistral::knowledge::knowledge_tools tools(settings, root.path.string(), nullptr);

  // No index yet: no summary, and no database created as a side effect.
  CHECK(tools.index_summary().empty());
  CHECK(!fs::exists(root.path / "knowledge.db"));

  run_async([&]() { return tools.refresh(mistral::knowledge::corpus_kind::code); });

  const std::string summary = tools.index_summary();
  CHECK(summary.find("search_code") != std::string::npos);
  CHECK(summary.find("search_docs") != std::string::npos);
  CHECK(summary.find("source") != std::string::npos);
  CHECK(summary.find("workspace") != std::string::npos);
  CHECK(summary.find("vendor/lib") != std::string::npos);
}

TEST_CASE("knowledge_tools: index_summary does not rebuild the index")
{
  temp_dir root;
  root.write("main.cpp", "int main() { return 0; }\n");
  root.write("vendor/lib/impl.cpp", "int impl() { return 0; }\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (root.path / "knowledge.db").string();

  {
    mistral::knowledge::knowledge_tools tools(settings, root.path.string(), nullptr);
    run_async([&]() { return tools.refresh(mistral::knowledge::corpus_kind::code); });
  }

  // Re-open with a different embedding dimension. A normal open() would
  // see the identity mismatch and wipe the index; the summary must not.
  mistral::knowledge::knowledge_settings other_settings = settings;
  other_settings.embedding_dim = 128;
  mistral::knowledge::knowledge_tools other(other_settings, root.path.string(), nullptr);
  CHECK(other.index_summary().find("vendor/lib") != std::string::npos);

  // The index survived: the original settings still see it.
  mistral::knowledge::knowledge_tools original(settings, root.path.string(), nullptr);
  CHECK(original.index_summary().find("vendor/lib") != std::string::npos);
}

TEST_CASE("knowledge_store: refresh_corpus reports per-phase progress")
{
  temp_dir docs;
  docs.write("a.md", "# A\n\nFirst document.\n");
  docs.write("b.md", "# B\n\nSecond document.\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (docs.path / "knowledge.db").string();
  settings.exclude_dirs = {};

  mistral::knowledge::hashing_embedder embedder(settings.embedding_dim);
  mistral::knowledge::knowledge_store store(settings, embedder);
  std::string error;
  REQUIRE(store.open(error));

  std::vector<mistral::knowledge::index_progress> reports;
  store.set_progress_callback([&](const mistral::knowledge::index_progress& progress) {
    reports.push_back(progress);
  });

  std::string refresh_error;
  const auto stats = run_async([&]() {
    return store.refresh_corpus("docs", docs.path.string(), refresh_error);
  });
  CHECK(refresh_error.empty());
  REQUIRE(stats.files_indexed == 2);

  REQUIRE(!reports.empty());

  for (const auto& report : reports)
  {
    // The store never reports idle: only the caller knows whether it will
    // refresh again, so clearing the indicator is the caller's job.
    CHECK(report.phase != mistral::knowledge::index_phase::idle);
    // The tree size is known from the first report onwards, and is
    // constant for the rest of the refresh.
    CHECK(report.files_total == 2);
  }

  // Chunking is the only phase measured in files, so its counter is the
  // one that ends at the candidate count.
  CHECK(reports.front().phase == mistral::knowledge::index_phase::chunking);
  CHECK(reports.front().total == 2);
  CHECK(reports.front().done == reports.front().total);

  // Every phase appears, in pipeline order: embedding never follows a
  // write, because the phases run once each, in sequence.
  bool saw_embedding = false;
  bool saw_writing = false;
  for (const auto& report : reports)
  {
    if (report.phase == mistral::knowledge::index_phase::embedding)
    {
      CHECK_FALSE(saw_writing);
      saw_embedding = true;
    }
    if (report.phase == mistral::knowledge::index_phase::writing)
    {
      saw_writing = true;
    }
  }
  CHECK(saw_embedding);
  CHECK(saw_writing);

  // The last report accounts for every file written.
  CHECK(reports.back().phase == mistral::knowledge::index_phase::writing);
  CHECK(reports.back().done == 2);
  CHECK(reports.back().total == 2);
}

TEST_CASE("knowledge_store: a refresh with nothing to do reports no progress")
{
  temp_dir docs;
  docs.write("a.md", "# A\n\nFirst document.\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (docs.path / "knowledge.db").string();
  settings.exclude_dirs = {};

  mistral::knowledge::hashing_embedder embedder(settings.embedding_dim);
  mistral::knowledge::knowledge_store store(settings, embedder);
  std::string error;
  REQUIRE(store.open(error));

  std::string first_error;
  const auto first = run_async([&]() {
    return store.refresh_corpus("docs", docs.path.string(), first_error);
  });
  REQUIRE(first.files_indexed == 1);

  // search_docs and search_code refresh before every query, so a refresh
  // with nothing to do must stay silent: otherwise the indicator would
  // flash on every search even when the index is already current.
  int reports = 0;
  store.set_progress_callback([&](const mistral::knowledge::index_progress&) { ++reports; });

  std::string second_error;
  const auto second = run_async([&]() {
    return store.refresh_corpus("docs", docs.path.string(), second_error);
  });
  CHECK(second_error.empty());
  CHECK(second.files_indexed == 0);
  CHECK(second.files_unchanged == 1);
  CHECK(reports == 0);
}

TEST_CASE("knowledge_tools: an indexing session suppresses the idle report")
{
  temp_dir root;
  root.write("main.cpp", "int main() { return 0; }\n");
  root.write("vendor/lib/impl.cpp", "int impl() { return 0; }\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (root.path / "knowledge.db").string();
  mistral::knowledge::knowledge_tools tools(settings, root.path.string(), nullptr);

  std::vector<mistral::knowledge::index_progress> reports;
  tools.set_progress_callback([&](const mistral::knowledge::index_progress& progress) {
    reports.push_back(progress);
  });

  // A session spans several refresh_corpus() calls: /index loops, and
  // clearing between rounds would blink the footer once per round.
  tools.begin_indexing();
  run_async([&]() { return tools.refresh(mistral::knowledge::corpus_kind::code); });
  REQUIRE(!reports.empty());
  for (const auto& report : reports)
  {
    CHECK(report.phase != mistral::knowledge::index_phase::idle);
  }

  // A second round with nothing to do stays silent rather than clearing.
  reports.clear();
  run_async([&]() { return tools.refresh(mistral::knowledge::corpus_kind::code); });
  CHECK(reports.empty());

  // Closing the session clears the indicator exactly once.
  tools.end_indexing();
  REQUIRE(reports.size() == 1);
  CHECK(reports.back().phase == mistral::knowledge::index_phase::idle);

  // Outside a session each refresh clears the indicator itself.
  reports.clear();
  run_async([&]() { return tools.refresh(mistral::knowledge::corpus_kind::code); });
  REQUIRE(reports.size() == 1);
  CHECK(reports.back().phase == mistral::knowledge::index_phase::idle);
}

TEST_CASE("knowledge_tools: default db_path resolves under the workspace root")
{
  temp_dir docs;
  docs.write("guide.md", "# Guide\n\nHow to build the project with cmake and make.\n");

  // Default settings keep db_path = ".mistral-cli/cache/knowledge.db",
  // resolved against the workspace root — not doubled under it.
  mistral::knowledge::knowledge_settings settings;
  mistral::knowledge::knowledge_tools tools(settings, docs.path.string(), nullptr);

  const auto refresh = run_async([&]() { return tools.refresh_docs(); });
  CHECK(refresh.contains("files_indexed"));
  CHECK(refresh["files_indexed"].get<int>() == 1);

  CHECK(fs::exists(docs.path / ".mistral-cli" / "cache" / "knowledge.db"));
  CHECK(!fs::exists(docs.path / ".mistral-cli" / ".mistral-cli"));

  const auto result = run_async([&]() {
    return tools.search_docs("how do I build the project", "", 5);
  });
  REQUIRE(result.contains("results"));
  REQUIRE(result["results"].size() >= 1);
  CHECK(result["results"][0]["path"].get<std::string>() == "guide.md");
}

TEST_CASE("knowledge_tools: search_code indexes code and separates repositories")
{
  temp_dir docs;
  docs.write("main.cpp", "int main()\n{\n  return 0;\n}\n");
  // A submodule gitlink, as git writes for submodules.
  docs.write("vendor/lib/.git", "gitdir: ../../.git/modules/lib\n");
  docs.write("vendor/lib/helper.cpp", "int helper()\n{\n  return 42;\n}\n");
  docs.write("guide.md", "# Guide\n\nA document, not code.\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (docs.path / "knowledge.db").string();
  mistral::knowledge::knowledge_tools tools(settings, docs.path.string(), nullptr);

  const auto result = run_async([&]() { return tools.search_code("return a value", "", 5); });
  REQUIRE(result.contains("results"));
  // Only the two .cpp files belong to the code corpus.
  CHECK(result["count"].get<int>() == 2);

  // Both repositories are reported so the caller can discover them.
  REQUIRE(result["sources"].is_array());
  bool saw_workspace = false;
  bool saw_vendored = false;
  for (const auto& entry : result["sources"])
  {
    const auto name = entry["source"].get<std::string>();
    if (name == "workspace")
    {
      saw_workspace = true;
    }
    if (name == "vendor/lib")
    {
      saw_vendored = true;
    }
  }
  CHECK(saw_workspace);
  CHECK(saw_vendored);

  // Filtering by repository returns only that repository.
  const auto vendored = run_async([&]() { return tools.search_code("return a value", "vendor/lib", 5); });
  REQUIRE(vendored["count"].get<int>() == 1);
  CHECK(vendored["results"][0]["source"].get<std::string>() == "vendor/lib");
  CHECK(vendored["results"][0]["path"].get<std::string>() == "vendor/lib/helper.cpp");
}

TEST_CASE("knowledge_tools: refresh selects the corpus by kind")
{
  temp_dir docs;
  docs.write("main.cpp", "int main()\n{\n  return 0;\n}\n");
  docs.write("guide.md", "# Guide\n\nA document.\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (docs.path / "knowledge.db").string();
  mistral::knowledge::knowledge_tools tools(settings, docs.path.string(), nullptr);

  const auto doc_result = run_async([&]() { return tools.refresh(mistral::knowledge::corpus_kind::documents); });
  CHECK(doc_result["corpus"].get<std::string>() == "workspace-docs");
  CHECK(doc_result["files_indexed"].get<int>() == 1);

  const auto code_result = run_async([&]() { return tools.refresh(mistral::knowledge::corpus_kind::code); });
  CHECK(code_result["corpus"].get<std::string>() == "workspace-code");
  CHECK(code_result["files_indexed"].get<int>() == 1);

  // refresh_docs still targets the documents corpus.
  const auto again = run_async([&]() { return tools.refresh_docs(); });
  CHECK(again["corpus"].get<std::string>() == "workspace-docs");
  CHECK(again["files_indexed"].get<int>() == 0);
  CHECK(again["files_unchanged"].get<int>() == 1);
}

TEST_CASE("knowledge_store: oversized files are skipped and counted")
{
  temp_dir docs;
  docs.write("small.md", "# Small\n\nA small document.\n");
  docs.write("huge.md", "# Huge\n\n" + std::string(2048, 'x') + "\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (docs.path / "knowledge.db").string();
  settings.max_file_bytes = 1024;

  mistral::knowledge::hashing_embedder embedder(settings.embedding_dim);
  mistral::knowledge::knowledge_store store(settings, embedder);
  std::string error;
  REQUIRE(store.open(error));

  std::string refresh_error;
  const auto stats = run_async([&]() {
    return store.refresh_corpus("docs", docs.path.string(), refresh_error);
  });
  CHECK(refresh_error.empty());
  CHECK(stats.files_too_large == 1);
  CHECK(stats.files_seen == 1);
  CHECK(stats.files_indexed == 1);
}

TEST_CASE("knowledge_store: a single over-long line is split to fit the chunk ceiling")
{
  temp_dir docs;
  // One 20 KB line, as a minified bundle would have. Sent whole it would
  // exceed the embedding endpoint's 8192-token input limit.
  docs.write("bundle.js", std::string(20000, 'x') + "\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (docs.path / "knowledge.db").string();
  settings.max_chunk_chars = 4000;

  mistral::knowledge::hashing_embedder embedder(settings.embedding_dim);
  mistral::knowledge::knowledge_store store(settings, embedder);
  std::string error;
  REQUIRE(store.open(error));

  std::string refresh_error;
  const auto stats = run_async([&]() {
    return store.refresh_corpus(
      "workspace-code", docs.path.string(), refresh_error, mistral::knowledge::corpus_kind::code
    );
  });
  CHECK(refresh_error.empty());
  CHECK(stats.files_indexed == 1);
  // 20001 chars (20000 plus the newline) / 4000 rounds up to 6 pieces.
  CHECK(stats.chunks_written == 6);
}

TEST_CASE("code_chunker: breadcrumb accumulates nested scopes")
{
  // An anonymous namespace inside a named one: the inner declaration
  // must not hide the outer, which is what a last-declaration-wins
  // breadcrumb did.
  std::string src =
    "namespace outer\n"
    "{\n"
    "  namespace\n"
    "  {\n";
  for (int i = 0; i < 100; ++i)
  {
    src += "    int filler" + std::to_string(i) + " = 0;\n";
  }
  src += "  }\n}\n";

  mistral::knowledge::code_chunker chunker(40, 10);
  const auto chunks = chunker.chunk(src);
  REQUIRE(chunks.size() >= 2);

  const std::string& breadcrumb = chunks[1].breadcrumb;
  CHECK(breadcrumb.find("namespace outer") != std::string::npos);
  CHECK(breadcrumb.find(" > ") != std::string::npos);
}

TEST_CASE("code_chunker: a closed scope leaves the breadcrumb")
{
  std::string src =
    "namespace first\n"
    "{\n";
  for (int i = 0; i < 40; ++i)
  {
    src += "  int a" + std::to_string(i) + " = 0;\n";
  }
  src += "}\n\nnamespace second\n{\n";
  for (int i = 0; i < 40; ++i)
  {
    src += "  int b" + std::to_string(i) + " = 0;\n";
  }
  src += "}\n";

  mistral::knowledge::code_chunker chunker(20, 0);
  const auto chunks = chunker.chunk(src);
  REQUIRE(chunks.size() >= 3);

  // The last chunk is inside the second namespace only.
  const std::string& breadcrumb = chunks.back().breadcrumb;
  CHECK(breadcrumb.find("namespace second") != std::string::npos);
  CHECK(breadcrumb.find("namespace first") == std::string::npos);
}

TEST_CASE("knowledge_store: breadcrumbs can be refreshed without re-embedding")
{
  temp_dir docs;
  std::string src = "namespace outer\n{\n  namespace\n  {\n";
  for (int i = 0; i < 100; ++i)
  {
    src += "    int v" + std::to_string(i) + " = 0;\n";
  }
  src += "  }\n}\n";
  docs.write("a.cpp", src);

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (docs.path / "knowledge.db").string();

  mistral::knowledge::hashing_embedder embedder(settings.embedding_dim);
  mistral::knowledge::knowledge_store store(settings, embedder);
  std::string error;
  REQUIRE(store.open(error));

  std::string refresh_error;
  const auto indexed = run_async([&]() {
    return store.refresh_corpus(
      "workspace-code", docs.path.string(), refresh_error, mistral::knowledge::corpus_kind::code
    );
  });
  CHECK(refresh_error.empty());
  REQUIRE(indexed.chunks_written >= 2);

  std::string rechunk_error;
  const auto stats = run_async([&]() {
    return store.rechunk_breadcrumbs(
      "workspace-code", docs.path.string(), mistral::knowledge::corpus_kind::code, rechunk_error
    );
  });
  CHECK(rechunk_error.empty());
  CHECK(stats.files_updated == 1);
  CHECK(stats.files_skipped == 0);
  CHECK(stats.chunks_updated == indexed.chunks_written);

  // The accumulated scope path survives the round trip through the
  // database, and the vectors were never touched.
  std::string search_error;
  const auto hits = run_async([&]() {
    return store.search("int v", "workspace-code", "", 5, search_error);
  });
  CHECK(search_error.empty());
  REQUIRE(!hits.empty());
  bool saw_outer = false;
  for (const auto& hit : hits)
  {
    if (hit.breadcrumb.find("namespace outer") != std::string::npos)
    {
      saw_outer = true;
    }
  }
  CHECK(saw_outer);
}

TEST_CASE("knowledge_store: embedding requests span file boundaries")
{
  temp_dir docs;
  for (int i = 0; i < 10; ++i)
  {
    docs.write(
      "f" + std::to_string(i) + ".cpp", "int uniqueword" + std::to_string(i) + " = " + std::to_string(i) + ";\n"
    );
  }

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (docs.path / "knowledge.db").string();

  test_embedder embedder("pipeline/batching");
  embedder.set_batch_size(32);

  mistral::knowledge::knowledge_store store(settings, embedder);
  std::string error;
  REQUIRE(store.open(error));

  std::string refresh_error;
  const auto stats = run_async([&]() {
    return store.refresh_corpus(
      "workspace-code", docs.path.string(), refresh_error, mistral::knowledge::corpus_kind::code
    );
  });
  CHECK(refresh_error.empty());
  CHECK(stats.files_indexed == 10);

  // Ten one-chunk files used to cost ten round trips, which made
  // throughput proportional to file count rather than corpus size.
  // They now share a single request.
  CHECK(embedder.request_count == 1);
  CHECK(embedder.inputs_seen == 10);
}

TEST_CASE("knowledge_store: embedding requests run concurrently")
{
  temp_dir docs;
  for (int i = 0; i < 8; ++i)
  {
    docs.write(
      "f" + std::to_string(i) + ".cpp", "int value" + std::to_string(i) + " = " + std::to_string(i) + ";\n"
    );
  }

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (docs.path / "knowledge.db").string();
  settings.embed_concurrency = 4;

  test_embedder embedder("pipeline/concurrency");
  embedder.set_batch_size(1); // one request per chunk
  embedder.set_delay(std::chrono::milliseconds(20));

  mistral::knowledge::knowledge_store store(settings, embedder);
  std::string error;
  REQUIRE(store.open(error));

  std::string refresh_error;
  const auto stats = run_async([&]() {
    return store.refresh_corpus(
      "workspace-code", docs.path.string(), refresh_error, mistral::knowledge::corpus_kind::code
    );
  });
  CHECK(refresh_error.empty());
  CHECK(stats.files_indexed == 8);
  CHECK(embedder.request_count == 8);

  // Four workers over eight requests: all four are in flight at once
  // rather than one after another.
  CHECK(embedder.max_in_flight == 4);
}

TEST_CASE("knowledge_store: batched vectors land on the right chunks")
{
  temp_dir docs;
  for (int i = 0; i < 10; ++i)
  {
    std::string content;
    for (int line = 0; line < 5; ++line)
    {
      content += "int uniqueword" + std::to_string(i) + " = " + std::to_string(i) + ";\n";
    }
    docs.write("f" + std::to_string(i) + ".cpp", content);
  }

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (docs.path / "knowledge.db").string();

  // A tokenizing embedder: test_embedder truncates to the first 32 bytes
  // and these files share that prefix, so it cannot tell them apart.
  // batch_size() is 256, so all ten chunks still go in one request and
  // the cross-file offset path is what is under test.
  mistral::knowledge::hashing_embedder embedder(settings.embedding_dim);

  mistral::knowledge::knowledge_store store(settings, embedder);
  std::string error;
  REQUIRE(store.open(error));

  std::string refresh_error;
  const auto stats = run_async([&]() {
    return store.refresh_corpus(
      "workspace-code", docs.path.string(), refresh_error, mistral::knowledge::corpus_kind::code
    );
  });
  CHECK(refresh_error.empty());
  CHECK(stats.files_indexed == 10);

  // Cross-file batching writes vectors at an offset into one shared
  // array, so a misalignment would attach one file's vector to another
  // file's chunk. Searching for a token unique to one file catches it.
  std::string search_error;
  const auto hits = run_async([&]() { return store.search("uniqueword7", "workspace-code", "", 3, search_error); });
  CHECK(search_error.empty());
  REQUIRE(!hits.empty());
  CHECK(hits[0].path == "f7.cpp");
}

TEST_CASE("knowledge_store: source_stats is scoped to one corpus")
{
  temp_dir root;
  root.write("guide.md", "coroutines and scheduling\n");
  root.write("main.cpp", "int main() { return 0; }\n");
  root.write("vendor/lib/impl.cpp", "int coroutine_impl() { return 0; }\n");

  mistral::knowledge::knowledge_settings settings;
  settings.db_path = (root.path / "knowledge.db").string();

  mistral::knowledge::hashing_embedder embedder(settings.embedding_dim);
  mistral::knowledge::knowledge_store store(settings, embedder);
  std::string error;
  REQUIRE(store.open(error));

  std::string refresh_error;
  const auto doc_stats = run_async([&]() {
    return store.refresh_corpus(
      "workspace-docs", root.path.string(), refresh_error, mistral::knowledge::corpus_kind::documents
    );
  });
  CHECK(refresh_error.empty());
  CHECK(doc_stats.files_indexed == 1);

  const auto code_stats = run_async([&]() {
    return store.refresh_corpus(
      "workspace-code", root.path.string(), refresh_error, mistral::knowledge::corpus_kind::code
    );
  });
  CHECK(refresh_error.empty());
  CHECK(code_stats.files_indexed == 2);

  // Both corpora hold a chunk from the workspace's own repo. Counting
  // across corpora would report 2 for each, which is what search_code
  // used to advertise.
  const auto code_sources = store.source_stats("workspace-code", error);
  CHECK(error.empty());
  CHECK(code_sources.at("workspace") == 1);
  CHECK(code_sources.at("vendor/lib") == 1);

  const auto doc_sources = store.source_stats("workspace-docs", error);
  CHECK(error.empty());
  CHECK(doc_sources.at("workspace") == 1);
  CHECK(doc_sources.count("vendor/lib") == 0);
}

#endif // MISTRAL_ENABLE_KNOWLEDGE
