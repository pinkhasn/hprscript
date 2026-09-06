# Agent interaction benchmark

This benchmark measures whether hprscript reduces repository-search rounds
and total task time while preserving answer quality. It deliberately does
not prescribe an agent provider or runner.

`tasks.jsonl` is the mandatory task catalog. Each line has a stable `id`, a
`category`, a repository id, the agent-facing prompt, and scoring evidence.
`repositories.json` resolves repository ids for a runner. A run writes one
JSON object per task to a results JSONL file conforming to
`result.schema.json`.

Supported condition labels are `grep_only`, `hprscript_available`,
`hprscript_preferred`, `hprscript_required`, `hprscript_investigate`, and
`hprscript_query`.

Record complete wall time, all LLM turns and tool calls, and search-specific
calls and wall time. `search_calls` counts invocations, not scan stages inside
one invocation. `first_query_sufficient` records whether sufficient evidence
was reached without a follow-up search.

Validate and summarize a result file with:

```bash
python3 bench/agent/score.py bench/agent/reports/run.jsonl
```

The report groups successful runs by condition and prints medians for wall
time, search calls, LLM turns, output bytes, correctness, and first-query
sufficiency. For repository-understanding tasks, compare investigation mode
with basic hprscript search. The product targets are at least 25% lower median
wall time, at least 30% fewer search calls, no more than a five percentage-point
correctness reduction, and no more than 2x median search-output bytes unless
correctness materially improves. These targets are evaluated outside the unit
suite because they depend on an agent, repository snapshot, and provider.

The `investigate-source-01` through `investigate-source-04` tasks exercise
related source without the seed text, C++ definition evidence, caller noise,
and a fixed small output budget. The `investigation-fixture` repository is a
retrieval fixture, not a buildable application or a cryptographic example.
Use the same pinned corpus, budgets, model, task prompts, and comparable agent
context for baseline and candidate runs. Keep seed evidence correctness as a
gate: additional helper output must not displace necessary seed source.

For a quick local comparison of two executables on the same source snapshot:

```bash
python3 bench/agent/compare_investigation.py \
  --baseline /path/to/baseline/hprscript --candidate ./hprscript \
  --repository /path/to/pinned/source --output-dir /tmp/investigation-comparison
```

This records exact output bytes, source-presence checks, repeat determinism,
and median local process time (one warmup and three measured runs by default).
It saves the before/after reports beside `comparison.json`. These deterministic
checks do not establish answer correctness, fewer LLM turns, or end-to-end
agent speedups; those require the agent runner and normal result schema above.
