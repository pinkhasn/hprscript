"""MCP server exposing hprscript multi-pattern PCRE search to AI agents.

Wraps the `hprscript` binary (https://...). Tools return parsed JSON Lines
output as structured data so agents don't need to re-parse stdout.

Locate binary via (in order):
  1. $HPRSCRIPT_BIN
  2. `hprscript` on PATH
  3. ~/.local/bin/hprscript
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
from pathlib import Path
from typing import Any

from mcp.server.fastmcp import FastMCP

from .cheatsheet import CHEAT_SHEET


def _locate_binary() -> str:
    env = os.environ.get("HPRSCRIPT_BIN")
    if env:
        return env
    on_path = shutil.which("hprscript")
    if on_path:
        return on_path
    return str(Path.home() / ".local" / "bin" / "hprscript")


HPRSCRIPT_BIN = _locate_binary()
DEFAULT_TIMEOUT = float(os.environ.get("HPRSCRIPT_TIMEOUT", "60"))

mcp = FastMCP("hprscript")


def _run(args: list[str], cwd: str | None, timeout: float) -> tuple[int, str, str]:
    if not Path(HPRSCRIPT_BIN).exists():
        raise FileNotFoundError(
            f"hprscript binary not found at {HPRSCRIPT_BIN}. "
            "Install it (`make install` from the hprscript repo) "
            "or set HPRSCRIPT_BIN env var."
        )
    proc = subprocess.run(
        [HPRSCRIPT_BIN, *args],
        cwd=cwd,
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    return proc.returncode, proc.stdout, proc.stderr


def _parse_jsonl(text: str) -> list[dict]:
    out: list[dict] = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            out.append(json.loads(line))
        except json.JSONDecodeError:
            out.append({"_raw": line})
    return out


def _quick_flags(
    patterns: list[str] | None,
    case_insensitive_patterns: list[str] | None,
    globs: list[str] | None,
    excludes: list[str] | None,
    whole_word: bool,
    context: int | None,
    context_before: int | None,
    context_after: int | None,
    limit: int | None,
    per_file_limit: int | None,
) -> list[str]:
    args: list[str] = []
    for p in patterns or []:
        args.extend(["-p", p])
    for p in case_insensitive_patterns or []:
        args.extend(["-pi", p])
    for g in globs or []:
        args.extend(["-glob", g])
    for e in excludes or []:
        args.extend(["-exclude", e])
    if whole_word:
        args.append("-w")
    if context is not None:
        args.extend(["-C", str(context)])
    else:
        if context_before is not None:
            args.extend(["-B", str(context_before)])
        if context_after is not None:
            args.extend(["-A", str(context_after)])
    if limit is not None:
        args.extend(["-limit", str(limit)])
    if per_file_limit is not None:
        args.extend(["-m", str(per_file_limit)])
    return args


def _err(message: str, exit_code: int = 2, **extra: Any) -> dict:
    out: dict[str, Any] = {"error": message, "exit_code": exit_code}
    out.update(extra)
    return out


def _need_target(globs: list[str] | None, paths: list[str] | None) -> str | None:
    if not (globs or paths):
        return (
            "no target specified — pass `globs` (e.g. ['**/*.go']) "
            "and/or `paths` (files or directories)"
        )
    return None


def _need_pattern(p: list[str] | None, ci: list[str] | None) -> str | None:
    if not (p or ci):
        return "at least one pattern (in `patterns` or `case_insensitive_patterns`) is required"
    return None


@mcp.tool()
def search(
    patterns: list[str] | None = None,
    case_insensitive_patterns: list[str] | None = None,
    globs: list[str] | None = None,
    excludes: list[str] | None = None,
    paths: list[str] | None = None,
    whole_word: bool = False,
    context: int | None = None,
    context_before: int | None = None,
    context_after: int | None = None,
    limit: int | None = 1000,
    per_file_limit: int | None = None,
    cwd: str | None = None,
    timeout: float | None = None,
) -> dict:
    """Multi-pattern PCRE search. Returns parsed JSON match records.

    - `patterns`: case-sensitive PCRE patterns (any number; all match in one pass).
    - `case_insensitive_patterns`: case-insensitive patterns (Unicode-aware folding).
    - `globs`: glob patterns to scan (e.g. `["**/*.go"]`).
    - `excludes`: glob, bare directory name, or path prefix to skip (repeatable).
    - `paths`: extra positional files/directories.
    - `whole_word`: wraps every pattern as `\\b(?:...)\\b`.
    - `context` / `context_before` / `context_after`: surrounding lines.
    - `limit`: max global results (default 1000 to bound context size).
    - `per_file_limit`: max matches per file.
    - `cwd`: working directory (default = MCP server cwd).

    Pattern syntax: PCRE subset accepted by Hyperscan. NO lookarounds,
    backreferences, atomic groups, conditionals, or `\\K`. `^`/`$` are
    line-anchored by default. See `help` for full reference.
    """
    if msg := _need_pattern(patterns, case_insensitive_patterns):
        return _err(msg)
    if msg := _need_target(globs, paths):
        return _err(msg)
    args = _quick_flags(
        patterns, case_insensitive_patterns, globs, excludes,
        whole_word, context, context_before, context_after, limit, per_file_limit,
    )
    args.extend(paths or [])
    rc, stdout, stderr = _run(args, cwd, timeout or DEFAULT_TIMEOUT)
    if rc == 2:
        return _err(stderr.strip() or "hprscript error", rc, stderr=stderr)
    matches = _parse_jsonl(stdout)
    return {"matches": matches, "count": len(matches), "exit_code": rc}


@mcp.tool()
def list_files(
    patterns: list[str] | None = None,
    case_insensitive_patterns: list[str] | None = None,
    globs: list[str] | None = None,
    excludes: list[str] | None = None,
    paths: list[str] | None = None,
    whole_word: bool = False,
    mode: str = "matching",
    cwd: str | None = None,
    timeout: float | None = None,
) -> dict:
    """List files matching (or absent of) the patterns.

    - `mode="matching"` (default): files containing ANY pattern match (`-f`).
    - `mode="absent"`: files where the pattern is NOT found (`-absent`).

    Returns `{"files": [...], "count": N, "exit_code": ...}`.
    Cheaper than `search` when you only need filenames.
    """
    if msg := _need_pattern(patterns, case_insensitive_patterns):
        return _err(msg)
    if msg := _need_target(globs, paths):
        return _err(msg)
    if mode not in {"matching", "absent"}:
        return _err(f"mode must be 'matching' or 'absent', got '{mode}'")
    args = _quick_flags(
        patterns, case_insensitive_patterns, globs, excludes,
        whole_word, None, None, None, None, None,
    )
    args.append("-absent" if mode == "absent" else "-f")
    args.extend(paths or [])
    rc, stdout, stderr = _run(args, cwd, timeout or DEFAULT_TIMEOUT)
    if rc == 2:
        return _err(stderr.strip() or "hprscript error", rc, stderr=stderr)
    files = [line for line in stdout.splitlines() if line.strip()]
    return {"files": files, "count": len(files), "exit_code": rc}


@mcp.tool()
def count_per_file(
    patterns: list[str] | None = None,
    case_insensitive_patterns: list[str] | None = None,
    globs: list[str] | None = None,
    excludes: list[str] | None = None,
    paths: list[str] | None = None,
    whole_word: bool = False,
    cwd: str | None = None,
    timeout: float | None = None,
) -> dict:
    """Count matches per file (`-c` mode).

    Returns `{"counts": [{"file": "path", "count": N}, ...], "exit_code": ...}`.
    """
    if msg := _need_pattern(patterns, case_insensitive_patterns):
        return _err(msg)
    if msg := _need_target(globs, paths):
        return _err(msg)
    args = _quick_flags(
        patterns, case_insensitive_patterns, globs, excludes,
        whole_word, None, None, None, None, None,
    )
    args.append("-c")
    args.extend(paths or [])
    rc, stdout, stderr = _run(args, cwd, timeout or DEFAULT_TIMEOUT)
    if rc == 2:
        return _err(stderr.strip() or "hprscript error", rc, stderr=stderr)
    counts: list[dict] = []
    for line in stdout.splitlines():
        line = line.strip()
        if not line or ":" not in line:
            continue
        f, n = line.rsplit(":", 1)
        try:
            counts.append({"file": f, "count": int(n)})
        except ValueError:
            counts.append({"_raw": line})
    return {"counts": counts, "exit_code": rc}


@mcp.tool()
def extract_blocks(
    patterns: list[str] | None = None,
    case_insensitive_patterns: list[str] | None = None,
    block_open: str = "{",
    block_close: str = "}",
    globs: list[str] | None = None,
    excludes: list[str] | None = None,
    paths: list[str] | None = None,
    whole_word: bool = False,
    limit: int | None = 200,
    cwd: str | None = None,
    timeout: float | None = None,
) -> dict:
    """For each pattern match, extract the following balanced delimiter block.

    Walks forward from match-end, finds the first `block_open`, then tracks
    nesting until depth returns to zero. Use for function bodies, JSON
    objects, JSX subtrees, SQL `BEGIN`/`END` blocks, etc.

    - `block_open` / `block_close`: delimiters (default `{` / `}`).
      Multi-character strings work (`"<div>"` / `"</div>"`).
    - All `search` filter params (`globs`, `excludes`, `paths`, etc) apply.

    Returns records with `file`, `line`, `match`, `block`, `block_full`,
    `block_line_start`, `block_line_end`, `block_start`, `block_end`.

    NOTE: depth tracking is lexical, not language-aware. Strings/comments
    containing the close delimiter can skew the count. Python/Ruby (no
    `{}` blocks) need a different approach — see `help`.
    """
    if msg := _need_pattern(patterns, case_insensitive_patterns):
        return _err(msg)
    if msg := _need_target(globs, paths):
        return _err(msg)
    args = _quick_flags(
        patterns, case_insensitive_patterns, globs, excludes,
        whole_word, None, None, None, limit, None,
    )
    args.extend(["-block-open", block_open, "-block-close", block_close])
    args.extend(paths or [])
    rc, stdout, stderr = _run(args, cwd, timeout or DEFAULT_TIMEOUT)
    if rc == 2:
        return _err(stderr.strip() or "hprscript error", rc, stderr=stderr)
    blocks = _parse_jsonl(stdout)
    return {"blocks": blocks, "count": len(blocks), "exit_code": rc}


@mcp.tool()
def run_script(
    script: dict,
    paths: list[str] | None = None,
    cwd: str | None = None,
    timeout: float | None = None,
) -> dict:
    """Run a full hprscript JSON script (passed via `-s`).

    Use this when convenience tools aren't enough — for aggregations
    (variables + on_complete), cross-file resolution (phases), per-file
    relevance ranking (`rank: true`), grouping (`group_by`), submatch +
    block actions, absent patterns, conditional emits, etc.

    - `script`: the hprscript JSON object (Python dict).
    - `paths`: positional files/dirs that override `script.scan`.

    Returns `{"records": [...], "count": N, "exit_code": ...}`.

    See `help` for the full DSL reference.
    """
    if not isinstance(script, dict):
        return _err("`script` must be a JSON object (dict)")
    args = ["-s", json.dumps(script)]
    args.extend(paths or [])
    rc, stdout, stderr = _run(args, cwd, timeout or DEFAULT_TIMEOUT)
    if rc == 2:
        return _err(stderr.strip() or "hprscript error", rc, stderr=stderr)
    records = _parse_jsonl(stdout)
    return {"records": records, "count": len(records), "exit_code": rc}


@mcp.tool()
def help() -> str:
    """Return a concise reference for hprscript's CLI flags, JSON DSL,
    and Hyperscan PCRE quirks. Read this once at session start.
    """
    return CHEAT_SHEET


@mcp.tool()
def binary_info() -> dict:
    """Diagnostic: report which hprscript binary this MCP is using and
    whether it's reachable. Useful for debugging install issues.
    """
    path = HPRSCRIPT_BIN
    exists = Path(path).exists()
    info: dict[str, Any] = {"binary": path, "exists": exists}
    if exists:
        try:
            proc = subprocess.run(
                [path, "-h"],
                capture_output=True,
                text=True,
                timeout=5,
            )
            info["help_first_lines"] = "\n".join(
                (proc.stdout or proc.stderr).splitlines()[:5]
            )
        except Exception as exc:
            info["probe_error"] = str(exc)
    return info


def main() -> None:
    mcp.run()


if __name__ == "__main__":
    main()
