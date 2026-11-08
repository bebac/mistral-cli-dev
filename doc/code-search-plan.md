# Semantic code search (Codestral Embed) — plan

**Status:** phases 1-3 implemented and in use. The markdown knowledge store,
`search_docs`, `search_code`, `refresh_docs` and `/index` are live. The
tree-sitter code tools are now compiled out by default
(`MISTRAL_ENABLE_TREE_SITTER=OFF`), which this plan anticipated; they remain
buildable and are documented in `doc/code-tools.md`. The `grep` tool is not
built — `exec` + `grep` covers it. Current state and measurements: §12.3.
What to index: §12.4. Open questions: §12.5.

## 1. Why

Three separate problems, one fix.

1. **The code tools are not used.** In practice agents reach for system
   `grep` via `exec` instead. That is not a prompt failure so much as a
   tooling one: `grep` is instant, exact, works on every file type, and
   prints `file:line:text`. The code tools are C/C++-only, return verbose
   JSON, and two of the five re-parse the entire tree on every call
   (`find_references` and `ast_search` walk and parse every supported file
   each time; only `find_symbol` is incremental, via mtime+size stamps).
2. **C/C++ only.** `symbol_index::is_supported_source` is extension-gated
   and `parsed_source` is hardcoded to `tree_sitter_cpp()`. Every new
   language costs a grammar, a query, a CMake target and a language table
   (see `doc/code-tools.md`, "Adding another language"). This repo also
   contains Vue, JS, Python and shell, none of which the tools can see.
3. **Binary size.** The C++ grammar's parser tables are ~5.6 MB `__TEXT`,
   the largest single component of the ~7.8 MB release binary
   (`doc/code-tools.md`, "Binary size").

An embedding index replaces all three: one index, every language, ~10 ms
queries, no per-language grammar, no parse tables.

## 2. Goals and non-goals

**Goals**

- Semantic search over workspace *code* as well as docs, using
  `codestral-embed` through the existing HTTP transport.
- A native `grep` tool so agents stop shelling out for exact matches.
- Keep the binary small: tree-sitter becomes opt-in, then removable.

**Non-goals**

- Replacing `read`. Fetching a known file or line range stays `read`.
- Type-aware navigation (go-to-definition, find-usages with resolution).
  We never had it — `find_references` matches bare identifiers without
  type resolution — and embeddings do not provide it either.
- Incremental/background indexing in v1 (see §12, phase 2).

**Explicit non-claim:** semantic search does not answer exact-identifier
questions ("where is `patchset_apply` used"). That is what `grep` is for.
Shipping `search_code` *instead of* `grep` would make agents use system
`grep` more, not less.

## 3. What stays

- `lib/knowledge` in its entirety: SQLite + `sqlite-vec`, the `chunks` /
  `chunks_vec` schema, corpus partitioning, content-hash incremental
  refresh, `markdown_chunker`.
- `hashing_embedder` — kept as the offline `"provider": "local"` option.
  It is the only embedder that works without network or API key, and it
  keeps the existing tests network-free.
- `exec_tools::exec` — the `grep` tool is a thin wrapper over it (§8.1).

## 4. The endpoint

### 4.1 What the repo already gives us

`net::http_coordinator_task` is spawned once per `(host, port, api_key_env)`
in `agent_system::start_transport()`. It already supports **non-streaming**
requests: `stream_response_state::finalize()` sends the whole body as one
`data` event followed by `done`, and sets `http_status` on non-2xx
responses. `agent_task_base` uses exactly this path minus `stream`.

So an embeddings call needs no new HTTP client, no new TLS connection, and
no new process machinery:

```cpp
request_ch_->try_send(api::api_request{
  .path = "/v1/embeddings",
  .body = R"({"model":"codestral-embed-2505","input":["..."],"output_dimension":1536})",
  .stream = false,
  .response_ch = ch,
});
// then: co_await ch->recv(this) until done / error
```

`resolve_transport_endpoints()` dedups by `(host, port, api_key_env)`, so an
embeddings endpoint of `api.mistral.ai:443` + `MISTRAL_API_KEY` shares the
existing chat coordinator and costs zero extra connections.

### 4.2 Request and response schema

From `mistral-openapi.yaml` (`POST /v1/embeddings`, line 3013):

| Field | Notes |
|---|---|
| `model` | required |
| `input` | required; string **or** array of strings; **max 256 inputs** |
| `output_dimension` | optional; **default 1536, max 3072** |
| `output_dtype` | `float` \| `int8` \| `uint8` \| `binary` \| `ubinary`; `int8` verified |
| `encoding_format` | `float` \| `base64` — **accepted but ignored** (§4.3) |
| `metadata` | optional object |

`EmbeddingRequest` is `additionalProperties: false` — **do not send extra
fields**.

Response (`EmbeddingResponse`): `{id, object, data: [{object, embedding,
index}], model, usage}`. `index` preserves input order; do not rely on
array order alone. `usage.prompt_tokens` is populated, so real token
counts can be accumulated for a cost report.

### 4.3 Verified against the live endpoint

Probed with `codestral-embed-2505` and a real key. These are measurements,
not documentation — several contradict the published summaries:

