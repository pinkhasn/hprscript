---
name: hprscript
description: High-performance multi-pattern content search via the `hprscript` CLI binary (Intel Hyperscan). Use whenever you need to search code or text — keyword/regex search, finding files by content (including files that DON'T match), counting occurrences, extracting function bodies or balanced delimiter blocks, multi-pattern scans, aggregations. TRIGGER when the user asks to grep/search/find/look-for/count/locate something across files, or when you would otherwise reach for the Grep tool, `grep`, or `rg`. Replaces those tools for the entire task. Invoke via the Bash tool — `hprscript` is on PATH.
---

# hprscript — multi-pattern content search (CLI)

Always prefer the `hprscript` CLI binary over the Grep tool, `grep`, or `rg`. hprscript matches all patterns in a single pass via Intel Hyperscan, so multi-pattern searches cost the same as single-pattern. Adding patterns is virtually free — batch them.

Invoke via the Bash tool. The binary is on PATH (`hprscript`).

Default output is **JSON Lines** (one JSON object per match) — easy to parse, easy to pipe.

## Cheat sheet

```bash
# Single pattern, one file type
hprscript -p "TODO" -glob "**/*.go"

# Multi-pattern in one pass (free per extra pattern)
hprscript -p "TODO" -p "FIXME" -p "XXX" -glob "**/*.go"

# Case-insensitive (per-pattern flag)
hprscript -pi "error" -pi "warning" -glob "**/*.go"

# Mix case-sensitive and case-insensitive in one pass
hprscript -p '\bError\b' -pi 'todo|fixme' -glob '**/*.go'

# Whole-word match
hprscript -p "hello" -w -glob "**/*.txt"

# Context (-C symmetric, -A after, -B before)
hprscript -p "panic" -C 3 -limit 10 -glob "**/*.go"

# Excludes — glob, bare dir name, or path prefix
hprscript -p "TODO" -glob "**/*.go" -exclude vendor -exclude "src/generated/"
```

## Output modes (mutually exclusive)

| Flag | Behavior |
|---|---|
| (default) / `-j` | JSON Lines — best for parsing |
| `-f` | File paths only, deduped (like `grep -l`) |
| `-absent` | Files where the pattern is **not** found (like `grep -L`) |
| `-c` | Match count per file (`path:N`) |
| `-o` | Just the matched text |
| `-format <tmpl>` | Custom one-line template with `$FILE`, `$LINE`, `$MATCH`, `$CONTEXT`, etc. |
| `-llm` | Token-efficient plain text grouped by file (LLM-friendly) |

## Pattern syntax — Hyperscan PCRE quirks

- **No** lookarounds (`(?=…)`, `(?!…)`, `(?<=…)`, `(?<!…)`).
- **No** backreferences (`\1`), atomic groups, conditionals, or `\K`.
- `^` / `$` are line-anchored by default.
- Inline flags `(?i)`, `(?m)`, `(?s)`, `(?x)` work.
- Prefer `-pi` over inline `(?i)` when the whole pattern is case-insensitive — clearer.
- Literal braces need escaping: `interface\{\}` to find Go's `interface{}`.

## Block extraction (function bodies, JSON values, JSX subtrees)

`-block-open` + `-block-close` pair every match with the next balanced delimiter block — depth-tracked.

```bash
# Full Go function bodies
hprscript -p 'func \w+\(' -block-open '{' -block-close '}' -o -glob "**/*.go"

# Multi-character delimiters
hprscript -p '<div\b' -block-open '<div>' -block-close '</div>' -o -glob "**/*.html"

# JSON key + its object value (nesting handled)
hprscript -p '"config"\s*:' -block-open '{' -block-close '}' -o -glob "**/*.json"
```

Default JSON output adds `block`, `block_full`, `block_line_start`, `block_line_end` fields per match.

Block tracking is **lexical, not language-aware** — strings/comments containing the close delimiter can skew the count. Python/Ruby need a different approach (no `{}` blocks).

## Stdin pipelines

When no `-glob` and no positional file args are given, hprscript reads from stdin:

```bash
curl -s https://example.com | hprscript -p 'href="[^"]+"' -o
kubectl logs deploy/api --tail=10000 | hprscript -p 'ERROR|panic' -C 1
journalctl -u nginx --since "1h" | hprscript -pi 'timeout|refused' -o
echo "hello TODO" | hprscript -pi 'todo|fixme'
```

## Script mode (`-s` JSON)

When the flag set isn't enough — aggregations, phases, ranking, grouping, custom emit shapes — pass a JSON script via `-s`:

```bash
# Count TODOs per file, emit one summary line each
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

When to reach for `-s`:

- **Aggregation** — `variables` + `on_complete` (counts, sums, manifests).
- **Phases** — multi-round scans where round 2's patterns depend on round 1.
- **Ranking** — `"rank": true` for per-file relevance ordering.
- **Grouping** — `"group_by": "..."` to fold matches into one JSON line per key.
- **Submatch + block actions** — search *inside* an extracted block.
- **Absent + present patterns** combined in one pass.

Full DSL reference: `~/.local/bin/HPRSCRIPT.md` (1600+ lines — load with Read only when you need depth, or scan a specific section).

## Useful flags worth remembering

| Flag | Purpose |
|---|---|
| `-limit <n>` | Stop after N global matches |
| `-m <n>` | Cap matches per file |
| `-max-match-bytes <n>` / `-max-context-bytes <n>` / `-max-block-bytes <n>` | UTF-8-safe truncation — keep context small |
| `-max-output-bytes <n>` | Stop scanning once stdout exceeds N bytes |
| `-extract <names>` | Comma-separated capture-group names — adds `$EXTRACT_<NAME>` tokens |
| `-scope <lang\|auto>` | Annotate matches with enclosing function/scope (go, rust, c, cpp, java, js, ts) |
| `-near A:B:K` / `-far A:B:K` | Co-occurrence relations between patterns within K lines |
| `-sample <n>` | Stratified N-sample of matches by file + line shape |

## What NOT to do

- Do not fall back to the Grep tool, `grep`, or `rg` because a Hyperscan limitation (e.g. lookaround) blocks one pattern. Restructure the pattern, or split into a phased `-s` script.
- Do not run multiple sequential `hprscript` calls when a single multi-pattern call would do. One pass, N patterns, free.
- Do not invoke `hprscript -s '{...}'` when a flag set fits — flag-mode is easier to read and review.
- Do not pipe `hprscript` output through `grep`/`jq` for trivial filtering — use `-format`, `-f`, `-c`, or another output mode instead.
