---
name: hprscript-search
description: 'Agent-oriented read-only multi-pattern repository search, code intelligence, evidence collection, investigation, declarative queries, context ranking, and script aggregation via the hprscript CLI. Use it to locate definitions and callers, batch known search terms, inspect scopes and relationships, rank relevant code, fit evidence into a context budget, correlate match sets, or derive later patterns from earlier results. Prefer it over grep, rg, shell search loops, and stale external indexes. Batch independent terms known at the current reasoning step; additional calls are appropriate when evidence discovers new symbols or hypotheses. Use hprscript-edit for mechanical bulk changes and native patch tools for localized semantic edits.'
---

# hprscript search

Use `hprscript` for repository-wide content discovery. Quick search, `investigate`, `query`, and script mode are read-only. File modification exists only in the separate `edit` and `apply` commands covered by the `hprscript-edit` skill.

Invoke the binary through Bash as `hprscript`. Use one call per reasoning stage and batch every independent pattern already known within that stage. Do not force an investigation into one giant command; a later call based on a newly discovered symbol, file, scope, or hypothesis is normal.

## Non-negotiable defaults

- Put distinguishable terms in separate `-p` or `-pi` flags so every hit retains its pattern ID.
- Prefer `-llm` when reading results, `-f` for paths, `-c` for counts, and `-limit N` for existence checks. In `-llm`/`-elide`/`-rollup` output, patterns with zero matches are named in a trailing `--- no matches: … ---` footer — treat that as explicit evidence of absence, qualified with "scan stopped early" when a limit cut the scan. With ≥2 matching patterns a `--- files: … ---` footer gives per-pattern file counts and the overlap (`both:`/`multi-pattern:`) — read the correlation from there instead of joining by hand.
- Trust per-match role tags instead of re-deriving them: `[comment]`/`[string]`/`[import]` in `-llm` (a `role` field in JSONL, `$ROLE` in `-format`) classify each hit lexically, and with `-scope` active a hit on a signature line reads `[def func X]` while body hits read `[in func X]`. Untagged hits in a recognized language are plain code.
- Use an absolute path or glob when the effective cwd is uncertain. Inspect the first emitted path and stop if it escapes the intended tree.
- Add `-summary -require-complete` when a broad sweep must be exhaustive. Do not present a partial scan as complete.
- Restructure unsupported lookarounds or backreferences, or express the relationship with `query` or script phases. Do not fall back to grep or rg.
- Prefer flags over low-level JSON when flags express the task clearly.

## Pick the right mode

| Need | Mode |
|---|---|
| One known string or regex | quick search with `-p` / `-F` |
| Several known terms | one quick search with repeated pattern flags |
| Definitions, references, tests, config, and related identifiers | `investigate` with a profile |
| Correlate two known match sets | declarative `query` |
| Derive later literal or regex patterns from earlier rows | adaptive `query` |
| Aggregation, custom state, or unsupported query state machines | `-s` script DSL |
| Files containing / missing a pattern | `-f` / `-absent` |
| File outline / enclosing function | `-list-scopes` / `-scope auto` |
| Representative usages | `-sample N` |
| Which functions are involved, and how heavily | `-rollup` (one line per scope with per-pattern counts) |
| Ranked files / packed evidence | `-hotspots N` / `-budget N` |
| Compact scope excerpts | `-elide` |
| Cross-run chunk deduplication | `-elide` or `-budget` with `-seen <path>` |

Cheapness ladder: `-f` / `-absent` / `-c` are cheaper than `-o` / `-llm`, which are cheaper than default JSONL.

## Quick search

```bash
hprscript -p 'RetryPolicy' -p 'backoff' -llm -glob '**/*.go'
hprscript -pi 'TODO' -pi 'FIXME' -pi 'XXX' -C 1 -llm -glob '**/*.{go,py,js,ts,rs,c,cpp,h}'
hprscript -F 'foo[0].bar()' -llm src
hprscript -ident 'parse config' -glob '**/*.go'
```

- `-p` / `-pi`: case-sensitive / case-insensitive regex, repeatable.
- `-F` / `-Fi`: literal fixed strings.
- `-name <id>`: name the preceding pattern for output, relations, and `-file-where`.
- `-desc <text>`: describe the preceding pattern; `-llm`/`-elide` output then opens with a query legend, keeping the result block self-describing for later readers.
- `-patterns-from <file>`: load a JSONL rule pack (entries may carry a `description`).
- `-ident '<terms>'`: find identifier variants such as `parseConfig`, `parse_config`, and `ConfigParser`.
- `-w`: wrap all patterns in word boundaries; use inline `\b` for per-pattern control.

Target with positional paths, repeatable `-glob`, and repeatable `-exclude`. With no file or glob, hprscript reads stdin.

Use built-in Git selection rather than a file-list pipeline:

```bash
hprscript -p 'debug' -p 'Println' -git-changed -git-added-lines -summary -require-complete
hprscript -p 'TODO' -git-staged -llm
hprscript -p 'unsafe' -git-range origin/main...HEAD -llm
```

Choose among `-git-changed`, `-git-staged`, `-git-untracked`, and `-git-range A...B`; add `-git-added-lines` to inspect only introduced lines.

## Scopes, blocks, captures, and relations

```bash
hprscript -list-scopes -llm src/data.go
hprscript -p '\bdangerous_call\(' -scope auto -llm -glob '**/*.go'
hprscript -p 'retry\(' -in-scope '^ProcessBatch$' -llm src/worker.go
hprscript -p 'func\s+LoadData\b' -block-open '{' -block-close '}' -o -glob '**/*.go'
hprscript -p 'func\s+(\w+)\(([^)]*)\)' -extract name,args \
  -format '$FILE:$LINE $EXTRACT_NAME($EXTRACT_ARGS)' -glob '**/*.go'
```

