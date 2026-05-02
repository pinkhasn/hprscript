"""Compressed agent-facing reference for hprscript.

Returned by the `help` MCP tool. Read once at session start to internalise
the surface, then call the dedicated MCP tools rather than reaching for
shell `grep`/`rg`. The full HPRSCRIPT.md is exposed as an MCP resource.
"""

CHEAT_SHEET = """\
# hprscript MCP — agent quick reference

`hprscript` is a multi-pattern PCRE search tool (Hyperscan engine). It
scans files **once** and matches **all patterns simultaneously**, so adding
patterns is virtually free. Use this MCP instead of pipelining grep/rg.

## When to use which tool

| Tool              | Use when …                                                         |
|-------------------|--------------------------------------------------------------------|
| `search`          | You want match records (file, line, context) for one+ patterns.    |
| `list_files`      | You only need filenames that contain (or lack) a pattern.          |
| `count_per_file`  | You want per-file counts, not individual matches.                  |
| `extract_blocks`  | You want each match paired with its balanced `{...}` block body.   |
| `run_script`      | Aggregations, phases, ranking, grouping, submatch — full DSL.      |
| `help`            | Returns this reference (you're reading it).                        |

Prefer `list_files` / `count_per_file` over `search` when you don't need
the actual match text — they return far less data.

## Pattern semantics

- Patterns use **PCRE syntax** (Hyperscan subset). Most everyday regex works.
- **Not supported** (compile error): lookarounds `(?=)`/`(?!)`/`(?<=)`/`(?<!)`,
  backreferences `\\1`, atomic groups `(?>...)`, conditionals, `\\K`.
- Anchors: `^`/`$` are **line-anchored** by default (multiline mode on).
  Use `\\A`/`\\z` for buffer-anchored.
- `\\b` works (ASCII word notion). Use the `whole_word` flag for `\\b...\\b`.
- UTF-8 mode is on by default — literal Cyrillic/CJK/emoji "just works",
  and `case_insensitive_patterns` folds Unicode (CAFÉ ↔ café).
- `\\w`/`\\d`/`\\s` are ASCII by default. For Unicode, prefer explicit
  classes (`[\\p{L}]{1,32}`) over `\\p{L}+` (the engine often rejects
  unbounded UCP repetition as "too large").

## Output shape

`search` returns:
```json
{"matches": [{"file": "main.go", "pat": "p0", "line": 42, "col": 5,
              "from": 1023, "to": 1027, "match": "TODO",
              "context": "// TODO: refactor"}, ...],
 "count": N, "exit_code": 0}
```

`list_files` returns `{"files": [...], "count": N, "exit_code": 0}`.
`count_per_file` returns `{"counts": [{"file": "...", "count": N}, ...]}`.
`extract_blocks` records add `block`, `block_full`, `block_line_start`,
`block_line_end`, `block_start`, `block_end`.

`exit_code`: `0` = matches found, `1` = no matches, `2` = error (the
`error` field will be populated and `matches`/`files` will be empty).

## Common recipes

### TODO/FIXME sweep across multiple languages
```
search(case_insensitive_patterns=["TODO|FIXME|XXX"],
       globs=["**/*.{go,py,js,ts,rs,c,cpp,h,hpp}"],
       context=1)
```

### Files importing a specific package
```
list_files(patterns=["^import\\\\s+\\"fmt\\""], globs=["**/*.go"])
```

### Files missing a license header
```
list_files(patterns=["Copyright|SPDX-License-Identifier"],
           globs=["**/*.go"], mode="absent")
```

### Pull every Go function body
```
extract_blocks(patterns=["func\\\\s+\\\\w+\\\\("],
               block_open="{", block_close="}",
               globs=["**/*.go"])
```

### Pull a JSON object by anchor key
```
extract_blocks(patterns=["\\"config\\"\\\\s*:"],
               block_open="{", block_close="}",
               globs=["**/*.json"])
```

### Per-file TODO counts (single pass)
```
count_per_file(patterns=["TODO"], globs=["**/*.go"])
```

## run_script — the full DSL

Use `run_script` when convenience tools aren't enough. Skeleton:

```json
{
  "scan": ["**/*.go"],
  "exclude": ["vendor", "src/generated/"],
  "variables": {"counts": {"type": "map"}},
  "context": 1,
  "patterns": [
    {"id": "todo", "regexp": "TODO", "case_insensitive": true,
     "on_match": [
       {"action": "map_increment", "target": "counts", "key": "$FILE"}
     ]}
  ],
  "on_complete": [
    {"action": "for_each", "var": "counts", "key_as": "f", "as": "n", "do": [
      {"action": "emit", "data": {"file": "$f", "count": "$n"}}
    ]}
  ]
}
```

Variable types: `string`, `int`, `bool`, `list`, `map`. Reset to default
with `{"action": "reset", "vars": [...]}` (e.g. in `on_file_end`).

Built-in tokens (in `data`/`value`/`key` strings):
- `$FILE`, `$PAT_ID`, `$LINE`, `$COL`, `$FROM`, `$TO`
- `$MATCH`, `$CONTEXT`, `$CONTEXT_BEFORE`, `$CONTEXT_AFTER`
- Inside `block.on_block`: `$BLOCK`, `$BLOCK_FULL`, `$BLOCK_START`,
  `$BLOCK_END`, `$BLOCK_LINE_START`, `$BLOCK_LINE_END`
- Inside `lookup.on_hit`/`on_miss`: `$LOOKUP_KEY`, `$LOOKUP_VALUE`
- `$<varname>` for any user variable. If the whole string is `"$x"` the
  variable's native type is preserved; embedded uses are stringified.

Actions:
- Output: `emit` (JSON line), `print` (raw text)
- Variables: `set`, `increment`, `decrement`, `add`, `subtract`,
  `multiply`, `divide`, `reset`
- Lists: `append`, `collect`, `unique_append`, `sort`
- Maps: `map_set`, `map_increment`, `count`
- Control: `if`, `for_each`, `stop`
- Cross-line: `submatch` (sub-patterns over `$MATCH`/`$BLOCK`/etc),
  `block` (find balanced delimiter block then run `on_block`),
  `lookup` (map[key] → `on_hit`/`on_miss`)

Conditions for `if`: `eq`, `ne`, `gt`, `lt`, `gte`, `lte`, `and`, `or`,
`not`, `contains` (substring or list element), `isset`.

Top-level fields: `scan`, `exclude`, `patterns`, `phases` (sequential
rounds sharing variables — collect-then-resolve), `variables`,
`context`/`context_before`/`context_after`, `limit`, `limit_per_file`,
`skip` (pagination with limit), `group_by` (one record per distinct
field value), `rank: true` (per-file relevance score), `on_file_end`,
`on_complete`.

Per-pattern flags: `id`, `regexp`, `case_insensitive`, `word_boundary`,
`utf8` (default true), `ucp` (Unicode `\\w`/`\\d`/`\\s`), `weight` (for
ranking), `absent: true` (fires `on_match` once per file where the
pattern is **NOT** found), `on_match`.

## What hprscript does NOT do (vs `srscript`)

File modification is rejected: `replace`, `replace_span`, `insert_*`,
`delete*`, `--write`, `--backup`. Use `srscript` for edits.

Other rejected DSL fields (will error explicitly): top-level `boundary`,
`on_boundary`, `ascii_only`, `overlap`, `files`; per-pattern `pcre` (not
needed — Hyperscan is already PCRE), `run_pattern_at`/`from`/`to`/`until`.

## Tips

- Combine patterns in one call rather than calling `search` repeatedly —
  Hyperscan adds patterns to the same DFA and matches them in one walk.
- Mix case-sensitivity per pattern (`patterns` + `case_insensitive_patterns`)
  in the same call.
- Use `limit` aggressively when you only need to know whether something
  exists — scanning stops early.
- For "files that have A but not B", use `run_script` with one normal
  pattern and one `absent: true` pattern, gated by an `if` on a per-file
  bool variable that you `reset` in `on_file_end`.
- Block depth tracking is **lexical, not language-aware**. Strings or
  comments containing the close delimiter can fool it. For Python/Ruby
  (no `{}` blocks), anchor on the signature and `submatch` over a window
  ending at the next `^def `/`^class `.
"""
