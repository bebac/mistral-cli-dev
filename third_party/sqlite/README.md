# SQLite (vendored amalgamation)

The official SQLite amalgamation, vendored so the build does not depend on
the system SQLite. SQLite is in the public domain — see
https://sqlite.org/copyright.html (the blessing at the top of `sqlite3.h`).

- Version: 3.54.1
- Files: `sqlite3.c` (the amalgamation), `sqlite3.h`, `sqlite3ext.h`
  (`shell.c` from the zip is intentionally not vendored; we do not build the CLI)
- Source: https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip
- SHA3-256 of the zip: 92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30

Built as the static `sqlite3` CMake target in the top-level `CMakeLists.txt`.
Used by the knowledge store (`lib/knowledge/`) together with the vendored
sqlite-vec extension (`third_party/sqlite-vec/`). See
`lib/knowledge/knowledge_store.cpp`.
