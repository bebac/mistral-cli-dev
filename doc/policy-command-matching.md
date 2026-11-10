# Exec policy: command-line matching and containment

**Status:** design note. The direction was settled in a design review
(November 2026). `forbid_args` is implemented; the subcommand field,
path-taking options and the Landlock rung are not. Passages describing
current behaviour say so explicitly.

## Summary

An `executable` rule matches the executable name, optionally a set of
required arguments, and optionally a working directory. It cannot say
"`find`, but not with `-exec`", "`git`, but only `status`/`diff`/`log`",
or "`make`, but only with makefiles inside the workspace". Rules written
to allow a read-only command therefore grant arbitrary program
execution, and nothing scopes *where* a permitted command's effects
land.

The settled direction, in order:

1. **Per-command argv grammar** (the immediate work). Rules gain a
   `subcommand` allow-list, path-taking options whose values are scoped
   by the existing directory-rule machinery, and a `forbid_args`
   deny-list for execution flags.
2. **Version pinning at bootstrap.** A rule for a command applies only
   when the installed version is known to a static capability table
   shipped in the binary. Unknown versions tighten to approval; probe
   results never widen a rule.
3. **Landlock on Linux** as a future enforcement rung: exec calls run
   inside a kernel-enforced filesystem/network sandbox, which makes
   flag modelling unnecessary where it is available. `sandbox-exec` on
   macOS was considered and rejected.

Path resolution - which binary a rule names - is a separate concern and
is already implemented (a bare name resolves through PATH, then as
`<cwd>/<command>`). This note is about which *command lines* a rule
permits, and where enforcement should live.

## Design goal

The working direction: the agent should be allowed to do as much as
possible *inside* the workspace without prompting. What should be caught
and approved is reading files outside the workspace, and especially
changing files outside it. Perfect safety is out of scope without an
OS-level sandbox or a container.

That reframes the command-line matching options below. They are
advisory: they reduce accidental damage and keep an approval prompt
meaningful, but they cannot bound effects, because the set of programs
that launch other programs is unbounded - `find`, `make`, `cmake`,
`git`, `xargs`, `env`, `tar`, `sh`, ... The boundary that actually
matches the goal is the filesystem, not argv. The Landlock rung is the
piece that makes that boundary real on Linux; the argv grammar is the
advisory layer for everywhere else.

### Use cases

| Use case | Desired | Today | Served by |
|---|---|---|---|
| Read/explore code in the workspace | silent | silent (code tools, `read`, `ls` rules) | done |
| Edit files in the workspace | silent | prompts via `patchset_apply` | path-aware tool arguments (straightforward follow-up) |
| Build/test in the workspace | silent | silent (`make`, `tests` by name + cwd) | as-is; recipes running arbitrary code is the point of a build |
| Read-only inspection: `git status/diff/log`, `grep`, `find -name` | silent | silent, but over-broad (`find -exec` too) | the argv grammar |
| Read outside the workspace | prompt | prompt (directory rule) | done |
| Write/delete outside the workspace | prompt | unenforced - no shell means no `>` redirects, but `find -delete`, `make`, `cmake -E`, `git checkout --` write via the program itself | Landlock rung (Linux); advisory grammar elsewhere |
| Network, installs, everything else | prompt | prompt (default) | done |

### Two kinds of problem

The commands that motivate this note fail in two different ways, and
the difference decides which mechanism fits:

- **Location problems.** `make -f ../x.mk`, `make -C /elsewhere`,
  `find /etc -name x`. The danger is not the flag but where its value
  points. The answer is not to deny the flag but to declare it
  *path-taking* and scope its value with the existing directory rules:
  inside the workspace the rule applies, outside it does not. This is
  version-robust: `-f` has meant "the makefile" for decades, and the
  check is about location, not a model of the tool's behaviour.
