# hprscript

**Multi-pattern PCRE search for files, directory trees, and stdin — all patterns matched in a single pass.**

`hprscript` is a command-line search tool built on [Vectorscan](https://github.com/VectorCamp/vectorscan), the portable open-source fork of Intel's [Hyperscan](https://www.hyperscan.io/) regex engine. It scans any input — files, recursive globs, or arbitrary data piped on **stdin** — and matches **all patterns simultaneously**. One invocation of `hprscript` replaces N sequential `grep`/`rg` calls.

It is a single self-contained binary with no runtime dependencies beyond the platform C library. Builds for Linux (x86-64, ARM64) and macOS (Apple Silicon / Intel).

---

## Why hprscript?

| Need | grep / rg | hprscript |
|---|---|---|
| Search for one regex | ✅ | ✅ |
| Search for **N regexes in one scan** | ❌ (run N times) | ✅ (one DFA, one walk) |
| Pattern-per-file output (JSON Lines) | ❌ | ✅ default |
| Cross-line **block extraction** (function bodies, JSON objects, JSX subtrees) | ❌ | ✅ |
| Multi-pass workflows in one process (collect → resolve) | ❌ | ✅ via [phases](HPRSCRIPT.md#phases) |
| Per-file aggregation (counts, ranking, grouping) in one process | ❌ | ✅ via [scripts](HPRSCRIPT.md#script-mode--s---script) |
| Files **missing** a pattern | `grep -L` | `-absent` (also works inside scripts) |
| Output an agent can interpret cold (role tags, query legend, absence/overlap footers) | ❌ | ✅ [LLM-facing modes](HPRSCRIPT.md#llm-output-mode) |
| Pattern compile cost scales with N patterns | linear | constant — patterns share one DFA |

If you find yourself piping `grep` into `grep`, running ripgrep in a loop over a list of patterns, or writing throwaway Python to aggregate match counts per file, those are the workloads `hprscript` is designed for.

---

## Quick start

```bash
# Single pattern (default JSON Lines output)
hprscript -p "TODO" -glob "**/*.go"

# Multi-pattern in one pass — adding patterns is virtually free
hprscript -p "TODO" -p "FIXME" -p "XXX" -glob "**/*.go"

# Mix case-sensitive and case-insensitive in the same scan
hprscript -p '\bError\b' -pi 'todo|fixme' -glob '**/*.go'

# Pipeline use — content from stdin, no glob/files needed
curl -s https://example.com | hprscript -p 'href="[^"]+"' -o
kubectl logs my-pod | hprscript -p 'ERROR|panic' -C 2

# Extract every Go function body (signature + braces, balanced)
hprscript -p 'func \w+\(' -block-open '{' -block-close '}' -o '**/*.go'

# Files missing a license header (one pass, no scripting)
hprscript -p 'Copyright|SPDX-License-Identifier' -absent -glob '**/*.go'
```

Default per-match record:

```json
{"file":"main.go","pat":"p0","line":42,"col":5,"from":1023,"to":1027,"match":"TODO","context":"// TODO: refactor"}
```

---

## Key features

- **Multi-pattern in one pass.** Hyperscan compiles all patterns into a single DFA — adding patterns has near-zero cost.
- **PCRE syntax** (the subset Hyperscan accepts — see [Regex syntax](HPRSCRIPT.md#regex-syntax-hyperscan-pcre)). Most everyday patterns work unchanged.
- **JSON Lines output by default** — pipe-friendly, easy for scripts and AI agents to parse.
- **stdin-friendly.** With no files/globs given, content is read from stdin — slots into any bash pipeline.
- **Block extraction.** Pair every match with the balanced delimiter block that follows it (function bodies, JSON objects, JSX subtrees, SQL `BEGIN`/`END`).
- **Script mode (JSON DSL).** Variables, lifecycle hooks, sub-pattern matching, conditionals, grouping, ranking, and multi-phase scans — all in one invocation. See [Script mode](HPRSCRIPT.md#script-mode--s---script).
- **`-pi` per-pattern case-insensitivity.** Mix case-sensitive and case-insensitive patterns in the same scan.
- **`-absent` mode.** Find files where a pattern is *not* found (like `grep -L`, but also works inside scripts).
- **LLM-facing output modes.** `-llm` compact per-match text, `-elide` folded scope excerpts, `-rollup` one line per function with per-pattern counts — framed by an optional query legend (`-desc`) and automatic no-match / co-occurrence footers. See [below](#self-describing-output-for-llm-agents).
- **Per-match role tags.** Every hit self-classifies as definition, comment, string, or import — `[def func X]`/`[comment]`/`[string]`/`[import]` in `-llm`, a `"role"` field in JSONL, `$ROLE` in `-format`. See [Per-match role tags](HPRSCRIPT.md#per-match-role-tags).
- **Search → expand loop.** `-refs` stamps each hit as `file:line@hash`; `hprscript expand` later renders the full enclosing function — recovering moved lines by content and refusing stale ones instead of showing the wrong code. See [Stable refs & expand](HPRSCRIPT.md#stable-refs--expand--refs--hprscript-expand).
- **Unicode by default.** UTF-8 mode is on; `-pi` folds across scripts (`CAFÉ` ↔ `café`, `ПРИВЕТ` ↔ `привет`). See [UTF-8 / Unicode](HPRSCRIPT.md#utf-8--unicode-support).
- **grep-compatible output modes:** `-f` (file list), `-c` (per-file counts), `-o` (matched text only), `-format` (custom template), `-A`/`-B`/`-C` (context lines).
- **Single static binary** — no runtime dependencies beyond `libc`/`libm`/`libpthread`.
- **Guarded edit/apply workflow.** `hprscript edit -plan-out` discovers exact byte edits once; `hprscript apply` verifies file identity and commits that immutable plan. Plain search and script modes remain read-only. See below.

---

## Edit mode — search-precise file modification

`hprscript edit` and `hprscript apply` are separate write-capable commands;
plain search and script modes stay strictly read-only. The
pattern (and every filter: `-glob`, `-git-added-lines`, `-near`/`-far`,
`-file-where`, …) that finds the sites is exactly what edits them.

```bash
# Discover once, persist exact byte ranges/content plus file hashes, review,
# then apply those stored edits without rescanning.
hprscript edit -F 'retry(3)' -content 'retry(5)' -expect 1 \
    -plan-out retry.plan.json src/worker.go
hprscript apply retry.plan.json

# Replace a whole function BY NAME — no signature regex, no brace flags; the
# scope packs know the language (anchorless: no -p at all):
hprscript -list-scopes src/data.go        # outline: name, kind, line range per function
hprscript edit -in-scope '^LoadData$' -span scope \
    -content-file new_loaddata.go -expect 1 -plan-out loaddata.plan.json src/data.go
hprscript apply loaddata.plan.json

# Bump a constant only inside one function (-in-scope also filters search):
hprscript edit -F 'retry(3)' -content 'retry(5)' -in-scope 'ProcessBatch' \
    -expect 1 -plan-out retry.plan.json src/worker.go

# Rename, but only on lines this branch added:
hprscript edit -p '\bOldName\b' -content 'NewName' \
    -git-changed -git-added-lines -expect 14 -plan-out rename.plan.json
```

Safety model: `-expect` guards the planning scan. The plan stores the
normalized root, command metadata, file size/SHA-256, exact old bytes, and
replacement bytes. `apply` validates every target before staging any write;
preflight refusals exit 3 with files untouched. Changed files are all staged
before the rename phase. A failure after renames begin exits 4 and returns a
partial-commit receipt. Each individual replacement is atomic (temp + rename),
but multi-file application is not a filesystem transaction. CRLF and missing
trailing newlines survive byte-exactly, and permission bits are restored;
ownership, timestamps, xattrs, and ACLs are not preserved across the staged
inode replacement.
Full reference: [Edit mode](HPRSCRIPT.md#edit-mode-hprscript-edit).

---

## Agent-oriented investigation

Use investigation when the seed is known but the likely follow-up symbols,
tests, configuration, or impact area are not:

```bash
hprscript investigate -F validateToken -profile symbol -llm \
    -evidence-budget 65536 -glob '**/*.go'
```

It returns a deterministic, budgeted package of ranked files and scopes,
probable lexical definitions/references, path-heuristic file roles, related
identifiers, representative evidence, and explicit scan/truncation accounting.
It runs one seed stage and at most one internally batched related-identifier
follow-up stage. See [Investigation mode](HPRSCRIPT.md#investigation-mode-hprscript-investigate).

## Declarative relational query

Use query when the match sets and their value/location relationship are known:

```bash
hprscript query -query unresolved-functions.json -explain-plan -summary
```

Version 1 provides named match sets, typed capture/scope rows, inner/left/semi/
anti joins, value and location predicates, structured filtering, projection,
grouping/aggregation, ordering, strict resource limits, and adaptive
derived-pattern stages. Compatible sets share one matcher/traversal; equality
joins use hash indexes. See [Declarative query mode](HPRSCRIPT.md#declarative-query-mode-hprscript-query).

## Self-describing output for LLM agents

Search results are often read far from the command that produced them — by an
agent several turns later, or by a different agent entirely. The LLM-facing
modes (`-llm`, `-elide`, `-rollup`) make each result block carry its own
interpretation: a query legend says what each pattern means, role tags classify
every hit, and footers state absence and correlation explicitly.

```bash
hprscript -p 'sha1' -name weak_hash -desc 'weak hash algorithm usage' \
    -p 'rand\.Intn' -name weak_rand -rollup -refs -glob '**/*.go'
# query: 2 patterns over **/*.go
#   weak_hash — weak hash algorithm usage
#   weak_rand — /rand\.Intn/
# crypto/sign.go
#   (top level) — 2 hits (weak_hash×2)
#     3@6db2fd: import "crypto/sha1"
#   6-11 func SignToken — 2 hits (weak_hash×2)
#     7@9679f2: 	h := sha1.New()
# crypto/token.go
#   5-8 func Nonce — 1 hit (weak_rand×1)
#     6@bc19e4: 	n := rand.Intn(1 << 20)
# --- files: weak_hash 1, weak_rand 1; both: 0 ---

# Later — possibly after edits — turn any ref into the full function, verified:
hprscript expand crypto/sign.go:7@9679f2
```

- **Role tags** classify every hit lexically (`def` / `comment` / `string` / `import`) in `-llm`, JSONL (`"role"`), and `-format` (`$ROLE`) — [reference](HPRSCRIPT.md#per-match-role-tags).
- **`-rollup`** prints one line per enclosing function with per-pattern hit counts — "where is this concentrated?" answered in a handful of tokens — [reference](HPRSCRIPT.md#scope-rollup--rollup).
- **Query legend and footers.** `-desc` opens the block with each pattern's meaning; patterns that matched nothing are named explicitly (absence is evidence: `--- no matches: weak_cipher (1 of 2 patterns) ---`), and a co-occurrence footer reports per-pattern file counts plus their overlap — [reference](HPRSCRIPT.md#query-header-and-result-footers).
- **Stable refs.** `hprscript expand file:line@hash` recovers moved lines by content and reports vanished ones as stale (exit 3) rather than rendering the wrong code — [reference](HPRSCRIPT.md#stable-refs--expand--refs--hprscript-expand).

---

## Install

### Download a prebuilt binary

Prebuilt binaries for **Linux** (x86-64, ARM64) and **macOS** (Apple Silicon) are attached to every tagged release:

> **https://github.com/pinkhasn/hprscript/releases/latest**

Mark the binary executable and drop it in your `PATH`:

```bash
# Linux x86-64
curl -L -o hprscript https://github.com/pinkhasn/hprscript/releases/latest/download/hprscript

# Linux ARM64 (aarch64)
curl -L -o hprscript https://github.com/pinkhasn/hprscript/releases/latest/download/hprscript-linux-arm64

# macOS Apple Silicon (arm64)
curl -L -o hprscript https://github.com/pinkhasn/hprscript/releases/latest/download/hprscript-macos-arm64

chmod +x hprscript
mv hprscript ~/.local/bin/
```

> On macOS, Gatekeeper quarantines downloaded binaries. If the OS blocks it, clear the
> quarantine attribute with `xattr -d com.apple.quarantine hprscript` (or build from source).

### Build from source

Requires a C++17 compiler (`g++` or clang), `make`, and a Vectorscan install at `/opt/vectorscan` (override with `VECTORSCAN_PREFIX=...`).

Neither most Linux distros nor Homebrew package Vectorscan, so build it once from source. Install the build dependencies for your platform:

```bash
# Linux (Debian/Ubuntu)
sudo apt install -y build-essential cmake ragel pkg-config libboost-dev libsimde-dev

# macOS (Homebrew)
brew install cmake ragel pkg-config boost simde
```

Then build and install Vectorscan:

```bash
git clone --depth 1 --recurse-submodules https://github.com/VectorCamp/vectorscan.git
cmake -S vectorscan -B vectorscan/build \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_STATIC_LIBS=ON -DBUILD_SHARED_LIBS=OFF \
      -DBUILD_TOOLS=OFF -DBUILD_UNIT=OFF \
      -DBUILD_EXAMPLES=OFF -DBUILD_BENCHMARKS=OFF \
      -DCMAKE_INSTALL_PREFIX=/opt/vectorscan
cmake --build vectorscan/build -j"$(getconf _NPROCESSORS_ONLN)"
sudo cmake --install vectorscan/build
```

Then build `hprscript`:

```bash
make                                    # builds ./hprscript
make install                            # copies to ~/.local/bin/hprscript
```

The build statically links Vectorscan so the binary needs no Vectorscan package at runtime. On Linux it also statically links `libstdc++`/`libgcc` (verify with `ldd hprscript` — only `libc`, `libm`, `libpthread`, and `ld-linux` should appear); on macOS `libc++` is part of the OS and links dynamically (inspect with `otool -L hprscript`). The Makefile auto-detects the platform via `uname`.

The same recipe works on Linux x86-64 (SSE/AVX2), Linux/macOS ARM64 (NEON/SVE), and Intel macOS — Vectorscan auto-targets the host's SIMD.

### Run the test suite

```bash
make test
```

---

## Use it as agent skills

`hprscript` ships two portable agent skills:

- [`skills/hprscript-search/SKILL.md`](skills/hprscript-search/SKILL.md) teaches read-only quick search, investigation, query, ranking, context packing, scope surveys (`-rollup`), and the search → expand loop.
- [`skills/hprscript-edit/SKILL.md`](skills/hprscript-edit/SKILL.md) teaches persistent edit plans, guarded application, and the boundary with localized semantic patching.

Each is a single Markdown file with YAML frontmatter and focused instructions. Install either skill independently or install both for the complete workflow. They drive the CLI binary directly, carry inline cheat sheets, and point at [`HPRSCRIPT.md`](HPRSCRIPT.md) / [`COOKBOOK.md`](COOKBOOK.md) for depth.

Both follow the standard `SKILL.md` convention — filename in caps, with `name` and `description` frontmatter — that a growing number of agents discover automatically. The only requirement on your side: the `hprscript` binary must be on the agent's `PATH` (see [Install](#install)).

### Claude Code

Copy the skills into your skills directory — globally (every project) or per-project:

```bash
# Global — applies everywhere
mkdir -p ~/.claude/skills/hprscript-search ~/.claude/skills/hprscript-edit
cp skills/hprscript-search/SKILL.md ~/.claude/skills/hprscript-search/
cp skills/hprscript-edit/SKILL.md ~/.claude/skills/hprscript-edit/

# Or per-project — commit it with your repo
mkdir -p .claude/skills/hprscript-search .claude/skills/hprscript-edit
cp skills/hprscript-search/SKILL.md .claude/skills/hprscript-search/
cp skills/hprscript-edit/SKILL.md .claude/skills/hprscript-edit/
```

Start a new `claude` session and it reaches for `hprscript` whenever you ask it to search code. See the [Claude Code skills docs](https://docs.claude.com/en/docs/claude-code/skills).

### opencode

[opencode](https://opencode.ai) loads skills automatically via its native `skill` tool — **and it scans the same `.claude/skills/` and `~/.claude/skills/` paths as Claude Code**, so if you installed it above, opencode already sees it. To install it only for opencode:

```bash
# Global
mkdir -p ~/.config/opencode/skills/hprscript-search ~/.config/opencode/skills/hprscript-edit
cp skills/hprscript-search/SKILL.md ~/.config/opencode/skills/hprscript-search/
cp skills/hprscript-edit/SKILL.md ~/.config/opencode/skills/hprscript-edit/

# Or per-project
mkdir -p .opencode/skills/hprscript-search .opencode/skills/hprscript-edit
cp skills/hprscript-search/SKILL.md .opencode/skills/hprscript-search/
cp skills/hprscript-edit/SKILL.md .opencode/skills/hprscript-edit/
```

No config required. See the [opencode skills docs](https://opencode.ai/docs/skills/).

### Other agents

`SKILL.md` is just Markdown, so any agent can use it one of two ways:

- **Native skill discovery** — agents that scan skill directories typically also read `.agents/skills/` and `~/.agents/skills/` (alongside the Claude/opencode paths above). Drop either or both `hprscript-search/` and `hprscript-edit/` folders wherever your agent looks.
- **Instructions / rules file** — for agents driven by an instructions file (`AGENTS.md`, Cursor rules, OpenAI Codex, …), point that file at the skill or paste its contents. opencode's `opencode.json`, for example, can reference it directly (local path or remote URL):

  ```json
  { "$schema": "https://opencode.ai/config.json", "instructions": ["skills/hprscript-search/SKILL.md", "skills/hprscript-edit/SKILL.md"] }
  ```

---

## Documentation

- **[HPRSCRIPT.md](HPRSCRIPT.md)** — full reference: every CLI flag, the script-mode JSON DSL, Unicode handling, regex quirks, and exit codes.
- **[COOKBOOK.md](COOKBOOK.md)** — task-oriented recipes organized by problem domain (logs, source code, configs, pipelines). Copy-paste invocations with explanations of which hprscript features make each one work.

---

## License

See [LICENSE](LICENSE).