| Probe | Result |
|---|---|
| `output_dimension: 512` | 200, 512 floats |
| no `output_dimension` | 200, **1536 floats** — not 3072 |
| `output_dimension: 3072` | 200, 3072 floats |
| 512 vs 1536 vs 3072, same input | 512 is a strict prefix of 1536, which is a strict prefix of 3072 |
| `output_dimension: 9999` | **400**, `"output_dimension must be less than or equal to 3072."` |
| `input: [...]` × 3 | 200, `index` = 0, 1, 2 |
| `input: "..."` (bare string) | 200 |
| 256 inputs | 200 |
| 512 inputs | **400**, `"Too many inputs in request, split into more batches."` |
| `input: []` | **422** (request-shape validation error) |
| `input: [""]` | 200 — an empty string is embeddable |
| `encoding_format: base64` | 200, but the embedding is **still a JSON float list** |
| `output_dtype: int8` | 200, JSON ints (observed −16..46 at 512 dims) |
| 3000 chars of code | `usage.prompt_tokens` = **1222** |

Consequences for the implementation:

- **Default to 1536** — the model's own default, no quality trade-off.
  Expose 512/1024 for a smaller index. Ordered truncation is confirmed, so
  a smaller dimension is a quality trade-off rather than a different
  vector space — but it is still a different index (§6.3).
- **`encoding_format: base64` is a no-op.** Do not build on it; the
  response is always JSON floats. Budget ~1.2 MB of response per batch of
  64 at 1536 dims, and keep `batch_size` modest because of it.
- **Cap batches at 256 inputs** and never send an empty array.
- **`int8` is available** for a 4× smaller index (§9), at the cost of a
  different `vec0` column type — an index-wide choice, so it belongs in
  `embedder_identity`.
- **400 and 422 mean different things**: 400 is a semantic argument error
  (bad dimension, too many inputs), 422 is request-shape validation.
  Surface the `message` field verbatim — it is already actionable.

## 5. Architecture: the embedder seam

The seam already exists in intent — `hashing_embedder.h` says "a neural
embedder can replace it behind the same interface". The interface just has
to become asynchronous and batched.

```cpp
// lib/knowledge/embedder.h
class embedder
{
public:
  virtual ~embedder() = default;

  virtual int dim() const = 0;

  // Identifies the vector space: model id, output_dimension, output_dtype
  // and chunker version. Stored in the index; a mismatch wipes it (§6.3).
  virtual std::string identity() const = 0;

  // Embed a batch. Returns one vector per input, in input order.
  // On failure returns an empty vector and sets error.
  virtual dripcore::awaitable<std::vector<std::vector<float>>>
    embed(std::vector<std::string> inputs, std::string& error) = 0;
};
```

- `hashing_embedder` implements it with no suspension (returns immediately).
- `mistral_embedder` takes a `std::shared_ptr<dripcore::channel<api::api_request>>`
  plus model id and dimension. **It must not take an `agent_system&`:**
  `agent_system.h` already includes `knowledge_types.h`, so the reverse
  dependency would be circular. `agent_system::initialize_tools()` resolves
  the channel and injects it.
- For tests, `mistral_embedder` takes the request channel through a small
  interface so a fake can be injected. The transport is HTTP/2-only, so a
  local test server is not practical; test the request builder and response
  parser as pure functions instead.

### 5.1 Awaiting the transport from a non-task coroutine

`dripcore::channel::recv()` takes a `dripcore::task*`, but `embed_batch()`
is a plain coroutine and there is no task to hand it: tools are dispatched
through `tool_interface::execute()`, which carries no task reference, and
the knowledge store is not a task either. Threading a task down would mean
changing `tool_interface` and every tool implementation.

`recv()` only reads `task->control_block_` to obtain the executor and the
cancellation handle, and `recv_awaiter` tolerates a control block that
nothing ever cancels (every use is guarded by `if (control_block_)`). So
phase 1 built a local placeholder `dripcore::task` subclass, set its
`control_block_->executor_` to `executor::current()`, and awaited
`recv(&placeholder)`. Local, not a member, so two overlapping
`embed_batch()` calls could not clobber each other's awaiter registration.

**That placeholder is gone.** It was the right trade only while a round
trip was guaranteed to finish. Once the transport turned out to have no
timeout — a stalled stream hangs `recv()` forever (§12.1) — waiting on
the channel became the wrong primitive: there is no way to bound it
without a cancellation timer, and `recv_awaiter` reports cancellation by
*throwing* `dripcore::cancelled`, not by returning nullopt.

`embed_batch()` now drains the response with `try_recv()` and a 10 ms
sleep against a deadline. That costs ~200 wakeups for a 2 s request,
which is noise next to the round trip, and it buys a hard timeout with no
new cancellation machinery and no placeholder task.

## 6. Architecture: the async store

### 6.1 Signature changes

| Today | After |
|---|---|
| `std::vector<search_hit> search(...) const` | `dripcore::awaitable<std::vector<search_hit>> search(...)` |
| `bool index_file(...)` | `dripcore::awaitable<bool> index_file(...)` |
| `refresh_corpus(...)` (already a coroutine) | unchanged signature |

`knowledge_tools` and `test/knowledge_test.cpp` follow.

### 6.2 Batching and transaction ordering

`index_file()` currently embeds **one chunk per call** and holds
`BEGIN`…`COMMIT` across the embed (lines 446 → 518 → 557). That is fine for
a hash function and wrong for a network round trip.