- **Execution problems.** `find -exec`, `git -c alias.p='!sh -c ...'`.
  The danger is that a program is launched; there is no path to scope.
  The advisory answer is a deny-list of the flag family, plus version
  pinning so that a stale deny-list fails safe. Under the Landlock rung
  this whole class is moot: the launched program inherits the sandbox.

## Current behaviour

An `executable` rule is matched in `rule_matches`
(`lib/agent/policy_engine.cpp:861`) by three checks:

| Check | Code | Semantics |
|---|---|---|
| command | `command_matches` (`:552`) | exact string, or glob (`*`, `?`) |
| args | `args_match` (`:579`) | **any-of allow-list**: at least one test arg must equal (or glob-match) one rule arg |
| cwd | `cwd_matches` | exact, or prefix for subdirectories |

`args` is an allow-list, so it can *require* a flag but never *forbid*
one. That was the whole limitation: no way to express "this command,
without that flag", and no way to scope the paths a command operates
on. `forbid_args` (implemented) closes the negation half: any argument
matching the list sends the command line straight to the default
policy. Scoping paths and matching subcommands remain open.

The pre-check (`exec_tools::validate`, `lib/tools/exec_tools.cpp:48`)
runs before the policy and rejects shell syntax in the command field
and backticks/`$(` in arguments (`:59`, `:73`). It does not inspect what
a permitted program will itself launch.

Separately, the policy only understands a tool argument named `path`:
`normalize_context` (`lib/agent/policy_engine.cpp:492`) reads
`tool_arguments["path"]` and nothing else. `patchset_add` and
`patchset_update` use `file_path`, so a `directory` rule cannot
correctly scope either today. Letting each tool declare which of its
arguments are paths is straightforward follow-up work, independent of
the argv grammar.

## Why it matters

`find` is allowed with any arguments in the default policy
(`templates/policy.json`). So this runs with no approval prompt:

```
find . -maxdepth 0 -exec echo ... ;
```

`-exec` launches an arbitrary program with arbitrary arguments. The
policy engine never sees that program: the command is `find`, and `sh`
is started by `find`. `find -delete` needs no second program at all.
`-execdir`, `-ok`, `-okdir`, `-fprint*` and `-fls` are the same family.

The consequence is that no argv rule bounds what the agent can write.
Anything reachable from the workspace can be written or deleted through
`find -exec sh -c '...'`. The same class of hole exists in:

- `cmake` - `-P <script>` runs a CMake script, `-E` is a file
  manipulation mode (copy, remove, rename).
- `make` - `-f <file>` runs recipes from any makefile, not just the
  approved one; `-C` moves the working directory anywhere.

Both need a file to exist first, but `find -exec` can create it.

## The git case

The motivating example is wanting to allow `git status`, `git diff` and
`git log` without allowing `git push`, `git config` or `git checkout`.
The subcommand is the obvious thing to match, but it is not sufficient,
because git runs external commands in several ways:

- `git -c alias.p='!sh -c ...' p` - `-c` sets config before the
  subcommand, and an alias beginning with `!` is a shell command.
- `git -c core.pager='...' log` - the pager is run through the shell.
- `git submodule foreach '<command>'` - runs a shell command per
  submodule.
- `--ext-diff` / `--textconv` - run external diff drivers.
- `git diff --output=<file>`, `git format-patch`, `git archive -o` -
  write files.
- `git config` - writes `.git/config`, which can set aliases,
  `core.pager` and `core.hooksPath`.
- hooks - `git commit`, `git checkout`, `git merge` run
  `.git/hooks/*`, and a hook file can be planted with `find -exec`.

This list is illustrative, not exhaustive. It does not have to be
re-checked against the git version in use before being encoded in
rules - that is what version pinning (below) automates, fail-safe.

So a rule for git needs the subcommand *and* a forbidden-flag list, and
must account for options that precede the subcommand.

## Chosen design: per-command argv grammar

Rules stay JSON in `policy.json`; the engine gains a small per-command
grammar with token kinds matched to the failure mode they cover:

