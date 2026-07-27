---
name: hprscript
description: Full-power multi-pattern content search, code intelligence, and guarded file editing via the `hprscript` CLI (Vectorscan/Hyperscan). One pass matches ALL patterns at once, so multi-pattern search costs the same as single-pattern. Covers keyword/regex search, finding files that contain or lack a pattern, per-file counts, extracting function bodies and balanced delimiter blocks, annotating each match with its enclosing function, capture-group extraction, proximity filters (X near or without Y), representative-sample usages, ranking files by relevance to a query, and aggregation/grouping/cross-file symbol resolution via a JSON script DSL. The `edit` subcommand does precise search-targeted file modification (replace/insert/delete, whole functions by name via -in-scope/scope spans) with dry-run diffs, -expect count guards, and atomic writes. -in-scope/-lines restrict search or edits to a named function/class or line range; -list-scopes outlines a file's functions. TRIGGER whenever you would grep/search/find/locate/count across files, ask where something is defined, what calls it, show me usages, or which files contain X — or would otherwise reach for the Grep tool, `grep`, or `rg`; ALSO trigger for mechanical multi-site edits, renames, or function-body swaps where you would otherwise reach for `sed -i` or repeated Edit-tool calls. Invoke via the Bash tool; the `hprscript` binary is on PATH.
---

# hprscript — full-power multi-pattern search (CLI)

