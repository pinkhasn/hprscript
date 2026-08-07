#!/usr/bin/env python3
"""Validate and summarize hprscript agent-benchmark JSONL results."""

import json
import statistics
import sys
from collections import defaultdict
from pathlib import Path

REQUIRED = {
    "task_id": str, "condition": str, "agent": str, "started_at": str,
    "wall_ms": int, "llm_turns": int, "tool_calls": int,
    "search_calls": int, "search_wall_ms": int, "command_failures": int,
    "stdout_bytes": int, "files_read_whole": int,
    "answer_score": (int, float), "task_complete": bool, "notes": str,
}
CONDITIONS = {
    "grep_only", "hprscript_available", "hprscript_preferred",
    "hprscript_required", "hprscript_investigate", "hprscript_query",
}


def load(path):
    rows = []
    with path.open(encoding="utf-8") as stream:
        for line_no, line in enumerate(stream, 1):
            if not line.strip():
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path}:{line_no}: {exc}") from exc
            for field, expected in REQUIRED.items():
                if field not in row or not isinstance(row[field], expected):
                    raise ValueError(f"{path}:{line_no}: missing or invalid {field}")
            if row["condition"] not in CONDITIONS:
                raise ValueError(f"{path}:{line_no}: unknown condition {row['condition']}")
            if not 0 <= float(row["answer_score"]) <= 1:
                raise ValueError(f"{path}:{line_no}: answer_score outside 0..1")
            rows.append(row)
    return rows


def median(rows, field):
    return statistics.median(row[field] for row in rows)


def main(argv):
    if len(argv) != 2:
        print(f"usage: {argv[0]} RESULTS.jsonl", file=sys.stderr)
        return 2
    try:
        rows = load(Path(argv[1]))
    except (OSError, ValueError) as exc:
        print(exc, file=sys.stderr)
        return 2
    grouped = defaultdict(list)
    for row in rows:
        if row["task_complete"]:
            grouped[row["condition"]].append(row)
    print("condition\truns\twall_ms\tsearch_calls\tllm_turns\tstdout_bytes\tanswer_score\tfirst_query_sufficiency")
    for condition in sorted(grouped):
        group = grouped[condition]
        sufficient = [
            row.get("first_query_sufficient", row["search_calls"] <= 1)
            for row in group
        ]
        print(
            f"{condition}\t{len(group)}\t{median(group, 'wall_ms'):.0f}\t"
            f"{median(group, 'search_calls'):.1f}\t{median(group, 'llm_turns'):.1f}\t"
            f"{median(group, 'stdout_bytes'):.0f}\t{median(group, 'answer_score'):.3f}\t"
            f"{sum(sufficient) / len(sufficient):.3f}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
