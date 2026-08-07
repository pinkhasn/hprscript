---
name: hprscript
description: Agent-oriented multi-pattern repository search, code intelligence, evidence collection, context ranking, and guarded bulk editing via the hprscript CLI. Use it to locate definitions and callers, batch known search terms, inspect scopes and relationships, rank relevant code, fit evidence into a context budget, or prepare immutable multi-file edit plans. Prefer native patch tools for localized semantic changes.
---

# hprscript — full-power multi-pattern search (CLI)

`hprscript` scans files (or stdin) and matches the patterns known at the current reasoning step simultaneously via Vectorscan/Hyperscan. Batch independent terms that are already known; additional calls are appropriate when earlier evidence discovers new symbols, files, or hypotheses. Search and script modes are **read-only**. File modification exists only in the explicit `edit` and `apply` commands. Default output is **JSON Lines** (one JSON object per match). Invoke through the **Bash** tool; the binary is on `PATH` as `hprscript`.

Use `hprscript` for repository-wide content discovery. Do not force an entire investigation into one giant invocation: use one call per reasoning stage, and batch every independent pattern known within that stage. If a Hyperscan limitation blocks one pattern (for example lookaround), restructure it or move the relationship into a script phase.

---

## Pick the right tool fast

| User/agent need | Preferred mode |
|---|---|
| Find one known string or regex | quick search |
| Find several known terms | one batched quick search |
| Find the most relevant files/scopes for a concept | `investigate` |
| Find definition, uses, tests, config, or related identifiers | `investigate` with profile |
| Correlate two known match sets by value or location | `query` |
| Later patterns must be generated from earlier matches | declarative adaptive query; low-level phases only as fallback |
| Mechanical transformation across many selected sites | `edit` plan + `apply` |
| Localized semantic code change | agent-native patch/edit tool |
| Match records (file, line, text, context) | quick search, default (`-p`) |
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
| Files by commit churn / match density (no script) | `-file-where 'churn(30) > 2'` / `'count(pat) >= 3'` |
| **Records** (lines) missing a field | `-absent -records line` |
| Representative usages, dedup near-identical lines | `-sample N` |
| Identifier by meaning, any naming convention | `-ident 'parse config'` (matches `parseConfig`/`parse_config`/`ConfigParser`) |
| **"Where's the code for X?"** — rank files, no script | `-hotspots N` (or `-s '{"rank":true,...}'` for surprise/rich-cluster weighting) |
| **Fit relevant context into a byte budget** | `-budget N` — ranks + renders full→compact→dropped until spent |
| Compact excerpt instead of the whole function body | `-elide` (signature + matched lines; rest folded as `… (+N lines)`) |
| Skip re-showing unchanged code across repeated calls | `-elide`/`-budget -seen <path>` |
| Sort `-f`/`-c` by relevance instead of walk order | `-order-by score\|count\|path` |
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
- `-ident '<terms>'` — match identifiers by subtoken, any casing/separator: `-ident 'parse config'` finds `parseConfig`/`parse_config`/`ConfigParser`/`PARSE_CONFIG`. Space-separated terms inside one `-ident` AND together; repeat the flag to OR groups. Each group is a first-class pattern (`ident0`, `ident1`, …) usable in `-name`/relations/`-file-where`. Search-mode only, not `edit`.
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
| `-elide` | Scope-aware chunks: signature + matched lines, everything else folded as `… (+N lines)` — cheaper than `-llm` for a match buried in a large function |

`-llm` is the best choice when **you** are about to read the matches — it strips JSON noise and dedupes file headers (~30–50% fewer tokens than `-j`). Switch back to `-j` only when you need offsets, `pat`, or `extracted`. Reach for `-elide` instead of `-llm`/`-block-open` when the match sits inside a big function and you only want the relevant slice, not the whole body.

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

### Hotspot ranking — "where's the code for X?" without a script
`-hotspots N` buffers the whole scan and ranks files by a rarity/coverage/proximity score (files matching more of the queried terms, more densely, in fewer overall files, rank higher) — the same formula script mode's `rank` uses, but no JSON needed. Each result carries its densest match window.

