#ifndef MISTRAL_KNOWLEDGE_TYPES_H
#define MISTRAL_KNOWLEDGE_TYPES_H

#include <string>
#include <vector>

namespace mistral::knowledge
{
  // Which files a corpus holds, and therefore how they are chunked.
  enum class corpus_kind
  {
    documents, // prose: markdown_chunker, include_extensions
    code,      // source: code_chunker, code_extensions
  };

  // Phase of one refresh_corpus() call. The phases have different
  // denominators — files while chunking, chunks while embedding — so a
  // caller must read the counters together with the phase rather than
  // treat them as one running total.
  enum class index_phase
  {
    idle,      // nothing is indexing
    chunking,  // reading, hashing and chunking new and changed files
    embedding, // turning chunks into vectors; the expensive phase
    writing,   // committing one transaction per file
  };

  // Live progress of one refresh_corpus() call. `total` is exact for the
  // current phase: files_seen after the directory walk, the pending
  // chunk count after chunking. It is not a whole-build denominator —
  // the per-refresh input budget spreads a cold build over many rounds.
  struct index_progress
  {
    index_phase phase = index_phase::idle;
    int done = 0;
    int total = 0;
    // Candidate files in the tree, known once the walk finishes and 0
    // before that. Constant for the rest of the refresh, so a frontend
    // can show it as context for the per-phase counters.
    int files_total = 0;
  };

  // ---- Settings ----
  // Configurable via the "knowledge" section of the workspace config.
  struct knowledge_settings
  {
    // Master switch; when false the knowledge tools are not registered.
    bool enabled = true;

    // SQLite database path, relative to the workspace base dir.
    std::string db_path = ".mistral-cli/cache/knowledge.db";

    // File extensions indexed as documents (markdown/text by default).
    std::vector<std::string> include_extensions = {".md", ".txt"};

    // File extensions indexed as source code. Indexed into a separate
    // corpus (workspace-code) so retrieval never mixes prose and code.
    std::vector<std::string> code_extensions = {
      ".c",   ".h",   ".cc",  ".cpp", ".cxx", ".hpp", ".hxx", ".hh",  ".ipp", ".inl",
      ".py",  ".pyi", ".js",  ".mjs", ".cjs", ".ts",  ".tsx", ".jsx", ".rs",  ".go",
      ".java", ".kt", ".kts", ".scala", ".rb", ".php", ".cs",  ".swift", ".sh", ".bash",
      ".zsh", ".lua", ".pl",  ".sql", ".proto", ".cmake",
    };

    // Code chunking: line windows with overlap, snapped to blank lines.
    // Larger than the document chunk size because a code chunk should be
    // a whole function rather than a paragraph.
    int code_chunk_max_lines = 80;
    int code_chunk_overlap_lines = 20;
    int code_chunk_max_chars = 3000;

    // Files larger than this are skipped and counted in
    // refresh_stats::files_too_large. A generated parser table or a
    // bundled asset can be tens of megabytes and would swamp the corpus:
    // in mistral-cli itself, one generated parser.c is 24 MB against
    // 1.9 MB of real project code. 0 disables the cap.
    int max_file_bytes = 1024 * 1024;

    // Hard ceiling on one chunk's size, applied to every chunker's
    // output before embedding. No chunker is trusted to respect its own
    // budget: a single line can be arbitrarily long (a minified bundle
    // in this repo has a 79k-character line), and the embeddings
    // endpoint rejects inputs over 8192 tokens. Anything larger is split
    // into pieces that share the original line range.
    int max_chunk_chars = 4000;

    // Directory names excluded from indexing. Matched against every
    // path component. An entry ending in '*' is a prefix match, so
    // "build*" covers build, build_asan and build_debug.
    //
    // This is a heuristic and cannot know a project's own conventions:
    // a project that gitignores build_asan/ but names it something else
    // entirely will still leak into the index. Honouring .gitignore is
    // the principled version — see doc/code-search-plan.md §14.
    std::vector<std::string> exclude_dirs = {
      ".git",
      ".mistral-cli",
      ".cache",
      ".vscode",
      ".vibe",
      "build*",
      "cmake-build-*",
      "node_modules",
      "__pycache__",
      ".venv",
      "venv",
      ".pytest_cache",
    };

    // File names excluded from indexing, matched against the final path
    // component with the same exact-or-prefix semantics as exclude_dirs.
    //
    // Exists because an extension rule cannot express "this .txt is not
    // a document": CMakeLists.txt ends in .txt and would otherwise be
    // chunked as prose. Empty by default — which files are build
    // configuration rather than documentation is a property of the
    // project, so it belongs in the workspace config, not here.
    std::vector<std::string> exclude_files = {};

    // Target maximum chunk size in characters. Oversized sections are
    // split at paragraph boundaries.
    int chunk_max_chars = 1500;