| Token kind | Field | Mechanism | Covers |
|---|---|---|---|
| Subcommand | `subcommand` | allow-list on the first non-flag argument | `git push` vs `git status` |
| Path-taking option | `path_args` | value resolved and checked against the rule's directory scope | `make -f ../x.mk`, `git -C /etc` |
| Positional path operand | `path_operands` | non-flag arguments treated as paths, scoped the same way | `find /etc ...` |
| Execution flag | `forbid_args` | deny-list, checked before everything else | `find -exec`, `git -c` |

```json
{ "type": "executable", "tool": "exec", "command": "git",
  "subcommand": ["status", "diff", "log"],
  "forbid_args": ["-c", "--exec-path*", "--ext-diff", "--textconv", "--output*"] }
```

```json
{ "type": "executable", "tool": "exec", "command": "make",
  "path_args": ["-f", "--file", "-C", "--directory"] }
```

```json
{ "type": "executable", "tool": "exec", "command": "find",
  "forbid_args": ["-exec", "-execdir", "-ok", "-okdir", "-delete", "-fprint*", "-fls"],
  "path_operands": true }
```

Semantics:

- `forbid_args` is checked first and wins over the `args` allow-list.
  A match goes straight to the default policy, not to the next matching
  rule: a forbidden argument is a verdict about the command line, not
  about the rule that happened to name it. (Decided; implemented.)
- `subcommand` needs to know which options take a value (`-c`, `-C`,
  `--git-dir`, ...) so that `git -c x=y status` is read as subcommand
  `status`, not `x=y`. There is no generic argv parsing: the grammar
  table itself declares, per command, which options consume a value.
- Each `path_args` value is resolved and must satisfy the rule's scope
  (its `cwd`, or the workspace root). Any outside value means the rule
  does not apply.
- `path_operands: true` treats non-flag positional arguments as paths
  and scopes them the same way. It is per-command: for `git` the
  positional argument is the subcommand, not a path.
- The grammar table - which options take values, which are path-taking,
  which are the execution family - lives in code, versioned with the
  binary, not in `policy.json`. It encodes knowledge about tools, not
  about this workspace. `policy.json` names commands and picks
  constraints.

## Version pinning at bootstrap

Two probes at bootstrap, both against stable contracts:

- per command: the resolved PATH entry and `--version` output, recorded
  in a manifest next to `bootstrap.json` (same provenance pattern:
  version + hash, re-checked on drift);
- later, with the Landlock rung: the kernel ABI number via
  `landlock_create_ruleset(NULL, 0, LANDLOCK_CREATE_RULESET_VERSION)`.

The binary ships a static capability table: for each command, the
versions its deny-lists were written against. A rule applies only when
the probed version is known to the table. A newer `find` with a flag
the deny-list never imagined therefore *tightens to approval with a
warning* instead of silently bypassing the list - the failure direction
that motivated this design. Probe results may only ever narrow what a
rule permits.

What probing must not become: parsing `--help` to discover
capabilities. Help text is not an API (localized, reformatted between
versions, GNU/BSD/busybox variance), and a probe can discover presence,
never dangerousness. The danger classification is curated knowledge and
stays in the binary.

## Future: Landlock enforcement rung (Linux)

**What it is.** A Linux kernel security module for unprivileged
self-sandboxing, mainline since 5.13 (2021). A process builds a
deny-by-default ruleset with three syscalls (`landlock_create_ruleset`,
`landlock_add_rule`, `landlock_restrict_self`), allow-listing paths
against fine-grained rights (read/write/execute file, read dir,
remove, make, ...) and, on newer kernels, TCP/UDP ports. No root, no
container.

**Why it fits this problem.**

- Restrictions are inherited by all children, and children can only
  add restrictions. `find -exec sh -c '...'`, make recipes, git hooks,
  `git -c` aliases - every escape enumerated in "The git case" - run
  inside the same box. The "unbounded set of programs that launch
  programs" argument stops mattering: the kernel does not care who
  launches whom.