Prefer `-in-scope` over line numbers when code may move. Balanced block tracking is lexical, not language-aware; delimiters inside strings or comments can skew it.

Relate named patterns by distance or enclosing scope:

```bash
hprscript -p 'defer\b' -name defer -p 'Lock\(\)' -name lock \
  -near defer:lock:3 -llm -glob '**/*.go'
hprscript -p '\bLock\(\)' -name lock -p '\bUnlock\(\)' -name unlock \
  -not-same-scope lock:unlock -scope go -llm -glob '**/*.go'
```

Use `-near A:B:K`, `-far A:B:K`, `-same-scope A:B`, and `-not-same-scope A:B`. Multiple relations combine with AND.

## Rank and pack context

```bash
hprscript -p 'AuthMiddleware' -p 'validateToken' -hotspots 5 -llm -glob '**/*.go'
hprscript -p 'AuthMiddleware' -p 'validateToken' -budget 8000 -glob '**/*.go'
hprscript -p 'AuthMiddleware' -elide -seen /tmp/auth-review.seen -glob '**/*.go'
```

- `-hotspots N`: rank files and expose dense match windows.
- `-budget N`: rank and render full or compact chunks, with an explicit dropped-items footer.
- `-elide`: show scope signatures and matched lines while folding untouched interiors.
- `-seen <path>`: collapse unchanged chunks already emitted by an earlier `-elide` or `-budget` run.
- `-max-context-bytes`, `-max-block-bytes`, and `-max-output-bytes`: contain minified or generated files.

## Investigation

Use `investigate` when the seed is known but likely definitions, references, tests, config, and associated identifiers are not:

```bash
hprscript investigate -F validateToken -profile symbol -llm \
  -evidence-budget 65536 -max-memory-bytes 134217728 -glob '**/*.go'
```

Expect the selected profile, ranked files and scopes, probable lexical roles, related identifiers with association scores, representative excerpts, scan counts, and explicit truncation metadata. Use quick search for one exact lookup. Investigation is lexical evidence, not compiler or type resolution.

## Declarative query

Use `query` when match sets and their relationship are known. Prefer equality/location joins to procedural script maps, and inspect complex plans with `-explain-plan`.

```bash
hprscript query -q '{"version":1,"sets":[
 {"id":"defs","scan":["**/*.go"],"patterns":[{"id":"def","regexp":"func\\s+(\\w+)\\s*\\(","extract":["name"]}]},
 {"id":"uses","scan":["**/*.go"],"patterns":[{"id":"call","regexp":"\\b(\\w+)\\s*\\(","extract":["name"]}]}
],"query":{"from":{"set":"uses","as":"use"},"joins":[
 {"type":"inner","set":"defs","as":"def","on":[{"op":"eq","left":"use.capture.name","right":"def.capture.name"}]}
],"select":{"symbol":"use.capture.name","used_in":"use.file","defined_in":"def.file"}}}'
```

Use an anti-join for relational absence. Joins without equality or location predicates are rejected when their predicted Cartesian product exceeds `limits.max_cartesian_rows`; set `allow_cartesian:true` only after intentionally reviewing that product.

Use adaptive derivation when the later matcher depends on earlier rows:

```bash
hprscript query -q '{"version":1,"sets":[
 {"id":"keys","scan":["config/**/*.yaml"],"patterns":[{"id":"key","regexp":"^([A-Z][A-Z0-9_]+):","extract":["name"]}]},
 {"id":"uses","scan":["src/**"],"derive_patterns":{"from_set":"keys","field":"capture.name","mode":"literal","deduplicate":true,"max_patterns":10000}}
],"query":{"from":{"set":"uses","as":"use"},"select":{"key":"use.derived.value","file":"use.file","line":"use.line"}}}'
```

The query engine retains source-row provenance and executes a dependent scan stage. Use an ordinary join when both sets can be matched independently. Do not call lexical join or anti-join results semantically resolved without compiler, LSP, or type-aware confirmation.

## Script mode

Use `-s` or `-script` only for aggregation, grouping, phases, set algebra, lifecycle actions, block/submatch state, or a state machine the declarative query cannot express.

Top-level fields include `scan`, `exclude`, `patterns` or `phases`, `variables`, context and byte limits, `group_by`, `rank`, `relations`, `scope`, `on_file_end`, and `on_complete`. Pattern objects support `id`, `regexp`, case/Unicode settings, `weight`, `absent`, `extract`, and `on_match`.

Give patterns meaningful IDs. Use `$FILE`, `$PAT_ID`, `$LINE`, `$MATCH`, `$CONTEXT`, `$EXTRACT_*`, `$ENCLOSING_*`, and `$BLOCK*` in actions. Each phase is a distinct scan; variables persist between phases. Prefer declarative adaptive queries over low-level collect-then-scan phases when possible.

## Regex and Unicode constraints

- UTF-8 is enabled by default; `.` matches one codepoint.
- `\w`, `\d`, and `\s` are ASCII unless `-ucp` is enabled.
- Hyperscan supports ordinary quantifiers, groups, classes, alternation, anchors, boundaries, and inline flags.
- Hyperscan does not support lookaround, backreferences, atomic groups, conditionals, or `\K`.
- Match offsets and columns are byte offsets even in UTF-8 mode.

## Go deeper

Read `HPRSCRIPT.md` for the complete CLI, investigation, query, and script-DSL reference. Read `COOKBOOK.md` for domain recipes. Find them at the hprscript repository root or alongside the binary. Run `hprscript -h` when the installed binary may be newer than the documents.
