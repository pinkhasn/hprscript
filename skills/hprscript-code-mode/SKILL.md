---
name: hprscript-code-mode
description: 'Drive the hprscript CLI from a script instead of reading its output yourself. Use this whenever shell and file access go through a code-execution tool (a JavaScript REPL, "code mode") and you are about to run hprscript: parsing its JSONL into objects, chaining several scans in one script, deriving the next scan''s patterns or file list from the previous scan''s rows, processing matches in order (sequences, sessions, lifecycles), reading function bodies, document sections or log sessions as data, following an investigation''s related identifiers, or running search → guarded edit → verify without coming back between steps. Also use it when a search could return thousands of matches and only a count, ranking, join or digest is needed. Flag semantics live in the hprscript-search and hprscript-edit skills; this skill covers the script around them and the habits that change when code, not you, reads the output.'
---

# hprscript in code mode

In code mode every tool round-trip is expensive and everything inside one script is nearly free. That changes who should read hprscript's output. Interactively you read it, so the compact `-llm» text is the right default. In a script the code reads it, so the default JSON Lines output is the right default: it carries byte offsets, pattern ids, captures, scopes, blocks and regions as fields, and none of it enters your context unless the script returns it.

The working pattern is: scan → parse rows into objects → filter, join and aggregate in JavaScript → return a small digest. Chain as many scans as the decision needs inside one script, and come back only when the next step needs your judgment. Everything the cookbook does with `| jq … | sort | uniq -c» is a few lines of JS here.

The snippets below use Claude Code's REPL globals (`$», `file», `write»). In another runtime, spawn `hprscript» with an argument array and parse stdout the same way.

## The helper

Define it once; it stays in the REPL for the rest of the session.

```js
globalThis.hp = async (args) => {
  const o = await $`hprscript ${args}`.nothrow();
  // 0 = matches, 1 = none, 3 = guard refused or stale ref. Anything else is a real error.
  if (![0, 1, 3].includes(o.exitCode)) throw new Error(`hprscript exit ${o.exitCode}: ${o.stdout.slice(0, 500)}`);
  if (o.truncated && !o.file) throw new Error('hprscript output truncated with no spill file: redirect it to a file');
  const text = o.truncated ? await file(o.file).text() : o.stdout;   // output over ~1 MiB spills to a file
  const out = {exit: o.exitCode, rows: [], byType: {}, lines: []};
  for (const line of text.split('\n')) {
    if (!line) continue;
    let r = null; if (line[0] === '{') { try { r = JSON.parse(line); } catch {} }
    if (!r) { out.lines.push(line); continue; }
    const kind = r.type ?? (r.info ? 'info' : null);
    if (kind) (out.byType[kind] ??= []).push(r); else out.rows.push(r);
  }
  out.summary = out.byType.summary?.[0];
  return out;
};
```

Why each part exists:

- **Argument array.** `hp(['-p', '\\bretry\\(', '-glob', 'src/**/*.go'])» passes each item as one argument, so a regex is escaped once, as a JS string, and never meets a shell. A `query» or script document is a plain object: `['query', '-q', JSON.stringify(q)]».
- **`.nothrow()» and the exit check.** Exit 1 means "no matches" and exit 3 means a guard refused or a ref went stale; both are answers the script should branch on, not exceptions. Exit 2 (bad flag, regex the engine rejects, unreadable input under `-require-complete») and exit 4 (apply failed part-way) throw.
- **The spill file.** Inline stdout is capped at about 1 MiB. Past that the result is silently a prefix unless you check `truncated»; the full output is in `o.file».
- **Three buckets.** JSON records with a `type» go to `byType», JSON without one to `rows», anything else to `lines»:

