# Workspace Templates

Default configuration files for setting up mistral-cli / mistral-tui in a new
workspace. The application loads its configuration from a `.mistral-cli/`
directory in the working directory (agents, system prompts, tool policy, notes
and chat history).

## Bootstrap a new workspace

No manual setup is needed: `mistral-cli` and `mistral-tui` embed these
defaults and bootstrap `.mistral-cli/` automatically on startup (unless a
custom config file is passed with `-c`, or `--no-bootstrap` is given):

- `mistral-cli.json`, `policy.json`, `sys_prompt.md` are **seeded** — written
  only if missing, so local customizations are never overwritten.
- `sys_patchset_instructions.md` is **managed** — it documents the patchset
  editing workflow implemented by the executable's tools and is refreshed
  from the embedded content when a newer executable finds an unmodified
  stock copy (tracked via `.mistral-cli/bootstrap.json`). A locally
  modified copy is left in place with a warning; run with `--update-managed`
  to overwrite it and re-record the manifest hash.

Set your API key (referenced by `api_key_env` in the model configuration):

```sh
export MISTRAL_API_KEY="your_api_key_here"
```

## Files

| File | Purpose |
|------|---------|
| `mistral-cli.json` | Models, agents and default tool policy |
| `policy.json` | Tool policy: read-only tools allowed, everything else requires approval |
| `sys_prompt.md` | System prompt template for the `main` agent |
| `sys_patchset_instructions.md` | Patchset editing workflow (included by `sys_prompt.md`) |
| `bootstrap.json` | Bootstrap manifest: version + hash of the managed `sys_patchset_instructions.md` |

## Customization notes

- The system prompt is processed by the template engine. Includes of the form
  `{{> file.md}}` are resolved relative to the `.mistral-cli/` directory,
  `${var}` expands variables and `${{command}}` runs shell commands. A missing
  include is a hard error, so only reference files that exist.
- If your project has documentation at the workspace root (for example
  `AGENTS.md`), you can pull it into the system prompt with an include such as
  `{{> ../AGENTS.md}}`.
- For project-specific frameworks (e.g. dripcore), add a primer file to
  `.mistral-cli/` and include it from `sys_prompt.md`.
- The `patcher` and `reviewer` agents in a full setup are just additional
  entries in the `agents` section pointing at other models or prompts.