- Rules are one-way: a process can never relax its own ruleset.
  Applied in the child before `execvp`, no flag or environment can
  undo it.
- It is probeable by design: the ABI number is a documented contract;
  distros backport, so the probe result is trustworthy where a kernel
  version string is not.

**What it dissolves.** With a profile of "read-write under the
workspace, read-only on system paths, no network connect":

- `find` may be allowed with any arguments; `-exec`'s program inherits
  the sandbox, and `-delete` can only delete inside the writable set.
- `git` may be allowed wholesale: `push` fails on the connect denial,
  `config` writes `.git/config` *inside* the workspace (correctly
  allowed), hooks run but are bounded.
- `make -f /any/Makefile` runs its recipes, but every write outside the
  workspace fails.

The policy engine's job changes from predicting behaviour from argv to
picking the rung: sandbox (allow per policy), advisory grammar (allow /
prompt), or approval (for legitimate outside-write or network).

**ABI timeline** (verified November 2026 against
docs.kernel.org/userspace-api/landlock.html and landlock(7)):

| ABI | Kernel | Adds |
|---|---|---|
| 1 | 5.13 | filesystem rights - everything this design needs |
| 4 | 6.7 | TCP bind/connect per port |
| 10 | 7.2 | UDP |
| 11 | 7.3 (current stable) | atomic no_new_privs with enforcement |

Distro baselines: Ubuntu 24.04 ships ABI 4, RHEL 10 ships ABI 6. The
filesystem rights this design depends on are on any 2021+ kernel.

**Caveats.** Network rules are port-based only - no address filtering,
and ICMP/raw sockets are not mediated; deny-by-default still kills
`git push`, but this is containment, not a firewall. Files opened
before enforcement keep their access. Rules do not propagate through
OverlayFS merged views (relevant inside some Docker dev setups).
Requires `CONFIG_SECURITY_LANDLOCK=y` (mainstream distros ship it).

## Rejected: sandbox-exec on macOS

macOS has no supported equivalent for a CLI process. `sandbox-exec`
has been deprecated and undocumented for years; it keeps working
because major projects (Chromium, Bazel) depend on it, but Apple may
break it in any release, and building a security boundary on it means
building on something its owner disowns. App Sandbox requires a signed
bundle and does not fit a CLI tool. Decision: out of scope. macOS
stays on the advisory rung, which is exactly today's behaviour - it
loses nothing, it just does not gain the enforcement rung.

## Options considered

### A. `forbid_args` deny-list - adopted, for execution flags only

Checked before the `args` allow-list; any match makes the command line
go straight to the default policy - no fall-through to other rules.

- **For:** small, and it closes the demonstrated hole.
- **Against:** deny-lists are open-ended; a flag added by a future
  version bypasses the rule silently.
- **Verdict:** adopted for the execution family (`-exec`, `git -c`),
  with version pinning converting the silent bypass into a prompt.
  Not used where the problem is a location - that is what `path_args`
  is for.

### B. `subcommand` field - adopted, for git

Match the first non-flag argument against a list.

- **For:** expresses the git case directly and reads well.
- **Against:** requires knowing which options take a value, otherwise
  `git -c x=y status` looks like subcommand `x=y`.
- **Verdict:** adopted. The options-that-take-a-value knowledge lives
  in the grammar table in code, bounded by the handful of commands in
  the default policy.

### C. Full command-line pattern - rejected

A pattern language over the joined argv. Most expressive, but a
language to design, document and maintain; quoting and argument
boundaries are fiddly; still cannot know that `-c` changes the meaning
of the rest of the line.

### D. Exact command-line allow-list - rejected

Only reviewed argv sequences allowed; anything else prompts. Strongest
and simplest to reason about, but brittle for the agent - every variant
needs a rule, so the approval prompts come back.

### E. Dedicated tools instead of `exec` - later, maybe

