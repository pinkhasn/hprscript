# hprscript

`hprscript` is a command-line multi-pattern content-search tool. It scans **any input** — files, directory trees, or arbitrary data piped on **stdin** — in a single pass, matching **all patterns simultaneously** using [Vectorscan](https://github.com/VectorCamp/vectorscan), the portable open-source fork of Intel's Hyperscan regex engine. One invocation replaces N sequential `grep`/`rg` calls. Patterns use **PCRE** syntax (the subset Hyperscan/Vectorscan accepts).

Because hprscript reads content from stdin when no files/globs are given, it slots naturally into bash pipelines — `curl … | hprscript`, `cat … | hprscript`, `kubectl logs … | hprscript`, etc.

It is a single self-contained binary with no runtime dependencies beyond the platform C library. Builds for Linux (x86-64, ARM64) and macOS (Apple Silicon / Intel).

---

## Quick start

```bash
# Search for TODO across all Go files (default JSON Lines output)
hprscript -p "TODO" -glob "**/*.go"

# Multi-pattern search in one pass (no extra cost per pattern)
hprscript -p "TODO" -p "FIXME" -p "XXX" -glob "**/*.go"

# Pipeline use — content from stdin, no glob/files needed
curl -s https://example.com | hprscript -p 'href="[^"]+"' -o
echo "hello TODO" | hprscript -pi 'todo|fixme'
kubectl logs my-pod | hprscript -p 'ERROR|panic' -C 2

# Inline JSON script (multi-pattern with custom output shape)
hprscript -s '{
  "scan": ["**/*.go"],
  "patterns": [
    {"id": "todo", "regexp": "TODO", "on_match": [
      {"action": "emit", "data": {"file": "$FILE", "line": "$LINE", "text": "$CONTEXT"}}
    ]}
  ]
}'

# Aggregating script: count TODOs per file, emit one summary line
hprscript -s '{
  "scan": ["**/*.go"],
  "variables": {"counts": {"type": "map"}},
  "patterns": [
    {"id": "t", "regexp": "TODO", "on_match": [
      {"action": "map_increment", "target": "counts", "key": "$FILE"}
    ]}
  ],
  "on_complete": [
    {"action": "for_each", "var": "counts", "key_as": "f", "as": "n", "do": [
      {"action": "emit", "data": {"file": "$f", "count": "$n"}}
    ]}
  ]
}'
```

Default per-match JSON record (every match, both in `-p` mode and as the default `emit` in script mode):

```json
{"file":"main.go","pat":"p0","line":42,"col":5,"from":1023,"to":1027,"match":"TODO","context":"// TODO: refactor"}
```

---

## Command-line usage

```
hprscript -p <pattern> [options] [files/dirs...]
hprscript -s '<json>' [files...]
hprscript -script <path> [files...]
hprscript script.json [files...]            # positional arg as script file
cat script.json | hprscript                  # script piped on stdin
```

When neither `-p` nor `-s`/`-script` is given, the first positional argument is treated as a script file. If there are no positional arguments, `hprscript` reads the script from **stdin** (when piped).

When the script supplies no `scan` and no positional file/directory arguments are given, `hprscript` reads file content from **stdin**.

Positional file/dir args after `-s`/`-script` (or after a positional script file) **override** the script's `scan` field — useful for re-using a script against a different target tree.

---

## Quick search flags (with `-p`)

| Flag | Description |
|---|---|
| `-p <pattern>` | Case-sensitive search pattern (repeatable for multi-pattern, all match in one pass) |
| `-pi <pattern>` | Case-insensitive search pattern (HS `CASELESS`; folds Unicode by default; repeatable, mixable with `-p`) |
| `-F <string>` / `-Fi <string>` | Fixed-string pattern (case-sensitive / -insensitive) — matched literally, no regex interpretation. Repeatable, mixable with `-p`/`-pi`. |
| `-name <id>` | Name the preceding `-p`/`-pi`/`-F`/`-Fi`: the id (`[A-Za-z_]\w*`) replaces the auto `p<i>` in `pat`, `$PAT_ID`, `-llm` tags, relations, and `-file-where`. |
| `-patterns-from <f>` | Load additional patterns from a JSONL rule file — one `{"id","regexp"\|"literal","case_insensitive","word_boundary","utf8"}` object per line, `#` comments allowed. Repeatable. See [Fixed strings & pattern files](#fixed-strings--pattern-files--f--fi--patterns-from). |
| `-file-where <expr>` | Per-file predicate over pattern ids (`'err AND NOT recovery'`). See [Per-file conditions](#per-file-conditions--file-where). |
| `-records line` | With `-absent`: record-level absence — one JSON record per non-empty line lacking each pattern. See [Record-level absence](#record-level-absence--records-line). |
| `-glob <glob>` | Scan glob (e.g. `"**/*.go"`; repeatable). Brace alternation is supported (`"src/**/*.{ts,tsx}"`), and absolute bases work too (`"/var/log/**/*.log"`). |
| `-exclude <pat>` | Exclude rule (repeatable). Three forms: glob (`"*.log"`), bare directory name (`"vendor"` skips any `vendor/` dir), path prefix with `/` (`"src/generated/"`). |
| `-files-from <f>` | Scan the literal paths listed in `f`, one per line (`-` = stdin). Repeatable. See [File-list input](#file-list-input--files-from---files0-from). |
| `-files0-from <f>` | Same, NUL-separated — safe for any filename (`find -print0`, `git diff --name-only -z`). |
| `-git-changed` / `-git-staged` / `-git-untracked` | Scan the files git reports as changed vs HEAD / staged / untracked. See [Git-aware selection](#git-aware-selection--git-changed---git-staged---git-untracked---git-range---git-added-lines). |
| `-git-range <r>` | Scan the files changed in a diff range (`origin/main...HEAD`). Repeatable. |
| `-git-added-lines` | Restrict matches to lines **added** by the selected diffs (quick mode; needs `-git-changed`/`-git-staged`/`-git-range`). |
| `-w` | Whole-word matching (wraps the pattern as `\b(?:expr)\b`) |
| `-no-utf8` | Disable UTF-8 mode (byte-level matching — see [UTF-8 / Unicode](#utf-8--unicode-support)) |
| `-ucp` | Enable Unicode property classes for `\w`/`\d`/`\s` (opt-in; may reject some patterns) |
| `-limit <n>` | Maximum global results — scanning stops once reached |
| `-m <n>` | Maximum results per file (like `grep -m`) |
| `-context <n>` / `-C <n>` | Symmetric context lines (like `grep -C`) |
| `-A <n>` | Lines after match (like `grep -A`) |
| `-B <n>` | Lines before match (like `grep -B`) |
| `-block-open <s>` | Opening delimiter for block extraction (e.g. `"{"`) — see [Block extraction (CLI)](#block-extraction-cli) |
| `-block-close <s>` | Closing delimiter (e.g. `"}"`). Both required to enable. |
| `-extract <names>` | Comma-separated capture-group names for the most recent `-p`/`-pi`. See [Capture-group extraction](#capture-group-extraction). |
| `-scope <lang\|auto>` | Built-in language pack for enclosing-scope annotation (`auto`, `go`, `rust`, `c`, `cpp`, `java`, `js`, `ts`). See [Enclosing scope](#enclosing-scope). |
| `-scope-pattern <regex>` | Custom scope-anchor regex (capture group 1 = name). |
| `-scope-open <s>` / `-scope-close <s>` / `-scope-kind <s>` | Custom scope delimiters and emitted `kind` label (default `func`). |
| `-near A:B:K` | Emit pattern `A`'s matches with a `B`-match within `K` lines (repeatable). See [Pattern relations](#pattern-relations--near---far). |
| `-far A:B:K` | Emit `A`'s matches with **no** `B`-match within `K` lines (repeatable, ANDs with other relations). |
| `-same-scope A:B` | Emit `A`'s matches that share their innermost enclosing scope with a `B`-match. Requires `-scope`. |
| `-not-same-scope A:B` | Emit `A`'s matches with **no** `B`-match in the same enclosing scope (e.g. `Lock` without `Unlock` in the same function). |
| `-sample <n>` | Buffer all matches, emit `n` representatives stratified by file and surrounding-line shape. See [Sample mode](#sample-mode). |
| `-max-match-bytes <n>` | UTF-8-safe truncation of `$MATCH` at `n` bytes. See [Byte budgets](#byte-budgets). |
| `-max-context-bytes <n>` | Truncate `$CONTEXT` (and `$CONTEXT_BEFORE`/`$CONTEXT_AFTER`) at `n` bytes. |
| `-max-block-bytes <n>` | Truncate `$BLOCK` / `$BLOCK_FULL` at `n` bytes. |
| `-max-output-bytes <n>` | Stop scanning once total stdout exceeds `n` bytes; emit a final `output_truncated` info record. |
| `-summary` | Emit a trailing `{"type":"summary",...}` record with scan accounting. See [Scan accounting](#scan-accounting--summary---diagnostics---require-complete). |
| `-diagnostics` | Emit `{"type":"warning",...}` records on stdout for read errors, binary skips, and missing list paths (replaces the stderr text for those). |
| `-require-complete` | Exit 2 when any file couldn't be read or a listed path was missing — partial results become hard failures. |

### Output modes (mutually exclusive)

| Flag | Description |
|---|---|
| (default) / `-j` | **JSON Lines** — one JSON object per match (pipe-friendly) |
| `-f` | File paths only, deduplicated (like `grep -l`) |
| `-c` | Match count per file, format `path:N` (like `grep -c`) |
| `-o` | Matched text only, one per line (like `grep -o`) |
| `-format <tmpl>` | Custom one-line template |
| `-absent` | Files where the pattern is **not** found (like `grep -L`) |
| `-llm` | Token-efficient plain text grouped by file (LLM-friendly). See [LLM output mode](#llm-output-mode). |

`-format` template tokens (substituted per match):

| Token | Meaning |
|---|---|
| `$FILE` | File path as scanned |
| `$LINE` | 1-based line number of match start |
| `$COL` | 1-based column of match start |
| `$FROM` | Byte offset of match start (inclusive) |
| `$TO` | Byte offset of match end (exclusive) |
| `$MATCH` | Matched text |
| `$CONTEXT` | Match line plus `-A`/`-B`/`-C` surrounding lines |
| `$PAT_ID` | Pattern id (`p0`, `p1`, … unless overridden in script mode) |
| `$BLOCK` | Block content (when `-block-open`/`-close` is active) |
| `$BLOCK_FULL` | Match start through block end (signature + body) |
| `$BLOCK_START` / `$BLOCK_END` | Byte offsets of block start / end (exclusive) |
| `$BLOCK_LINE_START` / `$BLOCK_LINE_END` | Line numbers of block start / end |
| `$BLOCK_LINE_COUNT` / `$BLOCK_BYTE_COUNT` | Block size in lines / bytes (`0` when no block was found) |
| `$ENCLOSING_NAME` | Innermost enclosing scope's name (when `-scope` active) |
| `$ENCLOSING_KIND` | Scope kind label (`func` by default; pack-specific) |
| `$ENCLOSING_LINE_START` / `$ENCLOSING_LINE_END` | Line bounds of the enclosing scope |
| `$EXTRACT_<NAME>` | Capture group named `<name>` from `-extract` (case-insensitive). Empty when no match. |

### Quick-search examples

```bash
# Default JSON Lines output (best for scripting / agents)
hprscript -p "TODO" -glob "**/*.go"

# Case-insensitive multi-pattern (-pi marks one pattern as case-insensitive),
# exclude vendor and generated dirs
hprscript -pi "error" -pi "warning" -glob "**/*.go" -exclude vendor -exclude "src/generated/"

# Mixed: case-sensitive `Error` (the Go type) and case-insensitive `todo` in one pass
hprscript -p "Error" -pi "todo" -glob "**/*.go"

# Whole-word match — finds "hello" but not "othello" or "helloworld"
hprscript -p "hello" -w -glob "**/*.txt"

# Symmetric context (3 lines either side)
hprscript -p "panic" -C 3 -limit 10 -glob "**/*.go"

# Asymmetric context (like grep -B2 -A5)
hprscript -p "panic" -B 2 -A 5 -glob "**/*.go"

# Cap results per file (like grep -m)
hprscript -p "TODO" -m 3 -glob "**/*.go"

# Just the file paths (like grep -l)
hprscript -p "TODO" -f -glob "**/*.go"

# Counts per file (like grep -c)
hprscript -p "TODO" -c -glob "**/*.go"

# Just the matched text (like grep -o)
hprscript -p 'func\s+\w+' -o -glob "**/*.go"

# Custom one-line format
hprscript -p "TODO" -format '$FILE:$LINE:$COL  $MATCH' -glob "**/*.go"

# Files missing the pattern (like grep -L)
hprscript -p "Copyright" -absent -glob "**/*.go"

# Pipe content into hprscript (no glob/files → reads stdin)
cat README.md | hprscript -pi '\bclaude\b'

# Search the response body of an HTTP fetch — typical pipeline usage
curl -s https://example.com/page.html | hprscript -p 'href="([^"]+)"' -o

# Combine with other tools — input source is irrelevant to hprscript
kubectl logs deploy/api --tail=10000 | hprscript -p 'ERROR|panic|fatal' -C 1
journalctl -u nginx --since "1 hour ago" | hprscript -pi 'timeout|refused' -o
```

---

## File-list input (`-files-from` / `-files0-from`)

Scan exactly the files another tool selected — `git diff`, `find`, a build
manifest — instead of describing them with globs:

```bash
# Only the files changed on this branch
git diff --name-only -z --diff-filter=d origin/main...HEAD |
  hprscript -files0-from - -p 'console\.log' -p '\bdebugger\b'

# Only files find selected
find src -name '*.go' -newer build.stamp -print0 |
  hprscript -files0-from - -p 'TODO'

# From a saved manifest, one path per line
hprscript -files-from manifest.txt -pi 'copyright'
```

Semantics:

- Entries are **literal paths**, never glob-interpreted — filenames
  containing `*`, `{`, `[` stay literal. `-files0-from` (NUL separators) is
  safe for *any* filename, including spaces and newlines; prefer it whenever
  the producer offers `-z` / `-print0`.
- `-` reads the list from stdin. At most one list may use stdin, and not
  while the script itself is being piped on stdin. When a file list is
  given, stdin is never treated as content to scan.
- `-exclude` rules still apply to listed files. Directories in the list are
  walked recursively. Blank entries are ignored; in newline mode a trailing
  `\r` is stripped, so CRLF lists work.
- Missing entries print a warning to stderr and are skipped — a
  `git diff --name-only` list legitimately names deleted files (add
  `--diff-filter=d` to drop them at the source). The exit code is not
  affected.
- Works in script mode too: the list overrides the script's `scan`, exactly
  like positional paths do. Repeatable, and combinable with `-glob` and
  positional paths (the union is scanned).

---

## Git-aware selection (`-git-changed` / `-git-staged` / `-git-untracked` / `-git-range` / `-git-added-lines`)

The `git diff | hprscript -files0-from -` pipeline, built in — hprscript
shells out to `git` for the file list itself:

```bash
hprscript -p 'console\.log' -git-changed            # changed vs HEAD (staged + unstaged)
hprscript -pi 'TODO|FIXME'  -git-staged             # about to be committed
hprscript -p 'password'     -git-untracked          # new, not yet tracked
hprscript -p '\bdebugger\b' -git-range origin/main...HEAD   # the whole branch
```

Selection flags union when combined (`-git-changed -git-untracked` ≈ "my
working tree view") and compose with everything else — patterns, relations,
`-file-where`, `-summary`, script mode. Selected paths behave exactly like
`-files-from` entries: literal, exclude-filtered, missing ones warn and are
skipped (deleted files are already dropped via `--diff-filter=d`). Paths are
prefixed with the repository toplevel when you run from a subdirectory, so
they always resolve. Git errors (not a repository, bad range) exit 2 with
git's message.

**`-git-added-lines`** goes one step further: instead of scanning the whole
changed file, only matches whose line was **added** by the selected diffs
survive — the "did *this change* introduce a debug print / banned API /
TODO?" question:

```bash
# Debug statements introduced on this branch — not pre-existing ones
hprscript -p '\bconsole\.log\s*\(' -name console -pi 'TODO|FIXME' -name todo \
          -git-range origin/main...HEAD -git-added-lines -llm
```

Semantics: line tables come from `git diff -U0`; a match survives when its
start line is an added line. Untracked files (with `-git-untracked`) count
whole-file. Line numbers refer to the diff's new side, so results are exact
when the working tree matches the diff target (always true for
`-git-changed`/`-git-staged`; for historical ranges, re-run from the target
checkout). Quick mode only; requires a diff-based selection flag and takes
its input from git alone (no `-glob`/positional/`-files-from` mixing).

---

## Case-insensitive matching (`-pi`)

`-pi <pattern>` is a sibling of `-p` that compiles its pattern with Hyperscan's `CASELESS` flag. **The flag is per-pattern**, so a single invocation can mix case-sensitive and case-insensitive patterns:

```bash
# Two case-insensitive patterns in one pass
hprscript -pi 'todo' -pi 'fixme' -glob '**/*.go'

# Case-sensitive `Error` (the Go type) + case-insensitive `todo`/`fixme` notes —
# you get the type usages without false positives like `error` (the variable
# name), but still catch `TODO`/`Todo`/`todo`.
hprscript -p '\bError\b' -pi 'todo|fixme' -glob '**/*.go'
```

The match record's `pat` field tells you which pattern matched (`p0`, `p1`, …), so downstream code can route findings differently per pattern.

Because of this, **prefer separate `-p` patterns over a single alternation whenever you care which branch matched.** `-p 'alpha|beta|gamma'` tags every hit `pat=p0` — the alternation is opaque, you can't tell `alpha` from `gamma`. Split it into `-p alpha -p beta -p gamma` and each hit carries its own id (`p0`/`p1`/`p2`), surfaced as `pat` in `-j`, a `[p0]` prefix in `-llm`, and `$PAT_ID` in `-format`. Adding patterns is free (all compile into one Hyperscan database and match in the same pass), so splitting costs nothing. Keep an alternation only when the branches are genuinely one signal you never need to distinguish — e.g. a single ranking weight, or one operand of a `-near`/`-far` relation. In script mode, set each pattern's `"id"` to a meaningful label (`"auth"`, `"db"`) so `$PAT_ID` reads as that label instead of `p3`. In CLI mode, [`-name`](#named-patterns--name) does the same: `-p 'auth|token' -name auth`.

Folding is **Unicode-aware** by default (UTF-8 mode is on), so `-pi 'café'` matches `CAFÉ`, and `-pi 'привет'` matches `ПРИВЕТ`. See [UTF-8 / Unicode](#utf-8--unicode-support) for the details.

### Equivalents in other modes

| Where | Form |
|---|---|
| CLI (preferred) | `-pi <pattern>` |
| CLI (inline regex flag) | `-p '(?i)<pattern>'` — works because Hyperscan accepts `(?i)`/`(?m)`/`(?s)`/`(?x)` |
| Script mode | `{"id": "x", "regexp": "...", "case_insensitive": true}` |

The inline `(?i)` form is handy when you want to scope case-insensitivity to part of a larger pattern (`(?i)error|warn` folds both, `(?i:error)|warn` folds only `error`).

---

## Named patterns (`-name`)

`-name <id>` names the most recently declared pattern (like `-extract`, it's a
postfix modifier). The id replaces the auto-assigned `p<i>` everywhere the
pattern id surfaces — the `pat` field, `$PAT_ID`, `-llm`'s `[tag]`, relation
operands, and `-file-where`:

```bash
hprscript \
  -p  'WARN'  -name warn \
  -p  'ERROR' -name err  \
  -near warn:err:3 \
  -format '$PAT_ID  $FILE:$LINE  $MATCH' -glob '**/*.log'
```

Names must be identifiers (`[A-Za-z_][A-Za-z0-9_]*`) and unique — a collision
with another name or with a different pattern's auto id (`p0`, `p1`, …) is
rejected at startup.

---

## Fixed strings & pattern files (`-F` / `-Fi` / `-patterns-from`)

`-F <string>` (and case-insensitive `-Fi`) matches its argument **literally** —
no regex interpretation, so `foo[0].bar()` needs no escaping. Fixed strings
compile into the same one-pass database and mix freely with `-p`/`-pi`:

```bash
hprscript -F 'foo[0].bar()' -p '\bTODO\b' -glob '**/*.ts'
```

`-patterns-from <file>` loads a pattern pack from a JSONL rule file — one JSON
object per line; blank lines and `#` comments are ignored:

```jsonl
# ioc-pack.jsonl — literal entries are never regex-interpreted
{"id": "bad_ip",   "literal": "192.0.2.10", "word_boundary": true}
{"id": "bad_host", "literal": "evil.example.com", "case_insensitive": true}
{"id": "eval",     "regexp": "\\beval\\s*\\("}
```

```bash
hprscript -patterns-from ioc-pack.jsonl -C 1 -glob '**/*.log'
```

Each entry takes exactly one of `regexp` or `literal`, plus optional `id`
(same rules as `-name`), `case_insensitive`, `word_boundary`, and `utf8`
(the latter two default to the global `-w` / `-no-utf8` flags). Unknown
fields are rejected with the file and line number. `-patterns-from` is
repeatable, appends to any `-p`/`-F` patterns, and cannot be combined with
`-s`/`-script` (scripts declare their own patterns).

This replaces the fragile "build one giant alternation in shell" pattern for
IOC lists: no regex-escaping bugs, no argv-length limits, and every hit is
attributed to the entry (`pat`) that produced it.

---

## Block extraction (CLI)

`-block-open` and `-block-close` pair every match with its **balanced delimiter block**. The scanner searches forward from **match-start**, finds the first opening delimiter — one contained in the match itself counts, so anchors like `^@article\{` or a PEM `-----BEGIN` header pair with their own block — then tracks nesting until depth returns to zero. This is how you grep for a function signature and pull back the function body in one go.

```bash
# Print full function bodies (signature + braces) for every Go func.
hprscript -p 'func \w+\(' -block-open '{' -block-close '}' -o '**/*.go'

# Same, default JSON Lines — extra block fields per match.
hprscript -p 'func \w+\(' -block-open '{' -block-close '}' '**/*.go'

# Multi-character delimiters work too — pull every `<div>...</div>` subtree.
hprscript -p '<div\b' -block-open '<div>' -block-close '</div>' -o '**/*.html'

# Anchor on a JSON key, extract its object value (nesting handled).
hprscript -p '"config"\s*:' -block-open '{' -block-close '}' -o '**/*.json'
```

How each output mode is affected:

| Mode | Effect |
|---|---|
| `-o` | Prints the full block (signature + body) instead of just `$MATCH` |
| (default) / `-j` | Adds `block`, `block_full`, `block_start`, `block_end`, `block_line_start`, `block_line_end` fields to each JSON record |
| `-format` | New tokens: `$BLOCK`, `$BLOCK_FULL`, `$BLOCK_START`, `$BLOCK_END`, `$BLOCK_LINE_START`, `$BLOCK_LINE_END` |
| `-f` / `-c` / `-absent` | Unaffected (no per-match payload) |

If no balanced close is found, the block fields are omitted from that match's output.

For more complex block work — searching inside the block, extracting deeply nested structures — use the script-mode [`block` action](#block-action--cross-line-extraction).

---

## Script mode (`-s` / `-script`)

A script is a JSON object that describes a multi-pattern scan plus the actions to run per match (and at file/script lifecycle points). The whole script is compiled once and runs in a single pass over each file, emitting one JSON object per `emit` action.

### Top-level fields

| Field | Type | Description |
|---|---|---|
| `scan` | `string[]` | Glob patterns for files to scan (e.g. `["**/*.py", "src/*.js"]`). Supports `**` for recursive traversal and `{a,b}` alternation (`"**/*.{yml,yaml}"`). Patterns may be relative or absolute (`"/var/log/**/*.log"`). |
| `exclude` | `string[]` | Exclude rules: glob, bare directory name, or path prefix (same semantics as the `-exclude` flag). |
| `patterns` | `object[]` | Pattern definitions (see below). Required unless `phases` is used. |
| `phases` | `object[]` | Sequential scan rounds. See [Phases](#phases). When set, `patterns` at the top level is rejected. |
| `variables` | `object` | Variable declarations (see [Variables](#variables)). |
| `context` | `int` | Symmetric context lines (sets both `context_before` and `context_after`). |
| `context_before` | `int` | Lines captured before the match (used in `$CONTEXT_BEFORE`). |
| `context_after` | `int` | Lines captured after the match (used in `$CONTEXT_AFTER`). |
| `limit` | `int` | Maximum global emitted records. Scanning stops once reached. |
| `limit_per_file` | `int` | Maximum emitted records per file (like `grep -m`). |
| `skip` | `int` | Number of records to skip before emitting. Use with `limit` for pagination. |
| `group_by` | `string` | Buffer emits and flush one JSON line per distinct value of this field. See [Match grouping](#match-grouping-group_by). |
| `rank` | `bool` | Emit a per-file relevance ranking after the scan. See [Match ranking](#match-ranking-rank). |
| `rank_surprise` | `bool` | Opt-in: fold a corpus-derived IDF-style surprise factor into each pattern's effective weight. Default `false`. See [Match ranking](#match-ranking-rank). |
| `rank_rich_clusters` | `bool` | Opt-in: scale the proximity bonus by the number of distinct pattern IDs in each cluster. Default `false`. See [Match ranking](#match-ranking-rank). |
| `on_file_end` | `action[]` | Actions to run after every file is fully scanned. |
| `on_complete` | `action[]` | Actions to run after all files (and all phases) are processed. |
| `summary` | `bool` | Emit a trailing `{"type":"summary",...}` record (same as the `-summary` flag). Default `false`. |
| `diagnostics` | `bool` | Structured `{"type":"warning",...}` records instead of stderr text for read errors / binary skips / missing list paths. Default `false`. |
| `require_complete` | `bool` | Exit 2 when any file couldn't be read or a listed path was missing. Default `false`. |

Hidden files and directories (leading `.`) are skipped **during recursive traversal** — but a scan item or glob that explicitly names a hidden path is honored: `.github/workflows/*.yml` scans the workflows, `.env` as a literal path is read, and `**` descends normally *below* an explicitly named hidden base. The skip applies to hidden entries *discovered* while recursing, never to paths you asked for. Files containing a NUL byte in their first 512 bytes are treated as binary and skipped.

Traversal order is **deterministic**: each directory's entries are visited in sorted (lexicographic) order, depth-first, so identical inputs produce identically ordered output on every filesystem and every run. File-list inputs (`-files-from`) keep the producer's order.

### Pattern object

| Field | Type | Description |
|---|---|---|
| `id` | `string` | Pattern identifier (defaults to `p<index>`). Available as `$PAT_ID`. |
| `regexp` | `string` | The PCRE regex compiled by Hyperscan. Required. |
| `case_insensitive` | `bool` | Match irrespective of case (folds Unicode in UTF-8 mode). Default `false`. |
| `word_boundary` | `bool` | Wrap pattern as `\b(?:…)\b` before compile. Default `false`. |
| `utf8` | `bool` | UTF-8 mode (`.` = codepoint, Unicode case-fold). Default `true`. Set to `false` for byte-level matching. |
| `ucp` | `bool` | Unicode property classes for `\w`/`\d`/`\s`. Default `false` (opt-in). Requires `utf8`. May reject `\w+`-style patterns as "too large". |
| `weight` | `number` | Relevance weight for `rank` mode. Default `1.0`. |
| `absent` | `bool` | If `true`, `on_match` fires once per file where this pattern is **NOT** found. See [Absent patterns](#absent-patterns). |
| `on_match` | `action[]` | Actions executed on each match. If omitted, a default `emit` is used. |

All patterns match **simultaneously** in a single pass — adding a pattern is virtually free.

### Built-in tokens

Available inside `$`-substitution (every string in `data` / `value` / `key` / format templates):

| Token | Meaning |
|---|---|
| `$FILE` | Current file path |
| `$PAT_ID` | ID of the pattern that matched |
| `$LINE` | 1-based line number of match start |
| `$COL` | 1-based column of match start |
| `$FROM` / `$TO` | Match start (inclusive) / end (exclusive) byte offsets |
| `$MATCH` | The matched text |
| `$CONTEXT` | Match line plus configured `context_before`/`context_after` lines |
| `$CONTEXT_BEFORE` | Lines before the match line (only when `context_before > 0`) |
| `$CONTEXT_AFTER` | Lines after the match line (only when `context_after > 0`) |
| `$BLOCK`, `$BLOCK_FULL`, `$BLOCK_START`, `$BLOCK_END`, `$BLOCK_LINE_START`, `$BLOCK_LINE_END`, `$BLOCK_LINE_COUNT`, `$BLOCK_BYTE_COUNT` | Available inside an `on_block` (see [Block action](#block-action-cross-line-extraction)) |
| `$LOOKUP_KEY`, `$LOOKUP_VALUE` | Available inside a `lookup`'s `on_hit` / `on_miss` |
| `$<varname>` | Any user-declared variable. When the entire string is `"$x"` the variable's native type is preserved; when embedded in a larger string it's stringified. |

`$WORD` and `$SENTENCE` are reserved tokens but currently always resolve to `0` — the lexical pass that populates them is not yet implemented.

### Variables

Declare variables in the top-level `variables` object. They persist across matches, files, and phases (until reset).

```json
{
  "variables": {
    "count":  {"type": "int"},
    "found":  {"type": "bool"},
    "items":  {"type": "list"},
    "counts": {"type": "map"},
    "label":  {"type": "string", "default": "todo"}
  }
}
```

| Type | Default value | Description |
|---|---|---|
| `string` | `""` | Text value |
| `int` | `0` | 64-bit integer |
| `bool` | `false` | Boolean |
| `list` | `[]` | Ordered list of mixed values |
| `map` | `{}` | String-keyed map of mixed values |

`reset` returns variables to their declared default. `on_file_end` is the natural place to reset per-file accumulators.

### Actions

#### Output

| Action | Description |
|---|---|
| `emit` | Emit one JSON line. Without `data`, emits the default match record. With `data`, emits that object with `$`-substitution applied to string leaves (non-string leaves are passed through verbatim). |
| `print` | Emit one raw text line (not JSON). Without `value`, emits the default record as JSON; with `value`, emits the substituted string. Bypasses `group_by` buffering. |

```json
{"action": "emit"}
{"action": "emit", "data": {"file": "$FILE", "line": "$LINE", "msg": "$MATCH"}}
{"action": "print", "value": "$FILE:$LINE: $CONTEXT"}
```

#### Variable arithmetic

| Action | Description |
|---|---|
| `set` | Set a variable to a value. |
| `increment` / `decrement` | `var ± 1` |
| `add` / `subtract` / `multiply` / `divide` | `var ⊕ value`. Division by zero is silently ignored. |
| `reset` | Restore listed variables to their defaults. |

```json
{"action": "set", "var": "found", "value": true}
{"action": "increment", "var": "count"}
{"action": "add", "var": "total", "value": "$LINE"}
{"action": "reset", "vars": ["found", "count"]}
```

#### Lists

| Action | Description |
|---|---|
| `append` | Append `value` (or default record if omitted) to list `target`. |
| `collect` | Always append the default record. |
| `unique_append` | Append only if the value is not already present. |
| `sort` | Sort a list of objects by `key` field. Pass `"value": "desc"` for descending order. |

```json
{"action": "append", "target": "results", "value": {"f": "$FILE", "l": "$LINE"}}
{"action": "unique_append", "target": "files", "value": "$FILE"}
{"action": "sort", "var": "results", "key": "line"}
```

#### Maps

| Action | Description |
|---|---|
| `map_set` | `target[key] = value` (key supports `$`-substitution). |
| `map_increment` | `target[key] += 1` (creates the key with `1` if missing). |
| `map_append` | Append `value` to the list at `target[key]` (creates the list if missing; promotes an existing scalar to a one-element list). |
| `map_unique_append` | Like `map_append`, but skips values already present — accumulates a *set* per key. |
| `count` | Shorthand for `map_increment` with `key = $PAT_ID`. |

```json
{"action": "map_set", "target": "lines", "key": "$FILE", "value": "$LINE"}
{"action": "map_increment", "target": "counts", "key": "$FILE"}
{"action": "map_unique_append", "target": "files_by_sym", "key": "$EXTRACT_NAME", "value": "$FILE"}
{"action": "count", "var": "by_pat"}
```

`map_set` is last-write-wins — one value per key. When a key can legitimately
map to *several* values (a trace-id seen in many services, a symbol used in
many files), use `map_append`/`map_unique_append`; iterating with `for_each`
then binds the whole list, which emits as a JSON array.

#### Set algebra

| Action | Description |
|---|---|
| `set_difference` | `target` = elements in `a` but not in `b`. |
| `set_intersection` | `target` = elements present in both `a` and `b`. |
| `set_union` | `target` = elements present in either `a` or `b`. |

`a` and `b` may be lists or maps. Maps are coerced to their keysets. List
elements are coerced to strings via `to_str()` and deduped. The output
`target` is always a fresh list of strings, in insertion order (`a` first,
then `b` for unions). Missing variables are treated as the empty set.

```json
{"action": "set_difference",   "target": "unused", "a": "defs", "b": "uses"}
{"action": "set_intersection", "target": "shared", "a": "groupA", "b": "groupB"}
{"action": "set_union",        "target": "all",    "a": "errors", "b": "warnings"}
```

#### Control flow

| Action | Description |
|---|---|
| `if` | Conditional execution. `condition` plus `then` (and optional `else`) action lists. |
| `for_each` | Iterate over a list (`as`) or a map (`key_as` + `as`). |
| `stop` | Stop scanning the current file (move to the next). |

```json
{"action": "if",
 "condition": {"op": "and", "args": [
   {"op": "eq", "args": ["$COL", 1]},
   {"op": "contains", "args": ["$CONTEXT", "TODO"]}
 ]},
 "then": [{"action": "emit"}]}

{"action": "for_each", "var": "counts", "key_as": "f", "as": "n", "do": [
  {"action": "emit", "data": {"file": "$f", "count": "$n"}}
]}
```

#### Sub-pattern matching, blocks, lookup

| Action | Description |
|---|---|
| `submatch` | Run sub-patterns over `$MATCH` (or custom `text`). Sub-patterns can themselves be `absent`. See [Submatch](#submatch). |
| `block` | Find a balanced block from the match position and run `on_block`. See [Block action](#block-action-cross-line-extraction). |
| `lookup` | Check `map[key]` and branch on `on_hit` / `on_miss`. Useful with [Phases](#phases) for cross-file resolution. |

```json
{"action": "submatch", "patterns": [
  {"id": "num", "regexp": "\\d+", "on_match": [{"action": "emit"}]}
]}

{"action": "block", "open": "{", "close": "}", "on_block": [
  {"action": "emit"}
]}

{"action": "lookup", "map": "defs", "key": "$_name",
 "on_hit":  [{"action": "emit", "data": {"def": "$LOOKUP_VALUE"}}],
 "on_miss": [{"action": "emit", "data": {"undef": "$LOOKUP_KEY"}}]}
```

### Conditions

Used inside `if`'s `condition` field.

| Op | Args | Description |
|---|---|---|
| `eq` / `ne` | 2 values | Loose equality (numeric/string cross-compare). |
| `gt` / `lt` / `gte` / `lte` | 2 values | Numeric comparison; falls back to string compare when both args are non-numeric. |
| `and` / `or` | n conditions | Short-circuit logical combinators. |
| `not` | 1 condition | Negation. |
| `contains` | 2 values | String contains substring, OR list contains element. |
| `isset` | variable name | `true` when the named variable is set and non-zero / non-empty. |

```json
{"op": "and", "args": [
  {"op": "gt", "args": ["$count", 0]},
  {"op": "not", "args": [{"op": "eq", "args": ["$FILE", ""]}]}
]}
```

### Lifecycle hooks

| Hook | When it runs |
|---|---|
| `on_match` (per pattern) | Each time the pattern matches |
| `on_file_end` | After every file's scan completes (top-level and per-phase). Receives only `$FILE` (no match info). |
| `on_complete` | After all files (and phases) are processed. User-defined variables only. |

When both a top-level `on_file_end` and a phase-level `on_file_end` are set, the phase-level hook fires first, then the top-level — but only on the last phase, to avoid double-firing on multi-phase scripts.

### Submatch

`submatch` runs additional patterns against text. Default text is `$MATCH`; pass `"text": "..."` to override (recognised: `$MATCH`, `$BLOCK`, `$BLOCK_FULL`, `$CONTEXT`, or any literal/substituted string).

When the text source has a known file offset (`$MATCH`, `$BLOCK`, `$BLOCK_FULL`, `$CONTEXT`), sub-matches report **file-relative** `$FROM` / `$TO` / `$LINE` / `$COL`. For arbitrary text, offsets are relative to the substring.

Sub-patterns support `absent: true` — fires `on_match` when the sub-pattern is **not** found inside the substring.

```bash
# Pull the function name out of every "func <name>(" match.
hprscript -s '{
  "patterns": [
    {"id": "fn", "regexp": "func \\w+", "on_match": [
      {"action": "submatch", "patterns": [
        {"id": "name", "regexp": "\\w+", "on_match": [
          {"action": "emit", "data": {"file": "$FILE", "name": "$MATCH"}}
        ]}
      ]}
    ]}
  ],
  "scan": ["**/*.go"]
}'
```

### Absent patterns

Marking a pattern `"absent": true` flips it: `on_match` fires once per file where the regex is **not** found, with `$FILE` and `$PAT_ID` populated (no `$MATCH` / `$LINE` / `$CONTEXT`).

Inside a `submatch`, absent sub-patterns fire when the regex is missing from the substring; they inherit the outer match's `$FILE`, `$LINE`, `$FROM`, `$TO`, `$MATCH`.

```json
{"id": "no_copyright", "regexp": "Copyright", "absent": true,
 "on_match": [{"action": "emit", "data": {"file": "$FILE", "issue": "no copyright"}}]}
```

### Block action (cross-line extraction)

The `block` action pairs every match with the **balanced delimiter block** that follows it. This is the primary way to "anchor on a signature, pull back the body" — function definitions, struct/class declarations, JSON objects, JSX trees, anything bounded by paired delimiters.

Inside `on_block`, all match variables remain available and `$BLOCK*` are populated:

| Token | Meaning |
|---|---|
| `$BLOCK` | Block content (delimiters included) |
| `$BLOCK_FULL` | From match start to block end (signature + body) |
| `$BLOCK_START` / `$BLOCK_END` | Byte offsets |
| `$BLOCK_LINE_START` / `$BLOCK_LINE_END` | Line numbers |
| `$BLOCK_LINE_COUNT` / `$BLOCK_BYTE_COUNT` | Block size in lines / bytes — handy for "functions longer than N lines" filters |

If no balanced close is found, `on_block` is silently skipped (the match is still emitted by any sibling actions).

#### How depth tracking works

The walker starts at **match-start**, scans forward looking for the first `open` delimiter — which may sit inside the match itself, so `^@article\{` anchors its own `{` and a `-----BEGIN …-----` header anchors its own `-----BEGIN` — then counts:

```
depth = 1                # we just saw the opening delimiter
for each byte after that:
    if byte starts with `open`  → depth += 1
    if byte starts with `close` → depth -= 1
    if depth == 0 → block ends here
```

So when extracting a function body with `{`/`}`, every inner `if (...) { ... }` and `for (...) { ... }` correctly increments and decrements depth. The `}` that finally brings depth back to zero is the function's own closing brace — *not* an inner one.

Delimiters can be **multi-character strings**, not just single bytes — `open: "<div>"` / `close: "</div>"` works, as do `open: "BEGIN"` / `close: "END"` for SQL or Pascal-style blocks.

#### What can fool depth tracking

Depth counting is **lexical, not language-aware**. The walker doesn't know about strings, comments, regex literals, or heredocs. These will skew the count:

```go
// String literal containing the close delimiter — adds a phantom close
fmt.Println("}")

// Comment containing an open delimiter — adds a phantom open
// {  ← this counts as depth+1
```

For typical hand-written code in C/C++/Go/Rust/Java/JS/TS the noise is usually low enough that the right `}` still wins. For code heavy with brace-containing strings (template literals, code generators, format strings), expect occasional false ends. If it bites you in practice, a workaround is to anchor the *next* sibling instead — e.g. for Perl, use `submatch` over a generous line window and stop at the next `^sub\s+\w+`.

#### Indentation-based languages

The block action is delimiter-based. **Python and Ruby don't have one.** For those, anchor on the signature, then `submatch` over a window of lines that ends at the next sibling definition (`^def `/`^class ` for Python, `^def `/`^class `/`^module ` for Ruby).

#### Example: search inside the body

Combine with `submatch` (`"text": "$BLOCK"`) to find patterns *within* the extracted block. File-relative line numbers are preserved:

```bash
# Find every TODO that lives inside a `handleRequest` function body.
hprscript -s '{
  "scan": ["**/*.go"],
  "patterns": [{"id": "fn", "regexp": "func handleRequest", "on_match": [
    {"action": "block", "open": "{", "close": "}", "on_block": [
      {"action": "submatch", "text": "$BLOCK", "patterns": [
        {"id": "todo", "regexp": "TODO", "on_match": [
          {"action": "emit", "data": {"file": "$FILE", "line": "$LINE", "todo": "$CONTEXT"}}
        ]}
      ]}
    ]}
  ]}]
}'
```

#### Example: extract a named function (any brace language)

```bash
# Perl: pull the full body of `sub LoadData`, including signature.
hprscript -s '{
  "scan": ["**/*.pl", "**/*.pm"],
  "patterns": [{"id":"fn","regexp":"sub\\s+LoadData\\b","on_match":[
    {"action":"block","open":"{","close":"}","on_block":[
      {"action":"emit","data":{
        "file":"$FILE",
        "sig_line":"$LINE",
        "end_line":"$BLOCK_LINE_END",
        "body":"$BLOCK_FULL"
      }}
    ]}
  ]}]
}'
```

The same recipe works for any `{}`-delimited language by changing the regex anchor:

| Language | Anchor regex |
|---|---|
| C / C++ | `\\w[\\w\\s\\*&]*\\bLoadData\\s*\\(` |
| Go | `func\\s+(?:\\([^)]*\\)\\s+)?LoadData\\b` |
| Rust | `fn\\s+LoadData\\b` |
| Java | `\\w[\\w\\s<>,]*\\bLoadData\\s*\\(` |
| JavaScript / TypeScript | `(?:function\\s+\\|const\\s+\\|let\\s+)LoadData\\b` |

#### Example: list every function with name + line range

```bash
# One JSON record per Go func: name, start line, end line, byte length.
hprscript -s '{
  "scan": ["**/*.go"],
  "patterns": [{"id":"fn","regexp":"func\\s+(?:\\([^)]*\\)\\s+)?\\w+","on_match":[
    {"action":"block","open":"{","close":"}","on_block":[
      {"action":"emit","data":{
        "file":"$FILE",
        "sig":"$MATCH",
        "start":"$LINE",
        "end":"$BLOCK_LINE_END",
        "bytes":"$BLOCK_END"
      }}
    ]}
  ]}]
}'
```

#### Example: extract a JSON object by anchor key

```bash
# Pull the value object that follows `"config":` — the block walker handles
# nested objects/arrays correctly because both push depth on `{`.
hprscript -s '{
  "scan": ["**/*.json"],
  "patterns": [{"id":"cfg","regexp":"\"config\"\\s*:","on_match":[
    {"action":"block","open":"{","close":"}","on_block":[
      {"action":"emit","data":{"file":"$FILE","value":"$BLOCK"}}
    ]}
  ]}]
}'
```

#### Example: extract with a non-`{}` delimiter pair

```bash
# Pull the argument list of every Go func call — `(`/`)` instead of `{`/`}`.
hprscript -s '{
  "scan": ["**/*.go"],
  "patterns": [{"id":"call","regexp":"\\bSpawn\\s*","on_match":[
    {"action":"block","open":"(","close":")","on_block":[
      {"action":"emit","data":{"file":"$FILE","line":"$LINE","args":"$BLOCK"}}
    ]}
  ]}]
}'
```

#### Example: only emit blocks containing a marker

```bash
# Find functions whose body actually mentions `panic` — anchor → block →
# absent-style check via a counter, then emit only when the counter > 0.
hprscript -s '{
  "scan": ["**/*.go"],
  "variables": {"hits":{"type":"int"}},
  "patterns": [{"id":"fn","regexp":"func\\s+\\w+","on_match":[
    {"action":"block","open":"{","close":"}","on_block":[
      {"action":"reset","vars":["hits"]},
      {"action":"submatch","text":"$BLOCK","patterns":[
        {"id":"p","regexp":"\\bpanic\\(","on_match":[
          {"action":"increment","var":"hits"}
        ]}
      ]},
      {"action":"if","condition":{"op":"gt","args":["$hits",0]},
       "then":[{"action":"emit","data":{"file":"$FILE","sig":"$MATCH","start":"$LINE","end":"$BLOCK_LINE_END"}}]}
    ]}
  ]}]
}'
```

### Match grouping (`group_by`)

Buffers every `emit` and flushes one JSON line per distinct value of the named field at end-of-scan:

```bash
hprscript -s '{
  "scan": ["**/*.go"],
  "group_by": "file",
  "patterns": [{"id": "t", "regexp": "TODO", "on_match": [
    {"action": "emit", "data": {"file": "$FILE", "line": "$LINE"}}
  ]}]
}'
```

Output:
```json
{"key":"main.go","group":[{"file":"main.go","line":12},{"file":"main.go","line":40}]}
{"key":"util.go","group":[{"file":"util.go","line":7}]}
```

`limit` still caps the total buffered records. `print` actions are not buffered — they go straight to stdout.

### Match ranking (`rank`)

After scanning, emits one JSON line per file containing at least one match, sorted by score descending (density is the tiebreaker).

The score combines three signals:

- **Coverage** — fraction of queried (non-`absent`) pattern IDs the file matches. Scaled with exponent 1.5 so a file matching all patterns dominates one matching a subset.
- **Weighted hits** — Σ `weight` over **distinct** matched pattern IDs (re-matches of the same pattern don't accumulate), normalized by `log(file_lines + 10)` so big files don't win on size alone.
- **Proximity bonus** — `+0.5` for each cluster of matches where ≥2 distinct pattern IDs co-occur within 20 lines (rewards "all matches in one function").

`score = coverage^1.5 × Σweight / log(lines + 10) + 0.5 × clusters`

`density` (Σweight / line_count) is reported for diagnostics and used as a tiebreaker.

```bash
hprscript -s '{
  "scan": ["**/*.go"],
  "rank": true,
  "patterns": [
    {"id": "endpoint", "regexp": "func handle", "weight": 3},
    {"id": "todo",     "regexp": "TODO",         "weight": 0.5}
  ]
}'
```

```json
{"file":"src/api/handler.go","score":1.43,"density":0.07,"matched_patterns":["endpoint","todo"]}
{"file":"src/util.go","score":0.06,"density":0.01,"matched_patterns":["todo"]}
```

When `rank` is enabled, the rank table **replaces** match output: per-match `emit` and `print` are suppressed so only the rank rows are written. `on_match` actions still execute (so `count`, `set`, `map_increment`, etc. continue to update variables), but record-producing actions are silenced. Aggregations from `on_file_end` and `on_complete` are **not** suppressed.

#### Corpus-surprise weighting (`rank_surprise`)

Opt-in. A pattern that matches almost every file barely distinguishes files; a pattern that matches few files strongly distinguishes them. With `rank_surprise: true`, each pattern's user-supplied `weight` is multiplied by a corpus-derived **surprise factor** (IDF-style):

```
surprise_p        = log( (N + 1) / (df_p + 1) ) + 1
effective_weight_p = user_weight_p × surprise_p
```

where `N` is the number of files in the rank table and `df_p` is the number of those files that matched pattern `p` at least once. A pattern matching every file collapses to `surprise_p = 1` (its base weight); a rare pattern is boosted. `absent` patterns are excluded.

`effective_weight_p` replaces `weight` in both `Σweight` and `density`. The corpus is too small for document-frequency to be meaningful below 3 files, so when `N < 3` all factors collapse to `1` (i.e. equivalent to the flag being off for that run).

When on, each rank row carries a `surprise` diagnostic listing the factor per matched pattern:

```json
{"file":"src/api/handler.go","score":1.95,"density":0.78,"matched_patterns":["endpoint","todo"],"surprise":{"endpoint":1.92,"todo":1}}
```

#### Rich proximity clusters (`rank_rich_clusters`)

Opt-in. By default, every cluster (≥2 distinct pattern IDs within 20 lines) contributes a flat `+0.5`. With `rank_rich_clusters: true`, each cluster instead contributes `0.5 × (distinct_pat_ids_in_cluster − 1)` — so a 2-pattern cluster still contributes `0.5` (unchanged), a 3-pattern cluster contributes `1.0`, a 5-pattern cluster `2.0`. Denser co-occurrences score higher; 2-pattern clusters are unaffected.

Combined formula (both flags on):

```
score = coverage^1.5 × Σ_distinct_matched effective_weight_p / log(lines + 10)
        + 0.5 × Σ_clusters (distinct_pat_ids_in_cluster − 1)
```

With both flags off the formula reduces exactly to the default above.

### Skip + limit (pagination)

`skip: N` discards the first N matched emit calls before any output; skipped records do **not** count toward `limit`. `limit: M` then caps the records actually emitted and stops scanning once reached. Combine for paging: `"skip": 20, "limit": 20` returns records 21–40.

### Phases

Phases are sequential scan rounds that share the variable store. Use them to collect data in pass 1 and reference it in pass 2 — e.g. find function definitions, then resolve usages.

| Phase field | Description |
|---|---|
| `id` | Required identifier |
| `patterns` | Required, non-empty pattern array |
| `scan` / `exclude` | Per-phase overrides; fall back to script-level |
| `context` / `context_before` / `context_after` | Per-phase context settings |
| `on_file_end` / `on_complete` | Per-phase lifecycle hooks |

When `phases` is present, top-level `patterns` is rejected. Script-level `limit`, `group_by`, `rank` apply across all phases. Script-level `on_complete` fires after **all** phases (per-phase `on_complete` fires after each phase).

```bash
hprscript -s '{
  "variables": {"defs": {"type": "map"}, "_n": {"type": "string"}},
  "phases": [
    {"id": "collect", "scan": ["**/*.go"],
      "patterns": [{"id": "fn", "regexp": "func \\w+", "on_match": [
        {"action": "submatch", "patterns": [
          {"id": "name", "regexp": "\\w+", "on_match": [
            {"action": "set", "var": "_n", "value": "$MATCH"}
          ]}
        ]},
        {"action": "map_set", "target": "defs", "key": "$_n", "value": "$FILE"}
      ]}]
    },
    {"id": "report", "scan": ["main.go"],
      "patterns": [{"id": "anchor", "regexp": "package", "on_match": [
        {"action": "for_each", "var": "defs", "key_as": "n", "as": "f", "do": [
          {"action": "emit", "data": {"name": "$n", "defined_in": "$f"}}
        ]},
        {"action": "stop"}
      ]}]
    }
  ]
}'
```

### Script-mode examples

```bash
# Default emit shape (no `data`)
hprscript -s '{
  "scan": ["**/*.go"],
  "patterns": [{"id": "todo", "regexp": "TODO"}]
}'
# → {"file":"main.go","pat":"todo","line":3,"col":4,"match":"TODO","context":"// TODO: ..."}

# Custom emit shape
hprscript -s '{
  "scan": ["**/*.go"],
  "patterns": [
    {"id": "todo", "regexp": "TODO", "on_match": [
      {"action": "emit", "data": {"file": "$FILE", "line": "$LINE", "text": "$CONTEXT"}}
    ]}
  ]
}'

# Multi-pattern with per-pattern output, plus context lines
hprscript -s '{
  "scan": ["**/*.go"],
  "context": 1,
  "patterns": [
    {"id": "fn", "regexp": "func\\s+\\w+", "on_match": [
      {"action": "emit", "data": {"kind": "func", "where": "$FILE:$LINE", "sig": "$MATCH"}}
    ]},
    {"id": "todo", "regexp": "TODO", "case_insensitive": true, "on_match": [
      {"action": "emit", "data": {"kind": "todo", "where": "$FILE:$LINE", "context": "$CONTEXT"}}
    ]}
  ]
}'

# Use a script file
hprscript ./find_funcs.hpr

# Override the script's scan from the command line
hprscript ./find_funcs.hpr src/parser.go src/lexer.go

# Script piped on stdin
cat ./find_funcs.hpr | hprscript

# Script file with -script flag
hprscript -script find_funcs.hpr -- src/foo.go
```

---

## UTF-8 / Unicode support

`hprscript` runs Hyperscan in **UTF-8 mode by default**. This affects how patterns interpret characters in your input — but not how match offsets are reported.

### What is Unicode-aware by default

| Behaviour | Default | Notes |
|---|---|---|
| Literal multi-byte chars in patterns | ✅ | `-p 'привет'`, `-p '你好'`, `-p '🚀'` all match correctly. |
| `.` matches one codepoint | ✅ | `^.{3}$` matches `"аб🎉"` (3 codepoints) — not 8 bytes. |
| `-pi` case-folding across scripts | ✅ | `CAFÉ` ↔ `café`, `ПРИВЕТ` ↔ `привет`. |
| `\w`, `\d`, `\s` Unicode classes | ❌ | ASCII-only by default. Add `-ucp` to make them Unicode. |
| Anchors (`^`, `$`, `\b`) | ASCII | `\b` boundary uses ASCII word-character notion. |
| Match offsets (`from`, `to`, `col`) | byte offsets | Even in UTF-8 mode, all offsets are **byte** offsets, never codepoint indices. |

### Examples

```bash
# Literal Cyrillic / CJK / emoji — matched as expected
hprscript -p 'привет' file.txt
hprscript -p '你好'  file.txt
hprscript -p '🚀'    file.txt

# `.` is codepoint-aware
printf 'аб\n' | hprscript -p '^.{2}$'
# → match found ("аб" is 2 codepoints, 4 bytes)

# Case-insensitive folds Unicode
printf 'CAFÉ café\n' | hprscript -pi 'café' -o
# → CAFÉ
# → café

# Default \w is ASCII (so "café" splits into "caf")
printf 'café\n' | hprscript -p '\w+' -o
# → caf

# Add -ucp to make \w match Unicode letters (when the pattern compiles)
printf 'café\n' | hprscript -p '[\p{L}]+' -ucp -o
# → café
```

### When to use `-ucp` vs alternatives

`-ucp` enables Hyperscan's **UCP** flag, which makes `\w`/`\d`/`\s` (and explicit `\p{L}` etc.) Unicode-aware. The downside is Hyperscan rejects many UCP patterns as **"Pattern is too large"** — notably `\w+`. This is a Hyperscan engine limit, not an `hprscript` choice.

When `-ucp` rejects your pattern, prefer:

1. **Explicit codepoint classes** in UTF-8 mode (no `-ucp` needed):
   ```bash
   # Match runs of "letter-ish" codepoints by enumerating ranges
   hprscript -p '[\p{L}\p{N}_]+' -ucp file.txt
   # If that's "too large", widen with `.`:
   hprscript -p '\S+' file.txt           # ASCII whitespace boundary
   ```

2. **Anchored patterns** so the engine has more structure to work with — `[\p{L}]{1,32}` often compiles where `\p{L}+` doesn't.

3. **ASCII `\w` + non-ASCII bytes**: `[\w\x80-\xff]+` reads "ASCII word char or any non-ASCII byte" — works in default UTF-8 mode and is good enough for many "match an identifier including non-ASCII letters" cases.

### When to disable UTF-8 mode

Use `-no-utf8` (or `"utf8": false` in script mode) when:

- Your input is **not UTF-8** (Latin-1, CP1252, raw binary, mixed encodings).
- You want `.` to match exactly one byte.
- You want `from`/`to` byte offsets but **without** UTF-8 validation (slightly faster).

```bash
# Byte-level matching (e.g. for binary log formats)
hprscript -p '\xff\xd8\xff' -no-utf8 *.bin
```

### Invalid UTF-8 input

Hyperscan makes no promise about UTF-8-mode patterns scanned over invalid UTF-8: the engine may keep matching straight past the bad bytes (the common case for ASCII patterns), or stop early and return `HS_INVALID`. `hprscript` treats either outcome as a completed scan — whatever matches were reported are emitted, no error is raised, and the next file is scanned:

```bash
# Latin-1 bytes followed by ASCII — "hello" still gets found
printf '\xe9\xe0\xea hello\n' | hprscript -p 'hello' -o
# → hello
```

The honest caveat: on a file containing invalid UTF-8, results from UTF-8-mode patterns are **best-effort, not guaranteed complete** — and no diagnostic is emitted. If your data is (or might be) non-UTF-8 — legacy encodings, binary logs, mixed sources — use `-no-utf8` (or per-pattern `"utf8": false`), which has exact semantics: byte-level matching, every byte scanned.

### Per-pattern UTF-8 / UCP in script mode

```json
{
  "scan": ["**/*.txt"],
  "patterns": [
    {"id": "ascii-id",   "regexp": "[A-Z_][A-Z0-9_]*",
                         "utf8": false},
    {"id": "uni-letter", "regexp": "\\p{L}{1,32}",
                         "ucp": true},
    {"id": "literal-jp", "regexp": "こんにちは"}
  ]
}
```

Per-pattern flags override the defaults so a single script can mix Unicode-aware and byte-level patterns.

---

## Regex syntax (Hyperscan PCRE)

Hyperscan accepts a **subset** of PCRE syntax. Most everyday patterns work without modification.

### What works

```
literal text                           → matches it
.                                      → any char except newline (default)
*  +  ?  {n,m}                         → repetition (greedy)
*?  +?  ??                             → non-greedy repetition
^  $                                   → line anchors (multiline mode is on by default)
\A  \z                                 → start / end of buffer
\b  \B                                 → word boundary, non-word boundary
\d  \D  \w  \W  \s  \S                 → standard PCRE classes
[abc]  [^abc]  [a-z]                   → character classes
(...)                                  → capturing group (Hyperscan ignores captures)
(?:...)                                → non-capturing group
(?i)  (?m)  (?s)  (?x)                 → inline flags
|                                      → alternation
\xHH  \uHHHH  \n  \t  \r  \f  \v  \0   → escape sequences
```

### What doesn't work (will fail to compile with a clear error)

```
(?=...)  (?!...)  (?<=...)  (?<!...)   → lookarounds
\1  \2  ...                            → backreferences
\K                                      → match-reset
(?>...)                                → atomic groups
(?(...)yes|no)                         → conditionals
```

If a pattern uses one of these, `hprscript` reports:

```
hprscript: pattern compile failed: <Hyperscan's exact reason>
  in pattern: <your regex>
```

### Anchor behaviour

`^` and `$` default to **line-anchored** (HS `MULTILINE` flag is set). Use `\A` / `\z` for buffer-anchored matches:

```bash
# Match "foo" at the start of any line
hprscript -p '^foo' file.txt

# Match the file content "foo\n" exactly (buffer-anchored)
hprscript -p '\Afoo\z' file.txt
```

### Whole-word matching

Two ways to do whole-word matching:

```bash
# Flag form — convenient for quick CLI use
hprscript -p "hello" -w

# Inline form — useful when only one of multiple alternatives is whole-word
hprscript -p "\bhello\b"
```

In script mode, set `"word_boundary": true` on the pattern (the regex is wrapped as `\b(?:…)\b`):

```json
{"id": "h", "regexp": "hello", "word_boundary": true}
```

---

## Match deduplication

Hyperscan reports **every** position where a pattern accepts. For greedy patterns this can mean many overlapping matches at the same start (e.g. `func\s+\w+` against `func main` reports matches ending at every position from `func m` through `func main`).

`hprscript` post-processes raw matches into **leftmost-longest non-overlapping** matches **per pattern**, which is the behaviour grep users expect:

- For each `(pattern, start_offset)`, keep the longest match.
- Within each pattern, walk left-to-right and skip matches whose start lies within a previously-emitted match.
- Across different patterns, overlap is allowed (each pattern is independent).

So `hprscript -p 'func\s+\w+' -o` against `func main\nfunc helper\n` emits exactly:

```
func main
func helper
```

---

## Exit codes

Following grep's convention:

| Code | Meaning |
|---|---|
| `0` | At least one match emitted |
| `1` | No matches |
| `2` | Error (bad flag, compile error, IO error, unsupported script feature) |

---

## What hprscript does NOT support

The following are deliberately rejected with an explicit error so they don't fail silently:

**Top-level**: `boundary`, `on_boundary`, `ascii_only`, `overlap`, `files` (per-file `at`/`from`/`extract`/line-range mode).

**Per-pattern**: `pcre` (not needed — Hyperscan is already PCRE), `run_pattern_at`, `run_pattern_from`, `run_pattern_to`, `run_pattern_until`.

**Actions**: hprscript is a read-only search tool — any action that would alter file contents on disk is rejected, as are the `--write`/`--backup` CLI flags.

**Other**: word/sentence counters (`$WORD`/`$SENTENCE` resolve to 0 — the lexical pass is not yet wired up).

---

## Cookbook for CLI agents

Patterns an agent can copy-paste and adapt.

### "Find all TODO/FIXME with context"

```bash
hprscript -pi 'TODO|FIXME|XXX' -C 1 -glob '**/*.{go,py,js,ts,rs,c,cpp,h,hpp}'
```

### "Mix case-sensitive and case-insensitive patterns in one pass"

```bash
# `Error` is the Go interface (case-sensitive); `todo|fixme|hack` are notes
# (case-insensitive). One DFA, one walk over the tree, two pattern semantics.
hprscript \
  -p  '\bError\b' \
  -pi 'todo|fixme|hack' \
  -glob '**/*.go' -exclude vendor
```

### "Case-insensitive log-level scan, case-sensitive request id"

```bash
# Request IDs are uppercase hex (case-sensitive); log levels appear in any case
# across libraries (case-insensitive). Group by file for a per-file digest.
hprscript -s '{
  "scan": ["**/*.log"],
  "group_by": "file",
  "patterns": [
    {"id":"reqid","regexp":"req-[0-9A-F]{16}",
     "on_match":[{"action":"emit","data":{"file":"$FILE","line":"$LINE","kind":"reqid","val":"$MATCH"}}]},
    {"id":"err","regexp":"error|fatal|panic","case_insensitive":true,
     "on_match":[{"action":"emit","data":{"file":"$FILE","line":"$LINE","kind":"level","val":"$MATCH"}}]}
  ]
}'
```

### "List files that import a specific package"

```bash
hprscript -p '^import\s+"fmt"' -f -glob '**/*.go'
```

### "Files missing a license header"

```bash
hprscript -p 'Copyright|SPDX-License-Identifier' -absent -glob '**/*.go'
```

### "Function signatures with their file/line"

```bash
hprscript -p 'func\s+\w+\s*\(' -format '$FILE:$LINE  $MATCH' -glob '**/*.go'
```

### "Multi-pattern lint sweep"

```bash
hprscript -glob '**/*.py' \
  -p 'print\s*\(' \
  -p 'except\s*:' \
  -p 'eval\s*\(' \
  -p 'pickle\.loads'
```

Each match has a `pat` field (`p0`, `p1`, …) so a downstream tool can group findings by pattern.

### "Tag matches with rule ids using a script"

```bash
hprscript -s '{
  "scan": ["**/*.py"],
  "exclude": ["tests/", "*.pyc"],
  "patterns": [
    {"id": "bare-print",  "regexp": "print\\s*\\(", "on_match":[
      {"action":"emit","data":{"rule":"bare-print","sev":"warn","at":"$FILE:$LINE"}}]},
    {"id": "bare-except", "regexp": "except\\s*:",  "on_match":[
      {"action":"emit","data":{"rule":"bare-except","sev":"error","at":"$FILE:$LINE"}}]},
    {"id": "eval-call",   "regexp": "\\beval\\s*\\(", "on_match":[
      {"action":"emit","data":{"rule":"eval-call","sev":"error","at":"$FILE:$LINE","src":"$CONTEXT"}}]}
  ]
}'
```

### "Read content from stdin and search"

```bash
curl -s https://example.com/page.html | hprscript -p 'href="([^"]+)"' -o
```

### "Re-use a script against a different target tree"

```bash
# Saved script that scans **/*.go by default
hprscript ./lint.hpr ./other-project/src
```

### "Count matches per file (single pass)"

```bash
hprscript -s '{
  "scan": ["**/*.go"],
  "variables": {"counts": {"type": "map"}},
  "patterns": [{"id":"t","regexp":"TODO","on_match":[
    {"action":"map_increment","target":"counts","key":"$FILE"}]}],
  "on_complete": [{"action":"for_each","var":"counts","key_as":"f","as":"n","do":[
    {"action":"emit","data":{"file":"$f","count":"$n"}}]}]
}'
```

### "Files that have A but not B"

```bash
hprscript -s '{
  "scan": ["**/*.go"],
  "variables": {"has_err": {"type": "bool"}},
  "patterns": [
    {"id":"err","regexp":"if err != nil","on_match":[
      {"action":"set","var":"has_err","value":true}]},
    {"id":"no_wrap","regexp":"fmt\\.Errorf\\(","absent":true,"on_match":[
      {"action":"if","condition":{"op":"eq","args":["$has_err",true]},
       "then":[{"action":"emit","data":{"file":"$FILE","issue":"no error wrap"}}]}]}
  ],
  "on_file_end": [{"action":"reset","vars":["has_err"]}]
}'
```

### "Rank files by relevance to a query"

```bash
hprscript -s '{
  "scan": ["**/*.go"],
  "rank": true,
  "patterns": [
    {"id":"endpoint","regexp":"func handle","weight":3},
    {"id":"error",   "regexp":"err != nil",  "weight":1},
    {"id":"todo",    "regexp":"TODO",         "weight":0.5}
  ]
}' | grep '"score"' | head -20
```

### "Group results by file"

```bash
hprscript -s '{
  "scan": ["**/*.go"],
  "group_by": "file",
  "patterns": [{"id":"t","regexp":"TODO|FIXME","on_match":[
    {"action":"emit","data":{"file":"$FILE","line":"$LINE","match":"$MATCH"}}]}]
}'
```

### "Extract function bodies"

```bash
# Quick: every Go func body, signature + braces, one per line.
hprscript -p 'func \w+\(' -block-open '{' -block-close '}' -o '**/*.go'

# JSONL with body + line range — feeds cleanly into downstream tools.
hprscript -p 'func \w+\(' -block-open '{' -block-close '}' \
  -format '$FILE:$LINE-$BLOCK_LINE_END  $MATCH' '**/*.go'

# Scripted: pull body and search inside it for TODOs.
hprscript -s '{
  "scan": ["**/*.go"],
  "patterns": [{"id":"fn","regexp":"func handleRequest","on_match":[
    {"action":"block","open":"{","close":"}","on_block":[
      {"action":"submatch","text":"$BLOCK","patterns":[
        {"id":"todo","regexp":"TODO","on_match":[
          {"action":"emit","data":{"file":"$FILE","line":"$LINE"}}]}]}]}]}]
}'
```

See [Block action (cross-line extraction)](#block-action-cross-line-extraction) for how depth tracking works, what can fool it (strings/comments containing delimiters), and recipes for non-`{}` delimiters and indentation-based languages.

### "Extract a single named function"

```bash
# Anchor on the exact name — only that function's body is emitted.
hprscript -p 'func\s+LoadData\b' -block-open '{' -block-close '}' -o '**/*.go'

# Perl: same idea, different signature shape.
hprscript -p 'sub\s+LoadData\b' -block-open '{' -block-close '}' -o '**/*.pl' '**/*.pm'
```

Worked example against a real Perl source file — `qualify_to_ref` from the standard `Symbol.pm` module:

```
$ hprscript -p 'sub qualify_to_ref' -block-open '{' -block-close '}' -o perl/run/lib/5.36.1/Symbol.pm
sub qualify_to_ref ($;$) {
    no strict 'refs';
    return \*{ qualify $_[0], @_ > 1 ? $_[1] : caller };
}
```

Note how the depth counter handles the `\*{ ... }` dereference correctly: the inner `{` pushes depth to 2, the inner `}` brings it back to 1, and only the final `}` on its own line ends the block.

### "Function call arguments"

```bash
# `(`/`)` block extraction pulls the full argument list of every `Spawn(` call,
# nested parens included — useful for "what does this function get called with?"
# audits without writing a parser.
hprscript -p '\bSpawn\s*' -block-open '(' -block-close ')' -o '**/*.go'
```

### "Pull a JSON object by anchor key"

```bash
# Grab the value of "config" from every JSON file. The walker handles nested
# objects and arrays (both push depth on `{`/`[`), so you get the whole subtree.
hprscript -p '"config"\s*:' -block-open '{' -block-close '}' -o '**/*.json'
```

### "Resolve symbols across files (two phases)"

```bash
# Phase 1 collects pub fn definitions; phase 2 reports each `use crate::`
# import — hits get the file the symbol is defined in.
hprscript -s '{
  "variables": {"defs":{"type":"map"},"_n":{"type":"string"}},
  "phases": [
    {"id":"defs","scan":["**/*.rs"],
      "patterns":[{"id":"def","regexp":"pub fn \\w+","on_match":[
        {"action":"submatch","patterns":[
          {"id":"name","regexp":"\\w+","on_match":[
            {"action":"set","var":"_n","value":"$MATCH"}]}]},
        {"action":"map_set","target":"defs","key":"$_n","value":"$FILE"}]}]},
    {"id":"uses","scan":["**/*.rs"],
      "patterns":[{"id":"use","regexp":"use crate::\\w+::\\w+","on_match":[
        {"action":"submatch","patterns":[
          {"id":"sym","regexp":"\\w+","on_match":[
            {"action":"set","var":"_n","value":"$MATCH"}]}]},
        {"action":"lookup","map":"defs","key":"$_n",
         "on_hit":[{"action":"emit","data":{"sym":"$_n","def":"$LOOKUP_VALUE","use":"$FILE","line":"$LINE"}}],
         "on_miss":[{"action":"emit","data":{"sym":"$_n","undef":true,"use":"$FILE","line":"$LINE"}}]}]}]}
  ]
}'
```

### "Annotate every match with its enclosing function"

```bash
# `-scope auto` picks the language pack from each file's extension. The JSON
# record now includes `enclosing.{name,kind,line_start,line_end}` so an agent
# can group findings by function without a follow-up scan.
hprscript -p 'TODO|FIXME' -scope auto -glob '**/*.{go,rs,ts,js}'
```

### "Find calls to X with their containing function"

```bash
# Each match record carries the function name → great for security/audit work.
hprscript -p '\bdangerous_call\(' -scope auto \
          -format '$FILE  $ENCLOSING_NAME:$LINE  $MATCH' \
          -glob '**/*.go'
```

### "Pull names + arg lists out of every function signature"

```bash
hprscript -p 'func\s+(\w+)\s*\(([^)]*)\)' -extract name,args \
          -format '$EXTRACT_NAME($EXTRACT_ARGS)  @ $FILE:$LINE' \
          -glob '**/*.go'
```

### "All `defer` calls that have `Lock()` within 3 lines"

```bash
hprscript -p 'defer\b' -p 'Lock\(\)' -near p0:p1:3 -glob '**/*.go'
```

### "All `log.Print` calls without an `// allow-print` annotation on the same line"

```bash
hprscript -p 'log\.Print' -p 'allow-print' -far p0:p1:0 -glob '**/*.go'
```

### "Show me ~10 representative usages of `httpClient`"

```bash
hprscript -p 'httpClient' -sample 10 -glob '**/*.go'
# Returns at most 10 matches stratified by file and surrounding-line shape —
# avoids pages of near-duplicates from the same call pattern.
```

### "Search a noisy file with explicit context budget"

```bash
hprscript -p 'TODO' -max-context-bytes 200 -max-output-bytes 50000 \
          -glob '**/*.{js,ts,go}'
# Each match's $CONTEXT is capped at 200 bytes (UTF-8 safe). Scan stops once
# total stdout exceeds 50KB and emits a trailing `output_truncated` info line.
```

### "Page through results"

```bash
# First page (records 1–20)
hprscript -s '{"scan":["**/*.go"],"limit":20,"patterns":[{"id":"t","regexp":"TODO"}]}'

# Next page (records 21–40)
hprscript -s '{"scan":["**/*.go"],"skip":20,"limit":20,"patterns":[{"id":"t","regexp":"TODO"}]}'
```

---

## Capture-group extraction

Hyperscan ignores `(...)` capture groups. To surface them, declare names with
`-extract` (CLI) or `extract:[…]` (script) — `hprscript` runs a `std::regex`
post-pass over each `$MATCH` to pull out the groups by position. Names are
matched to groups left-to-right in pattern order.

**Single group — pull one value:**

```bash
# Extract the version from `version: X.Y.Z` lines.
hprscript -p 'version:\s*([\d.]+)' -extract version
# →  …,"extracted":{"version":"1.2.3"}
```

**Two groups — split a key/value pair:**

```bash
# Each `KEY=VALUE` assignment becomes two named fields.
hprscript -p '(\w+)=(\S+)' -extract key,value
# →  …,"extracted":{"key":"PATH","value":"/usr/bin"}
```

**Multiple groups — function name + arg list:**

```bash
# Extract function name + arg list from each Go signature.
hprscript -p 'func\s+(\w+)\(([^)]*)\)' -extract name,args -glob '**/*.go'
# →  …,"extracted":{"name":"main","args":""}
# →  …,"extracted":{"name":"helper","args":"x int, y string"}
```

Inside `-format`, use `$EXTRACT_<NAME>` (case-insensitive) tokens:

```bash
hprscript -p 'func\s+(\w+)\(([^)]*)\)' -extract name,args \
          -format '$FILE:$LINE  $EXTRACT_NAME($EXTRACT_ARGS)' '**/*.go'
```

In script mode:

```json
{"id": "fn", "regexp": "func\\s+(\\w+)\\(([^)]*)\\)", "extract": ["name", "args"]}
```

The default record gains an `extracted` map; the `$EXTRACT_<NAME>` token is
also available inside custom `data` shapes.

**Optional / alternation — unmatched groups become empty strings:**

```bash
# `TODO: …` or `TODO(alice): …` — `author` is empty when the `(name)` is omitted.
hprscript -p 'TODO(?:\(([^)]+)\))?:\s*(.*)' -extract author,message
# →  …,"extracted":{"author":"alice","message":"refactor this"}
# →  …,"extracted":{"author":"","message":"fix later"}
```

**Limitations.** The extract regex must compile under `std::regex`'s
ECMAScript flavor. Most patterns Hyperscan accepts compile cleanly, but a
few PCRE-only constructs (some Unicode classes, recursion) won't — those are
rejected at compile time with a clear error pointing to the offending
pattern. Fall back to `submatch` for those cases.

---

## Enclosing scope

For each match, attach the innermost containing function/class/struct to the
record. Saves the round-trip "what function is this in?" follow-up that
agents otherwise have to make.

Two ways to configure:

**Built-in language packs** — pick a language and `hprscript` uses a
sensible default scope-anchor regex:

```bash
# Auto-detect by file extension (recommended for mixed trees).
hprscript -p TODO -scope auto '**/*.{go,rs,c,cpp,h,java,js,ts}'

# Explicit pack:
hprscript -p TODO -scope go '**/*.go'
```

Supported packs: `go`, `rust`, `c`, `cpp` (`c++`/`cc`), `java`, `js`, `ts`. Any other value is rejected with an error (exit 2) rather than silently disabling scope annotation. The C-family packs ignore control-flow statements (`if (…) {`, `for (…) {`, `catch (…) {`, …) that would otherwise look like function signatures to the anchor regex.

**Custom anchors** — for languages or scope shapes the packs don't cover.
The anchor regex's first capture group becomes the scope name:

```bash
hprscript -p TODO -scope-pattern 'sub\s+(\w+)' \
                  -scope-open '{' -scope-close '}' \
                  -scope-kind perl-sub  '**/*.pl'
```

**JSON output** gains an `enclosing` field on every match inside a scope:

```json
{"file":"main.go","line":42,"match":"TODO","enclosing":{"name":"handleRequest","kind":"func","line_start":36,"line_end":58}}
```

**Format tokens**: `$ENCLOSING_NAME`, `$ENCLOSING_KIND`,
`$ENCLOSING_LINE_START`, `$ENCLOSING_LINE_END`.

In script mode, set `scope` at the top level — either a string for built-in
packs, or an object for custom:

```json
{"scope": "go"}
{"scope": {"pattern": "sub\\s+(\\w+)", "open": "{", "close": "}", "kind": "sub"}}
```

The default record gains an `enclosing` map; `$ENCLOSING_*` tokens work in
custom `data` shapes too.

**Brace-only.** Indentation-based languages (Python, Ruby) don't have a
delimiter to anchor on; for those, use `submatch` over a window ending at
the next sibling definition.

---

## Pattern relations (`-near` / `-far`)

Filter matches of one pattern by proximity (or distance) to matches of
another. Both flags are repeatable; multiple relations AND together.

```bash
# `defer` lines that have a `Lock()` within 3 lines (line distance):
hprscript -p 'defer\b' -p 'Lock\(\)' -near p0:p1:3 -glob '**/*.go'

# `log.Print` lines that DON'T have a `// allow-print` on the same line:
hprscript -p 'log\.Print' -p 'allow-print' -far p0:p1:0 -glob '**/*.go'

# Combine: defer with Unlock nearby AND no Lock nearby:
hprscript -p 'defer\b' -p 'Unlock' -p 'Lock' \
          -near p0:p1:3 -far p0:p2:3
```

`A:B:K` syntax — `A` and `B` are pattern IDs (`p0`, `p1`, …) or zero-based
indices; `K` is the line distance (0 = same line). When `A == B`, the match
itself is excluded from "nearby" matches.

In script mode:

```json
{"relations": [
  {"kind": "near", "a": "defer", "b": "lock",   "lines": 3},
  {"kind": "far",  "a": "defer", "b": "unlock", "lines": 0}
]}
```

### Scope relations (`-same-scope` / `-not-same-scope`)

Line distance is a proxy; structural containment is often what you mean.
`-same-scope A:B` keeps `A`'s matches only when a `B`-match lies inside the
**same innermost enclosing scope**; `-not-same-scope A:B` keeps them only when
none does. Both require an active `-scope` (built-in pack or custom anchor):

```bash
# Locks with no unlock in the same function — the classic leak sweep.
hprscript -p '\block\(\)' -name lk -p '\bunlock\(\)' -name ul \
          -not-same-scope lk:ul -scope c -glob '**/*.c'

# eval() calls in the same function as request input
hprscript -p '\beval\(' -name ev -p '\$_(GET|POST|REQUEST)' -name in \
          -same-scope ev:in -scope-pattern 'function\s+(\w+)' \
          -scope-open '{' -scope-close '}' -glob '**/*.php'
```

Details: scope relations AND with any `-near`/`-far` relations; when
`A == B`, "same scope" requires a *second* occurrence; `A`-matches outside
any recognized scope (or in files the scope pack doesn't map) are dropped by
`-same-scope` and kept by `-not-same-scope`. B-side matches are not filtered —
restrict output to the A side by `pat` if needed. CLI-only in v1 (script mode
can express the same via `$ENCLOSING_*` and variables).

---

## Per-file conditions (`-file-where`)

`-file-where <expr>` gates a file's **entire output** on a boolean predicate
over which patterns matched in it. Operators: `AND`/`OR`/`NOT`
(case-insensitive) or `&&`/`||`/`!`, plus parentheses; operands are pattern
ids (names, `p0`/`p1`, or numeric indices):

```bash
# Files with unrecovered errors — no script, no variables
hprscript -p 'ERROR|FATAL' -name err -pi 'recovered|retried' -name rec \
          -file-where 'err AND NOT rec' -f -glob '**/*.log'

# Mixed migration state: old API present, new API missing
hprscript -p '\boldClient\.' -name old -p '\bnewClient\.' -name new \
          -file-where 'old && !new' -c -glob '**/*.go'
```

The predicate is evaluated once per file after relations are applied; files
that fail it emit nothing (in any output mode) and don't count toward
`-limit`. `-file-where` composes with `-f`/`-c`/default/`-llm`/`-sample`, but
not with `-absent` — express absence inside the predicate instead
(`-file-where 'NOT x' -f` is exactly `grep -L`). This replaces the
script-mode "has A but not B" variable boilerplate for the common cases.

---

## Record-level absence (`-records line`)

`-absent` answers "which **files** lack the pattern". `-records line` refines
it to "which **records** (lines) lack it" — the JSONL/CSV/log-line question
`-absent` alone can't answer:

```bash
# Every JSONL record missing a required field — file, line, and the record
hprscript -p '"user_id"' -absent -records line -glob '**/*.jsonl'
# → {"file":"data.jsonl","pat":"p0","line":2,"record":"{\"other\": 2}"}
```

Semantics: one JSON record per non-empty line lacking each pattern (blank
lines are skipped; a line "contains" a pattern when a match **starts** on
it). `record` is truncated to `-max-context-bytes` (flagged
`record_truncated`). Multiple patterns are checked independently — name them
to tell the misses apart. `-limit` and `-max-output-bytes` apply. v1
requires `-absent`, supports `line` records only, and cannot combine with
relations.

---

## Sample mode

`-sample N` collects matches across all files, then emits `N`
representatives stratified by `(file, surrounding-line shape)`. The "shape"
is the matched line with identifier-like runs collapsed to `_` and
whitespace runs collapsed to a single space — so `x := foo()` and
`y := bar()` both reduce to `_ := _()` and count as one shape.

```bash
# 5 representative matches across the whole repo, one per (file, shape).
hprscript -p 'http\.Client' -sample 5 -glob '**/*.go'
```

Round-robins across files: pass 1 takes one match from each file's first
distinct shape; pass 2 advances to each file's next shape; etc. Stops at
`N` matches or when no buckets remain. Use this when you need to "see
representative usages" of a pattern without paging through hundreds of
near-duplicates.

**Constraints.**

- Memory cap: at most `max(100×N, 10000)` matches buffered across all
  files. Files exceeding the cap silently truncate.
- Mutually exclusive with output modes that don't emit per-match payloads
  (`-f`, `-c`, `-absent`) — combining them errors out.
- CLI-only in v1. Script-mode sampling is planned; for now, run a CLI
  sample and pipe results into a follow-up script if needed.

---

## Byte budgets

Cap text-field sizes and total output to keep agent context windows from
blowing up on minified/long-line files.

| Flag | Effect |
|---|---|
| `-max-match-bytes <n>` | Truncate `$MATCH` (UTF-8 safe). |
| `-max-context-bytes <n>` | Truncate `$CONTEXT` and the `$CONTEXT_BEFORE`/`$CONTEXT_AFTER` siblings. |
| `-max-block-bytes <n>` | Truncate `$BLOCK` / `$BLOCK_FULL`. |
| `-max-output-bytes <n>` | Stop scanning once total stdout exceeds `n` bytes; emit a final `{"info":"output_truncated","emitted":N}` line. |

Truncation is UTF-8 aware — the cut always lands on a codepoint boundary,
so JSON output never carries a half-codepoint. Per-field truncation is
flagged in JSONL records:

```json
{"file":"big.min.js","line":1,"match":"…cut…","match_truncated":true,"truncated":true}
```

In script mode, the same names are top-level fields:

```json
{"max_match_bytes": 200, "max_context_bytes": 500, "max_output_bytes": 200000}
```

---

## Scan accounting (`-summary` / `-diagnostics` / `-require-complete`)

Three opt-in switches (CLI flags in both modes; also script booleans
`summary` / `diagnostics` / `require_complete`) that turn silent gaps into
explicit signals:

**`-summary`** appends one typed record after all output:

```json
{"type":"summary","files_scanned":840,"files_skipped_binary":3,
 "files_failed":1,"missing_paths":0,"matches":131,"emitted":50,
 "complete":false,"stop_reason":"limit","elapsed_ms":210}
```

- `files_scanned` — files whose content was actually scanned; `files_skipped_binary` — NUL-detected skips; `files_failed` — open/read failures; `missing_paths` — nonexistent `-files-from` entries.
- `matches` — matches surviving dedup and relations; `emitted` — output records actually written (differs under `-limit`, `-m`, `-f`, grouping…).
- `complete` — `true` iff nothing failed, nothing was missing, and no early stop occurred. `stop_reason` (only when stopped early): `"limit"` or `"output_budget"`. An explicit `-limit` therefore reports `complete:false` — informative, not an error.
- In multi-phase scripts, counters accumulate across phases (a file scanned by two phases counts twice).

The summary is the only record carrying a `type` field, so it never collides with match records.

**`-diagnostics`** re-routes the per-file warnings as structured records on **stdout** (instead of stderr text), so agents parse a single stream:

```json
{"type":"warning","code":"read_error","file":"secrets/locked.env"}
{"type":"warning","code":"binary_skip","file":"assets/logo.png"}
{"type":"warning","code":"missing_path","file":"deleted-in-diff.go"}
```

**`-require-complete`** makes partial results a hard failure: exit 2 (with a stderr message) when `files_failed > 0` or `missing_paths > 0`. Explicit caps (`-limit`, `-max-output-bytes`) and by-design binary skips do **not** trip it — it guards against the scans you *didn't know* were partial. Typical belt-and-braces sweep:

```bash
hprscript -p 'BEGIN.*PRIVATE KEY' -summary -diagnostics -require-complete -glob '**/*'
```

---

## LLM output mode

`-llm` emits a compact, plain-text format intended for direct consumption by language models — no JSON parsing, no per-match metadata noise, just file → line → matched text. Far cheaper in tokens than `-j` for the same information.

Layout: one **file header** per file (deduped — never repeated), then each match indented as `  <line>: <text>`. With more than one pattern active, each line carries a `[<pat-id>]` tag so you can tell which pattern fired. The output adapts to whichever extras are active:

| Active flag | Per-match line shape |
|---|---|
| (none) | `  <line>: <match line>` — with ≥2 patterns: `  <line>: [<pat>] <match line>` |
| `-block-open`/`-block-close` | `  <line_start>-<line_end>` then the full block content on following lines |
| `-scope <pack>` / `-scope-pattern …` | `  <line>: <match line>  [in <kind> <name>]` |
| Both block + scope | block form, with a `[in <kind> <name>]` suffix on the header |

When `-limit` or `-max-output-bytes` truncates the output, a final `--- limit reached: ... ---` (or `--- output-byte budget reached ... ---`) footer line is emitted so the reader knows the result was cut, not finished.

### Examples

```bash
# Plain matches — one block per file
hprscript -p 'TODO|FIXME' -llm -glob '**/*.go'
# →
# pkg/foo.go
#   42:     // TODO refactor
#   88:     // FIXME edge case
# pkg/bar.go
#   17: // TODO add tests

# With scope — agent gets "what function is this in?" inline
hprscript -p 'TODO' -llm -scope auto -glob '**/*.{go,rs,ts}'
# →
# api/handler.go
#   42:     // TODO refactor  [in func handleRequest]

# With block extraction — full body of every Go func that mentions the anchor
hprscript -p 'func \w+\(' -llm -block-open '{' -block-close '}' -glob '**/*.go'
# →
# main.go
#   4-7
# func main() {
#     fmt.Println("hello world")
# }

# Truncation footer makes incomplete output explicit
hprscript -p 'TODO' -llm -limit 1 -glob '**/*.go'
# →
# main.go
#   3: // TODO: refactor this
# --- limit reached: stopped at 1 matches; more may exist (re-run with -limit 0 for all) ---
```

### When to use it

- **Agent reading matches into context.** `-llm` strips JSON keys and quoting, so the same N matches occupy ~30–50% fewer tokens than `-j`.
- **Quick human eyeballing.** The grouped layout reads like `grep -n` output rather than JSON Lines — easier to skim.
- **You don't need byte offsets / capture groups.** Pattern IDs still show up (as `[<pat>]` tags when multiple patterns are active), but if a downstream tool needs `from`/`to` offsets or the `extracted` map, stick with `-j`.

`-llm` is mutually exclusive with the other output modes (`-j`, `-f`, `-c`, `-o`, `-format`, `-absent`).

---

## Build and install

```bash
make            # produces ./hprscript (statically linked Vectorscan)
make install    # copies to ~/.local/bin/hprscript
```

Requires a Vectorscan install at `/opt/vectorscan` (override with `VECTORSCAN_PREFIX=...`). See [README → Build from source](README.md#build-from-source) for the one-time Vectorscan build recipe.

The binary depends only on the platform C library — on Linux verify with `ldd hprscript` (`libc`, `libm`, `libpthread`, `ld-linux`), on macOS with `otool -L hprscript` (`libSystem`, `libc++`). Builds for Linux (x86-64, ARM64) and macOS (Apple Silicon / Intel).

---

## Tips for agents using `hprscript`

- **Output is JSON Lines.** Every `-p` match is emitted as a JSON object on its own line — unambiguous, easy to parse line-by-line, and includes byte offsets you can feed back into other tools. `-j` is accepted as a no-op alias for the default.
- **Use `-limit` aggressively** when you only need to know whether a pattern exists; it stops scanning early and keeps your context small.
- **Prefer `-f` or `-c`** when you only need the file list or counts — they're far cheaper to read than per-match output.
- **Combine patterns** with multiple `-p` flags or a `patterns` array rather than running `hprscript` repeatedly. Hyperscan adds patterns to the same DFA in one pass.
- **Case-insensitivity is per pattern.** Use `-pi <pattern>` (CLI) or `"case_insensitive": true` (script) on the patterns that need folding — and leave the rest case-sensitive. Mixing in one invocation keeps you to a single scan.
- **Use `\b` (or `-w`) for identifier matching** to avoid matching inside larger words.
- **Anchor with `^` / `$`** for line-shaped matches (multiline mode is the default).
- **Pattern compile errors** mean the regex uses a feature Hyperscan doesn't support. Check the error message; common culprits are lookarounds, backreferences, and (with `-ucp`) `\w+`-style patterns. Rewrite to use plain `\b`, character classes, alternation, or `[\p{L}]{1,N}` instead of `\p{L}+`.
- **UTF-8 is on by default**, so literal Cyrillic/CJK/emoji patterns and Unicode case-folding "just work". `from`/`to`/`col` are always **byte** offsets even in UTF-8 mode.
- **Don't reach for `-ucp` reflexively.** Default `\w` is ASCII; that's usually what you want for code. Use `-ucp` only when you need Unicode `\w`/`\d`/`\s` and accept that some patterns won't compile.
- **Use scripts to keep work in one process.** `variables` + `on_complete` lets you compute aggregates (counts, distinct files, top-N) in a single scan instead of pipelining. `phases` collapses two-pass workflows (collect-then-resolve) into one invocation.
- **Reach for `group_by` / `rank` before sort+head.** They produce the right shape directly and avoid round-tripping through shell tools.
- **`absent` patterns are great for "files missing X" sweeps** — license headers, error wrapping, security headers. They fire once per file at end-of-scan, no scripting needed beyond a single pattern.
- **Use `block` (script) or `-block-open`/`-block-close` (CLI) for cross-line extraction.** It's the fastest way to grab a function body, struct definition, or JSON object given its anchor.
- **Annotate matches with their enclosing function via `-scope auto`** — agents almost always want "what function is this in?", and a single flag avoids the follow-up scan.
- **`-extract` saves the most common nested-`submatch` boilerplate** — declare names for capture groups inline and they show up as a typed `extracted` map in JSON, plus `$EXTRACT_<NAME>` tokens in `-format`.
- **`-near A:B:K` and `-far A:B:K` express "X with/without Y nearby" in one call.** Common agent intents (`defer` near `Lock()`, `log.Print` without `// allow-print` on the same line) become single-flag queries.
- **Use `-sample N` for "show me representative usages"** when an agent doesn't need every match — diversifying by file and surrounding-line shape produces a better picture in fewer tokens than `-limit N`.
- **Set byte budgets defensively.** A `-max-context-bytes 500 -max-output-bytes 200000` floor protects an agent from a single minified line wiping out its context. Truncation is reported explicitly via per-field `*_truncated` flags and a final `output_truncated` info record — never silent.
- **Prefer `-llm` over `-j` when piping matches into an LLM.** It strips JSON noise, dedupes file headers, tags each line with its pattern id when several patterns are active, and adapts to `-block-open`/`-scope` automatically — same information, ~30–50% fewer tokens. Switch back to `-j` only when you need byte offsets or `extracted` capture groups.
- **hprscript does not modify files.** It is a read-only search tool — any action that would alter file contents on disk is rejected, as are the `--write`/`--backup` flags.
