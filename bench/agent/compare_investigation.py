#!/usr/bin/env python3
"""Local evidence/time comparison; this does not simulate an agent benchmark."""

import argparse
import json
from pathlib import Path
import statistics
import subprocess
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--candidate", required=True, type=Path)
    parser.add_argument("--repository", required=True, type=Path,
                        help="same pinned hprscript source snapshot for both binaries")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    fixtures = Path(__file__).resolve().parent / "fixtures" / "investigation"
    cases = [
        ("helper-test", "validateToken", fixtures, 16000,
         ["return verifySignature(token)", "return len(token) > 12", 't.Fatal("short signature accepted")']),
        ("cpp-definition", "build_file_scope", args.repository / "src", 16000,
         ["if (sc.anchor_regex.empty()) return nullptr;", "return &scope;", "ScopeConfig resolve_scope_for_file("]),
        ("noisy-caller", "noisyTarget", fixtures, 16000,
         ["return directHelper(value)", "return value + 1"]),
        ("small-budget", "validateToken", fixtures, 3500, ["return verifySignature(token)"]),
    ]
    args.output_dir.mkdir(parents=True, exist_ok=True)
    results = []
    for label, binary in (("baseline", args.baseline.resolve()), ("candidate", args.candidate.resolve())):
        version = subprocess.check_output([str(binary), "--version"], text=True).strip()
        for name, seed, root, budget, required in cases:
            command = [str(binary), "investigate", "-F", seed, "-profile", "symbol",
                       "-llm", "-evidence-budget", str(budget), str(root.resolve())]
            subprocess.run(command, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, check=True)
            samples = []
            outputs = []
            for _ in range(args.runs):
                start = time.perf_counter_ns()
                result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=True)
                samples.append((time.perf_counter_ns() - start) / 1_000_000)
                outputs.append(result.stdout)
            output = outputs[-1]
            text = output.decode("utf-8")
            found = {item: item in text for item in required}
            (args.output_dir / f"{name}-{label}.llm").write_bytes(output)
            results.append({"case": name, "condition": label, "binary_version": version,
                            "command": command, "median_local_ms": statistics.median(samples),
                            "runs": args.runs, "stdout_bytes": len(output), "budget": budget,
                            "within_cap": len(output) <= budget, "source_requirements": found,
                            "required_source_present": all(found.values()),
                            "deterministic_output": all(item == output for item in outputs)})
    report = {"kind": "local-investigation-comparison", "agent_benchmark_run": False,
              "repository_snapshot": str(args.repository.resolve()), "results": results}
    (args.output_dir / "comparison.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