New order:

1. Chunk the file.
2. Build embed inputs (`breadcrumb + "\n" + text`, as today).
3. Embed in batches of `batch_size` — **outside** any transaction. Clamp
   to 256 inputs (§4.3) and never send an empty batch (422).
4. `BEGIN`; delete old chunks and vectors; insert new chunks, vectors and
   the file record; `COMMIT`.

Failure semantics improve as a side effect: if embedding fails, nothing is
written for that file, the stored content hash is unchanged, and the next
refresh retries it.

### 6.3 Index identity

`meta` currently stores `schema_version` and `embedding_dim`, and wipes the
index when either differs. Add `embedder_identity` and wipe on mismatch
too. The identity must cover **model id, `output_dimension`,
`output_dtype` and the chunker version** — all four change the meaning of
a stored vector. Without it, switching any of them silently mixes vector
spaces and returns plausible-looking garbage.

### 6.4 Bounded refresh

`search_docs` calls `ensure_ready()` → full workspace refresh before
**every** search. With a local hash embedder that is free; with a network
embedder a cold index costs seconds to minutes, and tool calls are
`co_await`ed inline in `agent_loop`, so it blocks the turn.

v1 mitigation: `max_inputs_per_refresh` (default 256). Index what fits,
return `indexing: {files_pending: N}` alongside the results, and add an
`/index` slash command for an explicit full build. Search over a partial
index is correct, just incomplete — which is the right trade.

`/index` loops until nothing is pending and reports a summary. Refresh
progress is also streamed to the footer while it runs — see §12.1.

Background indexing at startup is the better answer but needs scheduler
and shutdown lifecycle work; defer until the bounded version is measured.

## 7. Chunking

`markdown_chunker` stays for `.md`/`.txt`. Code needs a different splitter,
selected by extension:

- **v1: blank-line-snapped line windows.** No parsing. ~80 lines with ~20
  overlap, snapped to the nearest blank line, with a breadcrumb of the last
  top-level declaration line seen before the window (a cheap regex over
  `^(class|struct|namespace|enum|def|func|function|fn|impl|interface)\b`).
- **Later: declaration-aligned chunks** when tree-sitter is available
  (`MISTRAL_ENABLE_TREE_SITTER=ON`), reusing `parsed_source::definitions()`
  extents. Best of both worlds, but not required for v1.

