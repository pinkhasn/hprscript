# hprscript

**Multi-pattern PCRE search for files, directory trees, and stdin — all patterns matched in a single pass.**

`hprscript` is a command-line search tool built on [Vectorscan](https://github.com/VectorCamp/vectorscan), the portable open-source fork of Intel's [Hyperscan](https://www.hyperscan.io/) regex engine. It scans any input — files, recursive globs, or arbitrary data piped on **stdin** — and matches **all patterns simultaneously**. One invocation of `hprscript` replaces N sequential `grep`/`rg` calls.

It is a single self-contained binary with no runtime dependencies beyond `libc`/`libm`/`libpthread`. Built for Linux on x86-64 and ARM64.

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
- **Unicode by default.** UTF-8 mode is on; `-pi` folds across scripts (`CAFÉ` ↔ `café`, `ПРИВЕТ` ↔ `привет`). See [UTF-8 / Unicode](HPRSCRIPT.md#utf-8--unicode-support).
- **grep-compatible output modes:** `-f` (file list), `-c` (per-file counts), `-o` (matched text only), `-format` (custom template), `-A`/`-B`/`-C` (context lines).
- **Single static binary** — no runtime dependencies beyond `libc`/`libm`/`libpthread`.

---

## Install

### Download a prebuilt binary

A prebuilt Linux x86-64 binary is attached to every tagged release:

> **https://github.com/pinkhasn/hprscript/releases/latest**

Download `hprscript`, mark it executable, and drop it in your `PATH`:

```bash
curl -L -o hprscript https://github.com/pinkhasn/hprscript/releases/latest/download/hprscript
chmod +x hprscript
mv hprscript ~/.local/bin/
```

### Build from source

Requires `g++` (C++17), `make`, and a Vectorscan install at `/opt/vectorscan` (override with `VECTORSCAN_PREFIX=...`).

Most Linux distros don't yet package Vectorscan, so build it once from source:

```bash
sudo apt install -y build-essential cmake ragel pkg-config libboost-dev libsimde-dev
git clone --depth 1 --recurse-submodules https://github.com/VectorCamp/vectorscan.git
cmake -S vectorscan -B vectorscan/build \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_STATIC_LIBS=ON -DBUILD_SHARED_LIBS=OFF \
      -DCMAKE_INSTALL_PREFIX=/opt/vectorscan
cmake --build vectorscan/build -j"$(nproc)"
sudo cmake --install vectorscan/build
```

Then build `hprscript`:

```bash
make                                    # builds ./hprscript
make install                            # copies to ~/.local/bin/hprscript
```

The build statically links Vectorscan and `libstdc++` so the resulting binary runs on any modern Linux host without extra packages. Verify with `ldd hprscript` — only `libc`, `libm`, `libpthread`, and `ld-linux` should appear.

The same recipe works on x86-64 (SSE/AVX2) and ARM64 (NEON/SVE) — Vectorscan auto-targets the host's SIMD.

### Run the test suite

```bash
make test
```

---

## MCP server (for AI agents)

The `mcp/` directory contains an [MCP](https://modelcontextprotocol.io/) server that exposes `hprscript` to AI coding agents (Claude Code, Cursor, etc.) as a set of tools (`search`, `list_files`, `count_per_file`, `extract_blocks`, `run_script`, `help`, `binary_info`). See [`mcp/hprscript_mcp/`](mcp/) for setup.

---

## Use it as an agent skill

`hprscript` ships a portable **agent skill** at [`skills/hprscript/SKILL.md`](skills/hprscript/SKILL.md) — a single Markdown file (YAML frontmatter + instructions) that teaches an LLM coding agent *when* and *how* to reach for `hprscript` instead of `grep`/`rg`. It drives the CLI binary directly (no MCP required), carries an inline cheat sheet, and points at [`HPRSCRIPT.md`](HPRSCRIPT.md) / [`COOKBOOK.md`](COOKBOOK.md) for depth.

It follows the standard `SKILL.md` convention — filename in caps, with `name` and `description` frontmatter — that a growing number of agents discover automatically. The only requirement on your side: the `hprscript` binary must be on the agent's `PATH` (see [Install](#install)).

### Claude Code

Copy the skill into your skills directory — globally (every project) or per-project:

```bash
# Global — applies everywhere
mkdir -p ~/.claude/skills/hprscript && cp skills/hprscript/SKILL.md ~/.claude/skills/hprscript/

# Or per-project — commit it with your repo
mkdir -p .claude/skills/hprscript && cp skills/hprscript/SKILL.md .claude/skills/hprscript/
```

Start a new `claude` session and it reaches for `hprscript` whenever you ask it to search code. See the [Claude Code skills docs](https://docs.claude.com/en/docs/claude-code/skills).

### opencode

[opencode](https://opencode.ai) loads skills automatically via its native `skill` tool — **and it scans the same `.claude/skills/` and `~/.claude/skills/` paths as Claude Code**, so if you installed it above, opencode already sees it. To install it only for opencode:

```bash
# Global
mkdir -p ~/.config/opencode/skills/hprscript && cp skills/hprscript/SKILL.md ~/.config/opencode/skills/hprscript/

# Or per-project
mkdir -p .opencode/skills/hprscript && cp skills/hprscript/SKILL.md .opencode/skills/hprscript/
```

No config required. See the [opencode skills docs](https://opencode.ai/docs/skills/).

### Other agents

`SKILL.md` is just Markdown, so any agent can use it one of two ways:

- **Native skill discovery** — agents that scan skill directories typically also read `.agents/skills/` and `~/.agents/skills/` (alongside the Claude/opencode paths above). Drop the `hprscript/` folder wherever your agent looks.
- **Instructions / rules file** — for agents driven by an instructions file (`AGENTS.md`, Cursor rules, OpenAI Codex, …), point that file at the skill or paste its contents. opencode's `opencode.json`, for example, can reference it directly (local path or remote URL):

  ```json
  { "$schema": "https://opencode.ai/config.json", "instructions": ["skills/hprscript/SKILL.md"] }
  ```

---

## Documentation

- **[HPRSCRIPT.md](HPRSCRIPT.md)** — full reference: every CLI flag, the script-mode JSON DSL, Unicode handling, regex quirks, and exit codes.
- **[COOKBOOK.md](COOKBOOK.md)** — task-oriented recipes organized by problem domain (logs, source code, configs, pipelines). Copy-paste invocations with explanations of which hprscript features make each one work.

---

## License

See [LICENSE](LICENSE).