    // Directory names whose immediate children are indexed as separate
    // sources even when they carry no .git of their own — a plain
    // vendored copy rather than a submodule.
    std::vector<std::string> vendor_dirs = {"vendor", "third_party", "external", "deps"};

    // Embedding vector dimension for the local (hashing) embedder. The
    // remote provider uses embedding_settings::output_dimension; the
    // store always takes the vec0 width from the embedder it is given.
    int embedding_dim = 384;

    // Default maximum number of search results.
    int search_max_results = 10;

    // Chunks embedded per refresh_corpus() call. Bounds the cost of the
    // refresh that search_docs runs before every query: with a network
    // embedder an unbounded cold build would block the turn for minutes.
    // Files left over are reported as files_pending and picked up by the
    // next refresh. Search over a partial index is correct, just
    // incomplete.
    int max_inputs_per_refresh = 256;

    // Embedding requests kept in flight at once during a refresh.
    // Indexing is dominated by network latency rather than by the
    // embedding itself: with one request in flight the process sits idle
    // for the whole round trip, and throughput ends up proportional to
    // the number of files rather than to the size of the corpus. Each
    // concurrent request is a separate HTTP/2 stream on the shared
    // connection, so this costs no extra sockets.
    int embed_concurrency = 8;
  };

  // ---- Embedding provider ----
  // Configurable via the "embeddings" section of the workspace config.
  // Selects the embedder that turns chunks and queries into vectors.
  struct embedding_settings
  {
    // "local" (offline hashing embedder, no network) or "mistral"
    // (codestral-embed through the shared HTTP transport).
    std::string provider = "local";

    // Provider model id (mistral provider).
    std::string model = "codestral-embed-2505";

    // Environment variable holding the API key (mistral provider).
    std::string api_key_env = "MISTRAL_API_KEY";

    // Endpoint (mistral provider). Reuses the chat coordinator when it
    // resolves to the same host, port and key.
    std::string base_url = "https://api.mistral.ai";
    std::string api_version = "v1";

    // Output vector dimension. The model's default is 1536 and the
    // maximum is 3072; dimensions are ordered by relevance, so a smaller
    // value is a quality trade-off rather than a different vector space.
    int output_dimension = 1536;

    // "float" or "int8". int8 shrinks the index 4x once the store uses an
    // int8 vec0 column; until then both parse into float vectors.
    std::string output_dtype = "float";

    // Inputs per embeddings request. The endpoint rejects more than 256.
    int batch_size = 32;

    // Deadline for one embeddings request. The transport has no timeout
    // of its own, so a stalled HTTP/2 stream would otherwise hang the
    // caller forever (observed: /index froze for minutes with an
    // ESTABLISHED connection and no progress).
    int request_timeout_ms = 60000;

    // Total characters one embeddings request may carry. The endpoint
    // limits both per-input tokens and total tokens per request, so
    // batch_size alone is not a safe bound.
    int max_batch_chars = 32000;
  };

  // ---- Document chunk ----
  // One embeddable unit of a document. Line numbers are 1-indexed and
  // inclusive, referring to the source file.
  struct doc_chunk
  {
    std::string path;       // workspace-relative source path
    std::string breadcrumb; // header path, e.g. "C++20 > Coroutines"
    int line_start = 0;
    int line_end = 0;
    std::string text;
  };

  // ---- Search hit ----
  struct search_hit
  {
    std::string corpus;
    // Repository the chunk came from: the workspace-relative path of the
    // nearest enclosing repo root (a submodule or a vendored copy), or
    // empty for the workspace's own repo.
    std::string source;
    std::string path;
    std::string breadcrumb;
    int line_start = 0;
    int line_end = 0;
    std::string snippet;   // beginning of the chunk text
    double distance = 0.0; // cosine distance (0 = identical)
    double score = 0.0;    // cosine similarity (1 = identical)
  };

  // ---- Rechunk statistics ----
  // Result of recomputing breadcrumbs over an already-indexed corpus.
  struct rechunk_stats
  {
    int files_updated = 0;
    int chunks_updated = 0;
    // Files whose chunk boundaries no longer match the stored rows: they
    // need a real re-index, not a breadcrumb patch.
    int files_skipped = 0;
  };

  // ---- Refresh statistics ----
  struct refresh_stats
  {
    int files_seen = 0;
    int files_too_large = 0;
    int files_indexed = 0; // new or changed
    int files_unchanged = 0;
    int files_removed = 0;
    int chunks_written = 0;
    // Changed files skipped because the per-refresh input budget ran
    // out. They are picked up by the next refresh.
    int files_pending = 0;
  };

} // namespace mistral::knowledge

#endif // MISTRAL_KNOWLEDGE_TYPES_H
