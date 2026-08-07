# Security Policy

## Reporting a Vulnerability

If you believe you have found a security vulnerability in `hprscript`, please report it **privately** — do not open a public GitHub issue.

Preferred channel: [GitHub private vulnerability reporting](https://github.com/pinkhasn/hprscript/security/advisories/new).

Alternatively, email **pinkhas@nisanov.com** with:

- A description of the issue and its impact.
- Steps to reproduce (input, command line, expected vs. observed behavior).
- The `hprscript -version` output and your OS / kernel.
- Any proof-of-concept patterns or inputs (please keep these minimal).

You can expect an initial acknowledgement within **7 days**. Once the issue is confirmed, I will work on a fix and coordinate disclosure with you.

## Supported Versions

Only the latest release on the `main` branch is supported. Fixes are not backported.

## Scope

In scope:

- Crashes, hangs, or memory-safety issues triggered by crafted input (patterns, files, or stdin).
- Path-handling bugs that read or write outside the requested target.

Out of scope:

- Vulnerabilities in upstream dependencies (e.g. Vectorscan/Hyperscan, libc) — please report those upstream. Mention them here only if `hprscript` exposes them in a non-default way.
- Resource exhaustion from intentionally pathological regexes supplied by the user running the tool (this is expected; `hprscript` runs the patterns you give it).
