# Code Tools (tree-sitter based code exploration)

> **Compiled out by default.** These tools are built only when
> `MISTRAL_ENABLE_TREE_SITTER=ON` is passed to CMake; the default is `OFF`.
> In a default build they are not registered, and the agent reaches code
> through `search_code` (semantic) and `exec` + `grep` (exact) instead. The
> subsystem still exists in the tree and can be re-enabled, which is why this
> document is kept rather than deleted. The prompt section that describes
> these tools is included only in builds that have them
> (`templates/sys_prompt_code_tools.md`).

Syntax-aware code exploration tools for the agent, built on tree-sitter
(commits `51e443a`..`17cde40` on the `tree-sitter` branch). The executable is
fully standalone: tree-sitter core, the C++ grammar's parser tables and all
query text are compiled into the binary.

## The tools

| Tool | Scope | Use for |
|------|-------|---------|
| `outline` | one file | Structure of a file before reading parts of it |
| `read_symbol` | one file | Exact source text of a definition (handles overloads) |
| `find_symbol` | directory tree | "Where is X defined?" (exact, then substring fallback) |
| `find_references` | directory tree | "Where is X used?" (definition sites excluded) |
| `ast_search` | file or tree | Arbitrary tree-sitter query patterns |

All are read-only, capped at `code_index.max_results` (default 200,
`"truncated": true` when hit), and respect `code_index.exclude_dirs`.

Prefer these over `exec grep` for code questions: no matches in comments,
strings or preprocessor directives; no partial-identifier noise; structured
file/line output. `find_references` is syntactic (no type resolution):
same-named overloads all match, and common names hit the cap quickly. It
shines for distinctive symbol names.

## Qualified names

`read_symbol` and `find_symbol` accept qualified names — `Class::method`
or `ns::Class::method` — to pick a specific definition among same-named
symbols (e.g. the `on_event` overrides of every widget class in
`cpptui.hpp`). Matching is by scope *suffix*: `TextArea::on_event`
matches a definition scoped `cpptui::TextArea`. Every definition carries
a `scope` (enclosing namespaces and classes, outermost first, plus the
qualified scope of out-of-line definitions), and in-class member
functions are labeled kind `method` (like out-of-line ones) instead of
`function`. `find_references` matches by bare identifier, so a
qualified name is searched by its rightmost component.

`ast_search` query syntax reminder: predicates are siblings of the pattern
root inside the wrapping parens, e.g.
`((call_expression function: (identifier) @fn) (#eq? @fn "foo"))`.
Supported predicates: `#eq?`, `#not-eq?`, `#any-eq?`, `#not-any-eq?`,
`#match?`, `#not-match?`, `#any-match?`, `#not-any-match?`, `#is?`,
`#is-not?`. Anything else is rejected at compile time. Queries without
captures are rejected (a match would carry no position info). Query errors
report kind + byte offset + snippet; iterate on them.

## Architecture

- `lib/tools/code_index.{h,cpp}` — the engine layer:
  - `parsed_source`: owns a parse tree; extracts definitions (grammar's
    `tags.scm`), usage sites (`queries/cpp-references.scm`, repo-local), and
    runs arbitrary compiled queries.
  - `compiled_query`: compiles a query string, validates/rejects predicates,
    evaluates them per match.
  - `symbol_index`: in-memory definition index, incremental via per-file
    mtime+size stamps. Designed so a disk-based index can be added later:
    entries and stamps are plain serializable data (see header notes).
- `lib/tools/code_tools.{h,cpp}` — the tool layer; one class, five registered
  tools.
- Queries are embedded at build time by `cmake/embed_queries.cmake`:
  grammar queries from `third_party/grammars/<grammar>/queries/`, repo-local
  queries from `queries/`.
- Grammars live under `third_party/grammars/`. Their own CMakeLists is
  bypassed (it may try to regenerate `parser.c` with the tree-sitter CLI);
  we compile the checked-in `parser.c`/`scanner.c` into a static lib target.

## Hard-won caveats (read before touching the query code)

1. **The C core does not evaluate predicates.** `ts_query_cursor_next_match`
   returns matches regardless; the consumer must check
   `ts_query_predicates_for_pattern`. We evaluate the common operators in
   `compiled_query`/`predicates_hold` and reject the rest at compile time —
   never silently ignore a predicate.
