# Background process sessions: the `process_*` tools

Status: implemented (v1). This document records the design and the
decisions behind it.

## Motivation

`exec` is a one-shot tool: it blocks until the command exits or the
wall-clock timeout fires, and on timeout it SIGKILLs the whole process
group. That makes three workflows impossible:

1. running a dev server (web UI, `npx storybook dev`) and testing it
   while it runs;
2. driving a REPL (lldb/gdb) where state (breakpoints, inferior) must
   survive across calls — see `doc/skills.md`, which names exactly
   this gap;
3. any long-running process whose output must be polled and which must
   be stopped cleanly on demand.

## Design

A new tool family backed by a `process_manager`, instead of extending
`exec`:

- `process_start(command, args, cwd, env, name, wait_for, wait_timeout_seconds)`
  spawns the process with piped stdin/stdout/stderr in its own process
  group and returns immediately with a session id. `wait_for` (regex)
  blocks until the output matches, the process exits, or the timeout
  elapses — the "wait until the dev server is ready" primitive.
- `process_write(id, input)` writes verbatim to the session's stdin
  (non-blocking fd + `io_awaiter` POLLOUT on EAGAIN; SIGPIPE is ignored
  by the entry points, so a dead child is an EPIPE error, not a crash).
- `process_read(id, since, max_bytes)` returns output produced since
  byte offset `since` (use `next_offset` from the previous read for
  incremental reads). Each session keeps a 256 KiB ring buffer of
  merged stdout+stderr; older output is dropped and flagged via
  `truncated`.
- `process_list()` / `process_kill(id)` for status and group kill.

### Why a manager, and why not a singleton

`common::process_manager` is a plain class owned by `agent_system`
(the composition root) and injected into `process_tools` by
constructor reference — the same pattern as `patchset_tools` taking
`patchset_store&` (`agent_system` itself implements the store). Tests
construct their own manager + tools directly, so no singleton is
involved and everything is testable in isolation. The only singletons
remain `agent_system` (composition root) and `mcp_manager` (entrenched
because `policy_engine` queries it mid-evaluation).

All manager methods run on the dripcore scheduler thread: tool
coroutines run there, and `kill_all()` is registered on
`agent_system`'s `shutdown_signal`, which `fire_shutdown()` runs on
that same thread (from the interactive agent task's teardown). Sessions
die with the CLI; `~subprocess()` is the backstop.

### Pump task

One `dripcore::task` per session drains both pipes every 10 ms (the
same poll shape as `run_exec`) into the ring buffer, so the child never
blocks on a full pipe, and reaps the child on exit. The pump's
`teardown()` deliberately does not kill the child — the session is
owned by the manager and outlives the pump.

### Reuse of existing machinery

- `common::subprocess` already provided piped stdin, own process
  group, group kill, non-blocking CLOEXEC fds and exec-failure
  reporting — no changes needed. PTY support (`pty: true`) is a
  follow-up: lldb/gdb can be driven over pipes today; a pty adds
  Ctrl-C via `\x03` and tty-requiring programs.
- Policy: `policy_engine::normalize_context` extracts
  command/args/cwd for `process_start` exactly like `exec`, so
  executable rules work unchanged, e.g.
  `{"type":"executable","tool":"process_start","command":"npm"}`.
  Default posture matches exec: deny/require-approval unless a rule
  allows it. `precheck_tool_call` validates `process_start` with the
  existing `exec_tools::validate` (no shell syntax, command and cwd
  must exist). The approval prompt reuses exec's renderer with a
  "process_start (background process)" title.
- Per-agent visibility comes free via `available_tools` filtering in
  `create_tool_registry`.

## Files

- `lib/common/process_manager.{h,cpp}` — session, ring buffer, pump
  task, manager.
- `lib/tools/process_tools.{h,cpp}` — the five tools + `parse_request`.
- `lib/agent/agent_system.{h,cpp}` — ownership, registration, shutdown
  hook.
- `lib/agent/policy_engine.cpp`, `lib/agent/agent_task_base.cpp`,
  `lib/agent/approval_renderers.{h,cpp}` — policy/precheck/approval
  wiring.
- `test/process_tools_test.cpp`, plus registration tests in
  `test/agent_system_test.cpp` and policy tests in
  `test/policy_engine_test.cpp`.

## Out of scope / follow-ups

- PTY (`forkpty`) spawn variant for full REPL fidelity.
- Live streaming of session output to the TUI (the `exec_run_task`
  channel pattern can be reused; v1 returns output via `process_read`).
- Idle-based timeouts; sessions currently live until killed or until
  the CLI exits.
- The skills framework from `doc/skills.md` (e.g. an lldb skill) can
  build on this manager later.
