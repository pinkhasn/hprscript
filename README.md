# hprscript

**Multi-pattern PCRE search for files, directory trees, and stdin — all patterns matched in a single pass.**

`hprscript` is a command-line search tool built on Intel's [Hyperscan](https://www.hyperscan.io/) regex engine. It scans any input — files, recursive globs, or arbitrary data piped on **stdin** — and matches **all patterns simultaneously**. One invocation of `hprscript` replaces N sequential `grep`/`rg` calls.

It is a single self-contained binary with no runtime dependencies beyond `libc`/`libm`/`libpthread`. Built for Linux x86-64.

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

Requires `g++` (C++17), `make`, and Hyperscan (`libhs-dev` on Debian/Ubuntu).

```bash
sudo apt install libhs-dev g++ make    # Ubuntu/Debian
make                                    # builds ./hprscript
make install                            # copies to ~/.local/bin/hprscript
```

The build statically links Hyperscan and `libstdc++` so the resulting binary runs on any modern Linux x86-64 without extra packages. Verify with `ldd hprscript` — only `libc`, `libm`, `libpthread`, and `ld-linux` should appear.

Tested on Ubuntu 24.04 with Hyperscan 5.4.

### Run the test suite

```bash
make test
```

---

## MCP server (for AI agents)

The `mcp/` directory contains an [MCP](https://modelcontextprotocol.io/) server that exposes `hprscript` to AI coding agents (Claude Code, Cursor, etc.) as a set of tools (`search`, `list_files`, `count_per_file`, `extract_blocks`, `run_script`, `help`, `binary_info`). See [`mcp/hprscript_mcp/`](mcp/) for setup.

---

## Claude Code skill

If you use [Claude Code](https://docs.claude.com/en/docs/claude-code), the `skills/hprscript/` directory ships a ready-to-use [skill](https://docs.claude.com/en/docs/claude-code/skills) that teaches Claude when and how to invoke `hprscript` (CLI binary, no MCP required). Install with one copy:

```bash
mkdir -p ~/.claude/skills/hprscript
cp skills/hprscript/SKILL.md ~/.claude/skills/hprscript/SKILL.md
```

Start a new `claude` session and Claude will reach for `hprscript` instead of `grep`/`rg` whenever you ask it to search code. The skill carries an inline cheat sheet and points at [`HPRSCRIPT.md`](HPRSCRIPT.md) for full DSL depth.

---

## Documentation

- **[HPRSCRIPT.md](HPRSCRIPT.md)** — full reference: every CLI flag, the script-mode JSON DSL, Unicode handling, regex quirks, and exit codes.
- **[COOKBOOK.md](COOKBOOK.md)** — task-oriented recipes organized by problem domain (logs, source code, configs, pipelines). Copy-paste invocations with explanations of which hprscript features make each one work.

---

## License

See [LICENSE](LICENSE).
