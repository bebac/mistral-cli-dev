# sqlite-vec (vendored amalgamation)

Single-file amalgamation of [sqlite-vec](https://github.com/asg017/sqlite-vec)
by Alex Garcia, MIT license.

- Version: v0.1.9 (release asset `sqlite-vec-0.1.9-amalgamation.zip`)
- Files: `sqlite-vec.c` (the extension), `sqlite-vec.h` (entry point declaration)
- Source: https://github.com/asg017/sqlite-vec/releases/tag/v0.1.9

The amalgamation is compiled with `SQLITE_CORE` defined, so it links
statically against the vendored SQLite amalgamation in `third_party/sqlite`
and is registered per connection by calling `sqlite3_vec_init` directly.

Used by the knowledge store (`lib/knowledge/`) for vector search over
documentation chunks. See `lib/knowledge/knowledge_store.cpp`.
