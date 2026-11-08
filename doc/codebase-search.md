# Codebase search: options and setup

Status: decision recorded. This document summarises the code-search and
indexing experiments and how to set up each of the four options we built.

## Decision

The default build ships **no built-in code search or indexing**. Code
search is delegated to an **external MCP server** — `probe` in the current
workspace, with XERJ as the richer alternative.

Rationale:

- The built-in tree-sitter code tools roughly triple the binary size
  (measured below) and only cover C/C++.
- The `sqlite-vec-knowledge` experiment is promising for semantic
  "where is the code that does X" queries, but it is slow to build on
  large trees and weak at grep-like / exact-identifier lookups.

`MISTRAL_ENABLE_TREE_SITTER` is still ON by default on `main`; the intent
is to flip it to OFF. It is already OFF in this workspace.

## The options at a glance

| # | Option | Lives in | Index | Languages | Setup cost |
|---|--------|----------|-------|-----------|------------|
| 1 | Built-in `code_tools` (tree-sitter) | this repo | in-memory, per call | C/C++ | +~5.7 MiB binary |
| 2 | XERJ (MCP) | external Rust binary | persistent node | all files | run a node + index |
| 3 | probe (MCP) | external npm package | on demand | all files | `npx`, zero setup |
| 4 | `sqlite-vec-knowledge` | experimental branch | persistent sqlite-vec DB | all files (code + docs) | embedding API + index build |

---

## 1. Built-in `code_tools` (tree-sitter)

### What it is

Five tools, compiled in only when `MISTRAL_ENABLE_TREE_SITTER` is ON:

| Tool | Question |
|------|----------|
| `outline` | "what is in this file?" |
| `read_symbol` | "show me the code of X" |
| `find_symbol` | "where is X defined?" |
| `find_references` | "where is X defined and used?" |
| `ast_search` | "match an AST pattern" |

tree-sitter plus the C++ grammar parse the sources, so matches are real
code — never comments, strings or preprocessor directives. The symbol
index is in-memory and refreshed incrementally per call (mtime + size
stamps); it is not persisted to disk. Sources: `lib/tools/code_tools.{h,cpp}`,
`lib/tools/code_index.{h,cpp}`, queries in `queries/` and the grammar's
`queries/tags.scm`.

### Enable it

```sh
git submodule update --init --recursive      # tree-sitter + tree-sitter-cpp
cmake -S . -B build -DMISTRAL_ENABLE_TREE_SITTER=ON
cmake --build build -j
```

On `main` the option already defaults to ON, so `-D` is only needed to
override a cached OFF.

Config (`mistral-cli.json`):

```json
"code_index": {
  "exclude_dirs": [".git", ".mistral-cli", ".cache", ".vscode", ".vibe",
                   "build*", "cmake-build-*", "node_modules", "__pycache__",
                   ".venv", "venv", ".pytest_cache", "grammars"],
  "max_results": 200
}
```

- Add the five tool names to the agent's `available_tools`.
- Allow them in the policy: `outline`, `read_symbol`, `find_symbol`,
  `find_references`, `ast_search`.
- The system prompt's "Codebase Searching" section
  (`templates/sys_prompt.md`) documents them. When the feature is off the
  tools are absent and that section is misleading, so trim it too.

### Cost (measured, Release, AppleClang, this machine)

```
mistral-cli, tree-sitter OFF   2.63 MiB
mistral-cli, tree-sitter ON    8.33 MiB    (+5.70 MiB, ~3.2x)
```

The bulk is the C++ grammar: `third_party/grammars/tree-sitter-cpp/src/parser.c`
is 25.8 MB of C and links to a 5.6 MB static library. Build time grows
with it.

### What we learned (commit `4ecdaa6`)

Agents did not use the tools. In one long session with ~226 tool calls
(90 `exec`, 64 `read`) the code tools were invoked **zero** times. The
causes were structural, not cosmetic:

- `find_symbol` / `find_references` rejected file paths, so the most
  natural call ("where is X in this file?") errored and pointed at a
  different tool. Both now accept a file or a directory.
- `find_symbol` returned locations but no source, so seeing a hit cost a
  second `read`. Each match now carries the matched line's text.
- `find_references` excluded definitions, so "where is X defined and
  used?" needed two calls against grep's one. Definitions are now
  included by default (`is_definition: true`), with
  `include_definitions=false` restoring the old behaviour.
- The prompt taught grep first: four mentions and nine JSON examples
  before the tools appeared once. The "Codebase Searching" section now
  precedes "Exec Tool", opens with a question-to-tool table, and carries
  worked examples.

### Assessment

Precise for C/C++ symbol navigation and needs no external process, but a
heavy binary and C/C++ only. This is why it is being turned off by
default.

---

## 2. XERJ (MCP)

### What it is

