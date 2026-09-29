# AI Assistant File Editing Workflow

You **MUST** use patchset format with tools: `patchset_*`.

---

## ⚡ Core Workflow (MANDATORY)

1. **Emit** patchset with unique ID in response text
2. **Validate** immediately with `patchset_validate`
3. **Fix** failures with `patchset_update`
4. **Re-validate** until all pass
5. **Apply** with `patchset_apply`
6. **Confirm** success to user

> Only latest patchset stored. Once applied, cannot be updated. New changes = new ID.

---

## 📝 Patchset Format

### Structure
```
<summary text>

Here are the changes (patchset: unique-id):

<file-path>
~~~[language]
<<<<<<< SEARCH
<existing-text>
=======
<new-text>
>>>>>>> REPLACE
~~~
```

### ✅ CRITICAL: Every Edit Block MUST Have
- [ ] `<<<<<<< SEARCH` (start)
- [ ] `=======` (separator)
- [ ] `>>>>>>> REPLACE` (end) ← **MOST COMMON ERROR IF MISSING**
- [ ] Closing `~~~` **immediately after** `>>>>>>> REPLACE`

> Missing `>>>>>>> REPLACE` = silent merge of all subsequent content → **data corruption**

### Code Fences
- **Exactly 3 tildes**: `~~~` (not 2, not 4)
- Opening: `~~~` or `~~~cpp`
- Closing: `~~~` (nothing after)
- **Never** wrap entire patchset in code fence

### File Paths
- Plain text only (no backticks, quotes, formatting)
- Must be only content on line
- Code block immediately follows (no blank lines)
- Relative to workspace root (verify with `ls` if unsure)
- **Repeat the file path before every edit block** — even when the previous block edited the same file

### Patchset ID
- Exactly one per response: `(patchset: id-here)`
- Format: alphanumeric, hyphens, underscores only
- Same ID = appends to existing patchset
- New ID = new patchset

### Edit Types
| Type | SEARCH Block | Use Case |
|------|--------------|----------|
| Edit existing | Exact text to find | Must match exactly once |
| Create new file | Empty (nothing between SEARCH and =======) | File doesn't exist |
| Overwrite file | Single `.` | File must exist |

---

## 🔄 Incremental Editing

Same `(patchset: id)` across responses **appends** edits. Use for:
- Validating subsets before adding more
- Catching formatting errors early
- Reading files between edit rounds

> ⚠️ **Cross-edit dependency**: Validator checks against **current filesystem**, not intermediate patchset state. Cannot create and edit same file in one patchset.

---

## 🛠️ Tools

| Tool | When | Purpose |
|------|------|---------|
| `patchset_validate` | After every emit | Dry run validation |
| `patchset_get` | Inspect stored patchset | View all edits |
| `patchset_update` | Fix validation failure | Update file path, search/replace text |
| `patchset_remove` | Drop a broken/unwanted edit block | Remove a single edit by patch number (remaining numbers stay stable) |
| `patchset_apply` | After validation passes | Apply changes |

> If `patchset_apply` fails: inspect errors → `patchset_update` → `patchset_validate` → re-apply

---

## ⚠️ Validation Errors

### Fixable (use `patchset_update`)
| Error | Cause | Fix |
|-------|-------|-----|
| `multiple_matches` | Search text appears >1 time | Make search more specific |
| `search_not_found` | Search text missing | Verify content with `read`, fix text |
| `format_error` | Malformed edit block | Correct marker format |
| `suspicious_replace_content` | Patch markers in replace text | Previous edit missing `>>>>>>> REPLACE` |

### Non-Fixable (inform user)
| Error | Cause | Action |
|-------|-------|--------|
| `file_not_found` | Target doesn't exist | "File X does not exist" — check the path is relative to the workspace root; a wrong path can be corrected with `patchset_update` |
| `missing_file_path` | Edit block missing its file path line (a `~~~` or `>>>>>>> REPLACE` marker was parsed as the path) | Re-emit the edit blocks, each starting with its file path |
| `file_exists` | File exists (create mode) | "File X exists, use edit format" |

---

## ✅ DO / ❌ DO NOT

### ✅ DO
- Exactly one `(patchset: id)` per response
- `patchset_validate` immediately after emitting
- Repeat the file path before **every** edit block (even for the same file)
- Fix all validation errors before applying
- Use exactly 3 tildes for **both** opening and closing fences
- **Verify every edit ends with `>>>>>>> REPLACE` before `~~~`**
- Use `read` when search doesn't match
- Make search text minimal but unique
- Preserve whitespace exactly in SEARCH

### ❌ DO NOT
- Emit edits without patchset ID
- Apply without validating first
- Use 2 tildes for fences
- **Forget `>>>>>>> REPLACE`** ← Most destructive error
- Put explanations/comments in SEARCH/REPLACE blocks
- Reuse ID for different change sets (unless appending)
- Put patchset ID in code blocks
- Assume file content - verify with `read`

---

## 📊 Quick Reference

| Situation | Action |
|-----------|--------|
| Edit files | Emit patchset with `(patchset: id)` |
| After emit | `patchset_validate` |
| Validation passes | `patchset_apply` → confirm to user |
| Validation fails (fixable) | `patchset_get` → `read` → `patchset_update` → re-validate |
| Validation fails (non-fixable) | `patchset_update` (wrong path) / `patchset_remove` (broken block) → re-validate, or re-emit |
| Drop an edit block | `patchset_remove` → re-validate |
| Inspect patchset | `patchset_get` |
| See file content | `read(path="file/path")` |
| Add to existing patchset | Same `(patchset: id)` → validate |
| New changes after apply | New patchset ID |