```bash
hprscript -p 'RetryPolicy' -p 'backoff' -hotspots 5 -llm -glob '**/*.go'
# → src/retry/policy.go:12-64 score=1.4 patterns=p0,p1
```
Composes with `-elide` (render each hotspot's full match set as a compact chunk instead of a bare pointer) and `-llm` (flat one-line-per-file); default output is JSONL. Mutually exclusive with `-sample`.

### Elided scope output — a compact excerpt, not the whole function
`-elide` prints each matched scope's signature and matched lines, folding untouched interior lines as `… (+N lines)` — implies `-scope auto` automatically. Cheaper than `-block-open`/`-block-close` when the function is large and only a couple of lines matter; matches outside any scope fall back to a plain context line.

```bash
hprscript -p 'RetryPolicy' -elide -scope auto -glob '**/*.go'
```

### Budget-packed context — the closest thing to one RAG retrieval call
`-budget N` ranks every matching file (same formula as `-hotspots`) and renders them score-descending as `-elide` chunks until `N` bytes are spent: a file that doesn't fit in full degrades to a one-line summary, then to a named "dropped" entry in a trailing footer — nothing disappears silently. Defines its own output shape (no `-j`/`-f`/`-llm`/etc; no `-sample`/`-hotspots`).

```bash
hprscript -p 'AuthMiddleware' -p 'validateToken' -budget 6000 -glob '**/*.go'
```
This is usually the **first thing to try** when a task starts with "find everything relevant to X and summarize/fix it" — one call replaces search → read N files → manually trim to fit context.

### `-seen` — stop re-paying tokens for code you already showed yourself
Iterating (search → edit → search again) re-reads unchanged functions every round. `-seen <path>` hashes each `-elide`/`-budget` chunk's raw source and collapses anything unchanged since the last run against that state file to a one-line `(unchanged, already shown)` pointer.

```bash
hprscript -p 'AuthMiddleware' -elide -seen .hpr-seen -glob '**/*.go'
```
Requires `-elide` or `-budget` (there's no "chunk" to collapse in any other mode). Worth reaching for whenever a multi-turn task keeps re-scanning the same files.

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

**Phases — sequential scans with preserved state (cross-file symbols)**

Each phase is a distinct scan. Variables populated by an earlier phase remain available to later phases. A two-phase script therefore normally reports two scan stages; it is not a single physical repository pass. Use low-level phases only when a declarative query cannot express the adaptive collect-then-scan workflow.
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

## Advanced agent examples

### Investigation

Intent

Find where `validateToken` is defined, referenced, tested, configured, and what identifiers are closely associated with it.

Why this mode

The likely follow-up questions are known categories of evidence, but the related symbol names are not known yet. Investigation performs one seed stage, local lexical analysis, and at most one batched adaptive follow-up scan.

Command

```bash
hprscript investigate -F validateToken -profile symbol -llm \
  -evidence-budget 65536 -max-memory-bytes 134217728 -glob '**/*.go'
```

Expected evidence

A selected profile, ranked files/scopes, probable lexical definitions and references, path-heuristic test/config roles, related identifiers with association scores, representative excerpts, scan counts, and explicit truncation metadata.

When not to use it

Use quick search for one exact lookup. Use query when both match sets and their relationship are already known.

### Query join

Intent

Associate captured function calls with captured function definitions by name.

Why this mode

Both sets and the equality relationship are known, so a declarative hash join is clearer than procedural script maps.

Command

```bash
hprscript query -q '{"version":1,"sets":[
 {"id":"defs","scan":["**/*.go"],"patterns":[{"id":"def","regexp":"func\\s+(\\w+)\\s*\\(","extract":["name"]}]},
 {"id":"uses","scan":["**/*.go"],"patterns":[{"id":"call","regexp":"\\b(\\w+)\\s*\\(","extract":["name"]}]}
],"query":{"from":{"set":"uses","as":"use"},"joins":[
 {"type":"inner","set":"defs","as":"def","on":[{"op":"eq","left":"use.capture.name","right":"def.capture.name"}]}
],"select":{"symbol":"use.capture.name","used_in":"use.file","defined_in":"def.file"}}}'
```

Expected evidence

One projected JSON row per lexical use/definition pair; compatible sets share one scan stage, which `-explain-plan` shows.

Joins without any equality or location predicate are conservatively rejected
when their predicted Cartesian product exceeds `limits.max_cartesian_rows`.
Use `allow_cartesian:true` only after reviewing that product intentionally.

When not to use it

Do not treat lexical pairs as compiler-resolved symbols. Use an LSP/compiler when exact overload, package, or type semantics are required.

### Query anti-join

Intent

Find captured declarations with no captured reference of the same name.

Why this mode

Absence is relational here: it means no equality partner in another named set, not merely no regex in one file.

Command

```bash
hprscript query -q '{"version":1,"sets":[
 {"id":"decls","scan":["**/*.go"],"patterns":[{"id":"decl","regexp":"var\\s+(\\w+)","extract":["name"]}]},
 {"id":"refs","scan":["**/*.go"],"patterns":[{"id":"ref","regexp":"\\b(\\w+)\\b","extract":["name"]}]}
],"query":{"from":{"set":"decls","as":"decl"},"joins":[
 {"type":"anti","set":"refs","as":"ref","on":[{"op":"eq","left":"decl.capture.name","right":"ref.capture.name"}]}
],"select":{"symbol":"decl.capture.name","file":"decl.file","line":"decl.line"}}}'
```

Expected evidence

Only declaration rows with no lexical equality match in `refs`, plus explicit resource-limit behavior when requested with `-summary`.

When not to use it

Do not call the result semantically unused without compiler/type-aware confirmation.

### Adaptive query

Intent

Extract declared configuration keys, then search for those exact values in source files even though the keys are unknown before stage one.

Why this mode

The later matcher is derived from earlier rows. The query engine deduplicates values, preserves source-row provenance, caps the pattern set, and executes one dependent scan stage.

Command

```bash
hprscript query -q '{"version":1,"sets":[
 {"id":"keys","scan":["config/**/*.yaml"],"patterns":[{"id":"key","regexp":"^([A-Z][A-Z0-9_]+):","extract":["name"]}]},
 {"id":"uses","scan":["src/**"],"derive_patterns":{"from_set":"keys","field":"capture.name","mode":"literal","deduplicate":true,"max_patterns":10000}}
],"query":{"from":{"set":"uses","as":"use"},"select":{"key":"use.derived.value","file":"use.file","line":"use.line"}}}'
```

Expected evidence

Two plan stages, a stable derived-value/source-row mapping, and usage rows whose `derived.value` identifies the declaration-generated pattern.

When not to use it

Use an ordinary join when both sets can be matched independently in one traversal. Use low-level script phases only when the declarative form cannot express the required state machine.

### Guarded edit plan

Intent

Apply the same mechanical retry-count change across an exactly reviewed target set.

Why this mode

Search qualifiers define many equivalent edit sites, and the persistent plan binds the review to exact paths, file hashes, byte ranges, old bytes, and replacements.

Command

```bash
hprscript edit -F 'retry(3)' -content 'retry(5)' -expect 14 \
  -plan-out retry.plan.json -glob 'src/**/*.go'
hprscript apply retry.plan.json -receipt json
```

Expected evidence

A dry-run diff and versioned immutable plan with no target writes, followed by an all-file preflight and a complete, no-op, refused, or partial-commit receipt.

When not to use it

Use the agent-native patch tool for one localized semantic change or for sites that need different implementations.

---

## Agent recipes (intent → invocation)

```bash
# Locate a symbol's definition across shapes, one pass
hprscript -p 'func\s+LoadData\b' -p 'type LoadData\b' -p 'LoadData\s*=' -llm -glob '**/*.go'

# Who calls X — with the enclosing function of each call site
hprscript -p '\bLoadData\(' -scope auto -llm -glob '**/*.go'

# Rank files by relevance to a feature — start here for "where do I look?"
hprscript -p 'RetryPolicy' -p 'backoff' -hotspots 5 -llm -glob '**/*.go'

# "Find everything relevant to X and fix it" — one call, budget-fit, no manual trimming
hprscript -p 'AuthMiddleware' -p 'validateToken' -budget 8000 -glob '**/*.go'

# Symbol search that survives naming-convention drift (parseConfig ~ parse_config)
hprscript -ident 'parse config' -glob '**/*.go'

# Multi-turn task on the same tree — don't re-read functions you already showed yourself
hprscript -p 'AuthMiddleware' -elide -seen .hpr-seen -glob '**/*.go'

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

The `edit` subcommand is for search-shaped, mechanical transformations across many sites. The pattern that finds the sites is the pattern that edits them, and targeting flags such as `-glob`, `-git-added-lines`, `-near`/`-far`, and `-file-where` work as edit qualifiers. Prefer a native patch tool for a localized change whose correctness depends on code semantics.

**Preferred persistent-plan workflow:**

```bash
# 1. Discover once and persist the exact byte edits plus file identities.
hprscript edit -F 'retry(3)' -content 'retry(5)' -expect 1 \
  -plan-out retry.plan.json src/worker.go
# 2. Review the plan/diff, then apply those stored edits without rescanning.
hprscript apply retry.plan.json
```

`-expect N` guards the discovery/planning invocation only. Repeating `edit ... -write -expect N` performs another scan and is not proof that the reviewed sites are unchanged. Direct `edit -write` remains available for compatibility and warns unless `-no-plan-warning` is supplied.

**What to edit** — `-span match` (default) | `line` (incl. newline) | `block` / `block-full` (balanced `$BLOCK`/`$BLOCK_FULL` ranges; needs `-block-open`/`-block-close`) | `scope` / `scope-body` (the enclosing function — with `-in-scope` active, the innermost scope *matching the filter*, so you target the function you named, not a closure).
**New content** — `-content '<tmpl>'` (tokens: `$MATCH`, `$EXTRACT_<NAME>`, `$FILE`, `$LINE`, `$$`; `\n`/`\t` interpreted) | `-content-file <f>` / `-content-stdin` (verbatim; **use for anything multi-line** — no shell quoting). | `-delete` removes the span | `-insert before|after|start|end` adds without removing.
**Guards (all exit 3, nothing written):** `-expect N` (exact site count) · `-max-span-lines N` (default 500 — catches runaway blocks) · `-assert-contains <re>` (span must still contain this) · automatic overlap/conflict/drift detection.

```bash
# Locate a function before a localized semantic patch:
hprscript -list-scopes src/data.go
hprscript -p 'func\s+LoadData\b' -scope auto -llm src/data.go

# Prepare a reviewable mechanical edit plan, then apply exactly that plan:
hprscript edit -F 'retry(3)' -content 'retry(5)' -in-scope 'ProcessBatch' \
    -expect 1 -plan-out retry.plan.json src/worker.go
hprscript apply retry.plan.json

# Append a statement at the end of main()'s body:
hprscript edit -in-scope '^main$' -span scope-body -insert end -content '\tflush()\n' -expect 1 -write cmd/run.go

# Rename only on lines this branch added:
hprscript edit -p '\bOldName\b' -content 'NewName' -git-changed -git-added-lines \
    -expect 14 -plan-out rename.plan.json
hprscript apply rename.plan.json

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

Exit codes: `0` previewed/applied · `1` no sites · `2` error · **`3` preflight/guard refused, nothing written** · **`4` apply failed after commit began and the receipt identifies the partial state**. Reapplying a completed plan is stale and fails verification with exit 3. `apply` output is a JSON or human receipt of exactly what was verified and changed.

---

## Anti-patterns

- ❌ Separate calls for patterns already known at the same reasoning step → ✅ one call with separate `-p`/`-pi` flags.
- ✅ A later call based on a symbol, scope, or hypothesis discovered by an earlier call is normal iterative investigation.
- ❌ Cramming terms you want to tell apart into one alternation (`-p 'a|b|c'`) — every hit reports `pat=p0`, so you can't see which matched → ✅ split into separate `-p` and read the `pat`/`[p0]`/`$PAT_ID` tag per match.
- ❌ Falling back to grep/rg because a lookaround/backref won't compile → ✅ restructure, or split into a `-s` phase.
- ❌ `-s '{…}'` when a flag set fits → ✅ flag mode is easier to read/verify.
- ❌ Piping output through `grep`/`jq` for trivial filtering → ✅ use `-f`/`-c`/`-o`/`-format`/`-absent`, or `group_by`/`rank` in a script.
- ❌ Dumping full JSON when you'll just read it → ✅ `-llm` (or `-f`/`-c` if you only need files/counts).
- ❌ Letting a minified line blow your context → ✅ set `-max-context-bytes`/`-max-output-bytes`.
- ❌ `sed -i` for a mechanical bulk edit (exit 0 whether it changed 0 or 500 places) → ✅ create an immutable `edit -plan-out` plan with `-expect`.
- ❌ Review one scan, then rescan with `edit … -write` → ✅ review the persisted plan and apply its exact stored edits.
- ❌ Multi-line code in `-content '…'` (shell-quoting hell) → ✅ write it to a scratch file and use `-content-file`.
- ❌ A relation-qualifier pattern without `-ref` in edit mode → ✅ mark it `-ref` or its own matches get rewritten too.
- ❌ Reading N whole files to find "the relevant part" → ✅ `-hotspots`/`-budget` rank and pack it in one call.
- ❌ Enumerating every casing variant of a symbol by hand (`parseConfig|parse_config|ConfigParser`) → ✅ `-ident 'parse config'`.
- ❌ Re-reading unchanged functions every turn of a multi-step task → ✅ `-elide`/`-budget -seen <path>`.
- ❌ Reaching for an external RAG/embedding index for "find relevant code" → ✅ hprscript computes relevance fresh from the live tree, no index to go stale.

## Going deeper

Two exhaustive references ship with hprscript — read them on demand for the long tail:
- **HPRSCRIPT.md** — complete CLI + script-DSL reference (every flag, action, condition, Unicode/UTF-8 rules, exit codes).
- **COOKBOOK.md** — 200+ task recipes by domain (logs/observability, security/DFIR, source code, config/infra, data wrangling, DevOps).

Find them in this skill's directory (personal install), at the hprscript repo root, or alongside the binary. `hprscript -h` prints the live flag list; exit codes follow grep (`0` match, `1` none, `2` error; edit mode adds `3` = guard refused, nothing written).
