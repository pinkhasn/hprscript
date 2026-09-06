# Investigation implementation handoff

Implemented the local changes proposed in `hprscript-investigation-implementation.md`.
The result is source-first investigation evidence, with one optional batched
follow-up scan, bounded retained evidence, and explicit completeness reporting.
This is a lexical investigation facility, not compiler-backed symbol resolution.

## Delivered changes

| Requirement | Implementation |
| --- | --- |
| FR1: declaration-aware classification | Shared offset-aware occurrence classifier; declared-name spans; multiline signatures; comment/string exclusion, including C++ raw strings; explicit lexical fallback. |
| FR2: useful source excerpts | Owned, deduplicated source chunks; full small bodies when possible; bounded large-scope excerpts; omitted ranges/bytes and completeness flags; context flags and expansion refs. |
| FR3: relevant, deterministic candidates | Candidate discovery before frequency aggregation; origin and seed provenance; direct-dependency priority; multiseed round-robin selection; ambiguity reporting. |
| FR4: useful follow-up evidence | Helper definitions and associated test/config/documentation occurrences retained from seed-absent files; same resolved corpus; filter contract exposed; read errors accounted; no recursive expansion. |
| FR5: byte-budgeted reports | Category anchors before extra examples and rankings; full serialized stdout counted, including plan/summary/footer/diagnostics; progressive source reduction; UTF-8-safe long-line excerpts. |
| FR6: honest coverage | Separate `scan_complete`, `expansion_complete`, and `output_complete`; conservative legacy `complete`; independent candidate, retention, source, and output omissions. |

Core code lives in `src/investigate.cpp`, `src/investigation_report.*`,
`src/source_chunk.*`, and the shared scope/role/language-evidence helpers.
README, HPRSCRIPT, COOKBOOK, golden outputs, and the repository's
`skills/hprscript-search/SKILL.md` describe the new contract. The search skill now
recommends investigation for implementation/dependency/test questions and expansion
for missing detail; no global installed skill was modified.

Compatibility notes:

- JSONL retains its investigation summary first, but source records and evidence
  now precede optional rankings. Consumers should dispatch on record kind, not
  positional assumptions. Evidence links to shared source chunks.
- Legacy `complete` and `-require-complete` are stricter: a successful scan is not
  sufficient if expansion or requested output is incomplete.
- A positive evidence budget limits all stdout bytes. If the minimum valid report
  cannot fit, the command returns exit 2 with a diagnostic on stderr and no stdout.
- Memory limits estimate retained evidence, not process RSS or every transient
  matcher/index/serialization allocation. `memory_limit_events` counts denied
  allocations or partial retention events, not omitted rows.
- File predicates and relations select seeds; scope, line, and Git selection apply
  to both stages. This boundary is disclosed in reports.

## Validation

Validated locally on Linux x86-64, GCC 13.3.0, Vectorscan 5.4.12. Normal build uses
`-O2`. Baseline source is commit
`4fb4cb1aaf002d202303c0909662c2dbe4656f48` (v0.6.4); candidate is the uncommitted
implementation on that revision.

`make test` passed all suites:

| Suite | Passing checks/tests |
| --- | ---: |
| General functional | 609 |
| Accounting | 16 |
| Immutable edit plans | 52 |
| Investigation shell | 33 |
| New investigation evidence | 27 |
| Query | 41 |
| Golden outputs | 7 |

The new Python suite covers A1-A19 and additional regressions for input ordering,
late definitions, raw strings, shared source chunks, missing literal inputs,
multiline declarations, context selection, and long single-line source reduction.
It exercises independent candidate/memory/output caps, injected follow-up read
failure, Unicode, minimum report failure, ambiguity, and filter consistency.

All 27 evidence tests also passed under AddressSanitizer and UndefinedBehaviorSanitizer:

```sh
make -j4 BUILD_DIR=/tmp/hprscript-investigation.9nI94Y/asan-build \
  BIN=/tmp/hprscript-investigation.9nI94Y/asan \
  OPT='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' \
  LDFLAGS='-pthread -fsanitize=address,undefined'
env HPRSCRIPT_BIN=/tmp/hprscript-investigation.9nI94Y/asan \
  ASAN_OPTIONS=detect_leaks=0 python3 tests/investigation_evidence.py
```

Leak detection was disabled; this is not a leak-check claim. `git diff --check`
passed, and the skill-creator validator accepted the updated search skill.
macOS/ARM and hosted CI have not been run.

## Local before/after comparison

See [machine-readable results](comparison.json),
[helper/test baseline](helper-test-baseline.llm),
[helper/test candidate](helper-test-candidate.llm),
[C++ baseline](cpp-definition-baseline.llm), and
[C++ candidate](cpp-definition-candidate.llm).

Both binaries used the same fixtures and the same original-source snapshot for
the C++ case. Each case had one warm-up and three measured subprocess runs.
The final comparison ran after builds/tests finished. Times are medians of local
command wall time, including startup. Source-presence checks look for specified
source snippets; they do not grade an agent's answer.

| Case | Byte cap | Baseline → candidate bytes | Baseline → candidate ms | Required snippets, baseline → candidate |
| --- | ---: | ---: | ---: | --- |
| Helper and associated test | 16,000 | 1,001 → 5,120 | 9.44 → 12.13 | Missing → present |
| C++ definition and dependency | 16,000 | 3,511 → 13,145 | 355.54 → 660.31 | Missing → present |
| Noisy caller | 16,000 | 1,527 → 6,795 | 12.43 → 13.75 | Missing → present |
| Small budget | 3,500 | 1,001 → 3,415 | 9.38 → 12.42 | Missing → present |

All outputs were deterministic across measured repetitions and within their
byte caps. The candidate met all four source-presence checks; the baseline met
none. Richer evidence increased output size and local command time. These are
small local samples, not statistical performance guarantees.

Reproduce with a baseline executable and a checkout/archive of the pinned source:

```sh
python3 bench/agent/compare_investigation.py \
  --baseline /tmp/hprscript-investigation.9nI94Y/baseline \
  --candidate ./hprscript \
  --repository /tmp/hprscript-pinned.qIeiUY \
  --output-dir bench/agent/reports/investigation-implementation
```

Executable SHA-256 at measurement time:

```text
baseline  9efd935422ff8a2f030fadfa0c66a4cd381f04b919463ce0fb659886a970b04d
candidate c14443498b5e908055c1ee0c6406cf9f79aa87f553fc96d7a201a695ecfa49d4
```

Four investigation tasks were added to the agent task catalog (24 total), with
local fixtures and a repeatable comparison script. No agent/model benchmark was
executed: answer correctness, tool-call reduction, and end-to-end agent elapsed
time remain unmeasured. No such gains are claimed.

## Remaining boundaries

Name-based follow-up associations can be ambiguous; arbitrary syntax may fall
back to lexical windows. Large scopes remain explicitly partial even with an
unlimited output budget because source retention is bounded. Compiler resolution,
recursive traversal, and graph indexing remain deferred. Local timing overhead
should be profiled before any performance claim or release decision.

No commit, push, tag, or release was made. Existing unrelated workspace artifacts
were preserved.