Chunk size for code is larger than for prose: 3000 chars with 500 overlap
is a reasonable start (the doc's 3000/1000 is fine too; measure).

The point of declaration alignment is that a hit should be *a whole
function*, so the agent does not have to `read` immediately. That is the
single biggest lever on perceived quality versus `grep`.

## 8. Tools

Three tools, each good at one thing. No hybrid retrieval inside
`search_code` — let the agent pick.

| Tool | Answers | Cost | Built? |
|---|---|---|---|
| `grep` | exact / regex, any file type, instant | local, no index | no — `exec` + `grep` |
| `search_code` | "where do we handle X", conceptual, cross-language | vector index | yes |
| `read` | fetch the body once you know where | local | yes |

`grep` is designed below but **not built**. `exec` already runs `grep`
and `policy.json` pre-approves it, so the capability exists; a dedicated
tool would add structured `path:line:text` output, normalised exit codes
(`git grep` returns 1 for "no match", which through `exec` looks like a
failure) and result capping. That is quality, not capability.

### 8.1 `grep` — a `git grep` wrapper

**Why `git grep` and not a hand-rolled scanner.** The hard part is already
written: `exec_tools::exec` does executable resolution, `fork`/`execvp`,
non-blocking pipe reads, a 64 KiB output cap that keeps draining (so the
child never blocks on a full pipe), timeout, and process-group kill. The
wrapper is argv building plus output parsing. And `git grep` solves the
problems a naive scanner gets wrong: `.gitignore` semantics (this repo has
`examples/vue-storybook/node_modules` with thousands of files — the code
tools' answer today is a hand-maintained `exclude_dirs` list), binary
skipping, and speed.

**Why a tool rather than a prompt line.** The current problem is that
agents *choose* grep over the code tools. A tool named `grep` in the
schema gets picked; a paragraph in `sys_prompt.md` gets ignored. It also
gives the policy engine something to allow without allowing arbitrary
`exec`, and returns JSON in the same shape as `search_code`.

**Shape**

```
grep(pattern, path=".", mode="content"|"files"|"count",
     context=0, max_results=200, case_sensitive=true,
     regex="auto"|"fixed"|"extended")
  -> { pattern, mode, matches: [{file, line, text}], count, truncated }
```

`mode="files"` (`-l`) is a common need and far cheaper in tokens.

**The wrapper owns the argv.** The model supplies values, never flags.
Otherwise `--interactive`, `-O` and `--open-files-in-pager` are exposed.

**Sharp edges — each one is a bug if missed**

1. **Untracked files are invisible by default.** The agent writes a file,
   searches for it, gets nothing, concludes the code is absent. Default to
   `--untracked` (tracked + untracked, still honouring `.gitignore`).
2. **Exit codes mean different things.** `0` = match, `1` = *no match*,
   `128` = error. Mapping `1` to `{error: ...}` makes agents retry
   pointlessly. `1` must become `{count: 0, matches: []}`.
3. **Not a repo → 128.** `--no-index` searches the working tree without the
   index. One retry on that specific failure; detecting the repo up front
   costs an extra spawn per call.
4. **Submodules are not searched.** `third_party/*` here are submodules.
   Desirable (less noise) but silently misses code: expose
   `--recurse-submodules` as an opt-in and say so in the description.
5. **Regex flavour.** Default is POSIX *basic*, so `\b`, `\d`, `(?:...)`
   silently match nothing. Use `-F` when the pattern has no metacharacters,
   `-E` otherwise, and surface git's stderr verbatim ("Invalid regular
   expression") instead of swallowing it.
6. **Output parsing.** `path:line:text` is ambiguous when a path contains
   `:`. Parse with `^(.+?):(\d+):(.*)$` and drop a trailing partial record
   when the 64 KiB cap cut mid-line. The `-z`/NUL-separated format may be
   cleaner — verify experimentally before relying on it.
7. **Volume.** `std::string` in this repo returns tens of thousands of
   lines. `-m` per file plus a total cap with `truncated: true`.
8. **Flag hygiene.** `git --no-pager grep --color=never ...` — note
   `--no-pager` is a *git* option and must precede the subcommand, while
   `--color` is a `grep` option. `color.grep=always` in a user's config
   would otherwise inject ANSI into the JSON. Put `--` before the path so
   a leading `-` is not parsed as a flag.
9. **cwd and pathspec.** Run with `cwd` = the requested directory (or the
   file's parent) and pathspec `.` (or the filename). Avoids
   pathspec-relative confusion and absolute paths outside the repo.

Estimated size: ~150–200 lines including tests, because §8.1's first
paragraph is already done.

### 8.2 `search_code`

A thin wrapper over the same store as `search_docs`, differing in corpus
and output:

- corpus `workspace-code` (docs stay in `workspace-docs`)
- returns the **full chunk text** (capped, e.g. 4000 chars), not the
  300-char prefix that `knowledge_store::search()` hardcodes in SQL today
  (`substr(c.text, 1, 300)`). Make that a parameter rather than a constant.
- `path` + `line_start`/`line_end` so the agent can `read` for more.

## 9. Cost and latency

Measured corpus sizes in this repo (`du -sh`): `lib` 1.1 MB, `src` 40 KB,
`test` 1.0 MB, `doc` 112 KB, `templates` 44 KB.

Measured token density: **3000 chars of code = 1222 tokens** (~2.45
chars/token, roughly 1.6× more than a naive 4-chars-per-token guess).

Estimates for a full cold build of code + docs (~1.2k chunks at 3000/500
for code and 1500 for markdown):

| Quantity | Estimate |
|---|---|
| Tokens | ~1.15M |
| Cost at $0.15/M | **~$0.17** |
| Index size at 1536 dims (default) | ~7 MB |
| Index size at 512 dims | ~2.5 MB |
| Index size at 1536 dims, `int8` | ~1.8 MB |
| Round trips at batch 32 | ~38 |
| Cold build wall time | ~10–15 s |

Cost is a non-issue. **First-call latency is the issue** — hence §6.4.
Incremental refresh after the cold build is near-zero because unchanged
files are skipped by content hash, which is the number that matters.

`usage.prompt_tokens` comes back on every request, so `/index` can report
the real token count and cost instead of an estimate.

`sqlite-vec` in `third_party/` supports `float32`, `int8` and `bit` element
types, and `output_dtype: int8` is verified working — a 4× index shrink is
available if size becomes a problem.

## 10. Configuration

New `embeddings` section, plus an extended `knowledge` section:

```json
{
  "embeddings": {
    "provider": "mistral",
    "model": "codestral-embed-2505",
    "api_key_env": "MISTRAL_API_KEY",
    "base_url": "https://api.mistral.ai",
    "api_version": "v1",
    "output_dimension": 1536,
    "batch_size": 32,
    "max_inputs_per_refresh": 256
  },
  "knowledge": {
    "enabled": true,
    "db_path": ".mistral-cli/cache/knowledge.db",
    "search_max_results": 10,
    "corpora": {
      "workspace-docs": { "include_extensions": [".md", ".txt"], "chunk_max_chars": 1500 },
      "workspace-code": {
        "include_extensions": [".cpp", ".h", ".hpp", ".c", ".py", ".js", ".ts", ".vue", ".sh"],
        "chunk_max_chars": 3000,
        "chunk_overlap": 500
      }
    }
  }
}
```

`"provider": "local"` selects `hashing_embedder` (offline, no key).

Operational notes, from `doc/code-tools.md`:

- **Seeded files never self-update.** `templates/` is the source of truth
  for new workspaces; existing `.mistral-cli/` copies must be edited by
  hand.
- A tool must be listed in the agent's `available_tools` **and** allowed in
  `policy.json` (default policy is `require_approval`).
- `grep` is read-only, so allow it by default:
  `{ "type": "tool", "tool": "grep" }`. Note `templates/policy.json`
  already allows the `grep` *executable* (line 30), so this is consistent
  with existing intent.

## 11. Testing

- **Pure functions:** request builder (model, `input` array,
  `output_dimension`, no extra fields) and response parser (`data[].index`
  ordering, float arrays, error bodies). No network.
- **Store:** existing `test/knowledge_test.cpp` keeps running against
  `hashing_embedder`; add cases for batching, embed-then-write ordering
  (a failing embed leaves the file unindexed and retryable), and
  `embedder_identity` mismatch wiping the index.
- **`grep`:** argv construction per mode, exit-code mapping (0/1/128),
  `path:line:text` parsing including a path containing `:`, truncation,
  and a non-repo fallback. Runs against a temp git repo — no network.
- **Live:** an opt-in test hitting the real endpoint, gated the same way as
  `MISTRAL_ENABLE_LIVE_MCP_TESTS` (off by default, skips without a key).

## 12. Phases

| Phase | Deliverable | Touches |
|---|---|---|
| 0 | ~~Confirm the endpoint~~ — **done**, see §4.3 | nothing |
| 1 | ~~`embedder` interface; `hashing_embedder` adapted; `mistral_embedder` + pure JSON functions; `embeddings` config; endpoint registration + request-channel accessor~~ — **done** | `lib/knowledge/`, `lib/agent/agent_system.{h,cpp}`, `lib/agent/agent_config.h` |
| 2 | ~~Async store: coroutine `search`/`index_file`, batching, embed-then-write, `embedder_identity`, bounded refresh, `/index`~~ — **done** | `lib/knowledge/`, `lib/agent/`, tests |
| 3 | Code chunker, `workspace-code` corpus, `search_code` — **done**; `grep` tool not built (§8) | `lib/knowledge/`, `lib/tools/`, `lib/agent/agent_system.cpp` |
| 4 | Measure against the current stack on real tasks — **inconclusive so far**; short sessions in a small repo do not exercise it | nothing |
| 5 | ~~Default `MISTRAL_ENABLE_TREE_SITTER=OFF`; rewrite the "Codebase Searching" section of `sys_prompt.md`~~ — **done**. The code tools were deliberately left in `templates/mistral-cli.json` and `policy.json`: harmless when the tools are absent, and they make re-enabling work without config edits | templates, docs |

Phase 2 is preceded by a transport slice, already done — see
`doc/transport-refactor-plan.md`. It replaced the bespoke
`embeddings_request_ch()` added in phase 1 with a generic
`agent_system::transport_add(uri, api_key_env)`, so the agent layer no
longer names the embeddings backend. `initialize_tools()` now holds the
embeddings channel in `embeddings_ch_`, ready for the embedder.

Phases 1–2 change no behaviour (still `local`); the switch happens in
phase 3. `-DMISTRAL_ENABLE_TREE_SITTER=OFF` already builds today, so the
size win is available immediately — the work is making the default config
not *depend* on those tools.

### 12.1 What phase 2 changed beyond the plan

- **`max_batch_inputs()` became `batch_size()`.** The store needs one
  number ("how many inputs per call"), not two overlapping concepts. Each
  embedder still enforces its own hard limit inside `embed_batch()`.
- **The store borrows the embedder and takes its width from `dim()`.**
  `knowledge_settings::embedding_dim` is now the *local* provider's
  dimension only; the remote provider uses
  `embedding_settings::output_dimension`. This removes the duplication
  flagged in §10.
- **`schema_version` went 1 → 2.** Existing indexes have no
  `embedder_identity`, so they rebuild once on first open. For the local
  provider that is a re-hash of the doc corpus — negligible.
- **`/index` reports a summary, and refresh progress now reaches the
  footer.** It still loops until `files_pending` is 0 (each round
  indexes at least one file, so it terminates) and returns totals, but
  `refresh_corpus()` also reports an `index_progress` through a callback
  while it runs, which `agent_system` forwards as `ui_status::index`.

  The footer's **left** slot is replaced for the duration of the build:
  `⠹ indexing 128/512 files` while scanning, then `⠹ indexing 512 files ·
  embed 96/256` once the file count has settled. The file fraction is
  the progress only while chunking, the one phase measured in files;
  afterwards `files_total` is a constant — the size of the tree — and
  the phase counter carries the movement.

  Left rather than centre on purpose: a centred string of changing width
  moves its own start position, so the whole footer appears to jump as
  the counter advances. Left-aligned it grows to the right from a fixed
  origin and the centre — agent, model, MCP servers — stays put. The
  cost is that `FooterBar` now clips the left text one column short of
  the centre, because the centre is drawn over it and would otherwise
  overdraw it mid-word.

  The counters are **per phase**, never a whole-build percentage:
  `chunking` counts files against `files_seen` (known once the walk
  finishes), `embedding` counts chunks against `pending_chunks` (known
  once chunking finishes), `writing` counts files against the pending
  set. A cold build spans many rounds and each round's chunk count is
  unknown until its own walk completes, so no global denominator exists.

  Three details are load-bearing:

  - **The store never reports `idle`.** It cannot tell whether the caller
    will refresh again — `/index` loops — so `knowledge_tools` clears the
    indicator after each `refresh_corpus()` call.
  - **`/index` brackets its loop with `begin_indexing()`/`end_indexing()`.**
    Without the session the indicator would clear once per round and the
    footer would blink between them. A `session_guard` in
    `index_command::execute` closes it on every exit path.
  - **Chunking reports only for a file that was queued.** The report
    sits at the end of the phase-1 loop body and every skip path
    `continue`s past it, so a refresh with nothing to do emits nothing.
    `search_docs`/`search_code` refresh before every query, so a report
    per search would flash the indicator even when the index is current.

  The spinner frame is advanced by the existing 200 ms footer timer
  rather than by a status snapshot, because a phase reports only once
  per embed request and those have a 60 s deadline — a frame driven by
  snapshots alone would sit frozen.

- **Embeddings requests have a deadline (`request_timeout_ms`, default
  60 s).** The transport has no timeout of its own: a stalled HTTP/2
  stream never reaches `stream_closed`, so
  `stream_response_state::finalize()` never runs and a blocking
  `channel::recv()` waits forever. This was observed in practice —
  `/index` froze for minutes on an ESTABLISHED connection with zero
  progress. `embed_batch()` therefore drains the response with
  `try_recv()` plus a deadline instead of `recv()`, which also removed
  the phase-1 placeholder-`task` workaround (§5.1). A closed-but-empty
  response channel fails immediately rather than waiting out the
  deadline.

  Still open: **the same exposure exists on the chat path**
  (`agent_task_base` has no timeout either). Chat is user-driven so a
  stall is visible, but it is not fixed here.

### 12.2 Phase 3 so far

- **Code chunker** (`lib/knowledge/code_chunker.{h,cpp}`): line windows
  (80 lines, 20 overlap) snapped backwards to a blank line so a window
  does not cut a function in half; breadcrumb from the last top-level
  declaration; 3000-character budget. Breadcrumbs are precomputed once
  per line — rescanning from the top for every window is O(n²), which on
  a 5000-line file means ~310k regex searches. When the character budget
  truncates a window, the next one resumes exactly where it stopped, so
  no lines are skipped.
- **`corpus_kind`** selects both the extension list and the chunker:
  `workspace-docs` uses `markdown_chunker` over `include_extensions`,
  `workspace-code` uses `code_chunker` over `code_extensions`.
- **`source` dimension.** A chunk's repository is the nearest enclosing
  directory holding a `.git` entry — a `.git` *file* for a submodule, a
  *directory* for a vendored checkout — or a direct child of
  `vendor`/`third_party`/`external`/`deps` for a copy with no `.git` of
  its own. Files in the workspace's own repo get `""`.
  It is a **second vec0 partition key**, so filtering by repository
  happens inside the KNN and stays exact; post-filtering a `k`-row result
  would silently return fewer than `k` hits. Vendored code is indexed by
  default and separated by `source` rather than excluded.
- Schema is now **v3**.

Phase 3 is complete except the `grep` tool, which is **not built**: `exec`
already runs `grep` and `policy.json` pre-approves it, so the capability
exists. A dedicated tool would add structured output, normalised exit
codes and result capping rather than capability — see §8.

### 12.3 Current state (2026-11-03)

**Implemented and tested.** 877 tests pass with knowledge enabled, 818
without; both configurations build clean, both with tree-sitter off.

- Tools: `search_docs`, `search_code`, `refresh_docs`, and `/index`.
- Corpora: `workspace-docs` (`markdown_chunker` over `include_extensions`)
  and `workspace-code` (`code_chunker` over `code_extensions`), selected
  by `corpus_kind`.
- `source` dimension: a second vec0 partition key, so per-repository
  filtering is exact rather than a post-KNN approximation. Vendored code
  is indexed by default and separated by `source`, not excluded.
  `source_stats()` is scoped to one corpus — counting across them merged
  documentation chunks into every repository's total, so `search_code`
  advertised repositories as larger than the code they hold.
- Schema v3. Breadcrumbs accumulate a scope path
  (`namespace foo > struct bar`) from brace depth.
- `rechunk_breadcrumbs()` rewrites breadcrumbs in place without
  re-embedding; `/index` runs it last.
- **Indexing reports live progress to the footer.** `refresh_corpus()`
  reports an `index_progress` (phase, done/total, tree size) through a
  callback; `agent_system` forwards it as `ui_status::index` and the TUI
  replaces the footer centre with `⠹ indexing 512 files · embed 96/256`
  for the duration of the build. §12.1 explains why the counters are per
  phase rather than a percentage.
- **The tree-sitter code tools are compiled out by default**
  (`MISTRAL_ENABLE_TREE_SITTER=OFF`). The prompt section describing them
  lives in `templates/sys_prompt_code_tools.md` and is included only in
  builds that have them (`{{> ... if=tree_sitter_enabled}}`), so the
  agent is never told about tools that are not registered. The subsystem
  still builds, and `doc/code-tools.md` documents it.

**Measured on this repo** — currently 930 files, 6266 chunks. The timings
below were taken at ~6200 chunks:

| Embedder | Full build |
|---|---|
| Local hashing (384d) | ~10 s |
| codestral-embed (1536d) | ~15–20 min |

Retrieval quality with codestral-embed is qualitatively better: the top
four hits for a chunker query are the chunker's own source at 0.73–0.66,
where the local hasher ranked an unrelated test file first at 0.28.

**Two API limits, both found by experiment, both now guarded:**

| Limit | Value | Guard |
|---|---|---|
| Tokens per input | 8192 | `max_chunk_chars = 4000` (store) |
| Tokens per request, total | ~16384 (undocumented) | `max_batch_chars = 32000` (embedder) |

The per-request total is not in Mistral's documentation; ~16384 comes
from their cookbook. Both are configurable. The per-input limit was
tripped by a 79k-character line in a minified bundle; the per-request
limit by 32 full-size chunks in one call.

**Performance.** Indexing used to be fully sequential and batched per
file, so a one-chunk file cost a whole round trip and throughput scaled
with *file count* rather than corpus size — ~0.7 files/s, and over a day
for a Linux-kernel-sized tree.

`refresh_corpus` is now a three-phase pipeline:

1. **read, hash, chunk** every changed file — all local;
2. **embed**, with requests formed across file boundaries and
   `knowledge.embed_concurrency` (default 8) in flight at once;
3. **write**, one transaction per file, serially — a single SQLite
   connection cannot run concurrent transactions.

Requests are bounded by both `batch_size` and `max_batch_chars`, so a
256-chunk refresh is ~8 requests instead of up to 256. Workers hold a
shared reference to the job, so a timeout cannot leave them writing into
a destroyed frame; the collector also drains every outcome after a
failure, which keeps the first error rather than whichever arrived
first.

`embed_once` retries transport failures ("connection closed", "connection
error", "stream reset", "send error") up to four times with linear
backoff, mirroring `agent_task_base::is_retryable_error`. Without it a
single dropped connection — a normal event on a long-lived HTTP/2
connection, since servers recycle them — aborted the whole build and
lost the round.

**Measured** — 146 files / 531 chunks (everything under `lib/`), same
machine and endpoint:

| | Wall time | Throughput |
|---|---|---|
| Sequential, per-file batches | ~209 s (extrapolated from 0.7 files/s) | 0.7 files/s |
| Pipelined | **9.7 s** | 15.1 files/s |

~21x. The embedding phase alone improved by more than that: the 9.7 s
also covers four tree walks — one per round, each re-hashing all 929
files — plus a 6237-chunk breadcrumb rewrite.

**Next bottleneck.** Because embedding is now fast, the per-round tree
walk and re-hash is a visible fraction of the total, and it is paid once
per round. `max_inputs_per_refresh` (256) sets the round count, so
raising it is the cheap lever: 531 chunks took four rounds where one
would have done.

**Unverified:** the HTTP/2 client does not track the server's
`SETTINGS_MAX_CONCURRENT_STREAMS`, so the real ceiling on concurrency is
Mistral's, not ours. 8 is modest and configurable; if requests begin
failing with stream errors, lower it.

### 12.4 What to index

Measured on the code corpus (4782 chunks, 719 files) but deliberately
not acted on yet: the inclusion policy needs an evaluation harness
before it changes, not intuition.

| Category | Chunks | Share |
|---|---|---|
| Vendored (submodules and vendor dirs) | 3487 | 73% |
| The workspace's own code | 1295 | 27% |
| Test code (path contains "test") | 1432 | 30% |
| Files with more than 100 chunks | 1529 | 32% |
| Gitignored build output | 151 | 3% |
| Byte-identical duplicates | 127 | 2.7% |

**Vendored code is the bulk, and is meant to stay.** It is separable by
`source`, so the question is ranking rather than exclusion.

**A few files dominate.** `nlohmann/json.hpp` is 415 chunks,
`cpp-tui/cpptui.hpp` 306, `sqlite-vec.c` 171 — amalgamated headers that
are really whole libraries in one file. They are not junk, but they
swamp ranking.

**Test code is 30%.** Three near-identical copies of `doctest.h` account
for 376 chunks. They differ slightly, so content-hash deduplication
would not remove them — it finds only 127 chunks (2.7%) overall.

**`.gitignore` awareness works, but not via `git ls-files`.** The
storybook bundle (151 chunks of minified JS) is gitignored by
`examples/vue-storybook/.gitignore`, so §14 would correctly drop it.
But `git ls-files` reports a submodule as a single gitlink — it returns
`third_party/cpp-tui`, not the files inside it. Filtering the workspace
by top-level `git ls-files` would therefore discard 73% of the corpus,
which is exactly what must be kept. Any §14 implementation has to run
per repository: `git -C <source_root> ls-files` for each detected
source, and the top-level command only for the workspace's own repo.

**Candidate policies, cheapest first:**

1. *Ranking, not exclusion* — keep everything, weight the workspace's
   own code above vendored. No recall loss, fully reversible.
2. *Path lists* — configurable test and build-output directories
   (`test/`, `storybook-static/`, `dist/`). Predictable, but another
   list to maintain and drift.
3. *Content heuristics* — skip minified or generated files by average
   line length or a `@generated` marker. Catches the storybook bundle
   and generated parser tables without configuration.
4. *`.gitignore` per repository* — the principled version, subject to
   the submodule constraint above.

**Before choosing:** build a small known-answer query set (a dozen
questions whose correct file is known, e.g. "where does the code chunker
track its scope stack") and score each policy by top-5 hit rate. Without
that, any of the above is a guess, and the risk is silently losing
recall that the current index provides.

### 12.5 Open questions

- Should the implicit refresh before every `search_docs`/`search_code`
  call be throttled? It costs a full tree walk and content hash of all
  930 files (~0.5 s) before each query, and it is the same budget
  (`max_inputs_per_refresh`) that bounds `/index`. Splitting the two
  budgets and throttling the implicit one are separate decisions.
  Worth knowing: throttling would shorten the *user's* wait, not change
  the agent's tool choice — an agent gets no per-call timing back, so it
  cannot prefer the faster tool. Adoption is a prompt problem, not a
  latency one.
- Is the ~1.5 s breadcrumb rewrite on every `/index` worth running when
  nothing was indexed? It is what makes a chunker change self-heal, but
  it is a fixed cost on a no-op run.

**Deferred:**

- `grep` tool — see §8. Not built; `exec` + `grep` covers exact matching
  and the policy pre-approves it.
- `.gitignore`-driven exclusion (§14). Until then `exclude_dirs` matches
  any path component at any depth, so `"grammars"` also excludes
  `src/grammars`. Path-anchored entries (`third_party/grammars`) were
  designed and deferred together with this.
- Unifying the two `exclude_dirs` lists (`code_index` and `knowledge`).
  They have already drifted once, silently disabling the `build`
  exclusion for the tree-sitter tools. Both now share
  `utils::matches_name_pattern`, but the config duplication remains.
- Result diversity: 4 of 5 hits can come from one file. Per-file
  deduplication or MMR would fix it.
- What to index — see §12.4. Measured but not yet acted on.
- **One unbounded index pass.** `/index` loops the *bounded*
  `refresh_docs()`, and `refresh_corpus()` re-walks the tree and
  re-hashes every file on each call. The pipeline made embedding fast, so
  this per-round walk is now the dominant fixed cost: 531 chunks took
  four rounds, i.e. four full walks of 930 files. One unbounded pass
  (single walk, all batches) is the fix; raising `max_inputs_per_refresh`
  is the cheap partial version.

## 13. Risks

| Risk | Mitigation |
|---|---|
| Embeddings need network + API key; tree-sitter is fully local | `"provider": "local"` fallback; `grep` and `read` stay local |
| First search blocks the turn during a cold build | Bounded refresh (§6.4), `/index`, then background indexing |
| Semantic search is worse than expected on identifier queries | `grep` is the answer for those; measure in phase 4 before deleting anything |
| `git grep` misses submodules / untracked files | only applies if the `grep` tool is built (§8); `exec` + `grep` is the current path |
| Index silently mixed across models | `embedder_identity` in `meta`, wipe on mismatch |
| Stalled HTTP/2 stream hangs the caller forever | `request_timeout_ms` deadline in `embed_batch()` (§12.1); **chat path still exposed** |
| `output_dimension` out of range | Verified: 400 with an actionable message; surface it verbatim |
| `encoding_format: base64` silently ignored | Verified; do not depend on it, budget for JSON floats |

## 14. Inclusion policy (open)

What enters the index is decided by two name-based heuristics:
`include_extensions` / `code_extensions`, and `exclude_dirs`. They got it
badly wrong in practice.

**Observed failure.** In `basic-cpp-server-spa` the index held 686 files:
492 from `build_asan/`, 172 from `build_debug/`, 16 from `vendor/`, and
**6 of real project content**. `exclude_dirs` listed `"build"` and
matched path components exactly, so `build_asan` and `build_debug` were
indexed in full — CMake caches, `CMakeFiles` probes, CTest logs. That is
13.6 MB and ~4700 chunks of noise, and it is why the first `/index` took
~146 requests.

**Fixed so far.** `exclude_dirs` entries ending in `*` are prefix
matches, and the default now carries `build*`, `cmake-build-*`, `.venv`,
`venv` and `.pytest_cache`. Newly excluded files are purged by the next
refresh automatically: `refresh_corpus()` removes indexed paths absent
from the current `seen` set, and excluded files never enter it.

**The principled fix: honour `.gitignore`.** That project's own
`.gitignore` already lists `/build/`, `/build_debug/` and `/build_asan/`
— the author had declared exactly the directories that leaked in. A
project knows its own conventions; a hardcoded name list never will.

Sketch: `git ls-files -z --cached --others --exclude-standard` yields
every tracked and untracked non-ignored file with git's exact semantics
(nested `.gitignore`s, negation, `**`), and the walk intersects against
it. Fall back to the current walk when the root is not a git repository.

**It must run per repository.** `git ls-files` reports a submodule as a
single gitlink — it returns `third_party/cpp-tui`, not the files inside
it — so intersecting the whole workspace against the top-level command
would discard 73% of the corpus, which is exactly what must be kept. Run
it once per detected `source` root (`git -C <source_root> ls-files`) and
use the top-level command only for the workspace's own repo. See §12.4.

Other candidates: skipping files carrying a `@generated` / `DO NOT EDIT`
marker, and skipping binary content. A file-size cap is already
implemented — `knowledge.max_file_bytes`, default 1 MB — added after a
24 MB generated parser table landed in the index.

## 15. Decisions taken

- Keep tree-sitter behind its existing flag, default **OFF**; delete the
  code tools only after phase 4 measures the replacement.
- Index **docs and code** in v1, as separate corpora.
- Add a native **`grep`** tool wrapping `git grep`, rather than a
  hand-rolled scanner or a prompt instruction. **Not built** — `exec` +
  `grep` covers it (§8).
- No hybrid lexical boost inside `search_code` — `grep` covers exact
  matching.
- `grep` allowed by default in `policy.json`.
