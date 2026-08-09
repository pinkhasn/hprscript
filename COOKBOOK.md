# hprscript Cookbook

Real-world problems solved with [`hprscript`](README.md) — multi-pattern PCRE search across files, directory trees, and pipelines, all matched in a single Hyperscan pass.

This cookbook is organized by problem domain, not by feature. Pick the section that matches the kind of data you're working with, copy a recipe, adapt the globs / patterns / field names. Every code block is meant to be copy-pasteable.

For the canonical reference (every flag, the script-mode JSON DSL, regex quirks, exit codes), see **[HPRSCRIPT.md](HPRSCRIPT.md)** — that file is the single source of truth.

---

## Recipe template

Each recipe has the same shape:

- **Problem** — what you're trying to find or measure, and why a naive grep falls short.
- **Input** — what shape the data is in.
- **Recipe** — the exact `hprscript` invocation, copy-pasteable.
- **Use the IDs / Use the output** *(when relevant)* — what to do with the result downstream (count per pattern, group by severity, feed jq, etc.).
- **Why hprscript** — the technique that makes hprscript a good fit (per-pattern IDs, one-DFA scan, balanced-block walk, multi-phase, `-absent`, etc.), explained for someone meeting the tool for the first time.

## Conventions

- Globs use `**/*.ext`. Replace with whatever file shape you have. Both relative (`**/*.go`) and absolute (`/var/log/**/*.log`) bases work; absolute is handy when the target tree is not under the working directory.
- Stdin recipes omit `-glob` and any positional file arg — content flows in from the upstream pipe.
- `-pi` is per-pattern case-insensitive; `-p` is case-sensitive. They mix freely in one invocation.
- Capture groups `(...)` are ignored by Hyperscan unless surfaced via `-extract <names>` (CLI) or `"extract": [...]` (script). Names map left-to-right in pattern order.
- Hyperscan PCRE has no lookarounds, no backreferences, no `\K`. See [HPRSCRIPT.md → Regex syntax](HPRSCRIPT.md#regex-syntax-hyperscan-pcre) for what compiles.
- `^` / `$` are line-anchored by default.
- Inside a `-s '...'` JSON script, every backslash in a regex doubles: `\d` → `\\d`.

---

## What makes hprscript special

Three properties separate hprscript from `grep` / `ripgrep`. Almost every recipe in this cookbook uses at least one. Read this section first — the recipes will make much more sense.

### 1. Per-pattern identification (the `pat` field)

When you give hprscript multiple `-p` flags, **every match record carries a `pat` field** that tells you which pattern matched. Five `-p` flags → five categories of result, each labelled. Compare:

```bash
# grep alternation — all matches look the same
grep -E 'WARN|ERROR|FATAL' app.log
# ... ERROR connection lost
# ... WARN slow query
# ... FATAL out of memory
# You can't easily count "how many ERROR vs FATAL" without re-parsing.

# hprscript multi-pattern — each match self-identifies
hprscript -p 'WARN' -p 'ERROR' -p 'FATAL' app.log
# {"file":"app.log","pat":"p0","line":42,"match":"WARN","context":"..."}
# {"file":"app.log","pat":"p1","line":88,"match":"ERROR","context":"..."}
# {"file":"app.log","pat":"p2","line":99,"match":"FATAL","context":"..."}
```

Default IDs are `p0`, `p1`, `p2`, …. In script mode you can name them so output is self-documenting:

```bash
hprscript -s '{
  "scan": ["app.log"],
  "patterns": [
    {"id":"warn",  "regexp":"WARN"},
    {"id":"error", "regexp":"ERROR"},
    {"id":"fatal", "regexp":"FATAL"}
  ]
}'
# {"file":"app.log","pat":"warn","line":42, ...}
# {"file":"app.log","pat":"error","line":88, ...}
# {"file":"app.log","pat":"fatal","line":99, ...}
```

This unlocks per-pattern counts, per-pattern ranking, group-by-category, and conditional logic that would otherwise need a second tool.

### 2. One-DFA scanning of N patterns

Hyperscan compiles all your patterns into a single deterministic finite automaton. Adding pattern #11 to a 10-pattern scan is virtually free. Running 100 patterns in one pass costs about the same as running 1.

This is why the recipes in this cookbook routinely scan for 5–20 things simultaneously where you'd otherwise run grep in a loop or pipe greps together. **It also means you should prefer many small specific patterns over one big alternation** — you get the same scan cost plus per-pattern identification.

### 3. Cross-line block extraction

`-block-open` / `-block-close` (CLI) or the `block` action (script) walks a balanced-delimiter block — function bodies, JSON objects, JSX trees, SQL `BEGIN`/`END`, multi-line PEM blobs. Depth-tracked, multi-character delimiters welcome. Line-based tools like grep stop at newline; hprscript follows the structure.

---

## Unicode & UTF-8 in 60 seconds

UTF-8 is on by default. The recipes in §9 (PII), §12 (phishing), §22 (web scraping), §23 (email), and §27 (NLP) put these to work in real scenarios — what follows is a quick tour.

**Case-folding works across scripts.** `-pi` (CLI) or `"case_insensitive": true` (script) folds case in *every* Unicode script — Latin, Cyrillic, Greek, Armenian, Georgian. Diacritics fold too:

```bash
hprscript -pi 'café' notes.txt
# Matches "café", "CAFÉ", "Café".

hprscript -pi 'привет' chat.log
# Matches "привет", "ПРИВЕТ", "Привет".
```

**For Unicode word matching, use `\p{L}+` — not `\w+` with `ucp`.** By default `\w`/`\d`/`\s` are ASCII-only, so `\w+` won't match `naïve` (it stops at `i`). `ucp` makes those classes Unicode-aware, but Vectorscan then rejects unanchored `\w+`-style repeats (and any bounded repeat such as `\p{L}{4,}`) as "Pattern is too large" — see [the reference](HPRSCRIPT.md#when-to-use--ucp-vs-alternatives). The form that compiles and matches `naïve`, `Москва`, `北京`, `العربية`, `日本語` is `\p{L}+`, which is Unicode-aware in the default UTF-8 mode without `ucp`. (`ucp` still earns its keep in *anchored* patterns — see [recipe 22.6](#226-non-latin-titles--ucp-for-cross-script-w-utf-8-demo).)

```bash
# Word-frequency over a multilingual corpus (Russian/Chinese/English mixed).
hprscript -s '{
  "scan": ["corpus/*.txt"],
  "variables": {"freq":{"type":"map"}},
  "patterns": [{"id":"w","regexp":"\\p{L}+",
    "on_match":[{"action":"map_increment","target":"freq","key":"$MATCH"}]}],
  "on_complete":[
    {"action":"for_each","var":"freq","key_as":"w","as":"n","do":[
      {"action":"emit","data":{"word":"$w","count":"$n"}}]}]
}'
```

**Match a specific script via Unicode ranges.** Hyperscan accepts `\x{NNNN}` codepoint escapes — perfect for detecting Cyrillic look-alikes in Latin contexts (homograph attacks), CJK in mostly-English files, etc.

| Script | Range |
|---|---|
| Cyrillic | `\x{0400}-\x{04FF}` |
| Greek | `\x{0370}-\x{03FF}` |
| Arabic | `\x{0600}-\x{06FF}` |
| Han (CJK) | `\x{4E00}-\x{9FFF}` |
| Hiragana | `\x{3040}-\x{309F}` |
| Hebrew | `\x{0590}-\x{05FF}` |

```bash
# Latin-looking domain names that secretly contain Cyrillic letters.
hprscript -p 'https?://[a-zA-Z0-9.\-]*[\x{0400}-\x{04FF}][a-zA-Z0-9.\-]*' suspect_emails.txt
```

---

## Table of contents

### Logs & Observability
1. [Application log triage](#1-application-log-triage)
2. [Web server logs (nginx / apache)](#2-web-server-logs-nginx--apache)
3. [Kubernetes & container logs](#3-kubernetes--container-logs)
4. [systemd, journald & sshd auth forensics](#4-systemd-journald--sshd-auth-forensics)
5. [Database slow-query logs](#5-database-slow-query-logs)
6. [Distributed trace correlation](#6-distributed-trace-correlation)

### Security & DFIR
7. [Secret & credential scanning](#7-secret--credential-scanning)
8. [IOC hunting](#8-ioc-hunting)
9. [PII & sensitive-data discovery](#9-pii--sensitive-data-discovery)
10. [Web shell & backdoor detection](#10-web-shell--backdoor-detection)
11. [Pentest tool output parsing](#11-pentest-tool-output-parsing)
12. [Phishing email triage](#12-phishing-email-triage)

### Source code
13. [SAST-lite security audit](#13-sast-lite-security-audit)
14. [Code review & lint sweeps](#14-code-review--lint-sweeps)
15. [Migration scans](#15-migration-scans)
16. [Dependency & license audit](#16-dependency--license-audit)

### Configuration & Infrastructure
17. [Container & cloud config audit](#17-container--cloud-config-audit)
18. [Network & firewall config audit](#18-network--firewall-config-audit)
19. [DNS, BGP & routing config](#19-dns-bgp--routing-config)

### Data wrangling
20. [CSV / TSV / fixed-width records](#20-csv--tsv--fixed-width-records)
21. [JSON Lines & XML triage](#21-json-lines--xml-triage)
22. [Web scraping & HTML extraction](#22-web-scraping--html-extraction)
23. [Email corpus mining (.eml / .mbox)](#23-email-corpus-mining-eml--mbox)

### Specialized verticals
24. [Bioinformatics](#24-bioinformatics)
25. [Finance — SWIFT / FIX / market data](#25-finance--swift--fix--market-data)
26. [Legal & contracts](#26-legal--contracts)
27. [NLP & corpus linguistics](#27-nlp--corpus-linguistics)

### DevOps & build tooling
28. [Build & test output triage](#28-build--test-output-triage)
29. [Git commit message audits](#29-git-commit-message-audits)

### Documentation
30. [Markdown & docs audit](#30-markdown--docs-audit)

### Forensics & misc
31. [Strings triage — memory dumps, firmware, binaries](#31-strings-triage--memory-dumps-firmware-binaries)
32. [Academic — LaTeX & BibTeX](#32-academic--latex--bibtex)

### Editing files
33. [Guarded code edits (`hprscript edit`)](#33-guarded-code-edits-hprscript-edit)

### AI-agent workflows
34. [Context retrieval & ranking for LLM agents](#34-context-retrieval--ranking-for-llm-agents)

---

# Logs & Observability

## 1. Application log triage

### 1.1 Per-severity log triage with named pattern IDs

**Problem:** You have a tree of service logs and want to triage by severity. A single `grep -E 'WARN|ERROR|FATAL'` gives you one undifferentiated stream — you can't tell at a glance how many warnings vs errors vs fatals, and you can't act differently on each. With four named patterns, every match record carries the severity, so you can bucket / count / route on it without a second pass.

**Input:** Plain-text application logs (`*.log`).

```bash
# Each pattern has its own id. -format makes the id visible per match.
hprscript \
  -p 'WARN'  \
  -p 'ERROR' \
  -p 'FATAL' \
  -p 'PANIC' \
  -format '$PAT_ID  $FILE:$LINE  $CONTEXT' \
  -C 1 -glob '**/*.log'
# →  p0  app.log:42  ... WARN: retrying ...
# →  p1  app.log:88  ... ERROR: connection lost ...
# →  p2  db.log:5    ... FATAL: out of memory ...
```

**Use the IDs — count occurrences per severity:**

```bash
hprscript -p 'WARN' -p 'ERROR' -p 'FATAL' -p 'PANIC' -glob '**/*.log' \
  | jq -r '.pat' | sort | uniq -c
#   1284 p0     ← WARN
#    317 p1     ← ERROR
#     12 p2     ← FATAL
#      3 p3     ← PANIC
```

**Or in script mode — name them, aggregate per name in one process:**

```bash
hprscript -s '{
  "scan": ["**/*.log"],
  "variables": {"by_sev": {"type":"map"}},
  "patterns": [
    {"id":"warn",  "regexp":"WARN",  "on_match":[{"action":"map_increment","target":"by_sev","key":"warn"}]},
    {"id":"error", "regexp":"ERROR", "on_match":[{"action":"map_increment","target":"by_sev","key":"error"}]},
    {"id":"fatal", "regexp":"FATAL", "on_match":[{"action":"map_increment","target":"by_sev","key":"fatal"}]},
    {"id":"panic", "regexp":"PANIC", "on_match":[{"action":"map_increment","target":"by_sev","key":"panic"}]}
  ],
  "on_complete":[
    {"action":"for_each","var":"by_sev","key_as":"sev","as":"n","do":[
      {"action":"emit","data":{"severity":"$sev","count":"$n"}}]}
  ]
}'
# {"severity":"warn","count":1284}
# {"severity":"error","count":317}
# {"severity":"fatal","count":12}
# {"severity":"panic","count":3}
```

**Why hprscript:** Four patterns share one DFA — same scan cost as searching for one. Each match record carries `pat`, so a single scan gives you both the matches and the categorization. Plain `grep -E 'WARN|ERROR|...'` flattens that distinction; you'd need a second regex pass downstream to bucket the results.

### 1.2 Correlate stack traces with the originating ERROR line

**Problem:** Service logs interleave error headers and stack frames from many concurrent requests. You want only the `ERROR` lines that actually have a stack trace nearby (the others are routine, and most error lines don't fail loudly enough to leave a frame). This is a relational filter — line A only matters if line B is within K lines — and pure grep can't express that.

**Input:** Application logs with `ERROR` headers and `at <pkg>.<class>(...)` or Python `File "...", line N` traces.

```bash
hprscript \
  -p 'ERROR.*Exception' \
  -p 'at \w+\.\w+\.[\w$]+\(' \
  -near p0:p1:30 \
  -glob '**/*.log'
```

**Why hprscript:** `-near A:B:K` is the missing primitive. It says "emit matches of A only when a match of B is within K lines." Two patterns, one DFA, one structural filter — no awk, no shell loops.

### 1.3 Per-file error count, ranked

**Problem:** Across many service logs, which file has the most errors? Useful for triage prioritization after a deploy or an incident. The `-c` flag emits one line per file with its match count — you sort and pick the top.

**Input:** Log tree.

```bash
hprscript -pi 'error|fatal|panic' -c -glob '**/*.log' | sort -t: -k2 -n -r | head -20
```

*(This recipe **does** use alternation — intentionally. The output is per-file, not per-severity, so distinguishing severities adds no value here.)*

### 1.4 Files with errors but no resolution

**Problem:** Find services where errors fired but no recovery line followed — likely real, unhandled failures (vs transient blips that the service recovered from). This is per-file state: you want the file emitted only if condition A holds AND condition B is absent. Beyond what flag-mode can express, but a natural fit for script-mode variables.

**Input:** Service logs that include both error-level lines and recovery markers (`recovered`, `retried`, `succeeded`).

```bash
hprscript -s '{
  "scan": ["**/*.log"],
  "variables": {"has_err": {"type":"bool"}, "has_fix": {"type":"bool"}},
  "patterns": [
    {"id":"err","regexp":"ERROR|FATAL","case_insensitive":true,
     "on_match":[{"action":"set","var":"has_err","value":true}]},
    {"id":"fix","regexp":"recovered|retried|succeeded","case_insensitive":true,
     "on_match":[{"action":"set","var":"has_fix","value":true}]}
  ],
  "on_file_end":[
    {"action":"if","condition":{"op":"and","args":[
      {"op":"eq","args":["$has_err",true]},
      {"op":"eq","args":["$has_fix",false]}]},
     "then":[{"action":"emit","data":{"file":"$FILE","status":"unresolved errors"}}]},
    {"action":"reset","vars":["has_err","has_fix"]}
  ]
}'
```

**Why hprscript:** Variables persist within a file and reset at file end (`on_file_end`). The conditional `emit` runs once per file, not per match — exactly the granularity you want for "did this file have errors without recovery?"

### 1.5 Multi-pattern alarm over a log snapshot

**Problem:** Sweep a recent log window for any of N alert patterns simultaneously — one invocation showing all the things-that-might-break, not N separate `grep` passes.

**Input:** Stdin from a bounded log dump (`kubectl logs --tail=…`, `journalctl --since=…`, a rotated file).

```bash
kubectl logs deploy/api --tail=20000 | hprscript \
  -p 'panic'      \
  -p 'OOM'        \
  -p 'deadlock'   \
  -p 'connection refused' \
  -format '$PAT_ID  $CONTEXT' -C 2
```

**Why hprscript:** When no `-glob` is given, hprscript reads stdin — slots straight into pipelines. Adding pattern #5 to the alarm has zero scan cost, and `$PAT_ID` makes it obvious which alarm fired.

**Caveat — no live tail:** hprscript buffers stdin to EOF before scanning, so a follow stream (`kubectl logs -f`, `tail -F`) produces no output until the stream closes. Feed it bounded snapshots (`--tail`, `--since`, log files) and re-run — or loop it: `while :; do kubectl logs deploy/api --since=30s | hprscript …; sleep 30; done`.

### 1.6 Extract request durations from log lines

**Problem:** Pull elapsed milliseconds out of every request log line. Capture groups `(...)` exist in your regex but Hyperscan ignores them by default — `-extract` adds a regex post-pass that pulls them out by name.

**Input:** Lines like `... duration=123ms ...`.

```bash
hprscript -p 'duration=(\d+)ms' -extract ms \
  -format '$FILE:$LINE  ms=$EXTRACT_MS' -glob '**/*.log'
```

**Why hprscript:** `-extract <name>` names the capture groups left-to-right; `$EXTRACT_<NAME>` then becomes available in `-format` and (script mode) inside `data` shapes. No awk, no second sed.

---

## 2. Web server logs (nginx / apache)

### 2.1 5xx burst hunt

**Problem:** Find every server-side error response in the access log. A 5xx burst is the classic outage signal — once you have the matches you can bucket by URI, by minute, or by upstream to localize the failure.

**Input:** Combined-log-format access logs (`access.log*`).

```bash
hprscript -p ' "[A-Z]+ [^"]+ HTTP/[\d.]+" 5\d\d ' -glob '**/access.log*'
```

### 2.2 Slow requests (response time > 2s)

**Problem:** When nginx is configured with `$request_time` at the end of the log line, slow requests stand out as decimal numbers ≥ 2.0. You want every line where the trailing time field qualifies.

**Input:** nginx logs ending with elapsed seconds (e.g. `... 2.347`).

```bash
hprscript -p ' ([2-9]\.\d+|[1-9]\d+\.\d+)$' -glob '**/access.log*'
```

### 2.3 Classify scanner traffic by tool family

**Problem:** Bots, scrapers, security scanners and exploit attempts all show up in the access log under different user-agent strings. Rather than dumping a flat list, classify each match by *which tool family* it came from — so you can route legitimate scrapers (curl, wget) one way and suspected attackers (sqlmap, nikto) another. With one named pattern per family, each match record carries the classification.

**Input:** Access logs with UA in the last quoted field.

```bash
hprscript \
  -pi '"[^"]*\bcurl\b[^"]*"'           \
  -pi '"[^"]*\bwget\b[^"]*"'           \
  -pi '"[^"]*python-requests[^"]*"'    \
  -pi '"[^"]*\b(nikto|sqlmap|nmap|masscan|zgrab|gobuster|fuzzer)\b[^"]*"' \
  -format '$PAT_ID  $FILE:$LINE  $MATCH' \
  -glob '**/access.log*'
# →  p0  access.log:18    "...curl/7.81..."
# →  p3  access.log:42    "...sqlmap/1.6..."
```

**Use the IDs — top tool families:**

```bash
hprscript -pi '"[^"]*\bcurl\b[^"]*"' -pi '"[^"]*\bwget\b[^"]*"' \
          -pi '"[^"]*python-requests[^"]*"' \
          -pi '"[^"]*\b(nikto|sqlmap|nmap|masscan|zgrab|gobuster|fuzzer)\b[^"]*"' \
          -glob '**/access.log*' \
  | jq -r '.pat' | sort | uniq -c
```

### 2.4 Top offending IPs (4xx/5xx hits per IP)

**Problem:** Aggregate "bad-status" hits per source IP to find sustained probing. Builds a histogram in one process — no awk pipeline needed.

**Input:** Combined-format access logs.

```bash
hprscript -s '{
  "scan": ["**/access.log*"],
  "variables": {"bad": {"type":"map"}},
  "patterns": [
    {"id":"hit","regexp":"^(\\d+\\.\\d+\\.\\d+\\.\\d+) .* \" [45]\\d\\d ",
     "extract": ["ip"],
     "on_match":[{"action":"map_increment","target":"bad","key":"$EXTRACT_IP"}]}
  ],
  "on_complete":[
    {"action":"for_each","var":"bad","key_as":"ip","as":"n","do":[
      {"action":"emit","data":{"ip":"$ip","bad_hits":"$n"}}]}
  ]
}'
```

**Why hprscript:** `extract` pulls the IP out of each match into `$EXTRACT_IP`, which is then used as the map key. One pass, one tool, sortable JSON-Lines histogram out the other end.

### 2.5 Web-app attack probes — classified by attack class

**Problem:** A SOC needs the access log scanned for the four big URL-borne attack classes (SQLi, path traversal, XSS, log4shell), and the report needs to *say which class* each finding belongs to so analysts can prioritize. One alternation pattern would catch them all but flatten the classification — four named patterns keep every match self-labelled.

**Input:** Access logs.

```bash
hprscript \
  -pi 'union\s+select|select.+from.+--|or\s+1=1' \
  -p  '\.\./|\.\.\\\\'                            \
  -pi '<script|javascript:'                       \
  -p  '\$\{jndi:'                                 \
  -format '$PAT_ID  $FILE:$LINE  $MATCH' \
  -glob '**/access.log*'
# →  p0  access.log:118  "...?id=1' UNION SELECT..."
# →  p1  access.log:204  "...?path=../../etc/passwd"
# →  p2  access.log:301  "...?q=<script>alert..."
# →  p3  access.log:44   "...User-Agent: ${jndi:ldap://..."
```

**Use the IDs — count attacks per class, per file:**

```bash
hprscript -s '{
  "scan": ["**/access.log*"],
  "variables": {"by_class": {"type":"map"}},
  "patterns": [
    {"id":"sqli", "regexp":"union\\s+select|select.+from.+--|or\\s+1=1","case_insensitive":true,
     "on_match":[{"action":"map_increment","target":"by_class","key":"sqli"}]},
    {"id":"trav", "regexp":"\\.\\./|\\.\\.\\\\\\\\",
     "on_match":[{"action":"map_increment","target":"by_class","key":"trav"}]},
    {"id":"xss",  "regexp":"<script|javascript:","case_insensitive":true,
     "on_match":[{"action":"map_increment","target":"by_class","key":"xss"}]},
    {"id":"jndi", "regexp":"\\$\\{jndi:",
     "on_match":[{"action":"map_increment","target":"by_class","key":"jndi"}]}
  ],
  "on_complete":[
    {"action":"for_each","var":"by_class","key_as":"c","as":"n","do":[
      {"action":"emit","data":{"attack_class":"$c","count":"$n"}}]}
  ]
}'
```

**Why hprscript:** Adding a fifth attack class (e.g. SSRF probes) costs nothing in scan time and gives you a fifth labelled bucket automatically. Try doing that with sequential greps without rewriting the whole pipeline.

### 2.6 Status code distribution

**Problem:** Quick traffic histogram by response code. A one-liner — extracts the code, sorts, counts.

**Input:** Access logs.

```bash
hprscript -p ' "[A-Z]+ [^"]+ HTTP/[\d.]+" (\d{3}) ' -extract code \
  -format '$EXTRACT_CODE' -glob '**/access.log*' | sort | uniq -c | sort -rn
```

---

## 3. Kubernetes & container logs

### 3.1 Pod failure mode classification

**Problem:** The kubelet event log mixes dozens of failure modes — `CrashLoopBackOff`, `OOMKilled`, `Evicted`, `ImagePullBackOff`, etc. To prioritize remediation you don't just want the count of failures, you want the count *per failure mode* so you can tell "we have 80 OOMs and 3 crash loops" at a glance. Named patterns give you that breakdown for free.

**Input:** kubelet log files or `kubectl get events -A -o json` text dumps.

```bash
hprscript \
  -p 'CrashLoopBackOff'  \
  -p 'OOMKilled'         \
  -p 'Evicted'           \
  -p 'ImagePullBackOff'  \
  -p 'CreateContainerError' \
  -format '$PAT_ID  $FILE:$LINE  $CONTEXT' \
  -C 2 -glob '**/*.log'
```

**Use the IDs — count failures per mode and per pod:**

```bash
hprscript -s '{
  "scan": ["**/*.log"],
  "variables": {"by_mode": {"type":"map"}, "by_pod": {"type":"map"}},
  "patterns": [
    {"id":"crashloop","regexp":"CrashLoopBackOff",
     "on_match":[{"action":"map_increment","target":"by_mode","key":"crashloop"}]},
    {"id":"oom","regexp":"OOMKilled",
     "on_match":[{"action":"map_increment","target":"by_mode","key":"oom"}]},
    {"id":"evicted","regexp":"Evicted",
     "on_match":[{"action":"map_increment","target":"by_mode","key":"evicted"}]},
    {"id":"pull","regexp":"ImagePullBackOff|ErrImagePull|manifest unknown",
     "on_match":[{"action":"map_increment","target":"by_mode","key":"image_pull"}]},
    {"id":"create","regexp":"CreateContainerError|RunContainerError",
     "on_match":[{"action":"map_increment","target":"by_mode","key":"container_create"}]}
  ],
  "on_complete":[
    {"action":"for_each","var":"by_mode","key_as":"mode","as":"n","do":[
      {"action":"emit","data":{"failure_mode":"$mode","count":"$n"}}]}
  ]
}'
# {"failure_mode":"crashloop","count":3}
# {"failure_mode":"oom","count":80}
# {"failure_mode":"evicted","count":12}
# ...
```

### 3.2 Image-pull failure root-cause classification

**Problem:** "Image pull failed" hides three very different problems: typo in image name, missing registry credentials, and rate-limit. Each one has a different fix and a different person to call. Naming the patterns separates the three buckets so the on-call engineer doesn't waste time on the wrong fix.

**Input:** kubelet logs.

```bash
hprscript \
  -pi 'manifest unknown|repository.*not found' \
  -pi 'unauthorized|authentication required'   \
  -pi 'pull rate limit|toomanyrequests'         \
  -pi 'no such host|i/o timeout'                \
  -format '$PAT_ID  $FILE:$LINE  $CONTEXT' \
  -C 1 -glob '**/*.log'
# →  p0  ... "manifest unknown" — typo or missing tag
# →  p1  ... "unauthorized" — registry auth
# →  p2  ... "pull rate limit" — Docker Hub throttling
# →  p3  ... "i/o timeout" — registry network reachability
```

### 3.3 Pods restarting frequently (count restarts per pod)

**Problem:** Aggregate the per-pod `restartCount` mentions to surface flappers.

**Input:** kubelet logs with `pod="<name>" ... restartCount=<n>`.

```bash
hprscript -s '{
  "scan": ["**/kubelet*.log"],
  "variables": {"restarts": {"type":"map"}},
  "patterns": [
    {"id":"r","regexp":"pod=\"([^\"]+)\".*restartCount=([1-9]\\d*)",
     "extract": ["pod","n"],
     "on_match":[{"action":"map_set","target":"restarts","key":"$EXTRACT_POD","value":"$EXTRACT_N"}]}
  ],
  "on_complete":[
    {"action":"for_each","var":"restarts","key_as":"pod","as":"n","do":[
      {"action":"emit","data":{"pod":"$pod","restarts":"$n"}}]}
  ]
}'
```

### 3.4 Liveness vs readiness probe failures (separated)

**Problem:** Liveness and readiness probe failures look similar but mean different things — liveness failure restarts the container, readiness failure just removes it from service. Two named patterns let you count each separately.

**Input:** kubelet logs.

```bash
hprscript \
  -p 'Liveness probe failed'  \
  -p 'Readiness probe failed' \
  -format '$PAT_ID  $FILE:$LINE  $CONTEXT' \
  -C 1 -glob '**/*.log'

# Per-probe-type histogram:
hprscript -p 'Liveness probe failed' -p 'Readiness probe failed' -glob '**/*.log' \
  | jq -r '.pat' | sort | uniq -c
```

### 3.5 Audit-log: privileged exec / attach into pods

**Problem:** Catch `kubectl exec` and friends in audit JSON. These three subresources (`exec`, `attach`, `portforward`) are the privileged-access endpoints — every invocation should be reviewed.

**Input:** kube-apiserver audit log JSON.

```bash
hprscript -p '"verb":"create".*"subresource":"(exec|attach|portforward)"' \
  -extract action \
  -format '$FILE:$LINE  action=$EXTRACT_ACTION  ctx=$CONTEXT' \
  -glob '**/audit*.log'
```

### 3.6 Stream live cluster events for failure patterns

**Problem:** Watch incoming events in real time; alarm on any of N classes.

**Input:** Stdin from `kubectl get events -A -w`.

```bash
kubectl get events -A -w | hprscript \
  -pi 'failed' -pi 'backoff' -pi 'killed' -pi 'evicted' -pi 'unhealthy' \
  -format '$PAT_ID  $CONTEXT'
```

---

## 4. systemd, journald & sshd auth forensics

### 4.1 Failed sshd logins

**Problem:** Surface every failed SSH login attempt and pull the source IP into a separate field. Easy to do, but the source IP is the thing you want to count downstream — `-extract` makes it a first-class field.

**Input:** `/var/log/auth.log*` or journalctl text export.

```bash
hprscript -p 'sshd\[\d+\]: Failed password for (invalid user )?\S+ from (\d+\.\d+\.\d+\.\d+)' \
  -extract user_part,ip \
  -format '$LINE  ip=$EXTRACT_IP' \
  -glob '/var/log/auth.log*'
```

### 4.2 Successful login immediately after failures (brute-force success)

**Problem:** Brute-force followed by a successful login is the strongest single indicator of an actual compromise. You want `Accepted password` lines that have a `Failed password` for the same host within a 10-line window. `-near` does the relational filter in one pass.

**Input:** auth logs.

```bash
hprscript -p 'Failed password.*from \d+\.\d+\.\d+\.\d+' \
          -p 'Accepted password.*from \d+\.\d+\.\d+\.\d+' \
          -near p1:p0:10 \
          -glob '/var/log/auth.log*'
```

**Why hprscript:** `-near A:B:K` says "emit A only when B is within K lines." Without it you'd shell-script over the output; with it the filter is one flag.

### 4.3 sudo abuse — which user runs what

**Problem:** Audit `sudo` usage per user across an auth-log archive: who ran how many `sudo` commands, and what did they run? Two outputs from one scan: a per-user count, plus a flat per-invocation log.

**Input:** auth.log.

```bash
hprscript -s '{
  "scan": ["/var/log/auth.log*"],
  "variables": {"by_user": {"type":"map"}},
  "patterns": [
    {"id":"sudo","regexp":"sudo:\\s+(\\w+)\\s+:.*COMMAND=(\\S+)",
     "extract": ["user","cmd"],
     "on_match":[
       {"action":"map_increment","target":"by_user","key":"$EXTRACT_USER"},
       {"action":"emit","data":{"user":"$EXTRACT_USER","cmd":"$EXTRACT_CMD","line":"$LINE"}}]}
  ],
  "on_complete":[
    {"action":"for_each","var":"by_user","key_as":"u","as":"n","do":[
      {"action":"emit","data":{"summary":true,"user":"$u","sudo_count":"$n"}}]}
  ]
}'
```

### 4.4 systemd unit failures — separated by failure kind

**Problem:** systemd reports two distinct failure modes — the unit's main process exited non-zero (`Main process exited`), or the unit's `ExecStartPre`/start condition rejected it (`Failed with result`). Different remediation for each.

**Input:** journalctl text export.

```bash
journalctl --since "24h ago" | hprscript \
  -p '\b\S+\.service: Main process exited' \
  -p '\b\S+\.service: Failed with result'  \
  -format '$PAT_ID  $CONTEXT'
```

### 4.5 cron job execution audit

**Problem:** Inventory which cron jobs ran when, by user.

**Input:** /var/log/syslog* or /var/log/cron*.

```bash
hprscript -p 'CRON\[\d+\]: \((\w+)\) CMD \((.+)\)' -extract user,cmd \
  -format '$FILE:$LINE  user=$EXTRACT_USER  cmd=$EXTRACT_CMD' \
  -glob '/var/log/syslog*' -glob '/var/log/cron*'
```

### 4.6 Service restart timeline

**Problem:** Pull every state-change line for one unit, time-ordered.

**Input:** journalctl text export.

```bash
journalctl -u nginx --since "7 days ago" | hprscript \
  -p 'Started'  \
  -p 'Stopping' \
  -p 'Stopped'  \
  -p 'Failed'   \
  -format '$PAT_ID  $CONTEXT'
```

---

## 5. Database slow-query logs

### 5.1 MySQL slow-query log: queries exceeding 1s

**Problem:** Surface every query whose `Query_time` exceeds 1.0 seconds.

**Input:** MySQL slow-query log.

```bash
hprscript -p '^# Query_time: ([1-9]\d*\.\d+|\d+\.\d{2,})\b' -extract qt \
  -format '$FILE:$LINE  query_time=$EXTRACT_QT' \
  -glob '**/mysql-slow*.log'
```

### 5.2 Pull the SQL body that follows each slow header

**Problem:** Each slow-query header is followed by the actual `SELECT ...;` block. To audit them you need the SQL, not just the header line — `context_after` in script mode pulls the next N lines into `$CONTEXT_AFTER`.

**Input:** MySQL slow-query log.

```bash
hprscript -s '{
  "scan": ["**/mysql-slow*.log"],
  "context_after": 20,
  "patterns": [
    {"id":"slow","regexp":"^# Query_time: [1-9]","on_match":[
      {"action":"emit","data":{"file":"$FILE","line":"$LINE","sql_block":"$CONTEXT_AFTER"}}]}
  ]
}'
```

### 5.3 Postgres lock-contention events — classified

**Problem:** Postgres lock issues come in three flavours that need different fixes:
- `deadlock detected` — application-level ordering bug
- `canceling statement due to lock timeout` — long-held lock somewhere; usually a query plan or transaction problem
- `process X still waiting for lock` — congestion, often resolves itself

Naming each pattern lets you tell which kind dominates.

**Input:** Postgres CSV/text logs.

```bash
hprscript \
  -p 'deadlock detected'                                      \
  -p 'canceling statement due to lock timeout'                \
  -p 'process \d+ still waiting for'                          \
  -format '$PAT_ID  $FILE:$LINE  $CONTEXT' \
  -C 2 -glob '**/postgresql*.log'
```

**Use the IDs:**

```bash
hprscript -p 'deadlock detected' \
          -p 'canceling statement due to lock timeout' \
          -p 'process \d+ still waiting for' \
          -glob '**/postgresql*.log' \
  | jq -r '.pat' | sort | uniq -c
```

### 5.4 Per-user slow-query count

**Problem:** Aggregate slow-query counts by `User@Host`.

**Input:** MySQL slow-query log.

```bash
hprscript -s '{
  "scan": ["**/mysql-slow*.log"],
  "variables": {"by_user": {"type":"map"}},
  "patterns": [
    {"id":"u","regexp":"^# User@Host:\\s+(\\w+)\\[",
     "extract": ["user"],
     "on_match":[{"action":"map_increment","target":"by_user","key":"$EXTRACT_USER"}]}
  ],
  "on_complete":[
    {"action":"for_each","var":"by_user","key_as":"u","as":"n","do":[
      {"action":"emit","data":{"user":"$u","slow_queries":"$n"}}]}
  ]
}'
```

### 5.5 Repeated query templates (group by normalized SQL prefix)

**Problem:** Group similar queries by their first few keywords so you see "we ran this template 8000 times" instead of 8000 individual lines. `group_by` buffers emits and flushes one JSON line per distinct value of a chosen field.

**Input:** Slow-query log.

```bash
hprscript -s '{
  "scan": ["**/mysql-slow*.log"],
  "group_by": "template",
  "patterns": [
    {"id":"sql","regexp":"^(SELECT|UPDATE|DELETE|INSERT)\\s+\\S+(\\s+\\S+){0,3}",
     "on_match":[
       {"action":"emit","data":{"template":"$MATCH","file":"$FILE","line":"$LINE"}}]}
  ]
}'
```

### 5.6 N+1 detection — same query template repeated within 5 lines

**Problem:** A burst of identical queries within a few lines is the classic N+1 anti-pattern (one parent query, N children). `-near A:A:K` filters for "this match has another match of the same pattern within K lines."

**Input:** Slow-query log.

```bash
hprscript -p '^SELECT \* FROM orders WHERE id =' \
  -near p0:p0:5 -glob '**/mysql-slow*.log'
```

---

## 6. Distributed trace correlation

### 6.1 Follow a single request-id across all service logs

**Problem:** A failed request hops through 5 services; the trace-id is the only thing tying them together. You want every line tagged with that ID, regardless of which file it lives in. A single grep over a tree works — but with hprscript you can do it as part of a larger script (e.g. extract + classify by service).

**Input:** Multi-service log tree.

```bash
hprscript -p 'request_id=7f3c2a1b-9d4e-4f3a-b5c1-8e7f9a0b1c2d' \
  -format '$FILE:$LINE  $CONTEXT' -glob '**/*.log'
```

### 6.2 Build an inverted index: which services touched each trace-id

**Problem:** For every trace-id, list the set of files it appears in. Useful for capacity planning ("which services are hot for this user?") and incident scoping.

**Input:** Microservice logs with `trace_id=<hex>`.

```bash
hprscript -s '{
  "scan": ["**/*.log"],
  "variables": {"by_trace": {"type":"map"}},
  "patterns": [
    {"id":"t","regexp":"trace_id=([0-9a-f]{16,32})",
     "extract": ["tid"],
     "on_match":[{"action":"map_unique_append","target":"by_trace","key":"$EXTRACT_TID","value":"$FILE"}]}
  ],
  "on_complete":[
    {"action":"for_each","var":"by_trace","key_as":"tid","as":"f","do":[
      {"action":"emit","data":{"trace_id":"$tid","seen_in":"$f"}}]}
  ]
}'
```

`map_unique_append` accumulates a deduplicated *list* per key (`map_set` would keep only the last file seen), so `seen_in` comes out as the full JSON array of services:

```json
{"trace_id":"7f3c2a1b","seen_in":["logs/api.log","logs/worker.log"]}
```

### 6.3 Errors grouped by trace-id

**Problem:** Get all error-tagged lines, grouped per trace, so you see the full failure footprint per user request in one buffered emit.

**Input:** Logs with `trace_id=...` and `level=error`.

```bash
hprscript -s '{
  "scan": ["**/*.log"],
  "group_by": "trace_id",
  "patterns": [
    {"id":"e","regexp":"trace_id=([0-9a-f]+).*\\blevel=error\\b",
     "extract": ["tid"],
     "on_match":[
       {"action":"emit","data":{"trace_id":"$EXTRACT_TID","file":"$FILE","line":"$LINE","msg":"$CONTEXT"}}]}
  ]
}'
```

### 6.4 Latency outliers — extract span duration, threshold filter

**Problem:** Spans where `dur_ms` exceeds 500.

**Input:** Tracing logs with `dur_ms=<int>`.

```bash
hprscript -p 'dur_ms=([5-9]\d{2,}|[1-9]\d{3,})\b' -extract dur \
  -format '$FILE:$LINE  dur_ms=$EXTRACT_DUR  $CONTEXT' \
  -glob '**/*.log'
```

### 6.5 Cross-service span linking via parent-id (two phases)

**Problem:** To reconstruct the parent/child structure of a distributed trace, you need to know what file every span lives in *before* you can resolve `parent_id` references. That's two passes: phase 1 builds the `span_id → file:line` index; phase 2 walks `parent_id` references and looks them up. Phases share the variable store, so the map persists across passes.

**Input:** Tracing logs with `span_id=<hex>` and `parent_id=<hex>`.

```bash
hprscript -s '{
  "variables": {"spans": {"type":"map"}, "_sid": {"type":"string"}},
  "phases": [
    {"id":"collect","scan":["**/*.log"],
      "patterns":[
        {"id":"sp","regexp":"span_id=([0-9a-f]+)","extract":["sid"],
         "on_match":[{"action":"map_set","target":"spans","key":"$EXTRACT_SID","value":"$FILE:$LINE"}]}]},
    {"id":"resolve","scan":["**/*.log"],
      "patterns":[
        {"id":"pid","regexp":"parent_id=([0-9a-f]+)","extract":["pid"],
         "on_match":[
           {"action":"lookup","map":"spans","key":"$EXTRACT_PID",
            "on_hit":[{"action":"emit","data":{"child_at":"$FILE:$LINE","parent_at":"$LOOKUP_VALUE","parent_id":"$EXTRACT_PID"}}],
            "on_miss":[{"action":"emit","data":{"child_at":"$FILE:$LINE","parent_id":"$EXTRACT_PID","missing_parent":true}}]}]}]}
  ]
}'
```

**Why hprscript:** `phases` + the shared variable store turn a multi-pass workflow into a single invocation. No temp files, no shell glue.

---

# Security & DFIR

## 7. Secret & credential scanning

### 7.1 AWS access keys (one-line fast scan)

**Problem:** Sweep a tree for AWS access-key ID prefixes (`AKIA…`, `ASIA…` for STS). The tightest one-liner.

**Input:** Source tree, config dumps, log archives.

```bash
hprscript -p '\b(AKIA|ASIA)[0-9A-Z]{16}\b' -glob '**/*'
```

### 7.2 Multi-vendor key sweep — every match labelled with vendor

**Problem:** Modern secret sweeps need to cover AWS, GitHub, OpenAI, Slack, JWTs, GCP service-account keys, generic bearer tokens — six or more shapes. Lumping them into one pattern works but loses the vendor information; one pattern per vendor preserves it. The output then routes naturally: AWS goes to the AWS team, GitHub leaks go to the platform team, etc.

**Input:** Any text tree.

```bash
hprscript \
  -p '\b(AKIA|ASIA)[0-9A-Z]{16}\b'                           \
  -p '\bgh[pousr]_[A-Za-z0-9]{36,}\b'                        \
  -p '\bsk-[A-Za-z0-9]{32,}\b'                               \
  -p '\bxox[baprs]-[A-Za-z0-9-]{10,}\b'                      \
  -p '\beyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\b' \
  -format '$PAT_ID  $FILE:$LINE  $MATCH' \
  -glob '**/*'
# →  p0  config/aws.tf:3      AKIAIOSFODNN7EXAMPLE
# →  p1  README.md:42         ghp_abcdef1234567890...
# →  p2  test.py:7            sk-proj-abcdef1234567890...
# →  p4  jwt-debug.log:11     eyJhbGciOi...
```

**In script mode — name the IDs by vendor and route:**

```bash
hprscript -s '{
  "scan": ["**/*"],
  "patterns": [
    {"id":"aws",     "regexp":"\\b(AKIA|ASIA)[0-9A-Z]{16}\\b"},
    {"id":"github",  "regexp":"\\bgh[pousr]_[A-Za-z0-9]{36,}\\b"},
    {"id":"openai",  "regexp":"\\bsk-[A-Za-z0-9]{32,}\\b"},
    {"id":"slack",   "regexp":"\\bxox[baprs]-[A-Za-z0-9-]{10,}\\b"},
    {"id":"jwt",     "regexp":"\\beyJ[A-Za-z0-9_-]{10,}\\.[A-Za-z0-9_-]{10,}\\.[A-Za-z0-9_-]{10,}\\b"}
  ]
}'
# {"file":"config/aws.tf","pat":"aws","line":3, ...}
# {"file":"README.md","pat":"github","line":42, ...}
```

**Why hprscript:** Adding the next vendor (Stripe, Twilio, Datadog API keys) is one extra pattern object — same scan time, new bucket. The `pat` field handles routing automatically.

### 7.3 PEM private keys (full block extraction)

**Problem:** A PEM blob spans multiple lines. Line-based grep can find the `-----BEGIN ... PRIVATE KEY-----` header but misses the body. The `block` action walks from `-----BEGIN` to `-----END` and emits the whole key as one record — exactly what you need to revoke or rotate.

**Input:** Source tree, config dumps.

```bash
hprscript -s '{
  "scan": ["**/*"],
  "patterns": [
    {"id":"pem","regexp":"-----BEGIN [A-Z ]*PRIVATE KEY-----","on_match":[
      {"action":"block","open":"-----BEGIN","close":"-----END",
       "on_block":[{"action":"emit","data":{"file":"$FILE","line":"$LINE","key":"$BLOCK_FULL"}}]}]}
  ]
}'
```

**Why hprscript:** Multi-character delimiters in `block` (here `-----BEGIN` / `-----END`) make this one of the cleanest ways to pull any multi-line bracketed structure out of arbitrary text.

### 7.4 JWTs in logs and source

**Problem:** Find JWT-shaped strings (three base64 segments separated by dots).

**Input:** Logs or source.

```bash
hprscript -p '\beyJ[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\.[A-Za-z0-9_-]{10,}\b' \
  -o -glob '**/*'
```

### 7.5 Hardcoded passwords in source

**Problem:** Catch literal-string password assignments.

**Input:** Source tree.

```bash
hprscript -pi '(password|passwd|pwd|secret|api_key|apikey)\s*[:=]\s*["\x27][^"\x27]{4,}["\x27]' \
  -extract kind -format '$FILE:$LINE  [$EXTRACT_KIND]  $CONTEXT' \
  -glob '**/*.{py,js,ts,go,rs,java,rb,php,yaml,yml,env}' -exclude vendor -exclude node_modules
```

### 7.6 Inventory `.env`-style files containing real-looking values

**Problem:** Find env files where the values aren't placeholders or comments.

**Input:** Repo / config tree.

```bash
hprscript -p '^[A-Z][A-Z0-9_]+=[^\s\x23][^\s]{8,}$' \
  -f -glob '**/.env*' -glob '**/*.env'
```

---

## 8. IOC hunting

### 8.1 Known-bad IPs from a threat feed

**Problem:** Sweep logs for any IP in an IOC list. Hyperscan compiles thousands of literals into one DFA, so the whole feed runs in one pass — and `-patterns-from` takes the feed as *literals*, so there's no regex-escaping, no shell alternation-building, and no argv-length limit.

**Input:** Logs; IOC list as a file of IPs.

```bash
# One-time: wrap each IP in a JSONL pattern entry (literals are never
# regex-interpreted, so the dots need no escaping).
sed 's/.*/{"literal":"&"}/' iocs.txt > iocs.jsonl

hprscript -patterns-from iocs.jsonl -w -C 1 -glob '**/*.log'
```

### 8.2 Suspicious domains in DNS logs

**Problem:** Match log lines against a domain IOC list, case-insensitively.

**Input:** DNS query logs.

```bash
sed 's/.*/{"literal":"&","case_insensitive":true}/' bad-domains.txt > domains.jsonl
hprscript -patterns-from domains.jsonl -w -glob '**/dns*.log'
```

### 8.3 Hash IOCs — match the known-bad list directly

**Problem:** Find references to specific known-bad file hashes. The hash list itself becomes the pattern set — every hit is attributed to the exact IOC entry that fired, with no post-filtering pipe.

**Input:** Reports, logs, EDR exports; a file of hex hashes.

```bash
sed 's/.*/{"literal":"&","case_insensitive":true}/' known-bad-hashes.txt > hashes.jsonl
hprscript -patterns-from hashes.jsonl -w -format '$FILE:$LINE  $MATCH' -glob '**/*'
```

To inventory hash-*shaped* strings by family instead (unknown feed), name one pattern per length:

```bash
hprscript \
  -p '\b[0-9a-f]{32}\b' -name md5    \
  -p '\b[0-9a-f]{40}\b' -name sha1   \
  -p '\b[0-9a-f]{64}\b' -name sha256 \
  -format '$PAT_ID  $FILE:$LINE  $MATCH' -glob '**/*'
```

### 8.4 Tor exit-node patterns

**Problem:** Identify connections from known Tor exit IPs.

**Input:** Web/firewall logs + a Tor-exit list (`exits.txt`).

```bash
PAT=$(paste -sd'|' exits.txt | sed 's/\./\\./g')
hprscript -p "\\b($PAT)\\b" -C 0 -glob '**/access.log*'
```

### 8.5 Beaconing detection — periodic outbound from same IP

**Problem:** Same internal→external connection appearing on a regular cadence (proxy of beaconing). `-near A:A:K` finds matches that have a sibling within K lines — repeated calls from a beacon stand out.

**Input:** Proxy / netflow logs.

```bash
hprscript -p 'src=10\.0\.\d+\.\d+ dst=185\.220\.\d+\.\d+' \
  -near p0:p0:1 -glob '**/proxy*.log'
```

---

## 9. PII & sensitive-data discovery

### 9.1 PII multi-class sweep with per-class labelling

**Problem:** A privacy audit needs *every* PII type counted *and labelled* — "this file has 12 SSNs, 3 credit cards, 800 emails" is far more actionable than "this file has 815 PII hits." Each PII class becomes its own named pattern, and the same scan produces both a per-match labelled stream and a per-class total.

**Input:** Document tree.

```bash
hprscript -s '{
  "scan": ["**/*"],
  "variables": {"by_class":{"type":"map"}},
  "patterns": [
    {"id":"ssn",   "regexp":"\\b\\d{3}-\\d{2}-\\d{4}\\b",
     "on_match":[{"action":"emit"},{"action":"map_increment","target":"by_class","key":"ssn"}]},
    {"id":"cc",    "regexp":"\\b(?:\\d[ -]?){15,16}\\d\\b",
     "on_match":[{"action":"emit"},{"action":"map_increment","target":"by_class","key":"cc"}]},
    {"id":"email", "regexp":"\\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}\\b",
     "on_match":[{"action":"emit"},{"action":"map_increment","target":"by_class","key":"email"}]},
    {"id":"phone", "regexp":"\\+\\d{1,3}[ \\-]?\\d{3,4}[ \\-]?\\d{3,4}[ \\-]?\\d{3,4}",
     "on_match":[{"action":"emit"},{"action":"map_increment","target":"by_class","key":"phone"}]},
    {"id":"iban",  "regexp":"\\b[A-Z]{2}\\d{2}[A-Z0-9]{10,30}\\b",
     "on_match":[{"action":"emit"},{"action":"map_increment","target":"by_class","key":"iban"}]}
  ],
  "on_complete":[
    {"action":"for_each","var":"by_class","key_as":"c","as":"n","do":[
      {"action":"emit","data":{"summary":true,"pii_class":"$c","count":"$n"}}]}
  ]
}'
# Stream 1: per-match records with pat=ssn|cc|email|phone|iban
# Stream 2 (at end): {"summary":true,"pii_class":"email","count":1284} ...
```

### 9.2 Credit-card-shaped numbers (4-grouped)

**Problem:** Find 16-digit numbers grouped by 4. Luhn validation is a downstream step.

**Input:** Logs, dumps, source.

```bash
hprscript -p '\b(?:\d[ -]?){15,16}\d\b' -glob '**/*'
```

### 9.3 Email addresses

**Problem:** Inventory unique email addresses across a corpus.

**Input:** Any text.

```bash
hprscript -p '\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}\b' -o -glob '**/*' | sort -u
```

### 9.4 International phone numbers (E.164-ish)

**Problem:** Find phone-number-shaped strings.

**Input:** Document tree.

```bash
hprscript -p '\+\d{1,3}[ \-]?\(?\d{1,4}\)?[ \-]?\d{3,4}[ \-]?\d{3,4}\b' \
  -o -glob '**/*'
```

### 9.5 IBAN / SWIFT BIC

**Problem:** Identify European bank account numbers and bank identifier codes.

**Input:** Document tree.

```bash
hprscript \
  -p '\b[A-Z]{2}\d{2}[A-Z0-9]{4,30}\b'              \
  -p '\b[A-Z]{4}[A-Z]{2}[A-Z0-9]{2}([A-Z0-9]{3})?\b' \
  -format '$PAT_ID  $FILE:$LINE  $MATCH' -glob '**/*'
# →  p0 = IBAN    p1 = SWIFT/BIC
```

### 9.6 International names with diacritics — Unicode case-fold (UTF-8 demo)

**Problem:** GDPR-scope discovery in a multilingual corpus needs to match employee names regardless of case AND regardless of how diacritics are typed. `José` / `JOSÉ` / `josé` should all match a single search for `josé`. Hyperscan's UTF-8 case-fold (via `-pi`) handles this without preprocessing.

**Input:** Documents in mixed European languages.

```bash
hprscript \
  -pi 'andré' -pi 'jürgen' -pi 'françois' -pi 'mañana' -pi 'naïve' \
  -format '$PAT_ID  $FILE:$LINE  $MATCH' -glob '**/*.{txt,md,csv}'
# →  p0  notes.txt:4   André      ← case-folded
# →  p0  notes.txt:7   ANDRÉ      ← case-folded across diacritic
# →  p1  hr.csv:12     Jürgen
# →  p4  doc.md:3      Naïve
```

**Match Cyrillic case-folded:** the same recipe works for non-Latin scripts.

```bash
hprscript -pi 'москва' -pi 'санкт-петербург' -glob '**/*.txt'
# Matches "Москва", "МОСКВА", "москва" indifferently.
```

**Why hprscript:** UTF-8 mode is on by default; `-pi` folds case across every Unicode script. No iconv/normalize preprocessing, no character-class hacks.

---

## 10. Web shell & backdoor detection

### 10.1 PHP web shell signatures — co-occurrence on the same line

**Problem:** A PHP web shell isn't just an `eval` call or a `$_REQUEST` reference — it's both *together* on the same line. Three signatures (`eval`, `assert`, `base64_decode`) feeding user-controlled input (`$_REQUEST` etc.) is a much higher-confidence finding than any of them alone. `-near p0:p3:0` filters for "same line."

**Input:** Web root.

```bash
hprscript \
  -p 'eval\s*\('         \
  -p 'assert\s*\('       \
  -p 'base64_decode'     \
  -p '\$_(REQUEST|POST|GET|COOKIE)' \
  -near p0:p3:0 -near p1:p3:0 -near p2:p3:0 \
  -format '$PAT_ID  $FILE:$LINE  $CONTEXT' \
  -glob '/var/www/**/*.php'
```

### 10.2 ASP / ASPX backdoor patterns

**Problem:** Common ASPX backdoors invoke shells.

**Input:** IIS web root.

```bash
hprscript \
  -pi 'wscript\.shell' \
  -pi 'cmd\.exe'        \
  -pi 'powershell'      \
  -pi 'process\.start'  \
  -format '$PAT_ID  $FILE:$LINE  $CONTEXT' \
  -glob '**/*.{asp,aspx,ashx}'
```

### 10.3 JSP shell signatures

**Problem:** JSP files invoking `Runtime.getRuntime().exec(...)`.

**Input:** WAR / Tomcat webapps.

```bash
hprscript \
  -p 'Runtime\.getRuntime\(\)\.exec\(' \
  -p 'ProcessBuilder\('                 \
  -format '$PAT_ID  $FILE:$LINE  $CONTEXT' \
  -glob '**/*.jsp'
```

### 10.4 Suspicious obfuscation — long base64 blobs

**Problem:** Files with unusually long base64-looking string literals are suspect.

**Input:** Source tree.

```bash
hprscript -p '["\x27][A-Za-z0-9+/]{200,}={0,2}["\x27]' \
  -format '$FILE:$LINE  $MATCH' -glob '**/*.{php,js,py,sh}'
```

### 10.5 Recently modified files containing eval

**Problem:** Combine filesystem freshness with content match — recent change + dynamic-eval = suspicious.

**Input:** Web root.

```bash
find /var/www -type f -mtime -7 -name '*.php' -print0 | \
  xargs -0 hprscript -p 'eval\s*\(' -f
```

---

## 11. Pentest tool output parsing

### 11.1 Extract host:port pairs from nmap greppable output

**Problem:** Pull `host:port` strings out of `-oG` nmap reports.

**Input:** nmap `.gnmap` files.

```bash
hprscript -p 'Host: (\d+\.\d+\.\d+\.\d+) .*Ports:.*?(\d+)/open' \
  -extract ip,port \
  -format '$EXTRACT_IP:$EXTRACT_PORT' -glob '**/*.gnmap'
```

### 11.2 URLs and titles from gobuster output

**Problem:** Pull discovered paths and HTTP status from gobuster.

**Input:** gobuster text output.

```bash
hprscript -p '^(/\S+)\s+\(Status:\s*(\d+)\)\s+\[Size:\s*(\d+)\]' \
  -extract path,status,size \
  -format '$EXTRACT_STATUS  $EXTRACT_SIZE  $EXTRACT_PATH' -glob '**/gobuster-*.txt'
```

### 11.3 Nuclei findings classification by severity

**Problem:** Group nuclei findings by severity tag (`group_by` flushes one buffered batch per severity).

**Input:** nuclei text output.

```bash
hprscript -s '{
  "scan": ["**/nuclei-*.txt"],
  "group_by": "severity",
  "patterns": [
    {"id":"f","regexp":"\\[(critical|high|medium|low|info)\\]\\s+\\[(\\S+)\\]\\s+(\\S+)",
     "extract":["sev","template","target"],
     "on_match":[{"action":"emit","data":{"severity":"$EXTRACT_SEV","template":"$EXTRACT_TEMPLATE","target":"$EXTRACT_TARGET"}}]}
  ]
}'
```

### 11.4 Burp Suite issue export — extract findings

**Problem:** Pull issue title + URL pairs.

**Input:** Burp text export.

```bash
hprscript -p 'Issue:\s*(.+?)$' -p 'URL:\s*(https?://\S+)' \
  -near p0:p1:3 -glob '**/burp-export*.txt'
```

### 11.5 Use a SecLists wordlist as a multi-pattern feed

**Problem:** Search a target tree for any of N thousand interesting strings (default credentials, banned strings, etc.). One DFA, N patterns — adding more is cheap.

**Input:** SecLists `Common-Credentials/best110.txt` (or similar).

```bash
while IFS= read -r p; do printf -- '-p\n%s\n' "$p"; done < best110.txt | \
  xargs -d '\n' hprscript -glob '**/*'
```

---

## 12. Phishing email triage

### 12.1 Extract every URL from an email corpus

**Problem:** Inventory every link present in any received email.

**Input:** `.eml` / `.mbox` files.

```bash
hprscript -p 'https?://[^\s<>"\x27]+' -o -glob '**/*.{eml,mbox}' | sort -u
```

### 12.2 Display-name spoofing — one named pattern per impersonated brand

**Problem:** Phishing emails impersonate known brands in the `From:` display name (`From: "PayPal" <attacker@…>`). Naming the pattern per brand tells you *which* brand was being spoofed in each case — useful for routing the alert to the right takedown team.

**Input:** Mail dump.

```bash
hprscript \
  -pi '^From:\s*"?(PayPal)[^<]*<[^>]+>'    \
  -pi '^From:\s*"?(Amazon)[^<]*<[^>]+>'    \
  -pi '^From:\s*"?(Microsoft)[^<]*<[^>]+>' \
  -pi '^From:\s*"?(Apple)[^<]*<[^>]+>'     \
  -pi '^From:\s*"?(Google)[^<]*<[^>]+>'    \
  -pi '^From:\s*"?(IRS|HMRC)[^<]*<[^>]+>'  \
  -format '$PAT_ID  $FILE:$LINE  $MATCH' -glob '**/*.eml'
# →  p0  msg1.eml:1  From: "PayPal Service" <noreply@evil.tld>
# →  p3  msg7.eml:1  From: "Apple Support" <attacker@elsewhere>
```

### 12.3 SPF / DKIM / DMARC failures — split by mechanism

**Problem:** Auth-results headers report three independent checks (SPF, DKIM, DMARC). Each failure category points to a different policy gap, so the report should distinguish them.

**Input:** `.eml` files.

```bash
hprscript \
  -p '^Authentication-Results:.*\bspf=fail\b'   \
  -p '^Authentication-Results:.*\bdkim=fail\b'  \
  -p '^Authentication-Results:.*\bdmarc=fail\b' \
  -format '$PAT_ID  $FILE  $MATCH' -glob '**/*.eml'
```

### 12.4 IDN homograph attacks — Cyrillic letters in Latin-looking domains (UTF-8 demo)

**Problem:** Attackers register `pаypаl.com` where the `а` is a Cyrillic A (`U+0430`), looking identical to the Latin `a`. Plain ASCII regexes can't see this. Hyperscan's `\x{...}` codepoint escapes let you pinpoint Cyrillic letters embedded inside otherwise-Latin domain names — a strong homograph indicator.

**Input:** Email corpus, URL extracts, browser-history dumps.

```bash
hprscript -p 'https?://[a-zA-Z0-9.\-]*[\x{0400}-\x{04FF}][a-zA-Z0-9.\-]*' \
  -o -glob '**/*.eml' | sort -u
# Catches:
#   https://pаypаl.com           ← Cyrillic а
#   https://www.gооgle.com       ← Cyrillic о
#   https://app1e.cоm            ← Cyrillic о (and digit-1)
```

**Multi-script homograph sweep — one named pattern per script range:**

```bash
hprscript -s '{
  "scan": ["**/*.eml"],
  "patterns": [
    {"id":"cyrl","regexp":"https?://[a-zA-Z0-9.\\-]*[\\x{0400}-\\x{04FF}][a-zA-Z0-9.\\-]*"},
    {"id":"grek","regexp":"https?://[a-zA-Z0-9.\\-]*[\\x{0370}-\\x{03FF}][a-zA-Z0-9.\\-]*"},
    {"id":"han", "regexp":"https?://[a-zA-Z0-9.\\-]*[\\x{4E00}-\\x{9FFF}][a-zA-Z0-9.\\-]*"}
  ]
}'
# pat=cyrl → Cyrillic homoglyph
# pat=grek → Greek homoglyph (e.g. ο looks like Latin o)
# pat=han  → CJK confusable
```

**Why hprscript:** Hyperscan accepts `\x{NNNN}` codepoint escapes natively. Pure-ASCII tools simply cannot detect these attacks without first running the data through a normalization step.

### 12.5 Multilingual subject-line search (UTF-8 demo)

**Problem:** Find `urgent` / `срочно` / `緊急` / `urgent` (any case) across an international mbox. Three languages, one pattern per language with case-fold turned on.

**Input:** mbox / eml corpus.

```bash
hprscript \
  -pi '^Subject:.*\burgent\b'    \
  -pi '^Subject:.*срочно'        \
  -pi '^Subject:.*緊急'           \
  -pi '^Subject:.*\binvoice\b'   \
  -pi '^Subject:.*счет'          \
  -format '$PAT_ID  $FILE  $MATCH' -glob '**/*.{eml,mbox}'
```

### 12.6 Attachment MIME-type inventory

**Problem:** Which file types arrive as attachments?

**Input:** `.eml` corpus.

```bash
hprscript -p '^Content-Type:\s*(\S+/\S+)' -extract mt \
  -format '$EXTRACT_MT' -glob '**/*.eml' | sort | uniq -c | sort -rn
```

---

# Source code

## 13. SAST-lite security audit

### 13.1 SQL string concatenation

**Problem:** Catch likely SQL-injection sites — string concat with SQL keywords. `-scope auto` annotates each match with the enclosing function name.

**Input:** Source tree.

```bash
hprscript -pi '"\s*(select|insert|update|delete)\b.*"\s*\+' \
  -scope auto -glob '**/*.{java,py,js,ts,go,php,rb}'
```

### 13.2 Dynamic-eval sites — split by primitive

**Problem:** "Find every dynamic eval" is too coarse. The four primitives — `eval()`, `exec()`, `new Function()`, `setTimeout("…", 0)` — have *different* exploit profiles. Naming each one separately turns the report from a flat list into a triage queue.

**Input:** Source tree.

```bash
hprscript \
  -p '\beval\s*\('                  \
  -p '\bexec\s*\('                  \
  -p '\bnew Function\s*\('          \
  -p '\bsetTimeout\s*\(\s*["\x27]'  \
  -scope auto \
  -format '$PAT_ID  $FILE  $ENCLOSING_NAME:$LINE  $MATCH' \
  -glob '**/*.{js,ts,py,php}'
```

**In script mode — name and aggregate per primitive:**

```bash
hprscript -s '{
  "scan": ["**/*.{js,ts,py,php}"],
  "scope": "ts",
  "variables": {"by_kind":{"type":"map"}},
  "patterns": [
    {"id":"eval",         "regexp":"\\beval\\s*\\(",
     "on_match":[{"action":"emit"},{"action":"map_increment","target":"by_kind","key":"eval"}]},
    {"id":"exec",         "regexp":"\\bexec\\s*\\(",
     "on_match":[{"action":"emit"},{"action":"map_increment","target":"by_kind","key":"exec"}]},
    {"id":"new_function", "regexp":"\\bnew Function\\s*\\(",
     "on_match":[{"action":"emit"},{"action":"map_increment","target":"by_kind","key":"new_function"}]},
    {"id":"timeout_str",  "regexp":"\\bsetTimeout\\s*\\(\\s*[\"\\x27]",
     "on_match":[{"action":"emit"},{"action":"map_increment","target":"by_kind","key":"setTimeout_str"}]}
  ],
  "on_complete":[
    {"action":"for_each","var":"by_kind","key_as":"k","as":"n","do":[
      {"action":"emit","data":{"summary":true,"primitive":"$k","count":"$n"}}]}
  ]
}'
```

### 13.3 Weak crypto primitives — one named pattern per algorithm

**Problem:** A "weak crypto" finding sounds the same whether it's MD5 (mostly fine for non-security uses) or DES (always wrong). Splitting into named patterns lets the audit report prioritize: how many DES vs how many MD5.

**Input:** Source tree.

```bash
hprscript \
  -pi '\bmd5\s*\('  \
  -pi '\bsha1\s*\(' \
  -pi '\bdes\s*\('  \
  -pi '\brc4\s*\('  \
  -pi '\bmd4\s*\('  \
  -format '$PAT_ID  $FILE:$LINE  $MATCH' \
  -scope auto -glob '**/*.{java,py,js,ts,go,c,cpp,rb}'

# Per-algorithm count:
hprscript -pi '\bmd5\s*\(' -pi '\bsha1\s*\(' -pi '\bdes\s*\(' -pi '\brc4\s*\(' -pi '\bmd4\s*\(' \
  -glob '**/*.{java,py,js,ts,go,c,cpp,rb}' \
  | jq -r '.pat' | sort | uniq -c
```

### 13.4 Hardcoded secrets in code

**Problem:** Literal-string assignments to suspicious variable names.

**Input:** Source tree.

```bash
hprscript -pi '\b(api_?key|secret|token|password|passwd)\s*[:=]\s*["\x27][A-Za-z0-9_\-./+]{12,}["\x27]' \
  -extract kind -format '$FILE:$LINE  [$EXTRACT_KIND]  $CONTEXT' \
  -glob '**/*.{py,js,ts,go,java,rb,php,yaml,yml}' -exclude tests -exclude vendor
```

### 13.5 Unsafe deserialization — one named pattern per vector

**Problem:** Each unsafe-deser vector (`pickle.loads`, `yaml.load` without SafeLoader, `Marshal.load`, Java's `ObjectInputStream.readObject`) lives in a different language with different remediation. The named patterns produce a report that says "Python pickle: 4, Java native: 7" instead of an undifferentiated count.

**Input:** Source tree.

```bash
hprscript \
  -p 'pickle\.loads?\('                              \
  -p 'yaml\.load\s*\([^,)]*\)'                       \
  -p 'Marshal\.load'                                 \
  -p 'ObjectInputStream\(.+?\)\.readObject'          \
  -format '$PAT_ID  $FILE:$LINE  $MATCH' \
  -scope auto -glob '**/*.{py,rb,java}'
# →  p0  app/loader.py     pickle.loads(...)
# →  p1  conf/parse.py     yaml.load(f)
# →  p3  RpcServer.java    new ObjectInputStream(...).readObject()
```

### 13.6 Command injection — `shell=True` and friends

**Problem:** Surface subprocess calls with `shell=True` or `os.system(<concat>)`.

**Input:** Python source.

```bash
hprscript \
  -p 'subprocess\.\w+\([^)]*shell\s*=\s*True' \
  -p 'os\.system\([^)]*\+'                    \
  -p '\bpopen\s*\('                            \
  -glob '**/*.py'
```

(No `-scope` here: the built-in packs cover brace-delimited languages only, and Python's indentation-based scopes have no delimiter to anchor on.)

---

## 14. Code review & lint sweeps

### 14.1 TODO / FIXME / XXX with author tag

**Problem:** Inventory technical debt with author and message extracted via capture groups.

**Input:** Source tree.

```bash
hprscript -p '\b(TODO|FIXME|XXX|HACK)(\(([^)]+)\))?:?\s*(.*)$' \
  -extract kind,_paren,author,msg \
  -format '$FILE:$LINE  [$EXTRACT_KIND] author=$EXTRACT_AUTHOR  $EXTRACT_MSG' \
  -glob '**/*.{go,py,js,ts,java,rs,c,cpp,rb}'
```

### 14.2 Banned API call sites with enclosing function

**Problem:** Every call to a deprecated API, annotated with the function it's in. `-scope auto` handles language detection per file; `$ENCLOSING_NAME` becomes available in `-format`.

**Input:** Source tree.

```bash
hprscript -p '\boldDeprecatedCall\s*\(' -scope auto \
  -format '$FILE  $ENCLOSING_NAME:$LINE  $MATCH' \
  -glob '**/*.{go,rs,ts,js,java}'
```

### 14.3 `console.log` / `print` left in production code — without same-line allow markers

**Problem:** Catch debug prints not annotated with `// allow-console`. `-far A:B:0` is the inverse of `-near` on the same line: emit A only when B is *not* on the same line. Two named patterns make the recipe self-documenting.

**Input:** Source tree.

```bash
hprscript \
  -p '\bconsole\.log\s*\(' -name console \
  -p 'allow-console'       -name allow   \
  -far console:allow:0 \
  -glob '**/*.{js,ts}' -exclude tests -exclude '*.test.*'

hprscript \
  -p '\bprint\s*\(' -name print \
  -p 'allow-print'  -name allow \
  -far print:allow:0 \
  -glob '**/*.py' -exclude tests
```

### 14.4 Functions exceeding N lines

**Problem:** Inventory functions whose body spans more than 100 lines. `block` extracts the body; `$BLOCK_LINE_COUNT` gives its size in lines.

**Input:** Go source.

```bash
hprscript -s '{
  "scan": ["**/*.go"],
  "patterns": [
    {"id":"fn","regexp":"^func\\s+(?:\\([^)]*\\)\\s+)?\\w+","on_match":[
      {"action":"block","open":"{","close":"}","on_block":[
        {"action":"if","condition":{"op":"gt","args":["$BLOCK_LINE_COUNT",100]},
         "then":[{"action":"emit","data":{"file":"$FILE","sig":"$MATCH","start":"$LINE","end":"$BLOCK_LINE_END"}}]}]}]}
  ]
}'
```

### 14.5 Files with more than 10 TODOs

**Problem:** Spot files dense with deferred work. Per-file counter, conditional emit in `on_file_end`.

**Input:** Source tree.

```bash
hprscript -s '{
  "scan": ["**/*.{go,py,js,ts,java}"],
  "variables": {"n": {"type":"int"}},
  "patterns": [{"id":"t","regexp":"\\b(TODO|FIXME)\\b","on_match":[
    {"action":"increment","var":"n"}]}],
  "on_file_end":[
    {"action":"if","condition":{"op":"gt","args":["$n",10]},
     "then":[{"action":"emit","data":{"file":"$FILE","todos":"$n"}}]},
    {"action":"reset","vars":["n"]}
  ]
}'
```

### 14.6 Extract bodies of functions whose name contains a substring

**Problem:** Pull every function whose name *includes* a fragment — handy when you remember a piece of the name (`qualify`, `Validate`, `parse`) but not the full identifier, or when sweeping every `Parse*` / `Validate*` family at once. The signature regex matches by substring; `-block-open` / `-block-close` walks each match forward and grabs the balanced body, depth-tracked across nested braces.

**Input:** Source tree.

```bash
# Perl: every `sub *qualify*` — signature + full body per match.
# Worked target: the standard `Symbol.pm` module, which defines both
# `qualify` and `qualify_to_ref`.
hprscript -p 'sub\s+\w*qualify\w*' -block-open '{' -block-close '}' \
  -o perl/run/lib/5.36.1/Symbol.pm

# Go: every function or method whose name contains `Validate`.
hprscript -p 'func\s+(?:\([^)]*\)\s+)?\w*Validate\w*' \
  -block-open '{' -block-close '}' -o '**/*.go'
```

For a worked single-function extraction (with the depth-tracking walk-through on `\*{ ... }` derefs), see [HPRSCRIPT.md → Extract a single named function](HPRSCRIPT.md#extract-a-single-named-function).

### 14.7 Defined functions never called (set difference of two phases)

**Problem:** Phase 1 collects every function definition. Phase 2 collects every callsite. The unused set is the keys present in `defs` but not in `uses` — one `set_difference` action over the two maps, no `for_each + lookup + on_miss` boilerplate.

**Input:** Source tree.

```bash
hprscript -s '{
  "variables": {"defs":{"type":"map"}, "uses":{"type":"map"}},
  "phases": [
    {"id":"d","scan":["**/*.go"], "patterns":[
      {"id":"fn","regexp":"^func\\s+(\\w+)","extract":["n"],
       "on_match":[{"action":"map_set","target":"defs","key":"$EXTRACT_N","value":"$FILE:$LINE"}]}]},
    {"id":"u","scan":["**/*.go"], "patterns":[
      {"id":"call","regexp":"\\b(\\w+)\\s*\\(","extract":["n"],
       "on_match":[{"action":"map_set","target":"uses","key":"$EXTRACT_N","value":"1"}]}]}
  ],
  "on_complete":[
    {"action":"set_difference","target":"unused","a":"defs","b":"uses"},
    {"action":"for_each","var":"unused","as":"n","do":[
      {"action":"emit","data":{"unused":"$n"}}]}
  ]
}'
```

**Why hprscript:** Two phases share the variable store, and `set_difference` collapses what would otherwise be `for_each` + `lookup` with `on_miss` into one declarative action. `set_intersection` and `set_union` are useful in the same shape — e.g., "endpoints documented in OpenAPI but not implemented" is a `set_difference` of `documented` and `implemented` map keysets. Operands can be lists or maps; maps are coerced to their keysets, and the output `target` is a deduped, insertion-ordered list of strings.

### 14.8 Review only the changes on a branch

**Problem:** Lint the diff, not the repo — and ideally only the lines the branch *added*, so pre-existing debt doesn't drown the new findings.

**Input:** A git branch.

```bash
# Files changed on the branch (built-in git selection)
hprscript -git-range origin/main...HEAD \
  -p '\bconsole\.log\s*\(' -name console \
  -p '\bdebugger\b'        -name debugger \
  -pi 'TODO|FIXME'         -name todo \
  -llm

# Only matches on lines the branch ADDED — the true review question
hprscript -git-range origin/main...HEAD -git-added-lines \
  -p '\bconsole\.log\s*\(' -name console \
  -pi 'TODO|FIXME'         -name todo \
  -llm

# Uncommitted work: changed + untracked, added lines only
hprscript -git-changed -git-untracked -git-added-lines \
  -pi 'TODO|FIXME' -format '$FILE:$LINE  $MATCH'
```

**Why hprscript:** No pipeline, no `xargs`, no quoting hazards — hprscript asks git itself and treats the paths literally. `-git-added-lines` answers "did *this change* introduce it?" instead of "does the changed file contain it somewhere?". For non-git producers (`find -print0`, build manifests), `-files0-from -` / `-files-from list.txt` take the same role:

```bash
git diff --name-only -z --diff-filter=d origin/main...HEAD |
  hprscript -files0-from - -pi 'TODO|FIXME' -c
```

### 14.9 Locks without unlocks in the same function

**Problem:** A `Lock()` whose `Unlock()` lives in a *different* function is a leak candidate. Line distance can't express this — `-not-same-scope` checks structural containment: emit `lk` matches only when no `ul` match falls inside the same innermost enclosing function.

**Input:** Go source (any `-scope` pack or custom anchor works).

```bash
hprscript \
  -p '\bmu\.Lock\(\)'   -name lk \
  -p '\bmu\.Unlock\(\)' -name ul \
  -not-same-scope lk:ul \
  -scope go -llm -glob '**/*.go'
```

**Why hprscript:** The scope index built for `-scope` annotation doubles as the containment oracle, so "X without Y in the same function" is one flag instead of a script. The same shape finds handlers doing DB access without an authorization call (`-not-same-scope db:authz`), or `eval` in the same function as request input (`-same-scope ev:input`).

---

## 15. Migration scans

### 15.1 Every call site of a deprecated API

**Problem:** Find every place calling `OldClient.Do(...)`, with the enclosing function name attached.

**Input:** Source tree.

```bash
hprscript -p '\bOldClient\.Do\s*\(' -scope auto \
  -format '$FILE:$LINE  in $ENCLOSING_NAME  $MATCH' -glob '**/*.go'
```

### 15.2 Files importing the legacy module — ranked by usage intensity

**Problem:** Migration prioritization needs a "how badly does this file depend on the old thing" score, not a flat list. `rank` weights distinct patterns and emits one line per file with a relevance score.

**Input:** Source tree.

```bash
hprscript -s '{
  "scan": ["**/*.py"],
  "rank": true,
  "patterns": [
    {"id":"import","regexp":"^(from|import)\\s+legacy_module\\b","weight":3},
    {"id":"call","regexp":"\\blegacy_module\\.\\w+\\(","weight":1}
  ]
}'
# {"file":"app/core.py","score":2.41,"density":0.18,"matched_patterns":["import","call"]}
# {"file":"app/util.py","score":0.55,"density":0.04,"matched_patterns":["call"]}
```

### 15.3 Old API call sites with their argument lists

**Problem:** Pull the args of every `oldFunc(...)` call so you can preview replacement. `(`/`)` block extraction handles nested parens.

**Input:** Source tree.

```bash
hprscript -s '{
  "scan": ["**/*.go"],
  "patterns": [{"id":"call","regexp":"\\boldFunc\\s*","on_match":[
    {"action":"block","open":"(","close":")","on_block":[
      {"action":"emit","data":{"file":"$FILE","line":"$LINE","args":"$BLOCK"}}]}]}]
}'
```

### 15.4 Files using both old and new (in-progress migration)

**Problem:** Files where migration is half-done — flag for review.

**Input:** Source tree.

```bash
hprscript -s '{
  "scan": ["**/*.go"],
  "variables": {"old": {"type":"bool"}, "new": {"type":"bool"}},
  "patterns": [
    {"id":"o","regexp":"\\bOldClient\\b","on_match":[{"action":"set","var":"old","value":true}]},
    {"id":"n","regexp":"\\bNewClient\\b","on_match":[{"action":"set","var":"new","value":true}]}
  ],
  "on_file_end":[
    {"action":"if","condition":{"op":"and","args":[
      {"op":"eq","args":["$old",true]},{"op":"eq","args":["$new",true]}]},
     "then":[{"action":"emit","data":{"file":"$FILE","status":"mixed"}}]},
    {"action":"reset","vars":["old","new"]}
  ]
}'
```

### 15.5 Files NOT yet migrated (have old, lack new)

**Problem:** Migration coverage report — files still on the old API. Combines a positive pattern with an `absent` pattern in one scan.

**Input:** Source tree.

```bash
hprscript -s '{
  "scan": ["**/*.go"],
  "variables": {"has_old": {"type":"bool"}},
  "patterns": [
    {"id":"old","regexp":"\\bOldClient\\b","on_match":[
      {"action":"set","var":"has_old","value":true}]},
    {"id":"new","regexp":"\\bNewClient\\b","absent":true,"on_match":[
      {"action":"if","condition":{"op":"eq","args":["$has_old",true]},
       "then":[{"action":"emit","data":{"file":"$FILE","status":"old-only"}}]}]}
  ],
  "on_file_end":[{"action":"reset","vars":["has_old"]}]
}'
```

---

## 16. Dependency & license audit

### 16.1 Files missing a license header

**Problem:** List source files without `Copyright` or `SPDX-License-Identifier`. `-absent` flips matching: emit one record per file where the pattern is *not* found.

**Input:** Source tree.

```bash
hprscript -p 'Copyright|SPDX-License-Identifier' -absent \
  -glob '**/*.{c,cpp,h,hpp,go,rs,py,js,ts,java}'
```

### 16.2 Banned licenses in dependency manifests

**Problem:** Detect copyleft licenses that conflict with proprietary distribution.

**Input:** `package.json`, `Cargo.toml`, `requirements.txt`.

```bash
hprscript -pi '"license":\s*"(GPL|AGPL|LGPL|SSPL)' -extract lic \
  -format '$FILE  $EXTRACT_LIC' -glob '**/package.json'
hprscript -pi '^license\s*=\s*"(GPL|AGPL|LGPL|SSPL)' -extract lic \
  -format '$FILE  $EXTRACT_LIC' -glob '**/Cargo.toml'
```

*Which* family matters — AGPL and LGPL carry very different obligations — so don't let the alternation swallow the answer. The variant is already a capture group, so `-extract lic` surfaces it directly (cleaner here than splitting into four full `-pi` patterns, since they share the `"license":` prefix). If you'd rather have per-license match IDs, split instead: `-pi '"license":\s*"GPL' -pi '"license":\s*"AGPL'` … and read `$PAT_ID`.

### 16.3 Pinned vs floating versions

**Problem:** Identify dependencies pinned with `^` / `~` / `>=`.

**Input:** `package.json`, `requirements.txt`.

```bash
hprscript -p '"\S+":\s*"[\^~>=<]' -glob '**/package.json'
hprscript -p '^[A-Za-z0-9_.\-]+>=' -glob '**/requirements.txt'
```

### 16.4 SPDX identifier inventory

**Problem:** Count SPDX license tags across the tree.

**Input:** Source tree.

```bash
hprscript -p 'SPDX-License-Identifier:\s*(\S+)' -extract spdx \
  -format '$EXTRACT_SPDX' -glob '**/*' | sort | uniq -c | sort -rn
```

### 16.5 Vendored copies of known libraries

**Problem:** Detect vendored copies (e.g. `vendor/` contains copies of libs you depend on directly).

**Input:** Repo.

```bash
hprscript -p '^module\s+(\S+)' -extract mod \
  -format '$FILE  $EXTRACT_MOD' -glob 'vendor/**/go.mod'
```

---

# Configuration & Infrastructure

## 17. Container & cloud config audit

### 17.1 Dockerfile risks — one named pattern per anti-pattern

**Problem:** Dockerfile lints check four common anti-patterns: `:latest` base tag (reproducibility), `USER root` (privilege), `ADD https://...` (no integrity), and `curl … | sh` (supply-chain). Each finding has a different severity and a different fix; named patterns let the report group findings by issue type.

**Input:** Dockerfile tree.

```bash
hprscript \
  -pi '^FROM\s+\S+:latest\s*$'                  \
  -p  '^USER\s+(0|root)\s*$'                    \
  -pi '^ADD\s+https?://'                        \
  -pi '^RUN\s+.*\bcurl.*\|\s*(sh|bash)'         \
  -format '$PAT_ID  $FILE:$LINE  $MATCH' \
  -glob '**/Dockerfile*'
# →  p0  Dockerfile:1   FROM node:latest         (no version pinning)
# →  p1  Dockerfile:5   USER root                (running as root)
# →  p2  Dockerfile:8   ADD http://...           (no checksum)
# →  p3  Dockerfile:12  RUN curl ... | bash      (supply-chain risk)
```

**Per-issue counts:**

```bash
hprscript -pi '^FROM\s+\S+:latest\s*$' -p '^USER\s+(0|root)\s*$' \
          -pi '^ADD\s+https?://' -pi '^RUN\s+.*\bcurl.*\|\s*(sh|bash)' \
          -glob '**/Dockerfile*' \
  | jq -r '.pat' | sort | uniq -c
```

### 17.2 k8s — multi-issue manifest scan

**Problem:** k8s pod-spec audit covers five frequent issues, each with its own severity tier (privileged > hostPath > hostNetwork > allowPrivilegeEscalation > imagePullPolicy:Always). Naming each pattern lets the SOC dashboard count them per issue type.

**Input:** k8s YAML.

```bash
hprscript \
  -p 'privileged:\s*true'                  \
  -p 'allowPrivilegeEscalation:\s*true'    \
  -p 'hostPath:'                           \
  -p 'hostNetwork:\s*true'                 \
  -p 'imagePullPolicy:\s*Always'           \
  -format '$PAT_ID  $FILE:$LINE  $MATCH' \
  -glob '**/*.{yaml,yml}'
```

**Aggregate per issue type:**

```bash
hprscript -s '{
  "scan": ["**/*.{yaml,yml}"],
  "variables": {"by_issue":{"type":"map"}},
  "patterns": [
    {"id":"privileged","regexp":"privileged:\\s*true",
     "on_match":[{"action":"emit"},{"action":"map_increment","target":"by_issue","key":"privileged"}]},
    {"id":"esc","regexp":"allowPrivilegeEscalation:\\s*true",
     "on_match":[{"action":"emit"},{"action":"map_increment","target":"by_issue","key":"escalation"}]},
    {"id":"hostpath","regexp":"hostPath:",
     "on_match":[{"action":"emit"},{"action":"map_increment","target":"by_issue","key":"hostpath"}]},
    {"id":"hostnet","regexp":"hostNetwork:\\s*true",
     "on_match":[{"action":"emit"},{"action":"map_increment","target":"by_issue","key":"hostnet"}]},
    {"id":"pull","regexp":"imagePullPolicy:\\s*Always",
     "on_match":[{"action":"emit"},{"action":"map_increment","target":"by_issue","key":"pull_always"}]}
  ],
  "on_complete":[
    {"action":"for_each","var":"by_issue","key_as":"i","as":"n","do":[
      {"action":"emit","data":{"summary":true,"issue":"$i","count":"$n"}}]}
  ]
}'
```

### 17.3 Terraform — open security groups, public S3, ACL settings

**Problem:** Three high-impact Terraform issues — open SG ingress, public-read S3 ACL, public-block setting disabled. Each one is a single line in HCL and each produces a different alert ticket.

**Input:** `*.tf`.

```bash
hprscript \
  -p  'cidr_blocks\s*=\s*\[\s*"0\.0\.0\.0/0"' \
  -pi 'acl\s*=\s*"public-read(-write)?"'      \
  -p  'block_public_acls\s*=\s*false'         \
  -format '$PAT_ID  $FILE:$LINE  $MATCH' -glob '**/*.tf'
```

### 17.4 GitHub Actions — unpinned action versions

**Problem:** `uses:` lines pinned to a moving branch (`@main`, `@master`) instead of a tag/SHA — supply-chain risk.

**Input:** `.github/workflows/*.yml`.

```bash
hprscript -p '^\s*uses:\s+\S+@(main|master|latest)\s*$' \
  -glob '.github/workflows/*.{yml,yaml}'
```

### 17.5 docker-compose — services bound to 0.0.0.0

**Problem:** Public-bound ports in compose files.

**Input:** `docker-compose*.yml`.

```bash
hprscript -p '^\s*-\s*"?(0\.0\.0\.0|::):\d+:\d+' \
  -glob '**/docker-compose*.{yml,yaml}'
```

### 17.6 Cloud secrets pasted into YAML — by vendor

**Problem:** YAML `value:` fields containing literal-looking vendor secrets. Naming each pattern by vendor lets you route findings to the right rotation team.

**Input:** k8s / compose YAML.

```bash
hprscript \
  -pi 'value:\s+["\x27]?AKIA'      \
  -pi 'value:\s+["\x27]?gh[pousr]_' \
  -pi 'value:\s+["\x27]?xox[baprs]-' \
  -pi 'value:\s+["\x27]?sk-'        \
  -format '$PAT_ID  $FILE:$LINE  $MATCH' \
  -glob '**/*.{yaml,yml}'
# →  p0 = AWS    p1 = GitHub    p2 = Slack    p3 = OpenAI
```

---

## 18. Network & firewall config audit

### 18.1 iptables / nftables — accept any

**Problem:** Catch overly-permissive rules.

**Input:** `iptables-save` output, nftables config.

```bash
hprscript -pi '\-A INPUT.+\-j ACCEPT.+(\-s 0\.0\.0\.0/0|\bany\b)' \
  -glob '**/*iptables*'

hprscript -pi 'accept\s+(comment|;|$)' -glob '**/*.nft'
```

### 18.2 nginx upstream proxies without TLS

**Problem:** `proxy_pass http://...` (not https) in production configs.

**Input:** nginx config tree.

```bash
hprscript -p 'proxy_pass\s+http://' -glob '/etc/nginx/**/*.conf'
```

### 18.3 Apache — directory indexing or AllowOverride All

**Problem:** Two distinct misconfigurations; named patterns separate them.

**Input:** Apache config tree.

```bash
hprscript \
  -pi '^\s*Options[^#]*\bIndexes\b' \
  -p  '^\s*AllowOverride\s+All'     \
  -format '$PAT_ID  $FILE:$LINE  $MATCH' \
  -glob '/etc/apache2/**/*.conf' -glob '/etc/httpd/**/*.conf'
```

### 18.4 SSH config audit — named pattern per setting

**Problem:** sshd hardening checks four independent settings: PermitRootLogin, PasswordAuthentication, PermitEmptyPasswords, Protocol 1. The remediation is different for each. Named patterns turn the output into a checklist where each line says exactly which setting tripped.

**Input:** sshd_config files.

```bash
hprscript \
  -p  '^\s*PermitRootLogin\s+yes'       \
  -p  '^\s*PasswordAuthentication\s+yes' \
  -p  '^\s*PermitEmptyPasswords\s+yes'  \
  -pi '^\s*Protocol\s+1'                \
  -format '$PAT_ID  $FILE:$LINE  $MATCH' \
  -glob '**/sshd_config*'
# →  p0  /etc/ssh/sshd_config:24  PermitRootLogin yes
# →  p1  /etc/ssh/sshd_config:31  PasswordAuthentication yes
# →  p3  /etc/ssh/sshd_config:7   Protocol 1
```

**Use the IDs — pass/fail report per setting:**

```bash
hprscript -p '^\s*PermitRootLogin\s+yes' \
          -p '^\s*PasswordAuthentication\s+yes' \
          -p '^\s*PermitEmptyPasswords\s+yes' \
          -pi '^\s*Protocol\s+1' \
          -glob '**/sshd_config*' \
  | jq -r '"\(.pat)  \(.file)"' | sort -u
```

### 18.5 WireGuard / OpenVPN private key extraction

**Problem:** Inventory private keys in VPN configs (audit only). For OpenVPN, `<key>...</key>` is a multi-line block — `block` extracts it whole.

**Input:** Config tree.

```bash
hprscript -p '^\s*PrivateKey\s*=\s*[A-Za-z0-9+/=]{40,}' \
  -glob '**/wg*.conf'

hprscript -s '{
  "scan": ["**/*.ovpn"],
  "patterns": [{"id":"k","regexp":"<key>","on_match":[
    {"action":"block","open":"<key>","close":"</key>",
     "on_block":[{"action":"emit","data":{"file":"$FILE","key":"$BLOCK_FULL"}}]}]}]
}'
```

---

## 19. DNS, BGP & routing config

### 19.1 DNS zone files — missing SPF / DMARC

**Problem:** Zones without an SPF or DMARC TXT record. `-absent` flags files where the pattern doesn't appear at all.

**Input:** BIND zone files.

```bash
hprscript -pi 'v=spf1' -absent -glob '**/db.*'
hprscript -pi '_dmarc.+v=DMARC1' -absent -glob '**/db.*'
```

### 19.2 Wildcard A records

**Problem:** Find risky wildcard `*` A records.

**Input:** Zone files.

```bash
hprscript -p '^\*\s+\S+\s+IN\s+A\s+\d+\.\d+\.\d+\.\d+' -glob '**/db.*'
```

### 19.3 BGP — bogon prefixes split by RFC class

**Problem:** Bogons fall into named buckets (RFC1918 private, loopback, link-local, multicast). Naming each pattern reports the bogon class so the analyst knows whether it's an internal config leak or a multicast misroute.

**Input:** BGP table dumps.

```bash
hprscript \
  -p '\b10\.\d+\.\d+\.\d+/'                            \
  -p '\b172\.(1[6-9]|2\d|3[01])\.\d+\.\d+/'             \
  -p '\b192\.168\.\d+\.\d+/'                            \
  -p '\b127\.\d+\.\d+\.\d+/'                            \
  -p '\b22[4-9]\.\d+\.\d+\.\d+/'                        \
  -format '$PAT_ID  $FILE:$LINE  $MATCH' \
  -glob '**/bgp-table*.txt'
# →  p0 = RFC1918/8    p1 = RFC1918/12    p2 = RFC1918/16
# →  p3 = loopback     p4 = multicast
```

### 19.4 Cisco / JunOS — open SNMP communities

**Problem:** Default `public` / `private` SNMP communities.

**Input:** Network device configs.

```bash
hprscript -pi '^snmp-server community (public|private)\b' \
  -glob '**/*.{cfg,conf}'
```

### 19.5 BIND zone — SOA + NS extraction

**Problem:** Inventory authoritative servers per zone.

**Input:** BIND zone files.

```bash
hprscript -p '^@?\s+IN\s+(SOA|NS)\s+\S+' \
  -format '$FILE  $MATCH' -glob '**/db.*'
```

---

# Data wrangling

## 20. CSV / TSV / fixed-width records

### 20.1 Find CSV rows with the wrong field count

**Problem:** Lines that don't have exactly 6 commas (= 7 fields). Express the *correct* shape as one pattern and flag every row that lacks it with record-level absence — one call catches both "too few" and "too many". (A direct too-few pattern like `^[^,\n]*(,[^,\n]*){0,5}$` can match an empty line, which Vectorscan rejects: "Start of match is not currently supported for patterns which match an empty buffer".)

**Input:** CSV file.

```bash
hprscript -p '^([^,\n]*,){6}[^,\n]*$' -absent -records line -glob '**/*.csv'
# → {"file":"data.csv","pat":"p0","line":3,"record":"1,2,3"}    ← wrong field count
```

### 20.2 Extract a specific column with `-extract`

**Problem:** Pull the 3rd column from a CSV.

**Input:** Comma-separated, no embedded commas/quotes.

```bash
hprscript -p '^([^,]*),([^,]*),([^,]*),' -extract c1,c2,c3 \
  -format '$EXTRACT_C3' -glob '**/*.csv'
```

### 20.3 Numeric threshold filter on a CSV column

**Problem:** Rows where the 4th numeric column exceeds 1000.

**Input:** CSV.

```bash
hprscript -p '^[^,]*,[^,]*,[^,]*,([1-9]\d{3,}|[1-9]\d*\.\d+e\+0[3-9])' \
  -glob '**/*.csv'
```

### 20.4 Mojibake / encoding-issue detection

**Problem:** Find rows containing common UTF-8-double-encoded artefacts (`Ã©`, `â€™`).

**Input:** Any text file.

```bash
hprscript -p 'Ã[©¨ª¢¤¶]|â€[™œžs]' -glob '**/*.{csv,tsv,txt}'
```

### 20.5 Detect mixed delimiters

**Problem:** Files containing both commas and semicolons in record separators — a sign of inconsistent export.

**Input:** Suspect CSV.

```bash
hprscript -s '{
  "scan": ["**/*.csv"],
  "variables": {"comma": {"type":"bool"}, "semi": {"type":"bool"}},
  "patterns": [
    {"id":"c","regexp":",","on_match":[{"action":"set","var":"comma","value":true}]},
    {"id":"s","regexp":";","on_match":[{"action":"set","var":"semi","value":true}]}
  ],
  "on_file_end":[
    {"action":"if","condition":{"op":"and","args":[
      {"op":"eq","args":["$comma",true]},{"op":"eq","args":["$semi",true]}]},
     "then":[{"action":"emit","data":{"file":"$FILE","mixed_delimiters":true}}]},
    {"action":"reset","vars":["comma","semi"]}
  ]
}'
```

### 20.6 Fixed-width record extraction

**Problem:** Pull cols 1-10 (id), 11-30 (name), 31-40 (amount) from a fixed-width file.

**Input:** Fixed-width records.

```bash
hprscript -p '^(.{10})(.{20})(.{10})' -extract id,name,amount \
  -format 'id=$EXTRACT_ID name=$EXTRACT_NAME amount=$EXTRACT_AMOUNT' \
  -glob '**/*.dat'
```

---

## 21. JSON Lines & XML triage

### 21.1 JSONL records missing a required field

**Problem:** Find the individual JSONL records that lack `"user_id"`. Plain `-absent` fires only when a whole *file* contains no `"user_id"` anywhere; `-records line` refines it to record granularity — one JSON record per line missing the field:

**Input:** `*.jsonl`.

```bash
hprscript -p '"user_id"' -absent -records line -glob '**/*.jsonl'
# → {"file":"data.jsonl","pat":"p0","line":2,"record":"{\"other\": 2}"}
```

The script-mode equivalent (useful when the check is part of a larger script) matches every record line and uses `submatch` with an absent sub-pattern:

```bash
hprscript -s '{
  "scan": ["**/*.jsonl"],
  "patterns": [{"id":"rec","regexp":"^\\{.*$","on_match":[
    {"action":"submatch","patterns":[
      {"id":"m","regexp":"\"user_id\"","absent":true,"on_match":[
        {"action":"emit","data":{"file":"$FILE","line":"$LINE","missing":"user_id"}}]}]}]}]
}'
```

For the coarser question — which *files* have no `"user_id"` at all — plain `-absent` remains the right primitive:

```bash
hprscript -p '"user_id"' -absent -glob '**/*.jsonl'
```

### 21.2 Extract one JSON field across files

**Problem:** Pull every `"order_id":"..."` value from a stream.

**Input:** JSONL.

```bash
hprscript -p '"order_id"\s*:\s*"([^"]+)"' -extract id \
  -format '$EXTRACT_ID' -glob '**/*.jsonl'
```

### 21.3 XML element counts per file

**Problem:** Count occurrences of `<row>` per file.

**Input:** XML tree.

```bash
hprscript -p '<row\b' -c -glob '**/*.xml'
```

### 21.4 XML attribute audit

**Problem:** Find every distinct `xmlns:*` namespace declaration.

**Input:** XML tree.

```bash
hprscript -p 'xmlns:(\w+)="([^"]+)"' -extract pfx,uri \
  -format '$EXTRACT_PFX -> $EXTRACT_URI' -glob '**/*.xml' | sort -u
```

### 21.5 JSON config drift across environments — one named pattern per environment

**Problem:** You run `dev`, `staging`, `prod` configs and want to spot drift in `feature_X`. Each file goes to its own named pattern via the file pattern, but here we use a single regex that captures the value, then group by environment from the path.

**Input:** Per-env config files (`config.dev.json`, `config.staging.json`, etc.).

```bash
hprscript -p '"feature_X"\s*:\s*("[^"]*"|true|false|\d+)' -extract val \
  -format '$FILE  $EXTRACT_VAL' -glob '**/config.*.json'
# config.dev.json      true
# config.staging.json  true
# config.prod.json     false   ← drift!
```

---

## 22. Web scraping & HTML extraction

### 22.1 Every `<a href="">` from a page

**Problem:** Pull links from an HTML dump.

**Input:** Stdin or HTML files.

```bash
curl -s https://example.com | hprscript -p 'href="([^"]+)"' -extract url \
  -format '$EXTRACT_URL'
```

### 22.2 OpenGraph meta tags

**Problem:** Scrape `og:*` properties.

**Input:** HTML files.

```bash
hprscript -p '<meta\s+property="og:(\w+)"\s+content="([^"]+)"' \
  -extract prop,val \
  -format '$EXTRACT_PROP=$EXTRACT_VAL' -glob '**/*.html'
```

### 22.3 mailto: harvesting

**Problem:** Inventory email addresses linked from pages.

**Input:** HTML tree.

```bash
hprscript -p 'mailto:([^"\x27>?]+)' -extract addr \
  -format '$EXTRACT_ADDR' -glob '**/*.html' | sort -u
```

### 22.4 JSON-LD structured data extraction

**Problem:** Pull every `<script type="application/ld+json">{...}</script>` payload. The script tag opens; the JSON object inside is delimited by `{...}` — the `block` action walks balanced braces.

**Input:** HTML files.

```bash
hprscript -s '{
  "scan": ["**/*.html"],
  "patterns": [{"id":"ld","regexp":"<script[^>]*application/ld\\+json[^>]*>","on_match":[
    {"action":"block","open":"{","close":"}",
     "on_block":[{"action":"emit","data":{"file":"$FILE","line":"$LINE","jsonld":"$BLOCK"}}]}]}]
}'
```

### 22.5 RSS / Atom item extraction

**Problem:** Pull every `<item>...</item>` block with multi-character delimiters.

**Input:** Feed files.

```bash
hprscript -s '{
  "scan": ["**/*.{rss,xml,atom}"],
  "patterns": [{"id":"item","regexp":"<item\\b","on_match":[
    {"action":"block","open":"<item","close":"</item>",
     "on_block":[{"action":"emit","data":{"file":"$FILE","line":"$LINE","item":"$BLOCK_FULL"}}]}]}]
}'
```

### 22.6 Non-Latin titles — `ucp` for cross-script `\w+` (UTF-8 demo)

**Problem:** Extract every `<title>...</title>` from a multilingual page tree (Chinese/Japanese/Korean/Russian content). The default `\w` is ASCII-only; turn on `ucp` and `\w+` covers Han, Hiragana, Cyrillic, etc.

**Input:** Multilingual HTML.

```bash
hprscript -s '{
  "scan": ["**/*.html"],
  "patterns": [
    {"id":"title","ucp":true,"regexp":"<title>([\\w\\s\\p{P}]+)</title>",
     "extract":["t"],
     "on_match":[{"action":"emit","data":{"file":"$FILE","title":"$EXTRACT_T"}}]}
  ]
}'
# {"file":"about.html","title":"关于我们"}
# {"file":"ru.html","title":"О компании"}
# {"file":"jp.html","title":"会社概要"}
```

**Why hprscript:** `ucp: true` flips `\w`/`\d`/`\s` from ASCII-only to all-Unicode, so a single regex covers every script. Without `ucp`, the same regex stops at the first non-Latin codepoint.

---

## 23. Email corpus mining (.eml / .mbox)

### 23.1 Subject-line inventory

**Problem:** List every Subject across an mbox.

**Input:** mbox / eml.

```bash
hprscript -p '^Subject:\s*(.+)$' -extract subj \
  -format '$EXTRACT_SUBJ' -glob '**/*.{eml,mbox}'
```

### 23.2 From / To frequency

**Problem:** Count messages per sender.

**Input:** mbox.

```bash
hprscript -s '{
  "scan": ["**/*.mbox","**/*.eml"],
  "variables": {"by_sender": {"type":"map"}},
  "patterns": [
    {"id":"f","regexp":"^From:\\s+.*<([^>]+)>","extract":["addr"],
     "on_match":[{"action":"map_increment","target":"by_sender","key":"$EXTRACT_ADDR"}]}
  ],
  "on_complete":[
    {"action":"for_each","var":"by_sender","key_as":"a","as":"n","do":[
      {"action":"emit","data":{"sender":"$a","count":"$n"}}]}
  ]
}'
```

### 23.3 Emails with attachments

**Problem:** Find messages whose MIME structure includes attachments.

**Input:** eml tree.

```bash
hprscript -p 'Content-Disposition:\s*attachment;\s*filename="([^"]+)"' \
  -extract fname -format '$FILE  $EXTRACT_FNAME' -glob '**/*.eml'
```

### 23.4 Risky MIME types — separated for routing

**Problem:** Different attachment MIME types map to different policies (executables get blocked outright; archives get scanned). Named patterns separate the alert categories.

**Input:** eml corpus.

```bash
hprscript \
  -pi 'Content-Type:\s*application/(x-msdownload|x-msdos-program)' \
  -pi 'Content-Type:\s*application/(zip|x-rar|x-7z|x-tar|gzip)'    \
  -pi 'Content-Type:\s*application/(java-archive|x-shockwave-flash)' \
  -format '$PAT_ID  $FILE  $MATCH' -glob '**/*.eml'
# →  p0 = executables    p1 = archives    p2 = legacy/risky
```

### 23.5 Multilingual full-text search across mbox subjects (UTF-8 demo)

**Problem:** A compliance officer needs to find emails about "invoices" or "contracts" in any language. Hyperscan's case-fold works across scripts; one named pattern per language keyword.

**Input:** International mbox.

```bash
hprscript \
  -pi '^Subject:.*\binvoice\b'   \
  -pi '^Subject:.*\bfactura\b'   \
  -pi '^Subject:.*\bfacture\b'   \
  -pi '^Subject:.*счет'          \
  -pi '^Subject:.*請求書'         \
  -pi '^Subject:.*\bcontract\b'  \
  -pi '^Subject:.*контракт'      \
  -pi '^Subject:.*契約'           \
  -format '$PAT_ID  $FILE  $MATCH' -glob '**/*.{eml,mbox}'
```

**Why hprscript:** Adding the next language is one extra `-pi` flag — no character-set conversion, no per-language tooling.

### 23.6 Message-thread reconstruction (Message-ID + In-Reply-To, two phases)

**Problem:** For each message, link it to its parent via headers. Phase 1 indexes Message-IDs; phase 2 resolves In-Reply-To.

**Input:** eml corpus.

```bash
hprscript -s '{
  "variables": {"msg_files": {"type":"map"}},
  "phases": [
    {"id":"index","scan":["**/*.eml"],
      "patterns":[
        {"id":"mid","regexp":"^Message-ID:\\s*<([^>]+)>","extract":["m"],
         "on_match":[{"action":"map_set","target":"msg_files","key":"$EXTRACT_M","value":"$FILE"}]}]},
    {"id":"link","scan":["**/*.eml"],
      "patterns":[
        {"id":"reply","regexp":"^In-Reply-To:\\s*<([^>]+)>","extract":["p"],
         "on_match":[
           {"action":"lookup","map":"msg_files","key":"$EXTRACT_P",
            "on_hit":[{"action":"emit","data":{"child":"$FILE","parent":"$LOOKUP_VALUE","parent_id":"$EXTRACT_P"}}],
            "on_miss":[{"action":"emit","data":{"child":"$FILE","parent_id":"$EXTRACT_P","missing":true}}]}]}]}
  ]
}'
```

---

# Specialized verticals

## 24. Bioinformatics

### 24.1 Sequences matching a motif (FASTA)

**Problem:** Find FASTA sequences containing motif `GAATTC` (EcoRI restriction site).

**Input:** FASTA files.

```bash
hprscript -p '^>(\S+).*\n[ACGTN]*GAATTC[ACGTN]*' \
  -extract id -format '$FILE  $EXTRACT_ID' -glob '**/*.{fa,fasta,fna}'
```

### 24.2 Count sequences per FASTA file

**Problem:** Number of `>` headers per file.

**Input:** FASTA tree.

```bash
hprscript -p '^>' -c -glob '**/*.{fa,fasta,fna}'
```

### 24.3 FASTQ read-quality header parse

**Problem:** Extract the read ID from each FASTQ record.

**Input:** FASTQ files.

```bash
hprscript -p '^@(\S+)\s' -extract rid \
  -format '$EXTRACT_RID' -glob '**/*.{fq,fastq}'
```

### 24.4 VCF — variants in a target gene

**Problem:** Filter a VCF for variants annotated in `BRCA1`.

**Input:** VCF file (text format).

```bash
hprscript -p '^[^\x23].*\bBRCA1\b' -glob '**/*.vcf'
```

### 24.5 GFF / GTF — extract `gene_id` values

**Problem:** Inventory genes annotated in a GTF.

**Input:** GTF/GFF.

```bash
hprscript -p 'gene_id\s+"([^"]+)"' -extract gid \
  -format '$EXTRACT_GID' -glob '**/*.{gtf,gff,gff3}' | sort -u
```

### 24.6 BED — regions on a specific chromosome

**Problem:** Pull chr17 regions only.

**Input:** BED file.

```bash
hprscript -p '^chr17\t(\d+)\t(\d+)\b' -extract start,end \
  -format 'chr17  $EXTRACT_START-$EXTRACT_END' -glob '**/*.bed'
```

---

## 25. Finance — SWIFT / FIX / market data

### 25.1 SWIFT MT message-type inventory — by named MT class

**Problem:** SWIFT MT messages come in classes (MT1xx = customer transfers, MT2xx = financial-institution transfers, MT9xx = cash management). Naming each class lets the report distinguish customer flow from interbank flow.

**Input:** SWIFT MT text files.

```bash
hprscript \
  -p '\{2:[IO]1\d{2}'  \
  -p '\{2:[IO]2\d{2}'  \
  -p '\{2:[IO]9\d{2}'  \
  -format '$PAT_ID  $FILE:$LINE  $MATCH' \
  -glob '**/*.swt'
# →  p0 = customer transfer    p1 = interbank    p2 = cash mgmt
```

### 25.2 FIX protocol — extract specific tags

**Problem:** Pull `35=` (msgType), `49=` (sender), `56=` (target) from FIX logs.

**Input:** FIX log.

```bash
hprscript -p '\b35=([A-Z0-9]+)\b' -extract msgtype \
  -format '$EXTRACT_MSGTYPE' -glob '**/fix-*.log'
```

### 25.3 Trade log scan by symbol

**Problem:** Pull every trade involving `AAPL`, side, qty, price.

**Input:** Trade-blotter logs.

```bash
hprscript -p '\bAAPL\b.*\b(BUY|SELL)\b.*\b(\d+)\s*@\s*([\d.]+)' \
  -extract side,qty,px \
  -format '$FILE:$LINE  $EXTRACT_SIDE  $EXTRACT_QTY @ $EXTRACT_PX' \
  -glob '**/trades*.log'
```

### 25.4 ISIN vs CUSIP — separated by identifier kind

**Problem:** ISIN (12 chars, country prefix) and CUSIP (9 chars) often appear in the same documents but mean different things. Two named patterns produce a labelled inventory.

**Input:** Documents, statements, exports.

```bash
hprscript \
  -p '\b[A-Z]{2}[A-Z0-9]{9}\d\b' \
  -p '\b[0-9A-Z]{8}\d\b'         \
  -format '$PAT_ID  $FILE:$LINE  $MATCH' \
  -glob '**/*.{txt,csv,pdf.txt}'
# →  p0 = ISIN    p1 = CUSIP
```

### 25.5 Suspicious round-number trades

**Problem:** Trades with quantities at exact round multiples of 100,000 (potential structuring or block-trade).

**Input:** Trade logs.

```bash
hprscript -p '\bQTY=([1-9]00000(?:000)?)\b' \
  -extract q -format '$FILE:$LINE  qty=$EXTRACT_Q' -glob '**/trades*.log'
```

### 25.6 Currency-pair quote extraction

**Problem:** Pull every `EURUSD=1.0823`-style quote.

**Input:** Market-data feed dump.

```bash
hprscript -p '\b([A-Z]{3})([A-Z]{3})=([\d.]+)\b' -extract base,quote,px \
  -format '$EXTRACT_BASE/$EXTRACT_QUOTE  $EXTRACT_PX' -glob '**/md-*.log'
```

---

## 26. Legal & contracts

### 26.1 Defined terms (Capitalized "Defined Term" in quotes)

**Problem:** Pull definitions of the form `"Service" means ...`.

**Input:** Contract text.

```bash
hprscript -p '"([A-Z][A-Za-z ]+)"\s+(?:means|shall mean|refers to)\b' \
  -extract term -format '$FILE:$LINE  $EXTRACT_TERM' -glob '**/*.{txt,md}'
```

### 26.2 Section cross-references

**Problem:** Identify every `Section 3.4(b)`-style reference.

**Input:** Contract text.

```bash
hprscript -pi '\b(Section|Article|Clause)\s+\d+(\.\d+)*(\([a-z]+\))?\b' \
  -o -glob '**/*.txt' | sort -u
```

### 26.3 Date extraction — labelled by format

**Problem:** Contracts mix three date formats: ISO (`2024-01-15`), US (`1/15/2024`), and long-form English (`January 15, 2024`). Naming each pattern by format makes downstream normalization straightforward — you know what to feed each value into.

**Input:** Documents.

```bash
hprscript \
  -p '\b\d{4}-\d{2}-\d{2}\b'                              \
  -p '\b\d{1,2}/\d{1,2}/\d{2,4}\b'                        \
  -p '\b(?:Jan(?:uary)?|Feb(?:ruary)?|Mar(?:ch)?|Apr(?:il)?|May|Jun(?:e)?|Jul(?:y)?|Aug(?:ust)?|Sep(?:tember)?|Oct(?:ober)?|Nov(?:ember)?|Dec(?:ember)?)\s+\d{1,2},?\s+\d{4}\b' \
  -format '$PAT_ID  $FILE:$LINE  $MATCH' \
  -glob '**/*.txt'
# →  p0 = ISO 8601    p1 = US numeric    p2 = long-form English
```

**Use the IDs to dispatch parsers:**

```bash
# pat=p0 → feed to date -d
# pat=p1 → feed to date -d (locale-sensitive!)
# pat=p2 → feed to a long-form parser (e.g. python dateutil)
```

### 26.4 Party / signatory blocks

**Problem:** Pull "By: ___" signature blocks with their following lines (`context_after`).

**Input:** Contract text.

```bash
hprscript -s '{
  "scan": ["**/*.txt"],
  "context_after": 4,
  "patterns": [{"id":"sig","regexp":"^By:\\s*_+\\s*$","on_match":[
    {"action":"emit","data":{"file":"$FILE","line":"$LINE","block":"$CONTEXT_AFTER"}}]}]
}'
```

### 26.5 Clause-type detection — labelled

**Problem:** Locate canonical clause headers; the named patterns let the output route each clause to its review template.

**Input:** Contracts.

```bash
hprscript \
  -pi '^\s*(\d+\.\s+)?Limitation of Liability\b' \
  -pi '^\s*(\d+\.\s+)?Indemnification\b'         \
  -pi '^\s*(\d+\.\s+)?Governing Law\b'           \
  -pi '^\s*(\d+\.\s+)?Confidentiality\b'         \
  -pi '^\s*(\d+\.\s+)?Termination\b'             \
  -pi '^\s*(\d+\.\s+)?Force Majeure\b'           \
  -format '$PAT_ID  $FILE:$LINE  $MATCH' \
  -glob '**/*.{txt,md}'
```

---

## 27. NLP & corpus linguistics

### 27.1 Citation extraction (author-year)

**Problem:** Pull `(Author, 2023)` and `(Author et al., 2023)` citations.

**Input:** Academic text.

```bash
hprscript -p '\(([A-Z][a-z]+(?:\s+(?:&|and)\s+[A-Z][a-z]+|\s+et\s+al\.?)?),\s*(\d{4})[a-z]?\)' \
  -extract authors,year \
  -format '$EXTRACT_AUTHORS  $EXTRACT_YEAR' -glob '**/*.{txt,md,tex}'
```

### 27.2 Quoted speech extraction

**Problem:** Pull every `"..."` span longer than ~4 words.

**Input:** Prose text.

```bash
hprscript -p '"[^"]{20,}"' -o -glob '**/*.txt'
```

### 27.3 Multi-script detection — UTF-8 in action

**Problem:** Identify documents that mix Latin, Cyrillic, CJK, and Arabic — useful for translation-pipeline routing and code-switching analysis. Each script range is its own named pattern; per-file presence flags get emitted at file end.

**Input:** Document tree.

```bash
hprscript -s '{
  "scan": ["**/*.txt"],
  "variables": {"latin":{"type":"bool"},"cyrl":{"type":"bool"},"cjk":{"type":"bool"},"arab":{"type":"bool"}},
  "patterns": [
    {"id":"latin","regexp":"[A-Za-z]","on_match":[{"action":"set","var":"latin","value":true}]},
    {"id":"cyrl", "regexp":"[\\x{0400}-\\x{04FF}]","on_match":[{"action":"set","var":"cyrl","value":true}]},
    {"id":"cjk",  "regexp":"[\\x{4E00}-\\x{9FFF}]","on_match":[{"action":"set","var":"cjk","value":true}]},
    {"id":"arab", "regexp":"[\\x{0600}-\\x{06FF}]","on_match":[{"action":"set","var":"arab","value":true}]}
  ],
  "on_file_end":[
    {"action":"emit","data":{"file":"$FILE","latin":"$latin","cyrl":"$cyrl","cjk":"$cjk","arab":"$arab"}},
    {"action":"reset","vars":["latin","cyrl","cjk","arab"]}
  ]
}'
# {"file":"news.txt","latin":true,"cyrl":true,"cjk":false,"arab":false}
# {"file":"poem.txt","latin":false,"cyrl":false,"cjk":true,"arab":false}
```

### 27.4 Capitalized-sequence NER proxy

**Problem:** Find sequences of 2+ capitalized tokens (proxy for proper nouns).

**Input:** Prose corpus.

```bash
hprscript -p '\b[A-Z][a-z]+(?:\s+[A-Z][a-z]+){1,4}\b' \
  -o -glob '**/*.txt' | sort | uniq -c | sort -rn | head -50
```

### 27.5 Cross-script word frequency — `\p{L}+` (UTF-8 demo)

**Problem:** Count word tokens across a multilingual corpus. By default `\w+` is ASCII-only, and its Unicode variant (`ucp: true`) is rejected by Vectorscan as "Pattern is too large" — as is any bounded repeat like `\p{L}{4,}`. The form that compiles is `\p{L}+`: Unicode-aware in default UTF-8 mode, no `ucp` needed. Filter short tokens downstream if you need a length floor.

**Input:** Mixed-language corpus.

```bash
hprscript -s '{
  "scan": ["corpus/**/*.txt"],
  "variables": {"freq": {"type":"map"}},
  "patterns": [
    {"id":"w","regexp":"\\p{L}+",
     "on_match":[{"action":"map_increment","target":"freq","key":"$MATCH"}]}
  ],
  "on_complete":[
    {"action":"for_each","var":"freq","key_as":"w","as":"n","do":[
      {"action":"emit","data":{"word":"$w","count":"$n"}}]}
  ]
}'
# {"word":"Москва","count":42}
# {"word":"日本語","count":18}
# {"word":"naïve","count":7}
# {"word":"because","count":234}
```

**Compare:** without Unicode awareness, an ASCII `\w+` emits `naïve` as `na` + `ve` (two fragments), miscounting the word entirely.

### 27.6 Function-word frequency

**Problem:** Count occurrences of common closed-class English words.

**Input:** English corpus.

```bash
hprscript -s '{
  "scan": ["**/*.txt"],
  "variables": {"freq": {"type":"map"}},
  "patterns": [
    {"id":"w","regexp":"\\b(the|a|an|of|in|to|for|with|on|at|by|from)\\b","case_insensitive":true,
     "on_match":[{"action":"map_increment","target":"freq","key":"$MATCH"}]}
  ],
  "on_complete":[
    {"action":"for_each","var":"freq","key_as":"w","as":"n","do":[
      {"action":"emit","data":{"word":"$w","count":"$n"}}]}
  ]
}'
```

---

# DevOps & build tooling

## 28. Build & test output triage

### 28.1 cargo — failed test names

**Problem:** Pull `test foo::bar::baz ... FAILED` lines.

**Input:** `cargo test` output.

```bash
hprscript -p '^test\s+(\S+)\s+\.\.\.\s+FAILED\b' -extract name \
  -format '$EXTRACT_NAME' -glob '**/cargo-test-*.log'
```

### 28.2 jest — failures with file:line

**Problem:** Extract failing test descriptions and attach the nearby stack-frame location.

**Input:** `jest --verbose` output.

```bash
hprscript -p '✕\s+(.+)$' -p '^\s*at\s+\S+\s+\((\S+):(\d+):\d+\)' \
  -near p0:p1:5 -glob '**/jest-*.log'
```

### 28.3 gradle / maven — BUILD FAILED reasons by phase

**Problem:** Build failures fall into compile, test, and packaging phases. Naming each one separates "the code didn't compile" from "the tests failed" from "publishing failed" — different teams, different next steps.

**Input:** gradle/maven output.

```bash
hprscript \
  -p 'Compilation failed'            \
  -p 'There were failing tests'      \
  -p 'BUILD FAILED'                  \
  -p 'FAILURE: Build failed with an exception' \
  -format '$PAT_ID  $FILE:$LINE  $CONTEXT' -C 5 -glob '**/gradle-build-*.log'
```

### 28.4 pytest — extract FAILED block

**Problem:** Pull the failure body (next 10 lines).

**Input:** pytest output.

```bash
hprscript -p '^FAILED\s+\S+::\S+' -A 10 -glob '**/pytest-*.log'
```

### 28.5 Compiler warnings ranked by file

**Problem:** Per-file warning count.

**Input:** Build logs.

```bash
hprscript -pi 'warning:' -c -glob '**/build-*.log' | sort -t: -k2 -n -r
```

### 28.6 CI step duration extraction

**Problem:** Pull `[step] duration: 12.3s` annotations.

**Input:** CI logs.

```bash
hprscript -p '\[(\S+)\]\s+duration:\s+([\d.]+)s' -extract step,sec \
  -format '$EXTRACT_SEC  $EXTRACT_STEP' -glob '**/ci-*.log' | sort -rn | head -20
```

---

## 29. Git commit message audits

### 29.1 Conventional-commits compliance

**Problem:** Find commits *not* prefixed with `type:` or `type(scope):`. Hyperscan doesn't support negative lookahead, so the recipe uses two patterns and `-far A:B:0` to mean "match A only when B is *not* on the same line."

**Input:** `git log` piped in.

```bash
git log --pretty='%H %s' | hprscript \
  -p '^[0-9a-f]{40} ' \
  -p '^[0-9a-f]{40} (feat|fix|chore|docs|refactor|test|ci|build|perf|style|revert)(\([^)]+\))?: ' \
  -far p0:p1:0
```

### 29.2 Commits without ticket reference

**Problem:** Subjects missing `JIRA-1234`-style refs.

**Input:** git log.

```bash
git log --pretty='%H %s' | hprscript \
  -p '^[0-9a-f]+ ' -p '\b[A-Z]{2,}-\d+\b' -far p0:p1:0
```

### 29.3 WIP / fixup commits to clean up before merge

**Problem:** Surface noise commits in a feature branch.

**Input:** git log of the branch.

```bash
git log main..HEAD --pretty='%H %s' | \
  hprscript -pi '^\S+\s+(wip|fixup!|squash!|tmp|debug)\b'
```

### 29.4 Long commit subjects (>72 chars)

**Problem:** Subjects exceeding the conventional 72-character limit. The large bounded repeat is rejected in UTF-8 mode ("Pattern is too large"); `-no-utf8` compiles it. Byte-level `.` means the 73 counts bytes, which matches the convention for ASCII subjects.

**Input:** git log.

```bash
git log --pretty='%s' | hprscript -p '^.{73,}$' -no-utf8
```

### 29.5 Co-author extraction

**Problem:** Inventory `Co-authored-by` collaborators across history.

**Input:** Full commit messages.

```bash
git log --pretty='%B' | hprscript -p '^Co-authored-by:\s*([^<]+)<([^>]+)>' \
  -extract name,email -format '$EXTRACT_EMAIL  $EXTRACT_NAME' | sort -u
```

---

# Documentation

## 30. Markdown & docs audit

### 30.1 Broken-looking markdown links

**Problem:** Catch `[text](path)` where `path` is neither a URL nor a clearly valid local path.

**Input:** Markdown tree.

```bash
hprscript -p '\]\(([^)]+)\)' -extract url \
  -format '$FILE:$LINE  $EXTRACT_URL' -glob '**/*.md' | \
  awk -F'  ' '$2 !~ /^(https?:|mailto:|#|\/|\.\/|\.\.\/)/'
```

### 30.2 Heading hierarchy survey

**Problem:** Inventory all headings and levels for a structure review.

**Input:** Markdown files.

```bash
hprscript -p '^#{1,6}\s' -format '$FILE:$LINE  $MATCH' -glob '**/*.md'
```

### 30.3 Banned terms — by category

**Problem:** Style guides ban words for different reasons (hedge words like "simply" / "obviously"; ableist terms; outdated tech terms). Naming the categories surfaces which kind of bad-style is most common.

**Input:** Docs tree.

```bash
hprscript \
  -pi '\b(simply|just|obviously|trivial|easy)\b'   \
  -pi '\b(crazy|insane|sane|dumb)\b'               \
  -pi '\b(blacklist|whitelist|master/slave)\b'     \
  -format '$PAT_ID  $FILE:$LINE  $MATCH  -- $CONTEXT' \
  -glob '**/*.md'
# →  p0 = hedge word    p1 = ableist    p2 = outdated tech
```

### 30.4 Code-block language tag inventory

**Problem:** Which languages do fenced code blocks declare?

**Input:** Markdown tree.

```bash
hprscript -p '^```(\w+)' -extract lang \
  -format '$EXTRACT_LANG' -glob '**/*.md' | sort | uniq -c | sort -rn
```

### 30.5 Image references missing alt text

**Problem:** Find `![](url)` (empty alt).

**Input:** Markdown.

```bash
hprscript -p '!\[\]\([^)]+\)' -glob '**/*.md'
```

### 30.6 Files referenced but never defined

**Problem:** Markdown links pointing to other `.md` files in the tree — flag those whose target file doesn't exist on disk.

**Input:** Docs tree.

```bash
hprscript -p '\]\((\S+\.md)(#\S+)?\)' -extract path,_anchor \
  -format '$FILE  $EXTRACT_PATH' -glob '**/*.md' | \
  awk '{cmd="test -e " $2; if (system(cmd)) print}'
```

---

# Forensics & misc

## 31. Strings triage — memory dumps, firmware, binaries

### 31.1 URLs in `strings(1)` output

**Problem:** Pull URLs from a binary or memory dump.

**Input:** Stdin from `strings binary`.

```bash
strings -a suspect.bin | hprscript -p 'https?://[^\s<>"\x27]+' -o
```

### 31.2 IP addresses in a binary

**Problem:** IP literals embedded in the binary.

**Input:** Stdin from `strings`.

```bash
strings -a firmware.bin | hprscript -p '\b(\d{1,3}\.){3}\d{1,3}\b' -o | sort -u
```

### 31.3 Embedded credentials

**Problem:** Common config-style credential strings.

**Input:** Stdin from `strings`.

```bash
strings -a memdump.raw | hprscript -pi '(password|passwd|user|host)\s*=\s*\S+'
```

### 31.4 Path leaks — Windows vs Unix labelled

**Problem:** Source paths or developer-machine paths leaking into binaries can disclose internal infrastructure. Two named patterns separate Windows-style from Unix-style for forensic provenance.

**Input:** Stdin from `strings`.

```bash
strings -a binary | hprscript \
  -p '\b[A-Z]:\\(?:[^\\/:*?"<>|\r\n]+\\)+[^\\/:*?"<>|\r\n]+' \
  -p '/(?:home|Users|usr/local|opt|var)/[^\s]+'              \
  -format '$PAT_ID  $MATCH'
# →  p0 = Windows path    p1 = Unix path
```

### 31.5 Suspicious Windows API names — by capability

**Problem:** Known-malicious Windows APIs cluster by capability (process injection, hooking, dynamic loading). Naming each capability lets you triage the binary's likely behaviour from the strings dump.

**Input:** Stdin from `strings`.

```bash
strings -a sample.exe | hprscript \
  -p '\b(CreateRemoteThread|VirtualAllocEx|WriteProcessMemory|NtUnmapViewOfSection)\b' \
  -p '\b(SetWindowsHookEx|UnhookWindowsHookEx)\b'                                       \
  -p '\b(GetProcAddress|LoadLibrary[AW]?)\b'                                            \
  -format '$PAT_ID  $MATCH'
# →  p0 = process injection    p1 = hooking    p2 = dynamic loading
```

### 31.6 Mutex / named-object indicators

**Problem:** Pull mutex / event names from `CreateMutex` argument strings.

**Input:** Stdin from `strings`.

```bash
strings -a sample.exe | hprscript -p '\b(Global|Local)\\\\\w{6,}\b' -o
```

---

## 32. Academic — LaTeX & BibTeX

### 32.1 BibTeX entries missing required fields

**Problem:** Find `@article{...}` blocks without an `author = ` line. `block` extracts the entry; `submatch` with `absent: true` reports entries lacking the field.

**Input:** `.bib` files.

```bash
hprscript -s '{
  "scan": ["**/*.bib"],
  "patterns": [{"id":"art","regexp":"^@article\\{","on_match":[
    {"action":"block","open":"{","close":"}",
     "on_block":[
       {"action":"submatch","text":"$BLOCK","patterns":[
         {"id":"a","regexp":"author\\s*=","absent":true,"on_match":[
           {"action":"emit","data":{"file":"$FILE","line":"$LINE","missing":"author","entry":"$BLOCK"}}]}]}]}]}]
}'
```

### 32.2 LaTeX — undefined references (cite vs bibitem)

**Problem:** Phase 1 indexes `\bibitem{key}` and BibTeX entry keys; phase 2 reports `\cite{key}` references with no matching definition.

**Input:** `.tex` and `.bib`.

```bash
hprscript -s '{
  "variables": {"defs": {"type":"map"}, "_k":{"type":"string"}},
  "phases": [
    {"id":"defs","scan":["**/*.bib","**/*.tex"],
      "patterns":[
        {"id":"b","regexp":"\\\\bibitem\\{([^}]+)\\}","extract":["k"],
         "on_match":[{"action":"map_set","target":"defs","key":"$EXTRACT_K","value":"$FILE"}]},
        {"id":"e","regexp":"^\\s*([A-Za-z0-9_:.\\-]+),\\s*$","extract":["k"],
         "on_match":[{"action":"map_set","target":"defs","key":"$EXTRACT_K","value":"$FILE"}]}]},
    {"id":"cites","scan":["**/*.tex"],
      "patterns":[
        {"id":"c","regexp":"\\\\cite[tp]?\\{([^}]+)\\}","extract":["k"],
         "on_match":[
           {"action":"lookup","map":"defs","key":"$EXTRACT_K",
            "on_miss":[{"action":"emit","data":{"file":"$FILE","line":"$LINE","undefined_cite":"$EXTRACT_K"}}]}]}]}
  ]
}'
```

### 32.3 Equation extraction (`\begin{equation} ... \end{equation}`)

**Problem:** Pull every `equation` environment body using multi-character `block` delimiters.

**Input:** `.tex` files.

```bash
hprscript -s '{
  "scan": ["**/*.tex"],
  "patterns": [{"id":"eq","regexp":"\\\\begin\\{equation\\}","on_match":[
    {"action":"block","open":"\\begin{equation}","close":"\\end{equation}",
     "on_block":[{"action":"emit","data":{"file":"$FILE","line":"$LINE","eq":"$BLOCK_FULL"}}]}]}]
}'
```

### 32.4 Section structure inventory

**Problem:** Outline a paper from its `\section` / `\subsection` / `\subsubsection` headings.

**Input:** `.tex` files.

```bash
hprscript -p '^\\(sub){0,2}section\*?\{([^}]+)\}' -extract _star,title \
  -format '$FILE:$LINE  $MATCH' -glob '**/*.tex'
```

### 32.5 Citation count per source

**Problem:** Which references are cited most?

**Input:** `.tex` files.

```bash
hprscript -s '{
  "scan": ["**/*.tex"],
  "variables": {"freq": {"type":"map"}},
  "patterns": [{"id":"c","regexp":"\\\\cite[tp]?\\{([^}]+)\\}","extract":["k"],
    "on_match":[{"action":"map_increment","target":"freq","key":"$EXTRACT_K"}]}],
  "on_complete":[
    {"action":"for_each","var":"freq","key_as":"k","as":"n","do":[
      {"action":"emit","data":{"key":"$k","cites":"$n"}}]}
  ]
}'
```

---

# Editing files

## 33. Guarded code edits (`hprscript edit`)

The `edit`/`apply` commands are the write-capable surface — everything else in
this cookbook stays read-only. Persistent plans are the preferred bulk-edit
contract; preflight guard violations exit 3 with nothing written. Full reference:
[Edit mode](HPRSCRIPT.md#edit-mode-hprscript-edit).

**The review/apply contract for mechanical bulk edits:**

```bash
# 1. Discover once: show the diff and persist exact sites plus file hashes.
hprscript edit -F 'retry(3)' -content 'retry(5)' -expect 1 \
    -plan-out retry.plan.json src/worker.go
# 2. Review and apply the exact stored edits without rescanning.
hprscript apply retry.plan.json
```

`-expect` protects the planning scan only; equal counts from a later rescan do
not prove that it selected the same paths and byte ranges.

**Localized function rewrite:** use hprscript to locate/confirm the function,
then use the agent-native patch tool because correctness is semantic rather
than one repeated mechanical transformation.

```bash
cat > /tmp/new_impl.go <<'EOF'
func LoadData(path string) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return fmt.Errorf("load %s: %w", path, err)
	}
	return parse(data)
}
EOF
printf '%s' "$(cat /tmp/new_impl.go)" > /tmp/new_impl.go  # span ends at }, so trim the trailing newline
hprscript -p 'func\s+LoadData\b' -scope auto -llm src/data.go
# Apply /tmp/new_impl.go with the agent-native patch tool.
```

**Rename an identifier — but only on lines this branch added:**

```bash
hprscript edit -p '\bOldName\b' -content 'NewName' \
    -git-changed -git-added-lines -expect 14 -plan-out rename.plan.json
hprscript apply rename.plan.json
```

**sed-style substitution with capture groups** (`-extract` + `$EXTRACT_*`):

```bash
hprscript edit -p 'log\.Printf\("([^"]*)"' -extract fmt \
    -content 'logger.Infof("$EXTRACT_FMT"' -write -glob '**/*.go'
```

**Add an import inside Go's `import ( … )` block:**

```bash
hprscript edit -p '^import \(' -block-open '(' -block-close ')' \
    -span block -insert end -content '\t"corp/pkg/log"\n' -expect 1 -write main.go
```

**Delete debug prints, guarded by an exact count:**

```bash
hprscript edit -p '^\s*debugPrint\(' -span line -delete -expect 6 -write -glob '**/*.go'
```

**Conditional edit — replace except where an allow-comment sits on the line.**
The qualifier pattern must be `-ref` or its own matches would be edited too:

```bash
hprscript edit -p 'http://' -name hit -p 'allow-insecure' -name allow -ref \
    -far hit:allow:0 -content 'https://' -write -glob '**/*.{yaml,conf}'
```

**Only inside one function** — bump a constant without touching the rest of
the file (`-in-scope` implies `-scope auto`; the enclosing chain counts, so
closures inside the function are covered too):

```bash
hprscript edit -F 'retry(3)' -content 'retry(5)' \
    -in-scope 'ProcessBatch' -expect 1 -write src/worker.go
```

**Replace a whole function by name** — anchorless: no pattern, no signature
regex, no brace flags; the scope pack knows the language:

```bash
hprscript -list-scopes src/data.go                    # outline: confirm the name
hprscript edit -in-scope '^LoadData$' -span scope \
    -content-file /tmp/new_loaddata.go -expect 1 -write src/data.go
```

**Append a statement at the end of a function's body:**

```bash
hprscript edit -in-scope '^main$' -span scope-body -insert end \
    -content '\tflush()\n' -expect 1 -write cmd/run.go
```

**Recover / verify:** `apply` emits a JSON receipt by default (`-receipt human`
is available); `git diff` is the ordinary undo/review surface. Applying a
completed plan again fails stale whole-file verification with exit 3. Exit 4
means commit began but did not finish; the receipt identifies applied and
untouched files.

---

## 34. Context retrieval & ranking for LLM agents

hprscript's edge over embedding-based RAG: relevance, ranking, and
context-budget-fit computed deterministically and freshly from the live
tree — no index to go stale, no setup, fully explainable. These recipes are
the ones an agent reaches for when the question isn't "does X exist" but
"where should I look, and what's the smallest slice of code that answers
the question."

### 34.1 "Where's the code for X?" in one call — hotspot ranking

**Problem:** N `grep` hits across a dozen files, no sense of which one is actually *the* place to look. `-hotspots` ranks every matching file by a rarity/coverage/proximity score (files matching more of the queried terms, more densely, in fewer overall files, rank higher) and reports each file's single densest match window.

**Input:** Source tree.

```bash
hprscript -p 'RetryPolicy' -p 'backoff' -hotspots 5 -llm -glob '**/*.go'
# → src/retry/policy.go:12-64 score=1.4 patterns=p0,p1
# → src/client/http.go:88-88 score=0.3 patterns=p0
```

### 34.2 Pack the best context into a token budget

**Problem:** You need "everything relevant to X" but have a hard context budget. `-budget N` ranks every matching file (same formula as `-hotspots`) and renders them score-descending in scope-aware chunks until `N` bytes are spent — degrading a file that doesn't fit to a one-line summary, then to a named "dropped" entry, rather than truncating mid-file or silently omitting anything.

**Input:** Source tree.

```bash
hprscript -p 'AuthMiddleware' -p 'validateToken' -budget 6000 -glob '**/*.go'
# → src/auth/middleware.go
# →   12-40 func AuthMiddleware
# → ...
# → src/auth/legacy.go:9-9 score=0.2 patterns=p1 (compact — full render didn't fit the budget)
# → --- budget: 2 file(s) in full, 1 compact, 0 dropped ---
```

### 34.3 Compact excerpts instead of whole functions

**Problem:** A block-extracted function body can be hundreds of lines when only two of them matter. `-elide` prints the signature and matched lines with a little context, folding everything else into `… (+N lines)` — the shape a human skimming the function would produce by eye, and far cheaper than `-block-open`/`-block-close`'s full body for a large function.

**Input:** Source tree.

```bash
hprscript -p 'RetryPolicy' -elide -scope auto -glob '**/*.go'
# → src/retry/policy.go
# →   12-64 func NewRetryPolicy
# → func NewRetryPolicy(opts ...Option) *RetryPolicy {
# →   … (+48 lines)
# →     return p
# → }
```

### 34.4 Find identifiers regardless of naming convention

**Problem:** The symbol you're hunting could be `parseConfig`, `parse_config`, or `ConfigParser` depending on who wrote it and when — a regex has to enumerate every casing variant by hand. `-ident 'term1 term2'` splits every identifier on camelCase/snake_case/acronym/digit boundaries and matches when all given terms appear as subtokens, in any order, any casing.

**Input:** Source tree.

```bash
hprscript -ident 'parse config' -glob '**/*.go'
# → matches parseConfig, parse_config, ConfigParser, PARSE_CONFIG, ...
```

Each `-ident` group is a first-class pattern (`ident0`, `ident1`, …) — usable in `-name`, `-near`/`-far`, and `-file-where` exactly like a `-p` pattern.

### 34.5 Don't re-pay tokens for unchanged context across agent turns

**Problem:** An agent iterating — search, edit, search again — re-reads the same unchanged function every round. `-seen <path>` hashes each rendered chunk's raw source and, on a later run against the same state file, collapses anything unchanged to a one-line pointer instead of the full body.

**Input:** Source tree, across repeated invocations in the same session.

```bash
hprscript -p 'AuthMiddleware' -elide -seen .hpr-seen -glob '**/*.go'
# first call: full chunks, .hpr-seen written
# every later call this session, for functions you haven't touched:
# →   12-40 func AuthMiddleware (unchanged, already shown)
```

Works with `-budget` too. A chunk `-budget` measures to decide whether it fits, then discards in favor of a compact summary or a drop, is never recorded as "shown" — a later run with more budget still renders it in full.

### 34.6 Filter by how actively a file is being worked on

**Problem:** "Files with lots of TODOs" is noisy across a whole repo; "files with lots of TODOs that are also under active development" is the actual review queue. `churn(days)` extends `-file-where` with a git-commit-count condition alongside the existing pattern-presence predicate; `count(pat)` raises the bar from "matched once" to "matched at least N times."

**Input:** A git repository.

```bash
hprscript -p TODO -name t -file-where 'count(t) >= 3 AND churn(30) > 2' -llm -glob '**/*.go'
```

`churn(30)` runs one `git log --since=30.days.ago` call regardless of how many files match — never one subprocess per file. `lang == go` (only `==`/`!=`) is also available, for globs that sweep in more than one language.

### 34.7 Ranked file listing instead of walk order

**Problem:** `-f`/`-c` stream in filesystem order, which has no relationship to relevance. `-order-by score` sorts the file list by the same ranking formula `-hotspots` uses; `count` sorts by total matches; `path` sorts lexicographically.

**Input:** Source tree.

```bash
hprscript -p TODO -c -order-by score -glob '**/*.go'
```

---

## See also

- **[HPRSCRIPT.md](HPRSCRIPT.md)** — full reference for every flag, the script-mode JSON DSL, regex syntax, exit codes, and the agent-focused cookbook.
- **[README.md](README.md)** — overview, installation, and agent-skill setup.
