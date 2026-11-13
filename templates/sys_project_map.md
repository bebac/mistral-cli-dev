---
## Project Map

Generated when this prompt is resolved (session start). Use it to orient yourself — file locations, git state — instead of re-deriving them with exploratory `ls`/`find`/`git` calls. It is a snapshot; verify with tools when precision matters.

### Repository tree (3 levels; `build/`, `node_modules/`, `.git/`, `.mistral-cli/` etc. excluded)

${{find . -maxdepth 3 \( -name .git -o -name .mistral-cli -o -name build -o -name node_modules -o -name __pycache__ -o -name .cache -o -name .vscode -o -name .vibe \) -prune -o -print | sort | head -n 400}}

### Git

${{if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then echo "branch: $(git branch --show-current)"; echo "recent commits:"; git log --oneline -n 10 | sed 's/^/  /'; echo "working tree (empty = clean):"; git status --porcelain | head -n 30 | sed 's/^/  /'; else echo "not a git repository"; fi}}
