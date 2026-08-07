---
name: hprscript-edit
description: 'Guarded mechanical multi-file editing through hprscript edit plans and exact-plan application. Use when search patterns and qualifiers define one or many equivalent replacements, renames, insertions, deletions, line or balanced-block changes, scope-limited edits, or whole-function swaps. Prefer `edit -plan-out` followed by review and `hprscript apply` so the approved paths, file identities, byte ranges, old bytes, and replacements cannot drift between discovery and mutation. Use native patch tools for localized semantic changes whose sites require different reasoning, and use hprscript-search for read-only discovery, investigation, ranking, counting, or relational queries.'
---

# hprscript edit

Use `hprscript edit` for search-shaped mechanical transformations across a verifiable target set. Use `hprscript apply` to apply a reviewed immutable plan. Quick search, `investigate`, `query`, and script mode are read-only.

Prefer a native patch tool for one localized semantic change or for sites that need different implementations. Use the `hprscript-search` skill first when the correct files, scopes, or patterns are not yet known.

## Prefer a persistent plan

```bash
# 1. Discover once and persist exact edits plus file identities.
hprscript edit -F 'retry(3)' -content 'retry(5)' -expect 14 \
  -plan-out retry.plan.json -glob 'src/**/*.go'

# 2. Review the diff and plan, then apply those stored edits without rescanning.
hprscript apply retry.plan.json -receipt json
```

The plan binds review to exact paths, file identities and hashes, byte ranges, old bytes, and replacements. `apply` performs an all-file preflight before committing changes.

`-expect N` guards the discovery/planning invocation. Repeating `edit ... -write -expect N` performs a new scan and does not prove that the reviewed sites stayed unchanged. Direct `edit -write` remains available for compatibility and warns unless `-no-plan-warning` is supplied; do not use it as the normal reviewed workflow.

## Establish a safe target

- Use an absolute positional path or glob when the effective cwd is uncertain.
- Inspect the first preview path and stop if it escapes the intended tree.
- Narrow with `-glob`, `-exclude`, `-git-changed`, `-git-staged`, `-git-range`, `-git-added-lines`, `-lines`, `-in-scope`, `-near`, `-far`, `-same-scope`, `-not-same-scope`, or `-file-where`.
- Prefer a stable scope name over line numbers when code may move.
- Run `hprscript -list-scopes -llm <file>` before a scope or function-body replacement.
- Mark relation-only patterns with `-ref`; otherwise their matches also become edit sites.

## Choose the edited span

| Span | Meaning |
|---|---|
| `-span match` | Replace each match; default |
| `-span line` | Replace the whole line, including newline |
| `-span block` | Replace the balanced block interior |
| `-span block-full` | Replace the block including delimiters |
| `-span scope` | Replace the enclosing function or class |
| `-span scope-body` | Replace or insert within the enclosing body |

Use `-block-open` and `-block-close` with block spans. With `-in-scope`, scope spans target the innermost enclosing scope matching the filter, not an unrelated nested closure.

## Choose the new content

- `-content '<template>'`: use for short single-line content. Tokens include `$MATCH`, `$EXTRACT_<NAME>`, `$FILE`, `$LINE`, and `$$`; `\n` and `\t` are interpreted.
- `-content-file <path>`: use for multiline or quoting-sensitive content.
- `-content-stdin`: read content verbatim from stdin.
- `-delete`: remove the selected span.
- `-insert before|after|start|end`: insert without replacing the span.

Use a scratch content file for multiline code. Check trailing-newline behavior for whole-scope replacements.

## Core recipes

Replace only inside one function:

```bash
hprscript edit -F 'retry(3)' -content 'retry(5)' \
  -in-scope '^ProcessBatch$' -expect 1 -plan-out retry.plan.json src/worker.go
hprscript apply retry.plan.json -receipt json
```

Replace a function by name:

```bash
hprscript -list-scopes -llm src/data.go
hprscript edit -in-scope '^LoadData$' -span scope \
  -content-file /tmp/new-load-data.go -expect 1 -plan-out load-data.plan.json src/data.go
hprscript apply load-data.plan.json -receipt json
```

Use a signature and balanced block when scope indexing is unavailable:

```bash
hprscript edit -p 'func\s+LoadData\b' -block-open '{' -block-close '}' \
  -span block-full -content-file /tmp/new-load-data.go -expect 1 \
  -plan-out load-data.plan.json src/data.go
```

Rewrite with a capture:

```bash
hprscript edit -p 'log\.Printf\("([^"]*)"' -extract fmt \
  -content 'logger.Infof("$EXTRACT_FMT"' -expect 6 \
  -plan-out logging.plan.json -glob '**/*.go'
```

Rename only lines added by the current change:

```bash
hprscript edit -p '\bOldName\b' -content 'NewName' \
  -git-changed -git-added-lines -expect 14 -plan-out rename.plan.json
hprscript apply rename.plan.json -receipt json
```

Edit X only where Y is absent on the same line:

```bash
hprscript edit -p 'http://' -name hit -p 'allow-insecure' -name allow -ref \
  -far hit:allow:0 -content 'https://' -expect 3 \
  -plan-out https.plan.json -glob '**/*.yaml'
```

## Guards and receipts

- `-expect N`: require exactly N planned edit sites.
- `-max-span-lines N`: cap span size; default 500 lines.
- `-assert-contains <regex>`: require every selected span to retain an invariant.
- Automatic overlap and conflict checks: reject ambiguous edit sets.
- Plan/apply verification: reject changed paths, identities, hashes, ranges, or old bytes before mutation.

Interpret exit codes precisely:

| Code | Meaning |
|---|---|
| `0` | Previewed, planned, or applied successfully |
| `1` | No edit sites |
| `2` | Usage, compile, plan, or I/O error |
| `3` | Planning guard or apply preflight refused; nothing was written |
| `4` | Apply failed after commit began; the receipt identifies partial state |

Reapplying a completed plan is stale and fails verification with exit 3. Retain the JSON or human apply receipt as evidence of exactly what was verified and changed.

## Avoid unsafe edit shapes

- Do not use `sed -i` or an unguarded broad replacement when a persistent plan can express the target.
- Do not review one scan and then rescan with `edit -write`; review and apply the same stored plan.
- Do not infer `-expect` without inspecting the preview and intended target set.
- Do not put multiline code in shell-quoted `-content`; use `-content-file`.
- Do not leave relation-only patterns editable; add `-ref`.
- Do not use stale line ranges when a stable scope name exists.
- Do not broaden to `.` or `src` until the effective cwd is verified.
- Do not treat exit 4 as success; inspect the receipt and partial state before recovery.

## Go deeper

Read `HPRSCRIPT.md` for every edit, plan, apply, targeting, and receipt option. Read `COOKBOOK.md` for guarded-edit recipes. Find them at the hprscript repository root or alongside the binary. Run `hprscript edit -h` and `hprscript apply -h` when the installed binary may be newer than the documents.
