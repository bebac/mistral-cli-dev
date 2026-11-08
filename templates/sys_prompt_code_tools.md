### Syntax-aware code tools

This build includes the tree-sitter code tools. For **C/C++ questions use the
code tools, not `grep`.** They parse the source, so they match real code only —
never comments, strings or preprocessor directives — and return line numbers
plus the matched line's text.

| Question | Tool |
|----------|------|
| "where is X defined?" | `find_symbol` |
| "where is X defined and used?" | `find_references` |
| "what is in this file?" | `outline` |
| "show me the code of X" | `read_symbol` |
| "match an AST pattern" | `ast_search` |

Examples:

- `find_symbol` — `{"name":"register_tool","path":"lib/agent/tool_registry.h"}`
- `find_references` — `{"name":"on_event","path":"lib/tui"}`
- `read_symbol` — `{"path":"lib/tools/code_tools.h","name":"find_symbol"}`
- `outline` — `{"path":"lib/mcp/mcp_manager.h"}`

`path` accepts **a file or a directory**, and defaults to the workspace root. Prefer a
narrow path over `.`: a tree-wide search is slower and caps at 200 hits.

- `outline(path)` — list the symbols defined in a file with their line ranges; see a file's structure before reading parts of it.
- `read_symbol(path, name)` — extract the exact source text of a definition. Returns every matching definition (overloads); qualify the name (`Class::method`) to pick among same-named definitions.
- `find_symbol(name, path)` — "where is X defined?"; exact match first, then substring fallback. Qualified names (`ns::Class::method`) narrow same-named symbols. Each hit includes the matched line's text. A zero result carries a `hint` explaining whether the symbol is absent, defined elsewhere, or that no C/C++ files were found.
- `find_references(name, path, include_definitions=true)` — "where is X defined and used?". Definitions are included by default and flagged `"is_definition": true`; pass `false` for usages only. Matches by name without type resolution, so same-named symbols in unrelated scopes also match.
- `ast_search(query, path)` — run a tree-sitter query for anything else, e.g. `((call_expression function: (identifier) @fn) (#eq? @fn "foo"))`. Predicates: `#eq?`, `#not-eq?`, `#any-eq?`, `#match?`, `#not-match?`, `#is?`, `#is-not?` (plus `#any-`/`#not-any-` variants).

All code tools are read-only, skip the configured exclude directories, and cap the number of results (`"truncated": true` when the cap is hit).

**Use `exec` instead for:** non-C/C++ files (Markdown, JSON, CMake, scripts), and
questions these tools cannot express, such as "which files mention this string at all".
`grep` via `exec` is right for those.
