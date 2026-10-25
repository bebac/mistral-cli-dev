**XERJ (optional, via MCP):** If the `xerj` MCP server is connected, additional search tools are available for broader code search across all file types:

- `xerj_search(query, k)` — full-text (lexical) search. Returns ranked code passages with file:line.
- `xerj_code_search(query, k)` — AST-aware code search. Finds code by structure, not just text.
- `xerj_semantic_search(query, k)` — vector/semantic search. Matches by meaning, not just keywords.
- `xerj_hybrid_search(query, k)` — combines lexical + semantic search (RRF fusion).
- `xerj_map()` — show what has been indexed (datasets, fields, file counts).

All XERJ tools are read-only and return `file:line` citations. Cite `ax_path:line` for anything you rely on.

---
## Memory

The `xerj_memory_store` and `xerj_memory_recall` tools give you a **durable, searchable memory** that persists across sessions. Use them to remember important context instead of re-learning it every conversation.

**When to store** (`xerj_memory_store`):
- Bug fixes and their root causes (e.g., "SIGPIPE crashes the TUI — ignore SIGPIPE in main()")
- User preferences (e.g., "user likes squashed commits, thorough testing before push")
- Project decisions and their rationale
- Debugging patterns (e.g., "TUI hangs → write a standalone simulator")
- API quirks (e.g., "xerj autoindex exit 3 is a warning, not a failure")

**When to recall** (`xerj_memory_recall`):
- At the start of a session, to get context on previous work
- When you need to remember how something was solved before
- When the user references past work ("like we did last time")

**How to organize**:
- Use namespaces per project or topic (e.g., `mistral-cli-xerj-mcp`, `user-preferences`)
- Add metadata (`type`, `date`) for filtering
- Recall is **semantic** — ask in natural language, get relevant memories even with different wording

Example:
- Store: `xerj_memory_store(namespace="my-project", text="Fixed SIGPIPE crash by ignoring SIGPIPE in main()...")`
- Recall: `xerj_memory_recall(namespace="my-project", query="how did we fix the TUI crash?", semantic=true)`