A `git_status` / `git_diff` / `git_log` tool that shells out with a
fixed argv. No pattern language; the implementation *is* the
allow-list. Costs code per command family and does not generalise;
worth revisiting for the read-only families used constantly if the
grammar turns out noisy.

### F. Path-taking options - adopted, for location problems

Declare which options take a path value and scope the value with the
existing directory-rule machinery. New relative to A-E; reuses
`path_matches` on argv values. Version-robust, because it asks *where*
rather than modelling the tool. This is the piece that matches the
design goal directly: inside the workspace free, outside prompts.

## Related: path matching boundaries

Two quirks in how `directory` rules compare paths, found while
refactoring `policy_engine.cpp`. Neither is about command lines, but
both are the same class of mistake: a raw string prefix used where a
directory boundary is meant. The `path_args` design above depends on
`path_matches` behaving correctly, so these matter to it directly.

### Fixed: exclusions over-matched siblings

`is_excluded` compared `path.starts_with(exclusion)` with no boundary
check, so an exclusion of `build` also excluded `buildfoo` and
`build.old`. That was inconsistent with rule paths, which do respect
the boundary, and the direction of the error was not always fail-safe:
for an `allow` rule the over-match falls through to a stricter default,
but for a `require_approval` rule with exclusions it falls through to
the default, which may be `allow`.

Exclusions now compare with trailing separators, the same way
`cwd_matches` and `path_matches` do. Pinned by the test "Exclusion does
not match a sibling that shares the prefix".

### Open: a rule path does not match its own directory

`path_matches` compares `test_path.starts_with(rule_path + "/")`, so a
rule for `/home/user/project` matches everything *below* it but not
`/home/user/project` itself. With the default policy
(`{ "type": "directory", "tool": "read", "path": "." }`) a `read` with
`path: "."` therefore falls through to the default instead of being
allowed. Low impact today - `ls` has its own `tool` rule - and the fix
is the same shape as the exclusion one: compare
`with_trailing_slash(test_path)` against `with_trailing_slash(rule_path)`.

## Open questions

- Should an `executable` rule with no `args`/`subcommand`/`path_args`
  constraint keep meaning "name only" (today), or be treated as too
  broad to allow?
- Where do `path_args` values resolve - against the call's cwd, the
  rule's `cwd`, or the workspace root? How does that compose with a
  rule-level `cwd`?
- Unknown-version tightening: silent fall-through to approval, or a
  visible warning in the session?
- Tool path-awareness: `normalize_context` reads only
  `tool_arguments["path"]`; patchset tools use `file_path`. Making each
  tool declare its path arguments is straightforward follow-up work.
- A `directory` rule still does not match the directory itself, and an
  exclusion on a `require_approval` rule removes the rule rather than
  narrowing it - see "Related: path matching boundaries".
- Where is the boundary with the OS beyond Landlock? A permitted
  `make` still runs whatever the approved `Makefile` contains, inside
  the sandbox or not. Is the goal "no silent arbitrary execution" or
  "no arbitrary execution"? On the advisory rung only the former is
  achievable; the Landlock rung is what makes the latter a real option.

## Test plan

- `find` with `-exec` / `-delete` / `-ok` -> rule does not apply.
- `find` with `-name` / `-type` -> allowed.
- `find . -name x` -> allowed; `find /etc -name x` -> rule does not
  apply (operand outside the workspace).
- `make -f Makefile` -> allowed; `make -f ../x.mk` -> rule does not
  apply; `make -C build` -> allowed; `make -C /elsewhere` -> not.
- `git status` / `diff` / `log` -> allowed; `git push` / `config` /
  `commit` -> not.
- `git -c alias.p='!sh -c ...' p` -> not allowed.
- `git -C /elsewhere status` -> not allowed.
- Glob patterns in `forbid_args` (`-fprint*`).
- A rule with both `forbid_args` and `args`: forbidden wins.
- Version pinning: installed version known to the capability table ->
  rule applies; unknown version -> rule does not apply, warning
  recorded.