| Output | Lands in | Shape |
|---|---|---|
| Default search | `rows» | `{file, pat, line, col, from, to, match, context}» plus the optional fields in the next section |
| `query», script `emit» with `data», `group_by», `rank», `-absent -records line» | `rows» | Whatever was selected or emitted: `{key, group: […]}», `{file, score, density, matched_patterns}», `{file, pat, line, record}» |
| `-summary», `-diagnostics», `-list-scopes», `-list-regions», `-hotspots», `-explain-plan», `edit -j», `apply», `investigate» | `byType» | `summary», `warning», `scope», `region», `hotspot», `execution-plan», `guard», `edit», `edit-summary», `apply-file», `apply-summary», `query-footer», `investigation-*» |
| `-max-output-bytes» reached | `byType.info» | `{info: 'output_truncated', emitted}» |
| `-f», `-c», `-o», `-format», `-absent», `-llm», `expand», stderr text | `lines» | Plain text, one entry per line |

## What the engine can hand a script

Ask for the field instead of computing it. Each of these costs a flag, not a second pass.

| Need | Ask for | Read |
|---|---|---|
| Which function a hit is in | `-scope auto» | `r.enclosing.{name, kind, line_start, line_end}» |
| Code, comment, string, import or definition | default; `-no-roles» turns it off | `r.role» (absent for plain code) |
| Parts of the match | `-extract a,b» after the pattern | `r.extracted.a» |
| The balanced body after an anchor: function, JSON object, argument list | `-block-open '{' -block-close '}'» | `r.block», `r.block_full», `r.block_line_start», `r.block_line_end» (absent when unbalanced) |
| Which section, log session or `#ifdef» a hit is in | `-region id -region-open re [-region-close re]» | `r.regions.id.{label, line_start, line_end}» |
| An outline | `-list-scopes» / `-list-regions» | `byType.scope» / `byType.region» |
| A name in any casing (`parseConfig», `parse_config») | `-ident 'parse config'» | rows, `pat» is `ident0» or its `-name» |
| The files that matter most | `-hotspots N» | `byType.hotspot»: `{file, score, line_start, line_end, patterns}» |
| A symbol's definition, helpers, callers and tests | `investigate -F name -profile symbol» | `byType['investigation-*']» |
| Files or records that lack something | `-absent»; `-absent -records line» | `lines»; `rows» |
| A per-file condition | `-file-where 'a AND NOT b'», `count(a) >= 3», `churn(30) > 2», `lang == go» | fewer rows |
| Only what a change touched | `-git-changed», `-git-range A...B», `-git-added-lines» | rows on changed files or added lines |
| Only the files an earlier step chose | `-files-from list.txt» | rows |
| A with or without B nearby | `-near a:b:K», `-far», `-same-scope a:b», `-same-region a:b:id» | A rows filtered; B rows still print, so keep `r.pat === 'a'» |
| A diverse handful instead of the first N | `-sample N» | rows stratified by file and line shape |

## Habits that change

| Reading the output yourself | Letting a script read it |
|---|---|
| `-llm», `-rollup», `-elide», `-budget» to save tokens | Default JSONL; size is free while it stays in a variable. Use the text modes only for the excerpt you return verbatim. |
| One call per reasoning step | Several scans per script, in parallel with `Promise.all» when independent |
| `-c» for counts | Count rows in JS, or `-f» for "which files" (`-c» prints a `:0» row for every scanned file) |
| `-limit» to keep output small | No limit; add `-summary» and check `summary.complete» and `summary.files_scanned > 0» before trusting a negative |
| Read the co-occurrence and no-match footers | Group rows by `file» or `enclosing.name»; a pattern with zero rows in a complete scan is the proven negative |
| `investigate -evidence-budget 16000» | `-evidence-budget 0»: nothing is trimmed, and the script picks what to return |
| Script DSL for every aggregation | JS for small sets; the DSL or `query» when the stream is large (see "Push work into the engine") |

## Recipes

Each one was run as written (with real paths) while writing this skill. `TMP» is your scratch directory.

### Survey in parallel, aggregate in JS

```js
const [scopes, hits] = await Promise.all([
  hp(['-list-scopes', '-glob', 'src/**/*.cpp']),
  hp(['-F', 'regex_escape', '-name', 'esc', '-p', '\\bstd::regex\\b', '-name', 'stdre',
      '-scope', 'auto', '-summary', '-require-complete', '-glob', 'src/**/*.cpp']),
]);
const perScope = {};
for (const r of hits.rows) {
  const k = `${r.file}:${r.enclosing?.name ?? '(top level)'}`;
  (perScope[k] ??= {})[r.pat] = (perScope[k][r.pat] ?? 0) + 1;
}
({functions: scopes.byType.scope.length, hits: hits.rows.length, complete: hits.summary.complete,
  both: Object.entries(perScope).filter(([, c]) => c.esc && c.stdre).map(([k]) => k)})
```