`hprscript` scans files (or stdin) **once** and matches **every pattern simultaneously** via Vectorscan/Hyperscan. Adding patterns is virtually free — **always batch patterns into one call** instead of running the tool N times. Search and script modes are **read-only**; file modification exists only in the explicit `hprscript edit` subcommand (dry-run by default — see [Editing files](#editing-files-hprscript-edit)). Default output is **JSON Lines** (one JSON object per match). Invoke through the **Bash** tool; the binary is on `PATH` as `hprscript`.

Always prefer `hprscript` over the Grep tool, `grep`, or `rg` — for the whole task. If a Hyperscan limitation blocks one pattern (e.g. lookaround), restructure the pattern or move to a `-s` script; **do not** fall back to grep/rg.

---

## Pick the right tool fast

| Your intent | Reach for |
|---|---|
| Match records (file, line, text, context) | default (`-p`) |
| Only filenames that **contain** a pattern | `-f` |
| Filenames **missing** a pattern | `-absent` |
| Per-file match counts | `-c` |
| Only the matched text | `-o` |
| Cheapest way to read matches into your own context | `-llm` |
| Function body / JSON value / JSX subtree (cross-line) | `-block-open` + `-block-close` |
| "What function/class is this match inside?" | `-scope auto` |
| Only matches **inside function/class named X** | `-in-scope 'X'` |
| Only matches in a **line range** | `-lines A:B` |
| **File outline** — every function/class with line ranges | `-list-scopes` |
| Pull capture groups out as named fields | `-extract a,b` |
| X **with** Y nearby / X **without** Y nearby | `-near` / `-far` |
| X with/without Y in the **same function** | `-same-scope` / `-not-same-scope` (+ `-scope`) |
| Files where A but not B (no script) | `-file-where 'a AND NOT b'` |
| **Records** (lines) missing a field | `-absent -records line` |
| Representative usages, dedup near-identical lines | `-sample N` |
| **Rank files by relevance** to a set of signals | `-s '{"rank":true,...}'` |
| Counts, sums, manifests, grouping, cross-file resolve | `-s` script DSL |

Cheapness ladder when you don't need the match text: `-f` / `-absent` / `-c` ≪ `-o` / `-llm` ≪ default JSON. Use `-limit N` aggressively when you only need to know *whether* something exists — scanning stops early.

---

## Core flags

**Patterns**
- `-p <pat>` — case-sensitive pattern (repeatable; all match in one pass).
- `-pi <pat>` — case-insensitive pattern (Unicode-folding; repeatable; mix freely with `-p`).
- `-F <str>` / `-Fi <str>` — fixed string, matched literally (`-F 'foo[0].bar()'` — no escaping).
- `-name <id>` — name the preceding pattern; the id replaces `p0`/`p1` in `pat`, `[tags]`, relations, `-file-where`.
- `-patterns-from <f>` — JSONL rule pack: `{"id","regexp"|"literal","case_insensitive",…}` per line, `#` comments. The right tool for IOC lists — no alternation-building in shell.
- `-w` — whole-word (`\b(?:…)\b`). Or write `\b…\b` inline for per-alternative control.

**Split alternations into separate `-p` patterns when you want to know *which* branch matched *where*.** Every match is tagged with the id of the pattern that produced it — `pat` in `-j`, a `[p0]`/`[p1]` prefix in `-llm`, `$PAT_ID` in `-format`/scripts. With one alternation (`-p 'alpha|beta|gamma'`) every hit collapses to `p0` — you lose attribution. Split it (`-p alpha -p beta -p gamma`) and each hit reports `p0`/`p1`/`p2`, so you see exactly which term fired on each line. Since adding patterns is free, **default to splitting**; keep an alternation only when the branches are one concept you don't need to tell apart, or must share a single `-near`/`-far` operand. In `-s` scripts give each pattern a meaningful `"id"` (`"auth"`, `"db"`) and it shows up verbatim as `$PAT_ID` instead of `p3`.

**Targeting**
- `-glob '<glob>'` — e.g. `'**/*.go'`, `'src/**/*.{ts,tsx}'`, absolute `'/var/log/**/*.log'` (repeatable).
- `-exclude <rule>` — glob (`'*.min.js'`), bare dir name (`vendor` skips any `vendor/`), or path prefix (`'src/generated/'`). Repeatable.
- `-files0-from -` / `-files-from <f>` — scan exactly the paths piped in (NUL / newline separated; `-` = stdin). Paths are literal (no glob interpretation); missing ones warn and are skipped. Works in script mode too (overrides `scan`).
- `-git-changed` / `-git-staged` / `-git-untracked` / `-git-range A...B` — scan what git says changed, no pipeline needed. Add `-git-added-lines` (with a diff-based flag) to match only lines the change **introduced** — the "did this diff add a debug print?" review question in one flag.
- Positional files/dirs also work. **No glob + no files → reads stdin** (pipelines).

**Matching / Unicode**
- UTF-8 is **on by default**: literal Cyrillic/CJK/emoji and `-pi` case-folding "just work"; `.` = one codepoint.
- `\w \d \s` are **ASCII** by default. `-ucp` makes them Unicode (but rejects many patterns as "too large" — prefer `[\p{L}]{1,32}` or `[\w\x80-\xff]+`).
- `-no-utf8` — byte-level matching (non-UTF-8 input, binary).

**Volume / context**
- `-limit <n>` global cap (stops scanning) · `-m <n>` per-file cap.
- `-C <n>` (= `-context`) symmetric · `-A <n>` after · `-B <n>` before.

## Output modes (mutually exclusive)

| Flag | Output |
|---|---|
| default / `-j` | JSON Lines: `{"file","pat","line","col","from","to","match","context"}` |
| `-f` | File paths only, deduped (`grep -l`) |
| `-absent` | Files where the pattern is **not** found (`grep -L`) |
| `-c` | `path:N` per-file counts (`grep -c`) |
| `-o` | Matched text only (`grep -o`) |
| `-format '<tmpl>'` | Custom line with `$FILE $LINE $COL $MATCH $CONTEXT $FROM $TO $PAT_ID` (+ `$BLOCK*`, `$ENCLOSING_*`, `$EXTRACT_*` when active) |
| `-llm` | Token-efficient text grouped by file; auto-adapts to `-block`/`-scope`; prints a `limit reached` footer when truncated |

`-llm` is the best choice when **you** are about to read the matches — it strips JSON noise and dedupes file headers (~30–50% fewer tokens than `-j`). Switch back to `-j` only when you need offsets, `pat`, or `extracted`.

---

## Pattern syntax (Hyperscan PCRE)

Most everyday regex works. **Supported:** `. * + ? {n,m}`, lazy `*? +? ??`, `^ $` (line-anchored — multiline is default), `\A \z` (buffer), `\b \B`, `\d \w \s` + negations, classes `[...]`, groups `(...)`/`(?:...)`, alternation `|`, inline flags `(?i)(?m)(?s)(?x)`, escapes `\xHH \uHHHH \n \t …`.

**NOT supported** (clear compile error — restructure, don't fall back to grep):
- Lookarounds `(?=) (?!) (?<=) (?<!)`
- Backreferences `\1`, atomic groups `(?>…)`, conditionals `(?(…))`, `\K`

Gotchas: escape literal braces (`interface\{\}`); captures `(...)` are ignored unless you add `-extract`; `from`/`to`/`col` are always **byte** offsets even in UTF-8 mode.

---

## Power features (the reasons to use hprscript)

### Block extraction — anchor on a signature, pull the whole body
`-block-open` + `-block-close` pair each match with its **balanced** delimiter block (depth-tracked; nesting handled). The search starts at match-start, so an opener inside the match itself counts — `^@article\{` anchors its own `{`, a PEM `-----BEGIN` header its own block.

```bash
# Every Go function body (signature + braces)
hprscript -p 'func \w+\(' -block-open '{' -block-close '}' -o '**/*.go'
# A single named function
hprscript -p 'func\s+LoadData\b' -block-open '{' -block-close '}' -o '**/*.go'
# Multi-char delimiters: a <div>…</div> subtree
hprscript -p '<div\b' -block-open '<div>' -block-close '</div>' -o '**/*.html'
# JSON value by anchor key (nested objects/arrays handled)
hprscript -p '"config"\s*:' -block-open '{' -block-close '}' -o '**/*.json'
# Call argument lists: ( ) instead of { }
hprscript -p '\bSpawn\s*' -block-open '(' -block-close ')' -o '**/*.go'
```
Depth tracking is **lexical, not language-aware** — a `}` inside a string/comment can skew it (usually fine for hand-written code). **Python/Ruby have no brace** — anchor the signature and use a script `submatch` over a window ending at the next `^def `/`^class `.

### Enclosing scope — "what function is this in?" inline
`-scope auto` (or `go|rust|c|cpp|java|js|ts`) adds `enclosing.{name,kind,line_start,line_end}` to every match — no follow-up scan.

```bash
hprscript -p '\bdangerous_call\(' -scope auto -llm -glob '**/*.go'
# Custom anchor (capture group 1 = name):
hprscript -p TODO -scope-pattern 'sub\s+(\w+)' -scope-open '{' -scope-close '}' -scope-kind perl-sub '**/*.pl'
```

**Scoped targeting** (search + edit; both imply `-scope auto`): `-in-scope '<name-re>'` keeps only matches inside a scope whose name matches — the whole enclosing chain counts, so code in a closure inside `ProcessBatch` is "inside ProcessBatch" (repeatable = OR; `-in-scope-kind <k>` restricts kind). `-lines N | A:B | A: | :B` keeps only matches starting on those lines (repeatable = OR; line numbers go stale — prefer `-in-scope`). `-list-scopes` dumps the scope index itself — a file outline, no patterns: `hprscript -list-scopes src/data.go` → one `{"type":"scope","name","kind","line_start","line_end"}` per function/class (`-llm` for flat lines; `-in-scope` filters it).

```bash
hprscript -p 'retry\(' -in-scope 'ProcessBatch' -llm '**/*.go'   # only inside that function
hprscript -list-scopes -llm src/data.go                          # outline: pick edit targets by name
```

### Capture-group extraction — groups become typed fields
`-extract name1,name2` re-extracts the **preceding** `-p`/`-pi`'s capture groups (left-to-right) into an `extracted` map; usable as `$EXTRACT_<NAME>` in `-format`.

```bash
hprscript -p 'func\s+(\w+)\(([^)]*)\)' -extract name,args \
          -format '$FILE:$LINE  $EXTRACT_NAME($EXTRACT_ARGS)' '**/*.go'
# Optional groups become empty strings:
hprscript -p 'TODO(?:\(([^)]+)\))?:\s*(.*)' -extract author,message
```

### Pattern relations — X with/without Y nearby (or in the same scope)
`A:B:K` — A and B are pattern IDs (names via `-name`, `p0`,`p1`,…, or indices); K = line distance (0 = same line). Repeatable; multiple relations AND. `-same-scope A:B` / `-not-same-scope A:B` use the innermost enclosing scope instead of line distance (requires `-scope`).

```bash
hprscript -p 'defer\b' -p 'Lock\(\)' -near p0:p1:3 -glob '**/*.go'      # defer with Lock within 3 lines
hprscript -p 'log\.Print' -p 'allow-print' -far p0:p1:0 -glob '**/*.go' # log.Print with no allow-print on the line
hprscript -p '\bLock\(\)' -name lk -p '\bUnlock\(\)' -name ul \
          -not-same-scope lk:ul -scope go -glob '**/*.go'               # Lock with no Unlock in the same func
```

### Sample — representative usages, not 500 near-duplicates
`-sample N` returns ≤N matches stratified by `(file, line-shape)` (identifiers→`_`), round-robined across files. Great for "show me how X is used."

```bash
hprscript -p 'httpClient' -sample 10 -glob '**/*.go'
```

### Byte budgets — protect your context from minified files
```bash
hprscript -p 'TODO' -max-context-bytes 200 -max-output-bytes 50000 -glob '**/*.{js,ts}'
```
`-max-match-bytes` · `-max-context-bytes` · `-max-block-bytes` · `-max-output-bytes` (stops scan, emits an `output_truncated` info line). UTF-8-safe; truncation is flagged with `*_truncated`, never silent.

### Scan accounting — know when a sweep was partial
`-summary` appends `{"type":"summary","files_scanned":…,"files_failed":…,"matches":…,"complete":…,"stop_reason":…}` — check `complete` before trusting a broad sweep. `-diagnostics` emits `{"type":"warning","code":"read_error|binary_skip|missing_path","file":…}` records on stdout instead of stderr text. `-require-complete` exits 2 if any file couldn't be read or a listed path was missing. Output order is deterministic (sorted traversal), so diffs between runs are meaningful.

---

## Script mode (`-s '<json>'`) — the full DSL

Reach for `-s` when flags aren't enough: **aggregation** (counts/sums/manifests), **grouping**, **ranking**, **phases** (collect→resolve across files), **submatch/block** logic, **absent+present** combined, **set algebra**, pagination. The whole script compiles once and runs in a single pass. Pass a file with `-script <path>`, positionally, or on stdin; positional paths after `-s` override the script's `scan`.

**Top-level:** `scan`, `exclude`, `patterns` (or `phases`), `variables`, `context`/`context_before`/`context_after`, `limit`, `limit_per_file`, `skip`, `group_by`, `rank` (+ `rank_surprise`, `rank_rich_clusters`), `relations`, `scope`, `max_*_bytes`, `on_file_end`, `on_complete`.

**Pattern object:** `id`, `regexp` (required), `case_insensitive`, `word_boundary`, `utf8` (default true), `ucp`, `weight` (for rank), `absent` (fire once per file where NOT found), `extract:[…]`, `on_match:[actions]` (omit → default emit).

**Tokens** (in any `data`/`value`/`key` string): `$FILE $PAT_ID $LINE $COL $FROM $TO $MATCH $CONTEXT $CONTEXT_BEFORE $CONTEXT_AFTER`; in `on_block`: `$BLOCK $BLOCK_FULL $BLOCK_START $BLOCK_END $BLOCK_LINE_START $BLOCK_LINE_END $BLOCK_LINE_COUNT $BLOCK_BYTE_COUNT`; in `lookup`: `$LOOKUP_KEY $LOOKUP_VALUE`; with scope: `$ENCLOSING_*`; any var: `$name` (whole-string `"$x"` keeps native type; embedded → stringified).

**Variables** (`{"v":{"type":…,"default":…}}`): `string` `int` `bool` `list` `map`. Reset to default with `{"action":"reset","vars":[…]}` (typically in `on_file_end`).

**Actions**

| Group | Actions |
|---|---|
| Output | `emit` (JSON line; `data` optional), `print` (raw text; bypasses `group_by`) |
| Arithmetic | `set` `increment` `decrement` `add` `subtract` `multiply` `divide` `reset` |
| Lists | `append` `collect` `unique_append` `sort` (by `key`; `"value":"desc"`) |
| Maps | `map_set` `map_increment` `map_append`/`map_unique_append` (list/set per key) `count` (= map_increment on `$PAT_ID`) |
| Set algebra | `set_difference` `set_intersection` `set_union` (`{target,a,b}`; lists/maps→keysets) |
| Control | `if` (`condition`+`then`/`else`) · `for_each` (`as` for lists; `key_as`+`as` for maps) · `stop` (skip rest of file) |
| Cross-line | `submatch` (sub-patterns over `$MATCH`/`text`; sub-patterns may be `absent`) · `block` (`open`/`close` → `on_block`) · `lookup` (`map`/`key` → `on_hit`/`on_miss`) |

**Conditions** (`if`): `eq ne gt lt gte lte` · `and or not` · `contains` (substring or list element) · `isset` (var set & non-zero/non-empty).

**Lifecycle:** `on_match` (per match) → `on_file_end` (after each file; `$FILE` only) → `on_complete` (after all files & phases).

### High-value script patterns

**Aggregate — count per file, one summary line each**
```bash
hprscript -s '{
  "scan":["**/*.go"],
  "variables":{"counts":{"type":"map"}},
  "patterns":[{"id":"t","regexp":"TODO","on_match":[
    {"action":"map_increment","target":"counts","key":"$FILE"}]}],
  "on_complete":[{"action":"for_each","var":"counts","key_as":"f","as":"n","do":[
    {"action":"emit","data":{"file":"$f","count":"$n"}}]}]
}'
```

**Group — one JSON line per file (`group_by`)**
```bash
hprscript -s '{"scan":["**/*.go"],"group_by":"file","patterns":[
  {"id":"t","regexp":"TODO|FIXME","on_match":[
    {"action":"emit","data":{"file":"$FILE","line":"$LINE","match":"$MATCH"}}]}]}'
```

**Rank — which files are most relevant to a query** (your fastest "where should I look?" tool)
```bash
hprscript -s '{
  "scan":["**/*.go"],
  "rank":true, "rank_surprise":true, "rank_rich_clusters":true,
  "patterns":[
    {"id":"endpoint","regexp":"func handle","weight":3},
    {"id":"auth","regexp":"auth|token|session","case_insensitive":true,"weight":2},
    {"id":"db","regexp":"db\\.|sql\\.","weight":1}]
}'
# → {"file":"api/handler.go","score":1.95,"density":0.78,"matched_patterns":[...],"surprise":{...}}
```
`score = coverage^1.5 × Σweight / log(lines+10) + clusters`. `rank_surprise` weights each pattern by IDF-style corpus rarity (matches-everywhere → ~1×, rare → boosted; needs ≥3 files). `rank_rich_clusters` rewards dense co-occurrence (3 patterns in 20 lines = +1.0). Rank rows **replace** per-match output.

**Phases — collect in pass 1, resolve in pass 2 (cross-file symbols)**
```bash
hprscript -s '{
  "variables":{"defs":{"type":"map"},"_n":{"type":"string"}},
  "phases":[
    {"id":"defs","scan":["**/*.go"],"patterns":[{"id":"def","regexp":"func (\\w+)","extract":["name"],"on_match":[
      {"action":"map_set","target":"defs","key":"$EXTRACT_NAME","value":"$FILE"}]}]},
    {"id":"uses","scan":["**/*.go"],"patterns":[{"id":"call","regexp":"(\\w+)\\(","extract":["name"],"on_match":[
      {"action":"lookup","map":"defs","key":"$EXTRACT_NAME",
       "on_hit":[{"action":"emit","data":{"sym":"$EXTRACT_NAME","defined_in":"$LOOKUP_VALUE","used_in":"$FILE","line":"$LINE"}}]}]}]}
  ]
}'
```

**Block + submatch — TODOs that live inside a specific function**
```bash
hprscript -s '{"scan":["**/*.go"],"patterns":[{"id":"fn","regexp":"func handleRequest","on_match":[
  {"action":"block","open":"{","close":"}","on_block":[
    {"action":"submatch","text":"$BLOCK","patterns":[{"id":"todo","regexp":"TODO","on_match":[
      {"action":"emit","data":{"file":"$FILE","line":"$LINE","todo":"$CONTEXT"}}]}]}]}]}]}'
```

**Absent + present — files that have A but not B** (CLI shortcut: `-file-where 'a AND NOT b' -f` covers the common case without a script)
```bash
hprscript -s '{
  "scan":["**/*.go"], "variables":{"has_err":{"type":"bool"}},
  "patterns":[
    {"id":"err","regexp":"if err != nil","on_match":[{"action":"set","var":"has_err","value":true}]},
    {"id":"no_wrap","regexp":"fmt\\.Errorf\\(","absent":true,"on_match":[
      {"action":"if","condition":{"op":"eq","args":["$has_err",true]},
       "then":[{"action":"emit","data":{"file":"$FILE","issue":"err without wrap"}}]}]}],
  "on_file_end":[{"action":"reset","vars":["has_err"]}]
}'
```

**Pagination:** `"skip":20,"limit":20` → records 21–40.

---

## Agent recipes (intent → invocation)

```bash
# Locate a symbol's definition across shapes, one pass
hprscript -p 'func\s+LoadData\b' -p 'type LoadData\b' -p 'LoadData\s*=' -llm -glob '**/*.go'

# Who calls X — with the enclosing function of each call site
hprscript -p '\bLoadData\(' -scope auto -llm -glob '**/*.go'

# Rank files by relevance to a feature (see Rank above) — start here for "where do I look?"

# Files importing a package
hprscript -p '^import\s+"net/http"' -f -glob '**/*.go'

# Files MISSING a license header
hprscript -p 'Copyright|SPDX-License-Identifier' -absent -glob '**/*.go'

# Multi-language TODO/FIXME sweep — split so each hit reports which tag fired (pat=p0/p1/p2)
hprscript -pi 'TODO' -pi 'FIXME' -pi 'XXX' -C 1 -llm -glob '**/*.{go,py,js,ts,rs,c,cpp,h}'

# API surface: every function signature with file:line
hprscript -p 'func\s+\w+\s*\(' -format '$FILE:$LINE  $MATCH' -glob '**/*.go'

# Representative usages of an identifier (no near-duplicate spam)
hprscript -p '\bhttpClient\b' -sample 8 -scope auto -glob '**/*.go'

# Mixed case-sensitivity in one pass: the Error type + case-insensitive notes
hprscript -p '\bError\b' -pi 'todo|fixme|hack' -glob '**/*.go' -exclude vendor

# Search piped input (stdin) — no glob/files
kubectl logs deploy/api --tail=10000 | hprscript -p 'ERROR|panic|fatal' -C 1
curl -s https://example.com | hprscript -p 'href="([^"]+)"' -extract url -o
```

---

## Editing files (`hprscript edit`)

The `edit` subcommand replaces `sed -i` and repeated exact-string Edit calls for search-shaped modifications. The pattern that finds the sites is the pattern that edits them, and **every targeting flag above** (`-glob`, `-git-added-lines`, `-near`/`-far`, `-file-where`, …) works as an edit qualifier. Dry-run is the default; **nothing is written without `-write`**.

**Always use the two-step contract:**

```bash
# 1. Dry-run: prints a unified diff + a site count. Verify both.
hprscript edit -F 'retry(3)' -content 'retry(5)' src/worker.go
# 2. Apply with that count. Any drift in between → exit 3, nothing written.
hprscript edit -F 'retry(3)' -content 'retry(5)' -expect 1 -write src/worker.go
```

**What to edit** — `-span match` (default) | `line` (incl. newline) | `block` / `block-full` (balanced `$BLOCK`/`$BLOCK_FULL` ranges; needs `-block-open`/`-block-close`) | `scope` / `scope-body` (the enclosing function — with `-in-scope` active, the innermost scope *matching the filter*, so you target the function you named, not a closure).
**New content** — `-content '<tmpl>'` (tokens: `$MATCH`, `$EXTRACT_<NAME>`, `$FILE`, `$LINE`, `$$`; `\n`/`\t` interpreted) | `-content-file <f>` / `-content-stdin` (verbatim; **use for anything multi-line** — no shell quoting). | `-delete` removes the span | `-insert before|after|start|end` adds without removing.
**Guards (all exit 3, nothing written):** `-expect N` (exact site count) · `-max-span-lines N` (default 500 — catches runaway blocks) · `-assert-contains <re>` (span must still contain this) · automatic overlap/conflict/drift detection.

```bash
# Replace a function PURELY BY NAME (anchorless: no -p needed). Write the new
# impl to a file first (no trailing newline: the span ends at `}`):
hprscript -list-scopes src/data.go                       # outline: confirm the name
hprscript edit -in-scope '^LoadData$' -span scope \
    -content-file /tmp/new_impl.go -expect 1 -write src/data.go

# Same via a signature pattern when you can't use scope packs:
hprscript edit -p 'func\s+LoadData\b' -block-open '{' -block-close '}' \
    -span block-full -content-file /tmp/new_impl.go -expect 1 -write src/data.go

# Bump a constant only inside one function:
hprscript edit -F 'retry(3)' -content 'retry(5)' -in-scope 'ProcessBatch' -expect 1 -write src/worker.go

# Append a statement at the end of main()'s body:
hprscript edit -in-scope '^main$' -span scope-body -insert end -content '\tflush()\n' -expect 1 -write cmd/run.go

# Rename only on lines this branch added:
hprscript edit -p '\bOldName\b' -content 'NewName' -git-changed -git-added-lines -expect 14 -write

# Capture groups (sed s/// equivalent):
hprscript edit -p 'log\.Printf\("([^"]*)"' -extract fmt -content 'logger.Infof("$EXTRACT_FMT"' -write -glob '**/*.go'

# Insert into a delimited block:
hprscript edit -p '^import \(' -block-open '(' -block-close ')' -span block \
    -insert end -content '\t"corp/log"\n' -expect 1 -write main.go

# Conditional: skip lines with an allow-comment. The qualifier MUST be -ref,
# or its own matches get edited too:
hprscript edit -p 'http://' -name hit -p 'allow-insecure' -name allow -ref \
    -far hit:allow:0 -content 'https://' -write -glob '**/*.yaml'
```

Exit codes: `0` previewed/applied · `1` no sites · `2` error · **`3` guard refused, nothing written** — branch on 3 as "my assumption was wrong". Reruns are safe: already-applied sites report `"status":"noop"`. Output with `-write` is JSONL edit records + an `edit-summary` — the receipt of exactly what changed.

---

## Anti-patterns

- ❌ Multiple sequential `hprscript` calls for different patterns → ✅ one call, many `-p`/`-pi` (free).
- ❌ Cramming terms you want to tell apart into one alternation (`-p 'a|b|c'`) — every hit reports `pat=p0`, so you can't see which matched → ✅ split into separate `-p` and read the `pat`/`[p0]`/`$PAT_ID` tag per match.
- ❌ Falling back to grep/rg because a lookaround/backref won't compile → ✅ restructure, or split into a `-s` phase.
- ❌ `-s '{…}'` when a flag set fits → ✅ flag mode is easier to read/verify.
- ❌ Piping output through `grep`/`jq` for trivial filtering → ✅ use `-f`/`-c`/`-o`/`-format`/`-absent`, or `group_by`/`rank` in a script.
- ❌ Dumping full JSON when you'll just read it → ✅ `-llm` (or `-f`/`-c` if you only need files/counts).
- ❌ Letting a minified line blow your context → ✅ set `-max-context-bytes`/`-max-output-bytes`.
- ❌ `sed -i` for code edits (exit 0 whether it changed 0 or 500 places) → ✅ `hprscript edit` with `-expect`: wrong assumptions become exit 3, not silent damage.
- ❌ `edit … -write` straight away → ✅ dry-run first, read the diff and count, then re-run with `-write -expect <count>`.
- ❌ Multi-line code in `-content '…'` (shell-quoting hell) → ✅ write it to a scratch file and use `-content-file`.
- ❌ A relation-qualifier pattern without `-ref` in edit mode → ✅ mark it `-ref` or its own matches get rewritten too.

## Going deeper

Two exhaustive references ship with hprscript — read them on demand for the long tail:
- **HPRSCRIPT.md** — complete CLI + script-DSL reference (every flag, action, condition, Unicode/UTF-8 rules, exit codes).
- **COOKBOOK.md** — 200+ task recipes by domain (logs/observability, security/DFIR, source code, config/infra, data wrangling, DevOps).

Find them in this skill's directory (personal install), at the hprscript repo root, or alongside the binary. `hprscript -h` prints the live flag list; exit codes follow grep (`0` match, `1` none, `2` error; edit mode adds `3` = guard refused, nothing written).