2. **Predicate operators are stored without the leading `#`.** The core's
   parser consumes the `#` before recording the name (`query.c`:
   `stream_advance` runs before `ts_query__parse_predicate`), so
   `ts_query_string_value_for_id` returns `eq?`, not `#eq?`.
   `parse_predicates` normalizes by prepending `#`.
3. **`tags.scm` captures declarators, not enclosing declarations.** E.g. for
   functions it captures the `function_declarator`. `enclosing_declaration()`
   walks up through `function_definition`/`declaration`/`field_declaration`/
   `template_declaration` so `read_symbol` returns the full definition.
4. **Class/struct extents exclude the trailing `;`** (their parent is
   `translation_unit`/`declaration_list` directly, not a `declaration`).
5. **`std::unique_ptr` with a function-pointer deleter has no default
   constructor** — always pass the deleter: `tree_ptr tree{nullptr, ts_tree_delete}`.
6. **`fs::path::starts_with` is not available in this libc++** — use the
   component-wise `is_under()` helper in `code_index.cpp`.
7. **Braced `co_return` evaluates left-to-right** — computing
   `definitions.size()` after `std::move(definitions)` in the same braced
   list reads a moved-from vector (this bug shipped for one build; count is
   now computed before the move).
8. **Yielding outside a `dripcore::task`**: `task::yield()` needs a task
   control block. The established pattern is
   `co_await dripcore::sleep_awaiter{dripcore::executor::current(), nullptr, 0ms}`.

## Binary size

The C++ grammar's parser tables are ~5.6 MB (`__TEXT`), the largest single
component of the ~7.8 MB release binary. C++ is the worst-case grammar;
most languages (C ~1 MB, Python ~1-2 MB) are far smaller. If size ever
matters: strip the binary, or make grammars opt-in per build.

## Adding another language (e.g. Python)

1. `git submodule add` under `third_party/grammars/tree-sitter-<lang>`.
2. Add a static lib target in `CMakeLists.txt` compiling its checked-in
   `parser.c` (+ `scanner.c` if present), like `tree-sitter-cpp`.
3. Add an `embed_file(<lang>_tags_query ...)` line in
   `cmake/embed_queries.cmake` + declaration in `embedded_resources.h`.
4. Extend `symbol_index::is_supported_source` and wire the language into
   `parsed_source` (currently hardcoded to `tree_sitter_cpp()`; a
   per-extension language table is the natural refactor).
5. Compile a query per language; the tools are otherwise language-agnostic.
6. Qualified-name lookup is C++-specific in exactly two places (both in
   `code_index.cpp`) and must move into the language table:
   - The scope separator: `qualified_name` / `split_scope` split on `::`
     (Python would use `.`).
   - The scope node types of the `enclosing_scope_parts` walk-up:
     `namespace_definition`, `class_specifier`, `struct_specifier`,
     `union_specifier` (tree-sitter-python's equivalent is
     `class_definition` with its `name` field; modules are files, so
     scope is just the class chain).

   The suffix-matching semantics, the `scope` string format
   (`ns::Class`, outermost first) and everything downstream of it
   (symbol_index, tool layer, JSON output) are language-agnostic.

## Configuration

`code_index` section of the workspace config (`mistral-cli.json`):
`exclude_dirs` (matched against every path component; defaults:
`.git`, `.mistral-cli`, `.cache`, `.vscode`, `.vibe`, `build`,
`node_modules`, `__pycache__`) and `max_results`.

**Seeded files never self-update**: `templates/` is the source of truth for
new workspaces; existing `.mistral-cli/` copies must be updated manually.
Tools must be listed in the agent's `available_tools` *and* allowed in
`policy.json` (default policy is `require_approval`).

## Future work (ideas, not commitments)

- Disk-based index under `.mistral-cli/cache/` for large repos (design notes
  in `code_index.h`; stamps make incremental refresh trivial).
- Post-edit syntax validation: parse the edited file, report ERROR nodes
  before applying a patchset.
- A `node_types` helper tool listing the grammar's node/field names — the
  main friction point when authoring `ast_search` queries.
- More languages (see above).