XERJ is an external search engine (Rust binary, Elasticsearch-compatible
API) with an MCP server. It indexes a folder into one or more datasets and
exposes lexical, semantic, vector and hybrid search, plus a durable agent
memory and a "brain" link index. Upstream:
<https://github.com/xerj-org/xerj> and <https://xerj.org>.

### Install and index

Install the `xerj` binary per the upstream README (on this machine it
lands in `~/.local/bin/xerj`). Then start the node and index the
workspace:

```sh
xerj --insecure --data-dir ~/xerj-data &          # the search node
xerj autoindex . --prefix mistral-cli-dev-main    # index this folder
```

`--insecure` disables auth for a local node. Use a **distinct `--prefix`
per source folder**: a shared prefix makes XERJ refuse the run ("this
folder needs its own --prefix") because it would overwrite another
folder's datasets.

### Configure the MCP server

This is the entry in `templates/mistral-cli.json`:

```json
"mcpServers": {
  "xerj": {
    "command": "xerj",
    "args": ["mcp"],
    "env": { "XERJ_URL": "http://localhost:9200" },
    "auto_connect": true,
    "preflight": {
      "check_url": "http://localhost:9200",
      "start_command": ["xerj", "--insecure", "--data-dir", "~/xerj-data"],
      "post_start_command": ["xerj", "autoindex", "."]
    }
  }
}
```

- `preflight` starts the node if `check_url` is not answering and then
  runs `post_start_command`. If the node is already up, neither runs.
- `auto_connect: true` connects at startup (otherwise `/mcp connect xerj`).
- Allow the server in the policy: `{ "type": "tool", "tool": "mcp:xerj" }`.
- The system prompt includes `sys_prompt_xerj.md` automatically when the
  `xerj` server is present and enabled (`{{> sys_prompt_xerj.md if=xerj_enabled}}`;
  `"enabled": false` counts as absent).

### Tools

`tools/list` reports 20 tools: `xerj_search`, `xerj_semantic_search`,
`xerj_vector_search`, `xerj_hybrid_search`, `xerj_code_search`,
`xerj_map`, `xerj_memory_store`, `xerj_memory_recall`,
`xerj_brain_overview` / `_ego` / `_link` / `_unlink`,
`xerj_index_budget`, `xerj_autoindex_remove`, `xerj_plan`,
`xerj_field_report`, `xerj_corpus_feedback`, `xerj_hub_find` / `_def` /
`_ask`. Call `xerj_map` first: it returns each index's field map so
structured queries use real field names.

### Gotchas

- `xerj autoindex` exit code **3 is a warning**, not a failure.
- A failing `post_start_command` is surfaced as a connection *warning*,
  not an error — the server still connects.
- XERJ sends an agent field report after use by default (a GitHub PR,
  falling back to a POST to xerj.org). Opt out with `--disable-feedback`
  or `XERJ_DISABLE_FEEDBACK=true` if that is not wanted.

### Assessment

The richest option — lexical + semantic + hybrid search over all file
types, plus memory — at the cost of running and indexing a node.

---

## 3. probe (MCP)

### What it is

probe (probelabs/probe) is an AI-oriented code search tool (ripgrep speed
plus tree-sitter AST) with an MCP server. Upstream:
<https://github.com/probelabs/probe>.

### Setup

Nothing to install; `npx` fetches it. Add to `mistral-cli.json` — this is
the current workspace entry:

```json
"mcpServers": {
  "probe": {
    "command": "npx",
    "args": ["-y", "@probelabs/probe@latest", "mcp"],
    "auto_connect": true
  }
}
```

Allow it in the policy: `{ "type": "tool", "tool": "mcp:probe" }`.

### Tools

probe 0.6.0 exposes `search_code`, `extract_code` and `grep`. Results come
back as MCP text content (probe's own `<matches>` / `<probe_results>`
format, not structured JSON), and `search_code` supports a session id for
caching across calls.

### Gotchas

- The stdio request timeout is a fixed 30 s and is not configurable per
  server. A cold `npx @latest` fetch can exceed it on the first run — pin
  a version (`@probelabs/probe@0.6.0`) if that happens.
- probe registers `search_code`, `extract_code` and `grep` into the global
  tool registry; `mcp:*` pulls them in. A literal `search_code` in
  `available_tools` therefore resolves to probe's tool. There are no
  built-in tools with those names, so nothing is shadowed.
- It indexes the whole workspace (including `third_party/`) and respects
  `.gitignore`.
- There is no `sys_prompt_*.md` include for probe; the model sees probe's
  own tool descriptions.

### Assessment

Zero-setup, grep-like and AST-aware search over all file types. The
current workspace default.

---

## 4. `sqlite-vec-knowledge` (experimental branch)

### What it is

An experiment that adds a persistent, embedding-based code + docs index to
the CLI. It is **not in this checkout** — the implementation lives on the
experimental branch under `lib/knowledge/` (`knowledge_store`,
`knowledge_tools`, the code and markdown chunkers, and the embedders). The
notes below come from the workspace config and the session logs, not from
reading the branch in this clone.

Tools: `search_code`, `search_docs`, `refresh_docs`.

### How it works

- **Embeddings**: `codestral-embed` via the Mistral API
  (`embeddings.provider: "mistral"`), with a local `hashing_embedder`
  fallback when the provider is not Mistral.
- **Storage**: SQLite with a `sqlite-vec` virtual table; cosine distance,
  `score = 1 - distance`. Pure vector search — no lexical/BM25 component.
- **Chunking**: code in 80-line windows with 20-line overlap, snapped to
  blank lines, with breadcrumbs from a declaration regex plus brace
  counting; docs chunked by markdown heading path. The result snippet is
  the first 300 characters of the chunk.
- **Refresh** is incremental and runs on every search call.

### Config

Already present in the workspace `mistral-cli.json`:

```json
"embeddings": { "provider": "mistral" },
"knowledge": {
  "enabled": true,
  "db_path": ".mistral-cli/cache/knowledge.db",
  "include_extensions": [".md", ".txt"],
  "exclude_dirs": [".git", ".mistral-cli", ".cache", ".vscode", ".vibe",
                   "build*", "cmake-build-*", "node_modules", "__pycache__",
                   ".venv", "venv", ".pytest_cache", "grammars",
                   "storybook-static", "fixtures"],
  "exclude_files": ["CMakeLists.txt", "doctest.h"],
  "chunk_max_chars": 1500,
  "max_file_bytes": 1048576,
  "embedding_dim": 384,
  "search_max_results": 10
}
```

### Setup

Check out the branch, build, set `embeddings.provider` and the `knowledge`
section, then run `refresh_docs` (or just search — the first search
refreshes) to build the index. Add `search_code`, `search_docs` and
`refresh_docs` to `available_tools` and allow them in the policy.

### What we learned (12 queries with known ground truth)

Strengths:

- 7/12 queries put the exact right code in the top 1–2.
- Typo-robust ("appproval renderer" → `approval_renderers.cpp`).
- Exact line ranges; code and docs corpora kept separate.
- A usable score signal: on-target queries score 0.60–0.80, no-target
  queries 0.47–0.59.

Weaknesses, ranked:

1. **No lexical component.** Ranking misses partial-match and identifier
   queries — exactly the grep-like searches. A hybrid (embedding +
   FTS5/BM25 with RRF fusion) is the highest-value fix.
2. **Snippet is the chunk head, not query-centred** — it often starts
   mid-function.
3. **No relevance floor**: it always returns `k` results, so "nothing
   matched" is not expressible; the caller must judge by score.
4. **`third_party/` dominates the corpus** (~190 of 216 files), and
   seeded `templates/*.md` files are indexed twice (the bootstrap copies
   them into `.mistral-cli/`).
5. **`corpus` is a no-op** — there is a single `workspace-docs` corpus.
6. **Silent fallback**: if `embeddings.provider` is not `mistral`, the
   hash-based `hashing_embedder` is used with no warning.

Cost: the index build and the per-search refresh embed changed files,
which is what makes it slow on large trees.

### Assessment

Promising for semantic "where is the code that does X", but not yet a
replacement for grep-like search, and slow to build. Revisit if a lexical
hybrid and query-centred snippets land.

---

## Cross-cutting: how the pieces fit

- **MCP client** (`lib/mcp/`): HTTP and stdio transports, `preflight`,
  `auto_connect`, `/mcp` commands, TUI footer status. Both XERJ and probe
  use the stdio transport.
- **Policy** (`policy.json`): MCP tools are matched by
  `{ "type": "tool", "tool": "mcp:<server>" }` (or `mcp:*`). While a
  server is not connected the rule matches nothing.
- **Conditional prompt includes**: `{{> sys_prompt_xerj.md if=xerj_enabled}}`
  is included only when the `xerj` server is configured and enabled
  (`"enabled": false` counts as absent). probe and the
  knowledge tools have no such include.
- **Per-agent visibility**: which search tools an agent may use is set by
  its `available_tools` list in `mistral-cli.json`.

## Recommendation

1. Flip `MISTRAL_ENABLE_TREE_SITTER` to OFF by default on `main` (it is
   already OFF in this workspace) and trim the code-tools section from the
   default system prompt.
2. Keep one external MCP search server configured. probe is the current
   choice: zero setup, grep-like and AST search. XERJ is the upgrade path
   when lexical + semantic + hybrid search and agent memory are wanted.
3. Leave the `sqlite-vec-knowledge` branch experimental until it gains a
   lexical hybrid component, query-centred snippets, and a faster index
   build.