Name every pattern (`-name») so `r.pat» is readable in the code that follows, and keep distinct terms as separate patterns rather than one alternation: adding a pattern costs nothing and each row says which one fired. Filter on `r.role» to drop comment and string mentions mechanically.

### Derive the next scan from this scan's rows

```js
const seed = await hp(['-F', 'regex_escape', '-scope', 'auto', '-glob', 'src/**/*.cpp']);
const fns = [...new Set(seed.rows.filter(r => r.enclosing).map(r => r.enclosing.name))];
await write(`${TMP}/rules.jsonl`,
  fns.map(n => JSON.stringify({id: `call_${n}`, regexp: `\\b${n}\\s*\\(`})).join('\n') + '\n');
await write(`${TMP}/files.txt`, [...new Set(seed.rows.map(r => r.file))].join('\n') + '\n');
const callers = await hp(['-patterns-from', `${TMP}/rules.jsonl`, '-scope', 'auto', '-glob', 'src/**/*.cpp']);
const sameFiles = await hp(['-p', '\\bstd::regex\\b', '-files-from', `${TMP}/files.txt`, '-summary']);
({callers: callers.rows.filter(r => r.role !== 'def').map(r => `${r.pat} ← ${r.file}:${r.line}`),
  rescanned: sameFiles.summary.files_scanned})
```

A rule pack is one JSON object per line: `id» plus `regexp» or `literal», and optionally `description», `case_insensitive», `word_boundary», `utf8». Use `literal» for values that are not regexes (IPs, hashes, config keys) so nothing needs escaping. Thousands of derived patterns still cost one pass. `-files-from» narrows the next scan to exactly the files you chose, one literal path per line. `-patterns-from» does not read stdin, so write the file.

### Order matters: reduce the event stream

Rows arrive grouped by file (sorted walk order) and, within a file, by `from» ascending with ties in pattern order. Different patterns may overlap; one pattern's own matches never do. That makes a multi-pattern scan an ordered event stream, and a loop over it a state machine.

```js
const ev = await hp(['-p', 'request start id=(\\w+)', '-name', 'start', '-extract', 'id',
                     '-p', 'request done', '-name', 'done', '-p', 'ERROR', '-name', 'err',
                     '-no-roles', 'logs/app.log']);
const sessions = [];
let cur = null;
for (const r of ev.rows) {
  if (cur && r.file !== cur.file) { sessions.push(cur); cur = null; }      // a file boundary ends a session
  if (r.pat === 'start') { if (cur) sessions.push(cur); cur = {file: r.file, id: r.extracted.id, start: r.line, end: null, errors: 0}; }
  else if (r.pat === 'err' && cur) cur.errors++;
  else if (r.pat === 'done' && cur) { cur.end = r.line; sessions.push(cur); cur = null; }
}
if (cur) sessions.push(cur);
({failed: sessions.filter(s => s.errors).map(s => s.id), unfinished: sessions.filter(s => s.end === null).map(s => s.id)})
```

Use this shape for anything the relation flags cannot say: counting, "third retry", gaps between timestamps, interleaved ids (keep a `Map» keyed by the captured id instead of one `cur»).

### Structure as data: regions and blocks

```js
const reg = await hp(['-region', 'section', '-region-open', '^## (.*)', '-list-regions', 'README.md']);
const sec = reg.byType.region.find(r => /^Key features/.test(r.label));
const body = await file(sec.file).slice(sec.line_start, sec.line_end - sec.line_start + 1);

const hits = await hp(['-pi', 'vectorscan', '-region', 'section', '-region-open', '^## (.*)', 'README.md']);
const bySection = {};
for (const r of hits.rows) { const k = r.regions?.section?.label ?? '(none)'; bySection[k] = (bySection[k] ?? 0) + 1; }

const fns = await hp(['-p', '^func (\\w+)\\(', '-extract', 'name', '-block-open', '{', '-block-close', '}',
                      '-max-block-bytes', '200', '-glob', 'bench/agent/fixtures/investigation/*.go']);
const long = fns.rows.filter(r => r.block_line_end - r.block_line_start + 1 > 20).map(r => r.extracted.name);
({section: [sec.label, sec.line_start, sec.line_end], bodyChars: body.length, bySection, long})
```

`-list-regions» gives every section, log session or block as a record with `label», `line_start», `line_end» and the byte range `from»/`to»; `"closed": false» marks one that never met its closing pattern. A block search returns the balanced body after each anchor as fields, so "functions longer than N lines" is a filter; cap `-max-block-bytes» when you only need the line range. Slice files by lines: `from»/`to» are byte offsets and only equal JS string indices in pure-ASCII files.

### Follow an investigation, or rank then read

```js
const inv = await hp(['investigate', '-F', 'validateToken', '-profile', 'symbol', '-evidence-budget', '0',
                      '-glob', 'bench/agent/fixtures/investigation/*.go']);
const sum = inv.byType['investigation-summary'][0];
const helpers = inv.byType['investigation-related']
  .filter(r => r.origins.some(o => o.association === 'called_in_seed_definition')).map(r => r.identifier);
const refs = inv.byType['investigation-scope'].map(r => `${r.file}:${r.line_start}`);
const bodies = await $`hprscript expand ${refs}`.nothrow();

const hot = (await hp(['-F', 'RegionIndex', '-F', 'find_innermost', '-hotspots', '3', '-glob', 'src/**/*.{cpp,hpp}'])).byType.hotspot;
({complete: sum.complete, coverage: sum.coverage, helpers, source: bodies.stdout.length,
  lookFirst: hot.map(h => `${h.file}:${h.line_start}-${h.line_end}`)})
```

`investigate» runs a seed scan plus one follow-up scan over the identifiers it found and returns typed records: `-evidence» (each occurrence with `category» and `classification»), `-source» (the text), `-scope», `-file», `-related» (next identifiers, each with the `association» that produced it) and a `-footer» naming anything omitted. Read `scan_complete», `expansion_complete» and `output_complete» separately, and treat same-named definitions as lexical candidates, not resolved call targets. `expand» takes many `file:line» refs in one call and prints each enclosing function. `-hotspots N» ranks files by how many of the patterns they match, how rarely and how close together, and gives each file's densest line window.

### Search → guarded edit → verify in one script

```js
const target = ['-F', 'log("prod")', '-content', 'log("live")', 'src/cfg.c'];
const dry = await hp(['edit', ...target, '-j']);
const sites = dry.byType['edit-summary']?.[0]?.sites ?? 0;
if (sites !== 1) throw new Error(`expected 1 edit site, found ${sites}: not editing`);
const plan = await hp(['edit', ...target, '-j', '-expect', '1', '-plan-out', 'build/cfg.plan.json']);
if (plan.exit !== 0) throw new Error('plan refused: ' + JSON.stringify(plan.byType.guard));
const applied = await hp(['apply', 'build/cfg.plan.json']);
const tests = await $`make test`.nothrow();
const introduced = await hp(['-pi', 'TODO|FIXME', '-name', 'todo', '-p', '\\bprintf\\(', '-name', 'debug',
                             '-git-changed', '-git-added-lines']);
({applied: applied.byType['apply-summary']?.[0], guard: applied.byType.guard, tests: tests.exitCode,
  introduced: introduced.rows.map(r => `${r.pat} ${r.file}:${r.line}`)})
```

The script decides on structured facts: the site count from the dry run, a `guard» record when hprscript refuses (exit 3, nothing written), the `apply-summary» receipt, and the test exit code. `apply» re-verifies file hashes, so a plan made stale by another edit is refused instead of misapplied. The last scan looks only at lines the working tree added, which answers "did this change introduce it" rather than "does the file contain it". Plans need their targets beneath the working directory, so run from the project root and do not `cd».

When each site needs a different replacement, try `-extract» with `$EXTRACT_NAME» in `-content» first: one templated edit covers most cases. When the new text has to be computed, edit site by site, bottom-up so earlier lines keep their numbers:

```js
const found = (await hp(['-p', 'timeout\\((\\d+)\\)', '-extract', 'ms', 'src/cfg.c'])).rows;
for (const s of [...found].sort((a, b) => a.file === b.file ? b.from - a.from : a.file < b.file ? -1 : 1)) {
  await write(`${TMP}/site.txt`, `timeout(${Math.max(100, s.extracted.ms * 2)})`);
  const r = await hp(['edit', '-F', s.match, '-lines', String(s.line), '-content-file', `${TMP}/site.txt`,
                      '-expect', '1', '-j', '-write', '-no-plan-warning', s.file]);
  if (r.exit !== 0) throw new Error(`site ${s.file}:${s.line} refused: ${JSON.stringify(r.byType.guard)}`);
}
found.length
```

`-content-file» is verbatim; `-content» interprets `\n» and `$TOKENS», which corrupts computed text. Each call pins the exact old text (`-F»), the line (`-lines») and the count (`-expect 1»), so a site that moved is refused, not guessed. To replace a whole function by name, write the new body to a file and use `edit -in-scope '^Name$' -span scope -content-file … -expect 1» with no pattern at all.

### Scan text the script already holds

Tool output, an API response, a diff or a build log is a string in a variable. Write it to the scratch directory and scan the file; no other tool gives it pattern ids, ordering and regions.

```js
const build = await $`make 2>&1`.nothrow();
await write(`${TMP}/build.log`, build.stdout);
const diag = await hp(['-p', '^(\\S+):(\\d+):\\d+: (error|warning): (.*)', '-extract', 'file,line,kind,msg',
                       '-no-roles', `${TMP}/build.log`]);
({exit: build.exitCode, errors: diag.rows.filter(r => r.extracted.kind === 'error').map(r => r.extracted)})
```

Anchor such patterns to the real shape of the line. A bare `warning:» also matches unrelated tool chatter.

### Push work into the engine

When a scan would ship far more rows than the answer needs, build the aggregation as a script or `query» object and let hprscript return only the result. Here 62,000 identifier matches come back as 4,300 counts:

```js
const doc = {scan: ['src/**/*.cpp'], variables: {freq: {type: 'map'}},
  patterns: [{id: 'w', regexp: '[A-Za-z_]\\w+', on_match: [{action: 'map_increment', target: 'freq', key: '$MATCH'}]}],
  on_complete: [{action: 'for_each', var: 'freq', key_as: 'w', as: 'n', do: [{action: 'emit', data: {word: '$w', count: '$n'}}]}]};
const freq = await hp(['-s', JSON.stringify(doc)]);

const q = {version: 1, sets: [
  {id: 'defs', scan: ['bench/agent/fixtures/investigation/*.go'], patterns: [{id: 'def', regexp: 'func\\s+(\\w+)\\s*\\(', extract: ['name']}]},
  {id: 'uses', scan: ['bench/agent/fixtures/investigation/*.go'], patterns: [{id: 'call', regexp: '\\b(\\w+)\\s*\\(', extract: ['name']}]},
], query: {from: {set: 'uses', as: 'use'},
  joins: [{type: 'anti', set: 'defs', as: 'def', on: [{op: 'eq', left: 'use.capture.name', right: 'def.capture.name'}]}],
  select: {symbol: 'use.capture.name', file: 'use.file', line: 'use.line'}}};
const plan = await hp(['query', '-q', JSON.stringify(q), '-plan-only']);
const undefinedCalls = await hp(['query', '-q', JSON.stringify(q)]);
({top: freq.rows.sort((a, b) => b.count - a.count).slice(0, 3), stages: plan.byType['execution-plan'][0].scan_stages.length,
  undefinedCalls: [...new Set(undefinedCalls.rows.map(r => r.symbol))]})
```

A document written as a JS object needs no shell quoting and one level of regex escaping. `-plan-only» validates it and shows the scan stages without reading a file; sets with the same inputs share one pass. The DSL also has `group_by», `rank» with per-pattern `weight», `phases» that keep variables between scans, and set algebra (`set_difference», `set_intersection»); `query» has inner, left, semi and anti joins with `eq», `same_file», `same_scope», `within_lines», `before», `after» and `contains_span», plus `derive_patterns» for a set whose patterns come from an earlier set's rows.

### Very large streams

Tens of thousands of matches are fine through `hp» (the spill file is read back), but JSONL repeats the path and context on every row, about 175 bytes each. When only positions matter, a tab-separated `-format» is about five times smaller:

```js
await $`hprscript ${['-p', '[A-Za-z_]\\w+', '-name', 'ident', '-no-roles',
  '-format', '$FILE\t$PAT_ID\t$LINE\t$FROM\t$TO', '-glob', 'src/**/*.cpp']} > ${TMP + '/events.tsv'}`.nothrow();
const events = (await file(TMP + '/events.tsv').text()).split('\n').filter(Boolean)
  .map(l => { const [f, pat, line, from, to] = l.split('\t'); return {file: f, pat, line: +line, from: +from, to: +to}; });
events.length
```

`-no-roles» and `-max-context-bytes» also cut work and bytes when the script does not use those fields.

## Engine or JS

Let hprscript do what needs the bytes or shrinks the stream before it leaves the process: matching, scopes, blocks, regions, relations, `-in-scope»/`-in-region», `-file-where» (including `churn()» and `lang»), git selection, whole-corpus ranking, hashing and atomic writes. Let JS do decisions, joins over a few thousand rows, counting and formatting. Move a join or aggregation into `query» or the script DSL when the rows it would otherwise ship number in the hundreds of thousands, or when a later scan depends on an earlier one and the derived set is large.

## What to return

Return what the next decision needs and nothing else: counts, the top few groups, `file:line» locations, and at most a handful of verbatim lines. Keep the full row arrays in variables; a later script can slice them without rescanning. For examples, prefer `-sample N» to the first N rows: it spreads them across files and line shapes. When you do want source text in context, fetch exactly that slice (`hprscript expand file:line», a region's line range, or one `-llm» call scoped with `-in-scope»/`-in-region») rather than returning rows wholesale.

## Gotchas

1. Without `.nothrow()», exit 1 ("no matches") throws. Check `exit», not the absence of an exception.
2. An unchecked `o.truncated» means you parsed a prefix. Use `hp», or redirect to a file as in "Very large streams".
3. `-c» lists every scanned file including `:0»; over a big tree that floods a result. Use `-f» or count rows.
4. `hp» files any record with a `type» or `info» field under `byType». Do not give a `query» column or an `emit» field those names.
5. Warnings and errors arrive as plain text in `lines», mixed with any text-mode output. Add `-diagnostics» to get read errors and binary skips as `warning» records.
6. `from», `to» and `col» are byte offsets, in every mode.
7. `expand» prints text, not JSON, and `-refs» hashes exist only in `-llm»/`-rollup» output. Call `expand» through `$» and read `stdout»; exit 3 means a ref went stale.
8. `-limit» stops the scan early and makes `summary.complete» false; do not assert absence from such a run. `-m N» caps rows per file without that side effect.
9. Mode limits: `-region» flags work in quick search and edit only (`investigate», `query» and script mode reject them, exit 2). `-ident» is rejected in edit and script mode. `investigate» does accept `-ident» and `-in-scope».
10. Changing directory may be refused by the sandbox, and `-plan-out» refuses targets outside the working directory. Pass paths explicitly, relative to the project root.
11. Relations filter only their A side. The B pattern's rows are still in the output.
12. Scopes, blocks, regions and roles are lexical. A brace in a string can end a block early, a delimiter in a comment still opens a region, and Python or Ruby have no scopes. Check a sample before trusting a count.
13. After any edit, the line numbers and offsets you hold are stale. Rescan; it is cheaper than a wrong edit.
14. stdin is read to end-of-file before scanning, so a live stream produces nothing until it closes. In this REPL, feed stdin with `< file».
15. On input that may not be valid UTF-8, results are best-effort unless you pass `-no-utf8».
16. A pattern that can match the empty string (`^.*$», `\w*») is rejected; write `^.+$». Lookarounds and backreferences are rejected too.
