#!/usr/bin/env bash
# Basic functionality tests for hprscript.
#
# Each test runs the binary against fixtures in tests/fixtures and asserts
# either an expected substring/regex on stdout, an expected exit code, or
# an exact line count. The runner exits non-zero on the first failure
# count > 0 so `make test` integrates with CI.

set -u

# Resolve repo root regardless of where the script is invoked from.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BIN="${HPRSCRIPT_BIN:-$ROOT/hprscript}"
FIX="$HERE/fixtures"
SAMPLE="$FIX/sample"

if [[ ! -x "$BIN" ]]; then
    echo "FATAL: $BIN not found or not executable. Run 'make' first." >&2
    exit 2
fi

PASS=0
FAIL=0
FAILED_NAMES=()

# Pretty-print a label and PASS/FAIL marker.
report() {
    local status=$1 name=$2
    if [[ "$status" == ok ]]; then
        printf "  \033[32mPASS\033[0m  %s\n" "$name"
        PASS=$((PASS + 1))
    else
        printf "  \033[31mFAIL\033[0m  %s\n" "$name"
        FAIL=$((FAIL + 1))
        FAILED_NAMES+=("$name")
    fi
}

# expect_eq NAME EXPECTED ACTUAL — compare strings (multi-line OK).
expect_eq() {
    local name=$1 expected=$2 actual=$3
    if [[ "$expected" == "$actual" ]]; then
        report ok "$name"
    else
        report fail "$name"
        printf "    expected:\n%s\n" "$expected" | sed 's/^/      | /'
        printf "    actual:\n%s\n"   "$actual"   | sed 's/^/      | /'
    fi
}

# expect_contains NAME NEEDLE HAYSTACK — substring must appear in HAYSTACK.
expect_contains() {
    local name=$1 needle=$2 hay=$3
    if [[ "$hay" == *"$needle"* ]]; then
        report ok "$name"
    else
        report fail "$name"
        printf "    expected to contain: %s\n" "$needle"
        printf "    actual:\n%s\n" "$hay" | sed 's/^/      | /'
    fi
}

# expect_not_contains NAME NEEDLE HAYSTACK
expect_not_contains() {
    local name=$1 needle=$2 hay=$3
    if [[ "$hay" != *"$needle"* ]]; then
        report ok "$name"
    else
        report fail "$name"
        printf "    expected NOT to contain: %s\n" "$needle"
        printf "    actual:\n%s\n" "$hay" | sed 's/^/      | /'
    fi
}

# expect_lines NAME N OUTPUT — assert exact line count (empty output = 0).
expect_lines() {
    local name=$1 n=$2 out=$3
    local got
    if [[ -z "$out" ]]; then got=0; else got=$(printf '%s' "$out" | grep -c '^'); fi
    if [[ "$got" == "$n" ]]; then
        report ok "$name"
    else
        report fail "$name (expected $n lines, got $got)"
        printf "%s\n" "$out" | sed 's/^/      | /'
    fi
}

# expect_exit NAME EXPECTED_CODE CMD…
expect_exit() {
    local name=$1 want=$2 ; shift 2
    local out rc
    out=$("$@" 2>&1) ; rc=$?
    if [[ "$rc" == "$want" ]]; then
        report ok "$name"
    else
        report fail "$name (expected exit $want, got $rc)"
        printf "%s\n" "$out" | sed 's/^/      | /'
    fi
}

section() { printf "\n\033[1m%s\033[0m\n" "$1"; }

# ---------------------------------------------------------------------------
section "smoke"
expect_contains "version flag"  "hprscript" "$("$BIN" --version)"
expect_contains "version flag has vectorscan" "vectorscan" "$("$BIN" --version)"
expect_contains "help"          "Usage:"   "$("$BIN" --help)"
expect_exit    "no args, no stdin"   2 "$BIN"
expect_exit    "unknown flag"        2 "$BIN" -nope
expect_exit    "-p without value"    2 "$BIN" -p

# ---------------------------------------------------------------------------
section "-p quick search: output modes"

OUT=$(cd "$SAMPLE" && "$BIN" -p TODO -j -glob '**/*.go')
expect_lines    "-j JSON Lines: 3 matches"  3 "$OUT"
expect_contains "-j contains a.go"         '"file":"a.go"'        "$OUT"
expect_contains "-j contains b.go"         '"file":"b.go"'        "$OUT"
expect_contains "-j contains vendor/"      '"file":"vendor/skip.go"' "$OUT"
expect_contains "-j contains line"         '"line":3'             "$OUT"
expect_contains "-j contains match"        '"match":"TODO"'       "$OUT"

OUT=$(cd "$SAMPLE" && "$BIN" -p TODO -f -glob '**/*.go')
expect_lines "-f files-only: 3"  3 "$OUT"
expect_contains "-f a.go"  "a.go"  "$OUT"
expect_contains "-f b.go"  "b.go"  "$OUT"

OUT=$(cd "$SAMPLE" && "$BIN" -p TODO -c -glob '**/*.go')
expect_lines    "-c counts: 3 lines"     3 "$OUT"
expect_contains "-c a.go:1"  "a.go:1"          "$OUT"
expect_contains "-c b.go:1"  "b.go:1"          "$OUT"

OUT=$(cd "$SAMPLE" && "$BIN" -p TODO -o -glob '**/*.go')
expect_lines "-o match-only: 3 lines" 3 "$OUT"
expect_eq    "-o match text"          "TODO
TODO
TODO"  "$OUT"

OUT=$(cd "$SAMPLE" && "$BIN" -p TODO -format '$FILE:$LINE $MATCH' -glob '**/*.go')
expect_contains "-format expanded FILE/LINE/MATCH" "a.go:3 TODO" "$OUT"

OUT=$(cd "$SAMPLE" && "$BIN" -p Copyright -absent -glob '**/*.go')
expect_lines "-absent: 3 files lack Copyright" 3 "$OUT"

# Default output is JSON Lines (no -j needed)
OUT=$(cd "$SAMPLE" && "$BIN" -p TODO -glob '**/*.go')
expect_lines    "default JSONL: 3 matches"  3 "$OUT"
expect_contains "default JSONL: file field"  '"file":"a.go"'  "$OUT"
expect_contains "default JSONL: match field" '"match":"TODO"' "$OUT"

# ---------------------------------------------------------------------------
section "-p flags: -pi -w -limit -m -context"

# -pi per-pattern case insensitive
OUT=$(cd "$SAMPLE" && "$BIN" -pi TODO -j -glob '**/*.go' -exclude vendor)
expect_lines "-pi: TODO + todo in a.go + b.go = 3" 3 "$OUT"
expect_contains "-pi finds lowercase 'todo'" '"match":"todo"' "$OUT"

# -p (case-sensitive) and -pi mixed: only the -pi pattern folds case.
# a.go: TODO@3, todo@6; b.go: TODO@4. p0 (-p TODO) hits 2; p1 (-pi todo) hits 3.
OUT=$(cd "$SAMPLE" && "$BIN" -p TODO -pi 'todo' -j -glob '**/*.go' -exclude vendor)
expect_lines    "mixed -p/-pi: 2 (case-sens) + 3 (case-insens) = 5"  5 "$OUT"
expect_contains "mixed: p0 (case-sens) tagged"  '"pat":"p0"' "$OUT"
expect_contains "mixed: p1 (case-insens) tagged" '"pat":"p1"' "$OUT"

# -w word boundary
HAY="hello othello hello world"
OUT=$(printf '%s\n' "$HAY" | "$BIN" -p hello -w -o)
expect_lines "-w 'hello' against $HAY: 2 matches"  2 "$OUT"
OUT=$(printf '%s\n' "$HAY" | "$BIN" -p hello -o)
expect_lines "no -w 'hello': 3 matches (incl. othello)" 3 "$OUT"

# -limit global
OUT=$(cd "$SAMPLE" && "$BIN" -pi TODO -limit 2 -j -glob '**/*.go')
expect_lines "-limit 2 stops globally" 2 "$OUT"

# -m per-file (with -pi a.go has 2 todos; -m 1 forces only 1 per file)
OUT=$(cd "$SAMPLE" && "$BIN" -pi TODO -m 1 -j -glob '**/*.go')
expect_lines "-m 1: 1 per file × 3 files = 3" 3 "$OUT"

# -A / -B / -C context — TODO is on line 3 of a.go, B=2 captures line 1
# ("package main"), A=1 captures line 4 ("func main() {").
OUT=$(cd "$SAMPLE" && "$BIN" -p TODO -B 2 -A 1 -j -glob '**/*.go' -exclude vendor)
expect_contains "-B 2 reaches up to line 1" "package main"  "$OUT"
expect_contains "-A 1 reaches line 4"        "func main() {" "$OUT"

# ---------------------------------------------------------------------------
section "-p exclude rules"

OUT=$(cd "$SAMPLE" && "$BIN" -p TODO -j -glob '**/*.go' -exclude vendor)
expect_lines    "-exclude vendor (bare name): 2"  2 "$OUT"
expect_not_contains "-exclude vendor: no skip.go" "skip.go" "$OUT"

OUT=$(cd "$SAMPLE" && "$BIN" -p TODO -j -glob '**/*.go' -exclude 'vendor/')
expect_lines "-exclude 'vendor/' (path prefix): 2" 2 "$OUT"

OUT=$(cd "$SAMPLE" && "$BIN" -p TODO -j -glob '**/*' -exclude '*.txt')
expect_not_contains "-exclude '*.txt' (glob)" 'c.txt' "$OUT"

# ---------------------------------------------------------------------------
section "absolute-path glob"

# Recursive glob with an absolute base must walk that base, not silently
# resolve to a non-existent relative path. Run from a neutral cwd so the
# only way the binary can find the fixture is via the absolute pattern.
OUT=$(cd / && "$BIN" -p TODO -j -glob "$SAMPLE/**/*.go" -exclude vendor)
expect_lines    "abs glob: 2 matches under absolute base" 2 "$OUT"
expect_contains "abs glob: a.go found" "/a.go" "$OUT"
expect_contains "abs glob: b.go found" "/b.go" "$OUT"

# Absolute path as a positional scan item (no -glob) — same expectation.
OUT=$(cd / && "$BIN" -p TODO -j -exclude vendor "$SAMPLE/**/*.go")
expect_lines "abs glob positional: 2 matches" 2 "$OUT"

# ---------------------------------------------------------------------------
section "match deduplication (greedy non-overlapping)"

# File walk order is fs-dependent (readdir / inode order on Linux), so
# sort the matches before comparing — the dedup invariant is the count
# and the set of values, not the order across files.
OUT=$(cd "$SAMPLE" && "$BIN" -p 'func\s+\w+' -o -glob '**/*.go' -exclude vendor | sort)
expect_eq "func\\s+\\w+ deduped to {func main, func helper}" "func helper
func main" "$OUT"

# ---------------------------------------------------------------------------
section "anchors (multiline by default)"

OUT=$(printf 'foo\nfoobar\nbarfoo\n' | "$BIN" -p '^foo$' -o)
expect_lines "^foo\$ multiline matches one line" 1 "$OUT"
expect_eq    "^foo\$ matches just 'foo'" "foo" "$OUT"

OUT=$(printf 'foo\nfoobar\nbarfoo\n' | "$BIN" -p '^foo' -o)
expect_lines "^foo line-start: 2 matches" 2 "$OUT"

# Buffer-anchored \A and \z. We can't easily compare the trailing newline
# in the output (bash $() strips trailing newlines), so verify match-count
# and exit code instead. \Afoo\n\z must match the entire 4-byte buffer.
expect_exit "\\Afoo\\n\\z buffer-anchored matches" 0 \
    bash -c "printf 'foo\n' | '$BIN' -p '\\Afoo\\n\\z' -o"
# Negative: \A pinned to start, so a leading 'x' breaks the match
expect_exit "\\Afoo\\z does not match when prefix differs" 1 \
    bash -c "printf 'xfoo' | '$BIN' -p '\\Afoo\\z' -o"

# ---------------------------------------------------------------------------
section "exit codes (grep convention)"

cd "$SAMPLE" || exit 2
expect_exit "exit 0 on match"     0 "$BIN" -p TODO -glob '**/*.go'
expect_exit "exit 1 on no match"  1 "$BIN" -p ZZZNOMATCH -glob '**/*.go'
expect_exit "exit 2 on bad regex" 2 "$BIN" -p '(?<=foo)bar' -glob '**/*.go'
cd - >/dev/null || true

# ---------------------------------------------------------------------------
section "multi-pattern"

OUT=$(cd "$SAMPLE" && "$BIN" -p TODO -p 'func\s+\w+' -j -glob '**/*.go' -exclude vendor)
expect_contains "multi-pattern: pat=p0" '"pat":"p0"' "$OUT"
expect_contains "multi-pattern: pat=p1" '"pat":"p1"' "$OUT"
expect_lines    "multi-pattern: 4 matches total" 4 "$OUT"

# ---------------------------------------------------------------------------
section "stdin"

OUT=$(printf 'TODO line\nfoo\nTodo line\n' | "$BIN" -pi TODO -o)
expect_lines "stdin -pi: 2 matches" 2 "$OUT"
expect_contains "stdin file label is <stdin>" "Todo" "$OUT"

# ---------------------------------------------------------------------------
section "script (-s / -script / positional / stdin)"

OUT=$(cd "$SAMPLE" && "$BIN" -s '{"scan":["**/*.go"],"exclude":["vendor"],"patterns":[{"id":"todo","regexp":"TODO"}]}')
expect_lines    "-s: 2 matches"        2 "$OUT"
expect_contains "-s: pat=todo"  '"pat":"todo"' "$OUT"

# emit with $-substitution
OUT=$(cd "$SAMPLE" && "$BIN" -s '{"scan":["**/*.go"],"exclude":["vendor"],"patterns":[{"id":"t","regexp":"TODO","on_match":[{"action":"emit","data":{"hit":"$MATCH","at":"$FILE:$LINE"}}]}]}')
expect_contains "emit data: hit=TODO"     '"hit":"TODO"'    "$OUT"
expect_contains "emit data: at a.go:3"    '"at":"a.go:3"'   "$OUT"
expect_not_contains "emit data: no default 'pat' field" "\"pat\":" "$OUT"

# script via positional file
SCRIPT="$HERE/_tmp_script.json"
printf '%s' '{"scan":["a.go","b.go"],"patterns":[{"id":"t","regexp":"TODO"}]}' > "$SCRIPT"
OUT=$(cd "$SAMPLE" && "$BIN" "$SCRIPT")
expect_lines "positional script file: 2 matches" 2 "$OUT"
rm -f "$SCRIPT"

# script via stdin
OUT=$(cd "$SAMPLE" && printf '%s' '{"scan":["**/*.go"],"exclude":["vendor"],"patterns":[{"id":"t","regexp":"TODO"}]}' | "$BIN")
expect_lines "stdin script: 2 matches" 2 "$OUT"

# CLI overrides scan
OUT=$(cd "$SAMPLE" && "$BIN" -s '{"scan":["nonexistent/**"],"patterns":[{"id":"t","regexp":"TODO"}]}' a.go)
expect_lines "CLI args override scan: 1 match in a.go only" 1 "$OUT"

# `phases` requires non-empty array; `set` without `var` is invalid; both
# surface as compile errors.
expect_exit "empty phases rejected" 2 "$BIN" -s '{"phases":[]}'
expect_exit "set without var rejected" 2 "$BIN" -s '{"patterns":[{"regexp":"x","on_match":[{"action":"set"}]}]}'
expect_exit "bad json rejected"    2 "$BIN" -s '{not json'
expect_exit "no patterns rejected" 2 "$BIN" -s '{"scan":["x"]}'

# ---------------------------------------------------------------------------
section "script: variables, conditions, lifecycle"

# variables + on_complete: count TODOs across files into an int and emit once.
OUT=$(cd "$SAMPLE" && "$BIN" -s '{
  "scan":["**/*.go"],"exclude":["vendor"],
  "variables":{"n":{"type":"int"}},
  "patterns":[{"id":"t","regexp":"TODO","on_match":[{"action":"increment","var":"n"}]}],
  "on_complete":[{"action":"emit","data":{"total":"$n"}}]
}')
expect_eq "variables + on_complete: total=2" '{"total":2}' "$OUT"

# if/condition: branch on $COL.
OUT=$(cd "$SAMPLE" && "$BIN" -s '{
  "scan":["**/*.go"],"exclude":["vendor"],
  "patterns":[{"id":"t","regexp":"TODO","on_match":[
    {"action":"if","condition":{"op":"eq","args":["$COL",1]},
     "then":[{"action":"emit","data":{"col1":true,"file":"$FILE"}}],
     "else":[{"action":"emit","data":{"col1":false,"file":"$FILE","col":"$COL"}}]}
  ]}]}')
expect_contains "if/eq: col1=false branch" '"col1":false' "$OUT"
expect_not_contains "if/eq: no col1=true (no TODO at col 1)" '"col1":true' "$OUT"

# contains
OUT=$(cd "$SAMPLE" && "$BIN" -s '{
  "scan":["**/*.go"],"exclude":["vendor"],
  "patterns":[{"id":"t","regexp":"TODO","on_match":[
    {"action":"if","condition":{"op":"contains","args":["$CONTEXT","refactor"]},
     "then":[{"action":"emit","data":{"sev":"high"}}]}
  ]}]}')
expect_lines "contains: 1 TODO has 'refactor'" 1 "$OUT"
expect_contains "contains: sev=high" '"sev":"high"' "$OUT"

# ---------------------------------------------------------------------------
section "script: absent + submatch"

OUT=$(cd "$SAMPLE" && "$BIN" -s '{
  "scan":["**/*.go"],"exclude":["vendor"],
  "patterns":[{"id":"missing","regexp":"Copyright","absent":true,
    "on_match":[{"action":"emit","data":{"file":"$FILE","issue":"no copyright"}}]}]
}')
expect_lines "absent: 2 files miss Copyright" 2 "$OUT"
expect_contains "absent: a.go missing" '"file":"a.go"' "$OUT"

# Submatch: digits + words inside outer match. Offsets are file-relative.
OUT=$(printf 'mm ab18 bed xy kk' | "$BIN" -s '{
  "patterns":[{"id":"o","regexp":"ab\\d+ \\w+ xy","on_match":[
    {"action":"submatch","patterns":[
      {"id":"d","regexp":"\\d+","on_match":[{"action":"emit","data":{"d":"$MATCH","at":"$FROM"}}]}
    ]}]}]
}')
expect_eq "submatch digits @ file-relative offset 5" '{"at":5,"d":"18"}' "$OUT"

# ---------------------------------------------------------------------------
section "script: list/map/for_each/count/group_by/rank/skip"

# map_increment + for_each
OUT=$(cd "$SAMPLE" && "$BIN" -s '{
  "scan":["**/*.go"],"exclude":["vendor"],
  "variables":{"counts":{"type":"map"}},
  "patterns":[{"id":"t","regexp":"TODO|todo","on_match":[
    {"action":"map_increment","target":"counts","key":"$FILE"}]}],
  "on_complete":[{"action":"for_each","var":"counts","key_as":"file","as":"n","do":[
    {"action":"emit","data":{"file":"$file","count":"$n"}}]}]
}' | sort)
expect_eq "map_increment + for_each" '{"count":1,"file":"b.go"}
{"count":2,"file":"a.go"}' "$OUT"

# count action shorthand (uses $PAT_ID)
OUT=$(cd "$SAMPLE" && "$BIN" -s '{
  "scan":["**/*.go"],"exclude":["vendor"],
  "variables":{"counts":{"type":"map"}},
  "patterns":[
    {"id":"todo","regexp":"TODO","on_match":[{"action":"count","var":"counts"}]},
    {"id":"fn","regexp":"func \\w+","on_match":[{"action":"count","var":"counts"}]}
  ],
  "on_complete":[{"action":"for_each","var":"counts","key_as":"p","as":"n","do":[
    {"action":"emit","data":{"pat":"$p","count":"$n"}}]}]
}' | sort)
expect_contains "count action: todo=2" '{"count":2,"pat":"todo"}' "$OUT"
expect_contains "count action: fn=2"   '{"count":2,"pat":"fn"}'   "$OUT"

# group_by buffers and flushes one line per key.
OUT=$(cd "$SAMPLE" && "$BIN" -s '{
  "scan":["**/*.go"],"exclude":["vendor"],"group_by":"file",
  "patterns":[{"id":"t","regexp":"TODO|todo","on_match":[
    {"action":"emit","data":{"file":"$FILE","line":"$LINE","match":"$MATCH"}}]}]
}')
expect_lines "group_by: 1 line per file (a.go, b.go)" 2 "$OUT"
expect_contains "group_by: a.go group has 2 records"  '"key":"a.go","group":[{' "$OUT"
expect_contains "group_by: b.go has 1 record"          '"key":"b.go","group":[{' "$OUT"

# rank by weighted score
OUT=$(cd "$SAMPLE" && "$BIN" -s '{
  "scan":["**/*.go"],"exclude":["vendor"],"rank":true,
  "patterns":[
    {"id":"todo","regexp":"TODO","weight":3},
    {"id":"fn","regexp":"func \\w+","weight":1}
  ]
}')
expect_contains "rank: a.go listed"  '"file":"a.go"'  "$OUT"
expect_contains "rank: b.go listed"  '"file":"b.go"'  "$OUT"
expect_contains "rank: matched_patterns" '"matched_patterns":["fn","todo"]' "$OUT"
expect_contains "rank: density field"    '"density":'  "$OUT"
# rank-only output: no per-match records mixed in (2 files matched → 2 lines).
expect_lines "rank: only rank rows emitted" 2 "$OUT"
expect_not_contains "rank: no match records" '"match":' "$OUT"

# rank: coverage factor — file matching all queried patterns outranks
# a file that matches only a subset, even at higher weight per match.
OUT=$(cd "$SAMPLE" && "$BIN" -s '{
  "scan":["**/*.go"],"exclude":["vendor"],"rank":true,
  "patterns":[
    {"id":"todo","regexp":"TODO","weight":1},
    {"id":"fn",  "regexp":"func \\w+","weight":1},
    {"id":"pkg", "regexp":"^package ","weight":1}
  ]
}')
# All three patterns match in both files → both files get coverage=1. Sanity:
# both files appear with a positive score.
expect_contains "rank cov: a.go listed" '"file":"a.go"' "$OUT"
expect_contains "rank cov: b.go listed" '"file":"b.go"' "$OUT"

# rank: proximity bonus — a file with co-located distinct patterns scores
# higher than a file where the same patterns are spread far apart.
mkdir -p "$SAMPLE/proxtest"
cat >"$SAMPLE/proxtest/near.go" <<'EOF'
package main
// TODO fix this
func handler() {}
EOF
# Build a "far" file: pattern A on line 1, then a wide gap, then pattern B.
{
  echo 'package main'
  echo '// TODO fix this'
  for _ in $(seq 1 80); do echo '// padding'; done
  echo 'func handler() {}'
} > "$SAMPLE/proxtest/far.go"
OUT=$(cd "$SAMPLE/proxtest" && "$BIN" -s '{
  "scan":["**/*.go"],"rank":true,
  "patterns":[
    {"id":"todo","regexp":"TODO","weight":1},
    {"id":"fn",  "regexp":"func \\w+","weight":1}
  ]
}')
# near.go must come first (proximity bonus + smaller divisor).
near_pos=$(printf '%s\n' "$OUT" | grep -n '"file":"near.go"' | head -1 | cut -d: -f1)
far_pos=$(printf '%s\n'  "$OUT" | grep -n '"file":"far.go"'  | head -1 | cut -d: -f1)
if [ -n "$near_pos" ] && [ -n "$far_pos" ] && [ "$near_pos" -lt "$far_pos" ]; then
  report ok "rank prox: near.go ranks above far.go"
else
  report fail "rank prox: expected near.go before far.go"
  printf "%s\n" "$OUT" | sed 's/^/      | /'
fi
rm -rf "$SAMPLE/proxtest"

# rank suppresses on_match emit/print but lets aggregations through
OUT=$(cd "$SAMPLE" && "$BIN" -s '{
  "scan":["**/*.go"],"exclude":["vendor"],"rank":true,
  "variables":{"n":{"type":"int"}},
  "patterns":[{"id":"t","regexp":"TODO","on_match":[
    {"action":"increment","var":"n"},
    {"action":"emit","data":{"hit":"$MATCH"}}]}],
  "on_complete":[{"action":"emit","data":{"total":"$n"}}]
}')
expect_contains "rank+on_complete: total emitted" '"total":2' "$OUT"
expect_not_contains "rank+on_complete: per-match emit suppressed" '"hit":"TODO"' "$OUT"

# skip + limit pagination
OUT=$(cd "$SAMPLE" && "$BIN" -s '{
  "scan":["**/*.go"],"skip":1,"limit":1,
  "patterns":[{"id":"f","regexp":"func \\w+","on_match":[
    {"action":"emit","data":{"f":"$MATCH"}}]}]
}')
expect_lines "skip+limit: 1 record" 1 "$OUT"

# ---------------------------------------------------------------------------
section "script: block action"

BLK="$HERE/_tmp_block.go"
printf 'package main\nfunc helper() {\n    a := 1\n    b := 2\n}\n' > "$BLK"
OUT=$("$BIN" -s '{
  "patterns":[{"id":"f","regexp":"func \\w+\\(","on_match":[
    {"action":"block","open":"{","close":"}","on_block":[
      {"action":"emit","data":{"fn":"$MATCH","lines":"$BLOCK_LINE_START-$BLOCK_LINE_END","body":"$BLOCK"}}]}]}]
}' "$BLK")
expect_contains "block action: lines=2-5" '"lines":"2-5"' "$OUT"
expect_contains "block action: body has both lines" 'a := 1' "$OUT"
expect_contains "block action: body has both lines (b)" 'b := 2' "$OUT"
rm -f "$BLK"

# ---------------------------------------------------------------------------
section "script: set algebra (set_difference / set_intersection / set_union)"

# All three actions over two maps (defaults), with both directions of diff.
OUT=$("$BIN" -s '{
  "variables":{
    "sa":{"type":"map","default":{"alpha":"1","beta":"1","gamma":"1"}},
    "sb":{"type":"map","default":{"beta":"1","delta":"1"}}
  },
  "scan":["nonexistent_glob"],
  "patterns":[{"id":"_x","regexp":"never_matches"}],
  "on_complete":[
    {"action":"set_difference",  "target":"diff_ab","a":"sa","b":"sb"},
    {"action":"set_difference",  "target":"diff_ba","a":"sb","b":"sa"},
    {"action":"set_intersection","target":"inter",  "a":"sa","b":"sb"},
    {"action":"set_union",       "target":"uni",    "a":"sa","b":"sb"},
    {"action":"for_each","var":"diff_ab","as":"x","do":[{"action":"emit","data":{"diff_ab":"$x"}}]},
    {"action":"for_each","var":"diff_ba","as":"x","do":[{"action":"emit","data":{"diff_ba":"$x"}}]},
    {"action":"for_each","var":"inter",  "as":"x","do":[{"action":"emit","data":{"inter":"$x"}}]},
    {"action":"for_each","var":"uni",    "as":"x","do":[{"action":"emit","data":{"uni":"$x"}}]}
  ]
}' | sort)
expect_eq "set ops: full output (sorted)" '{"diff_ab":"alpha"}
{"diff_ab":"gamma"}
{"diff_ba":"delta"}
{"inter":"beta"}
{"uni":"alpha"}
{"uni":"beta"}
{"uni":"delta"}
{"uni":"gamma"}' "$OUT"

# Lists as operands, with duplicate input that should be deduped.
OUT=$("$BIN" -s '{
  "variables":{
    "la":{"type":"list","default":["alpha","beta","gamma","beta"]},
    "lb":{"type":"list","default":["beta","delta"]}
  },
  "scan":["nonexistent_glob"],
  "patterns":[{"id":"_x","regexp":"never_matches"}],
  "on_complete":[
    {"action":"set_intersection","target":"inter","a":"la","b":"lb"},
    {"action":"set_union",       "target":"uni",  "a":"la","b":"lb"},
    {"action":"for_each","var":"inter","as":"x","do":[{"action":"emit","data":{"inter":"$x"}}]},
    {"action":"for_each","var":"uni",  "as":"x","do":[{"action":"emit","data":{"uni":"$x"}}]}
  ]
}' | sort)
expect_eq "set ops: list operands dedup" '{"inter":"beta"}
{"uni":"alpha"}
{"uni":"beta"}
{"uni":"delta"}
{"uni":"gamma"}' "$OUT"

# Mixed list+map and a missing variable (treated as empty set).
OUT=$("$BIN" -s '{
  "variables":{
    "la":{"type":"list","default":["alpha","beta","gamma"]},
    "mb":{"type":"map", "default":{"beta":"x","delta":"y"}}
  },
  "scan":["nonexistent_glob"],
  "patterns":[{"id":"_x","regexp":"never_matches"}],
  "on_complete":[
    {"action":"set_difference","target":"diff",   "a":"la","b":"mb"},
    {"action":"set_union",     "target":"with_nil","a":"la","b":"nope"},
    {"action":"for_each","var":"diff",    "as":"x","do":[{"action":"emit","data":{"diff":"$x"}}]},
    {"action":"for_each","var":"with_nil","as":"x","do":[{"action":"emit","data":{"with_nil":"$x"}}]}
  ]
}' | sort)
expect_eq "set ops: mixed list/map + missing var" '{"diff":"alpha"}
{"diff":"gamma"}
{"with_nil":"alpha"}
{"with_nil":"beta"}
{"with_nil":"gamma"}' "$OUT"

# Parser errors: missing required fields.
expect_exit "set_difference missing 'b' fails" 2 \
  "$BIN" -s '{"patterns":[{"id":"x","regexp":"x","on_match":[
    {"action":"set_difference","target":"o","a":"sa"}]}]}'
expect_exit "set_union missing 'a' fails" 2 \
  "$BIN" -s '{"patterns":[{"id":"x","regexp":"x","on_match":[
    {"action":"set_union","target":"o","b":"sb"}]}]}'

# Aliasing: target may name the same variable as `a` and/or `b`. The result
# must be computed from the pre-action contents, not from a destination that
# was cleared mid-flight.
OUT=$("$BIN" -s '{
  "variables":{
    "x":{"type":"list","default":["alpha","beta","gamma"]},
    "y":{"type":"list","default":["beta","delta"]}
  },
  "scan":["nonexistent_glob"],
  "patterns":[{"id":"_x","regexp":"never_matches"}],
  "on_complete":[
    {"action":"set_union","target":"x","a":"x","b":"y"},
    {"action":"for_each","var":"x","as":"e","do":[{"action":"emit","data":{"x":"$e"}}]}
  ]
}' | sort)
expect_eq "set ops: target aliases a (union)" '{"x":"alpha"}
{"x":"beta"}
{"x":"delta"}
{"x":"gamma"}' "$OUT"

OUT=$("$BIN" -s '{
  "variables":{
    "x":{"type":"list","default":["alpha","beta","gamma"]},
    "y":{"type":"list","default":["beta","delta"]}
  },
  "scan":["nonexistent_glob"],
  "patterns":[{"id":"_x","regexp":"never_matches"}],
  "on_complete":[
    {"action":"set_difference","target":"x","a":"x","b":"y"},
    {"action":"for_each","var":"x","as":"e","do":[{"action":"emit","data":{"x":"$e"}}]}
  ]
}' | sort)
expect_eq "set ops: target aliases a (difference)" '{"x":"alpha"}
{"x":"gamma"}' "$OUT"

OUT=$("$BIN" -s '{
  "variables":{
    "x":{"type":"list","default":["alpha","beta","gamma"]},
    "y":{"type":"list","default":["beta","delta"]}
  },
  "scan":["nonexistent_glob"],
  "patterns":[{"id":"_x","regexp":"never_matches"}],
  "on_complete":[
    {"action":"set_difference","target":"y","a":"x","b":"y"},
    {"action":"for_each","var":"y","as":"e","do":[{"action":"emit","data":{"y":"$e"}}]}
  ]
}' | sort)
expect_eq "set ops: target aliases b (difference)" '{"y":"alpha"}
{"y":"gamma"}' "$OUT"

OUT=$("$BIN" -s '{
  "variables":{
    "x":{"type":"list","default":["alpha","beta","gamma"]},
    "y":{"type":"list","default":["beta","delta"]}
  },
  "scan":["nonexistent_glob"],
  "patterns":[{"id":"_x","regexp":"never_matches"}],
  "on_complete":[
    {"action":"set_intersection","target":"x","a":"x","b":"y"},
    {"action":"for_each","var":"x","as":"e","do":[{"action":"emit","data":{"x":"$e"}}]}
  ]
}' | sort)
expect_eq "set ops: target aliases a (intersection)" '{"x":"beta"}' "$OUT"

# Edge case: a, b, and target all the same variable. Union/intersection are
# the deduped self; difference is empty.
OUT=$("$BIN" -s '{
  "variables":{"x":{"type":"list","default":["alpha","beta","alpha"]}},
  "scan":["nonexistent_glob"],
  "patterns":[{"id":"_x","regexp":"never_matches"}],
  "on_complete":[
    {"action":"set_intersection","target":"x","a":"x","b":"x"},
    {"action":"for_each","var":"x","as":"e","do":[{"action":"emit","data":{"x":"$e"}}]}
  ]
}' | sort)
expect_eq "set ops: target aliases both a and b (intersection)" '{"x":"alpha"}
{"x":"beta"}' "$OUT"

OUT=$("$BIN" -s '{
  "variables":{"x":{"type":"list","default":["alpha","beta"]}},
  "scan":["nonexistent_glob"],
  "patterns":[{"id":"_x","regexp":"never_matches"}],
  "on_complete":[
    {"action":"set_difference","target":"x","a":"x","b":"x"},
    {"action":"for_each","var":"x","as":"e","do":[{"action":"emit","data":{"x":"$e"}}]}
  ]
}')
expect_eq "set ops: target aliases both a and b (difference is empty)" "" "$OUT"

# ---------------------------------------------------------------------------
section "script: phases (cross-phase variable sharing)"

OUT=$(cd "$SAMPLE" && "$BIN" -s '{
  "variables":{"defs":{"type":"map"},"_n":{"type":"string"}},
  "phases":[
    {"id":"collect","scan":["**/*.go"],"exclude":["vendor"],
      "patterns":[{"id":"fn","regexp":"func \\w+","on_match":[
        {"action":"submatch","patterns":[
          {"id":"name","regexp":"\\w+","on_match":[{"action":"set","var":"_n","value":"$MATCH"}]}]},
        {"action":"map_set","target":"defs","key":"$_n","value":"$FILE"}]}]},
    {"id":"report","scan":["a.go"],
      "patterns":[{"id":"a","regexp":"package","on_match":[
        {"action":"for_each","var":"defs","key_as":"n","as":"f","do":[
          {"action":"emit","data":{"name":"$n","file":"$f"}}]},
        {"action":"stop"}]}]}
  ]
}' | sort)
expect_eq "phases: cross-phase symbol resolution" '{"file":"a.go","name":"main"}
{"file":"b.go","name":"helper"}' "$OUT"

# ---------------------------------------------------------------------------
section "-p block extraction (-block-open / -block-close)"

BLK="$HERE/_tmp_block2.go"
printf 'package main\nfunc helper() {\n    a := 1\n}\nfunc main() { return }\n' > "$BLK"

# -o emits the full block (signature + body)
OUT=$("$BIN" -p 'func \w+\(' -block-open '{' -block-close '}' -o "$BLK")
expect_contains "-o block: helper full"  "func helper() {" "$OUT"
expect_contains "-o block: main full"    "func main() { return }" "$OUT"

# -j adds block fields
OUT=$("$BIN" -p 'func \w+\(' -block-open '{' -block-close '}' -j "$BLK")
expect_contains "-j block: block_line_start" '"block_line_start":2' "$OUT"
expect_contains "-j block: block_line_end"   '"block_line_end":4'   "$OUT"

# -format $BLOCK_LINE_END / $BLOCK_LINE_START (single-quoted to keep bash
# from expanding the variable)
OUT=$("$BIN" -p 'func \w+\(' -block-open '{' -block-close '}' \
      -format '$LINE:$BLOCK_LINE_END' "$BLK")
expect_contains "-format BLOCK_LINE_END"  "2:4" "$OUT"
rm -f "$BLK"

# ---------------------------------------------------------------------------
section "UTF-8 / Unicode"

# Default UTF-8 mode
OUT=$(printf 'привет мир\n' | "$BIN" -p 'привет' -o)
expect_eq "literal Cyrillic" "привет" "$OUT"

OUT=$(printf '你好世界\n' | "$BIN" -p '你好' -o)
expect_eq "literal CJK" "你好" "$OUT"

OUT=$(printf 'аб\n' | "$BIN" -p '^.{2}$' -o)
expect_eq ". codepoint-aware (2 chars = 4 bytes)" "аб" "$OUT"

OUT=$(printf 'CAFÉ café\n' | "$BIN" -pi 'café' -o)
expect_lines "case-fold Unicode: CAFÉ↔café" 2 "$OUT"

OUT=$(printf 'ПРИВЕТ привет\n' | "$BIN" -pi 'привет' -o)
expect_lines "case-fold Cyrillic" 2 "$OUT"

# \w default ASCII
OUT=$(printf 'café\n' | "$BIN" -p '\w+' -o)
expect_eq "\\w default ASCII (matches 'caf')" "caf" "$OUT"

# byte-mode
OUT=$(printf 'аб\n' | "$BIN" -p '^.{2}$' -no-utf8 -o)
expect_lines "-no-utf8: ^.{2}\$ doesn't match 4-byte input" 0 "$OUT"

OUT=$(printf 'аб\n' | "$BIN" -p '^.{4}$' -no-utf8 -o)
expect_eq "-no-utf8: ^.{4}\$ matches 4 bytes" "аб" "$OUT"

# invalid UTF-8 tolerated (no error, partial match emitted)
OUT=$(printf '\xe9\xe0\xea hello world\n' | "$BIN" -p 'hello' -o)
expect_eq "invalid UTF-8: still finds 'hello'" "hello" "$OUT"

# script per-pattern utf8 toggle
OUT=$(printf 'café\n' | "$BIN" -s '{"patterns":[{"id":"a","regexp":"\\w+","utf8":false}]}')
expect_contains "script per-pattern utf8:false" '"match":"caf"' "$OUT"

# ---------------------------------------------------------------------------
section "byte budgets (-max-*-bytes)"
BUDGET_FIX="$HERE/_tmp_budget"
mkdir -p "$BUDGET_FIX"
printf 'short\nthis is a longer line with TODO that is intentionally over the cap\nanother TODO short\n' > "$BUDGET_FIX/a.txt"

# match truncation: $MATCH cut to N bytes, match_truncated:true marker present
OUT=$("$BIN" -p '.*TODO.*' -max-match-bytes 10 "$BUDGET_FIX/a.txt")
expect_contains "max-match-bytes: match_truncated flag" '"match_truncated":true' "$OUT"
expect_contains "max-match-bytes: top-level truncated flag" '"truncated":true' "$OUT"

# context truncation
OUT=$("$BIN" -p TODO -max-context-bytes 15 "$BUDGET_FIX/a.txt")
expect_contains "max-context-bytes: context_truncated flag" '"context_truncated":true' "$OUT"

# output cap stops scan and emits info record
OUT=$("$BIN" -p TODO -max-output-bytes 100 "$BUDGET_FIX/a.txt")
expect_contains "max-output-bytes: info record" '"info":"output_truncated"' "$OUT"

# UTF-8 boundary preserved (15-byte cut on 'café TODO ééééé' lands on é boundary)
OUT=$(printf 'café TODO éééééééé end\n' | "$BIN" -p TODO -max-context-bytes 15 -format '$CONTEXT')
expect_eq "max-context-bytes: UTF-8 boundary" 'café TODO éé' "$OUT"

# script-mode budget: same flags as top-level fields
OUT=$("$BIN" -s "{\"scan\":[\"$BUDGET_FIX/a.txt\"],\"max_context_bytes\":15,\"patterns\":[{\"id\":\"t\",\"regexp\":\"TODO\"}]}")
expect_contains "script max_context_bytes: truncation flag" '"truncated":true' "$OUT"

OUT=$("$BIN" -s "{\"scan\":[\"$BUDGET_FIX/a.txt\"],\"max_output_bytes\":80,\"patterns\":[{\"id\":\"t\",\"regexp\":\"TODO\"}]}")
expect_contains "script max_output_bytes: info record" '"info":"output_truncated"' "$OUT"

rm -rf "$BUDGET_FIX"

# ---------------------------------------------------------------------------
section "capture-group extraction (-extract / extract:[...])"
EXT_FIX="$HERE/_tmp_extract"
mkdir -p "$EXT_FIX"
cat > "$EXT_FIX/a.go" <<'EOF'
package main
func Foo(x int) {}
func Bar(y string, z int) {}
EOF

# CLI: extract surfaces capture groups in JSONL
OUT=$("$BIN" -p 'func\s+(\w+)\(([^)]*)\)' -extract name,args "$EXT_FIX/a.go")
expect_contains "extract CLI: name=Foo" '"extracted":{"name":"Foo","args":"x int"}' "$OUT"
expect_contains "extract CLI: name=Bar" '"extracted":{"name":"Bar","args":"y string, z int"}' "$OUT"

# CLI: format template with $EXTRACT_NAME / $EXTRACT_ARGS
OUT=$("$BIN" -p 'func\s+(\w+)\(([^)]*)\)' -extract name,args -format '$EXTRACT_NAME|$EXTRACT_ARGS' "$EXT_FIX/a.go")
expect_contains "extract format: Foo" 'Foo|x int' "$OUT"
expect_contains "extract format: Bar" 'Bar|y string, z int' "$OUT"

# CLI: -extract before any -p is an error
OUT=$("$BIN" -extract foo "$EXT_FIX/a.go" 2>&1) ; RC=$?
expect_contains "extract: error before -p" "must follow a -p" "$OUT"
[[ "$RC" == "2" ]] && report ok "extract: exit 2 on misuse" || report fail "extract: misuse exit code"

# Script: per-pattern extract surfaces in default record
OUT=$("$BIN" -s "{\"scan\":[\"$EXT_FIX/a.go\"],\"patterns\":[{\"id\":\"fn\",\"regexp\":\"func\\\\s+(\\\\w+)\\\\(([^)]*)\\\\)\",\"extract\":[\"name\",\"args\"]}]}")
expect_contains "extract script: extracted Foo" '"extracted":{"args":"x int","name":"Foo"}' "$OUT"

# Script: $EXTRACT_NAME in custom data
OUT=$("$BIN" -s "{\"scan\":[\"$EXT_FIX/a.go\"],\"patterns\":[{\"id\":\"fn\",\"regexp\":\"func\\\\s+(\\\\w+)\\\\(([^)]*)\\\\)\",\"extract\":[\"name\",\"args\"],\"on_match\":[{\"action\":\"emit\",\"data\":{\"name\":\"\$EXTRACT_NAME\",\"args\":\"\$EXTRACT_ARGS\"}}]}]}")
expect_contains "extract script: \$EXTRACT_NAME in data" '"args":"x int","name":"Foo"' "$OUT"

rm -rf "$EXT_FIX"

# ---------------------------------------------------------------------------
section "enclosing-scope (-scope / scope:)"
SCOPE_FIX="$HERE/_tmp_scope"
mkdir -p "$SCOPE_FIX"
cat > "$SCOPE_FIX/a.go" <<'EOF'
package main

func Outer() {
	x := 1
	if x == 1 {
		// TODO inner
	}
}

func Helper(s string) string {
	return s + " hi"  // TODO comment
}
EOF

# CLI: scope=go via auto detection
OUT=$("$BIN" -p TODO -scope auto "$SCOPE_FIX/a.go")
expect_contains "scope auto: TODO in Outer" '"enclosing":{"name":"Outer","kind":"func"' "$OUT"
expect_contains "scope auto: TODO in Helper" '"enclosing":{"name":"Helper","kind":"func"' "$OUT"

# CLI: explicit -scope=go
OUT=$("$BIN" -p TODO -scope go "$SCOPE_FIX/a.go")
expect_contains "scope go: enclosing field" '"enclosing"' "$OUT"

# CLI: format tokens
OUT=$("$BIN" -p TODO -scope go -format '$LINE:$ENCLOSING_NAME' "$SCOPE_FIX/a.go")
expect_contains "scope format: line 6 in Outer" '6:Outer' "$OUT"
expect_contains "scope format: line 11 in Helper" '11:Helper' "$OUT"

# CLI: custom -scope-pattern
OUT=$("$BIN" -p TODO -scope-pattern 'func\s+(\w+)' -scope-open '{' -scope-close '}' -scope-kind myfn "$SCOPE_FIX/a.go")
expect_contains "scope custom: myfn kind" '"kind":"myfn"' "$OUT"

# Script: top-level scope as string
OUT=$("$BIN" -s "{\"scan\":[\"$SCOPE_FIX/a.go\"],\"scope\":\"go\",\"patterns\":[{\"id\":\"t\",\"regexp\":\"TODO\"}]}")
expect_contains "scope script string: enclosing Outer" '"enclosing":{"kind":"func","line_end":8,"line_start":3,"name":"Outer"}' "$OUT"

# Script: $ENCLOSING_NAME in custom data
OUT=$("$BIN" -s "{\"scan\":[\"$SCOPE_FIX/a.go\"],\"scope\":\"go\",\"patterns\":[{\"id\":\"t\",\"regexp\":\"TODO\",\"on_match\":[{\"action\":\"emit\",\"data\":{\"line\":\"\$LINE\",\"fn\":\"\$ENCLOSING_NAME\"}}]}]}")
expect_contains "scope script \$ENCLOSING_NAME" '"fn":"Outer"' "$OUT"

# Script: scope as object (custom)
OUT=$("$BIN" -s "{\"scan\":[\"$SCOPE_FIX/a.go\"],\"scope\":{\"pattern\":\"func\\\\s+(\\\\w+)\",\"open\":\"{\",\"close\":\"}\",\"kind\":\"f\"},\"patterns\":[{\"id\":\"t\",\"regexp\":\"TODO\"}]}")
expect_contains "scope script object: kind=f" '"kind":"f"' "$OUT"

# Match outside any scope: enclosing field absent (graceful)
cat > "$SCOPE_FIX/b.go" <<'EOF'
package main
// TODO at file scope, not in any function
EOF
OUT=$("$BIN" -p TODO -scope go "$SCOPE_FIX/b.go")
expect_not_contains "scope: no enclosing when outside fn" '"enclosing"' "$OUT"

rm -rf "$SCOPE_FIX"

# ---------------------------------------------------------------------------
section "pattern relations (-near / -far)"
REL_FIX="$HERE/_tmp_rel"
mkdir -p "$REL_FIX"
cat > "$REL_FIX/a.go" <<'EOF'
package main

func A() {
	mu.Lock()
	defer mu.Unlock()
}

func B() {
	defer cleanup()
	mu.Lock()
}

func C() {
	defer onlyDefer()
}

func D() {
	mu.Lock()
}
EOF

# -near: defer near Lock within 2 lines (lines 5 & 9 keep; 14 drops)
OUT=$("$BIN" -p 'defer\b' -p 'Lock\(\)' -near p0:p1:2 -format '$LINE/$PAT_ID' "$REL_FIX/a.go")
expect_contains "near: line 5 defer kept" '5/p0' "$OUT"
expect_contains "near: line 9 defer kept" '9/p0' "$OUT"
expect_not_contains "near: line 14 defer dropped" '14/p0' "$OUT"

# -far: defer with NO Lock within 2 lines (only line 14 qualifies)
OUT=$("$BIN" -p 'defer\b' -p 'Lock\(\)' -far p0:p1:2 -format '$LINE/$PAT_ID' "$REL_FIX/a.go")
expect_not_contains "far: line 5 defer dropped" '5/p0' "$OUT"
expect_not_contains "far: line 9 defer dropped" '9/p0' "$OUT"
expect_contains "far: line 14 defer kept" '14/p0' "$OUT"

# K=0 means same line (line 5: "defer mu.Unlock()" — defer & Unlock on same line)
OUT=$("$BIN" -p 'defer\b' -p 'Unlock' -near p0:p1:0 -format '$LINE/$PAT_ID' "$REL_FIX/a.go")
expect_contains "near K=0: line 5 defer kept" '5/p0' "$OUT"
expect_not_contains "near K=0: line 9 defer dropped (no Unlock there)" '9/p0' "$OUT"

# Bad relation: unknown pattern → exit 2
OUT=$("$BIN" -p 'defer\b' -near p0:zzz:2 "$REL_FIX/a.go" 2>&1) ; RC=$?
expect_contains "relation: unknown pattern error" "unknown pattern" "$OUT"
[[ "$RC" == "2" ]] && report ok "relation: exit 2 on bad pattern" || report fail "relation: bad pattern exit"

# Script form
OUT=$("$BIN" -s "{\"scan\":[\"$REL_FIX/a.go\"],\"relations\":[{\"kind\":\"near\",\"a\":\"d\",\"b\":\"l\",\"lines\":2}],\"patterns\":[{\"id\":\"d\",\"regexp\":\"defer\\\\b\"},{\"id\":\"l\",\"regexp\":\"Lock\\\\(\\\\)\"}]}")
expect_contains "script near: line 5 defer kept" '"line":5,"match":"defer"' "$OUT"
expect_not_contains "script near: line 14 defer dropped" '"line":14,"match":"defer"' "$OUT"

rm -rf "$REL_FIX"

# ---------------------------------------------------------------------------
section "sample mode (-sample N)"
SAMP_FIX="$HERE/_tmp_sample"
mkdir -p "$SAMP_FIX"
for i in 1 2 3 4 5; do
    printf "// TODO line one\n// TODO line two\n// TODO line three\n" > "$SAMP_FIX/file_$i.go"
done

# 5 files × 3 matches = 15 total. -sample 5 should return one per file.
OUT=$("$BIN" -p TODO -sample 5 -format '$FILE:$LINE' "$SAMP_FIX"/file_*.go)
expect_lines "sample 5: exactly 5 records" 5 "$OUT"
COUNT=$(printf '%s\n' "$OUT" | awk -F: '{print $1}' | sort -u | awk 'END{print NR}')
[[ "$COUNT" == "5" ]] && report ok "sample 5: 5 distinct files" || report fail "sample 5: distinct files (got $COUNT)"

# Sample > total → return all matches available.
OUT=$("$BIN" -p TODO -sample 100 -format '$LINE' "$SAMP_FIX"/file_*.go)
expect_lines "sample 100: returns all 15" 15 "$OUT"

# Sample with incompatible output mode (-f) → exit 2
OUT=$("$BIN" -p TODO -sample 5 -f "$SAMP_FIX"/file_1.go 2>&1) ; RC=$?
expect_contains "sample: -f rejected" "requires a per-match output mode" "$OUT"
[[ "$RC" == "2" ]] && report ok "sample: exit 2 on -f" || report fail "sample: -f exit code"

rm -rf "$SAMP_FIX"

# ---------------------------------------------------------------------------
section "binary file skip"
HEX_FIX="$HERE/_tmp_bin"
printf 'TODO\x00rest of file with TODO\n' > "$HEX_FIX"
OUT=$("$BIN" -p TODO -j "$HEX_FIX")
expect_lines "binary file (NUL in first 512 bytes) skipped" 0 "$OUT"
rm -f "$HEX_FIX"

# ---------------------------------------------------------------------------
section "brace globs ({a,b} alternation)"
BR_FIX="$HERE/_tmp_brace"
mkdir -p "$BR_FIX/sub"
printf 'needle\n' > "$BR_FIX/x.aaa"
printf 'needle\n' > "$BR_FIX/x.bbb"
printf 'needle\n' > "$BR_FIX/x.ccc"
printf 'needle\n' > "$BR_FIX/sub/y.aaa"

OUT=$("$BIN" -p needle -f -glob "$BR_FIX/*.{aaa,bbb}")
expect_lines "brace suffix picks 2 of 3 extensions" 2 "$OUT"

OUT=$("$BIN" -p needle -f -glob "$BR_FIX/**/*.{aaa,ccc}")
expect_lines "brace + ** recursion: 3 files" 3 "$OUT"

OUT=$("$BIN" -p needle -f -glob "$BR_FIX/{sub,nosuchdir}/*.aaa")
expect_lines "brace over a directory segment" 1 "$OUT"

OUT=$("$BIN" -p needle -f -glob "$BR_FIX/*.{aaa,{bbb,ccc}}")
expect_lines "nested braces expand fully" 3 "$OUT"

OUT=$("$BIN" -p needle -f -glob "$BR_FIX/*.{aaa,bbb,ccc}" -exclude '*.{bbb,ccc}')
expect_lines "braces in -exclude" 1 "$OUT"
expect_contains "braces in -exclude keeps aaa" "x.aaa" "$OUT"

rm -rf "$BR_FIX"

# ---------------------------------------------------------------------------
section "block extraction: opener inside the match"
BS_FIX="$HERE/_tmp_blockstart"
mkdir -p "$BS_FIX"

# Anchor regex ends with the block's own opener (`^@article\{`): the block
# must be the whole entry, not the first nested {...} value inside it.
printf '@article{k1,\n  author = {A. Uthor},\n  title = {T}\n}\n@article{k2,\n  title = {No Author}\n}\n' > "$BS_FIX/refs.bib"
cat > "$BS_FIX/bib.hpr" <<'EOF'
{"patterns":[{"id":"art","regexp":"^@article\\{","on_match":[
  {"action":"block","open":"{","close":"}","on_block":[
    {"action":"submatch","text":"$BLOCK","patterns":[
      {"id":"a","regexp":"author\\s*=","absent":true,"on_match":[
        {"action":"emit","data":{"line":"$LINE"}}]}]}]}]}]}
EOF
OUT=$("$BIN" -script "$BS_FIX/bib.hpr" -- "$BS_FIX/refs.bib")
expect_lines "bibtex: exactly one entry flagged" 1 "$OUT"
expect_contains "bibtex: the author-less entry (line 5)" '"line":5' "$OUT"

# Opener at the START of the match (PEM header contains `-----BEGIN`): each
# key pairs with its own block; a lone key must not be silently dropped.
printf -- '-----BEGIN RSA PRIVATE KEY-----\nAAAA\n-----END RSA PRIVATE KEY-----\n' > "$BS_FIX/one.pem"
printf -- '-----BEGIN A KEY-----\nAAAA\n-----END A KEY-----\nx\n-----BEGIN B KEY-----\nBBBB\n-----END B KEY-----\n' > "$BS_FIX/two.pem"
cat > "$BS_FIX/pem.hpr" <<'EOF'
{"patterns":[{"id":"pem","regexp":"-----BEGIN [A-Z ]*KEY-----","on_match":[
  {"action":"block","open":"-----BEGIN","close":"-----END","on_block":[
    {"action":"emit","data":{"s":"$BLOCK_LINE_START","e":"$BLOCK_LINE_END","n":"$BLOCK_LINE_COUNT"}}]}]}]}
EOF
OUT=$("$BIN" -script "$BS_FIX/pem.hpr" -- "$BS_FIX/one.pem")
expect_eq "pem: single key found, spans lines 1-3" '{"e":3,"n":3,"s":1}' "$OUT"
OUT=$("$BIN" -script "$BS_FIX/pem.hpr" -- "$BS_FIX/two.pem")
expect_lines "pem: two keys, two records" 2 "$OUT"
expect_contains "pem: first key block" '"s":1' "$OUT"
expect_contains "pem: second key block" '"s":5' "$OUT"

# Opener after match-end (the classic case) is unchanged.
printf 'func main() {\n\tdo()\n}\n' > "$BS_FIX/m.go"
OUT=$("$BIN" -p 'func \w+\(' -block-open '{' -block-close '}' -format '$BLOCK_LINE_START-$BLOCK_LINE_END' "$BS_FIX/m.go")
expect_eq "go func body pairing unchanged" "1-3" "$OUT"
OUT=$("$BIN" -p 'func \w+\(' -block-open '{' -block-close '}' -format '$BLOCK_LINE_COUNT/$BLOCK_BYTE_COUNT' "$BS_FIX/m.go")
expect_eq "-format \$BLOCK_LINE_COUNT/\$BLOCK_BYTE_COUNT" "3/9" "$OUT"

rm -rf "$BS_FIX"

# ---------------------------------------------------------------------------
section "scope packs: C-family pairing + keyword filter"
SC_FIX="$HERE/_tmp_scope"
mkdir -p "$SC_FIX"
printf 'void f(int x) {\n  if (a) {\n    b();\n  }\n  c();\n}\n' > "$SC_FIX/t.c"
printf 'void g(void) { a(); }\n' > "$SC_FIX/t2.c"

# The C anchor regex ends in `{`, so the scope must still span the whole
# function — not end at the first inner block's close brace.
OUT=$("$BIN" -p 'c\(\)' -scope c -format '$ENCLOSING_NAME:$ENCLOSING_LINE_END' "$SC_FIX/t.c")
expect_eq "match after an inner block is still inside f" "f:6" "$OUT"

# `if (a) {` fits the anchor shape but must not become a scope.
OUT=$("$BIN" -p 'b\(\)' -scope c -format '$ENCLOSING_NAME:$ENCLOSING_LINE_END' "$SC_FIX/t.c")
expect_eq "control-flow keyword is not a scope" "f:6" "$OUT"

# Function body with no inner braces (opener inside the anchor match).
OUT=$("$BIN" -p 'a\(\)' -scope c -format '$ENCLOSING_NAME' "$SC_FIX/t2.c")
expect_eq "brace-free one-line body gets a scope" "g" "$OUT"

# Unknown pack names error instead of silently disabling annotation.
OUT=$("$BIN" -p 'a\(\)' -scope py "$SC_FIX/t2.c" 2>&1) ; RC=$?
expect_contains "unknown -scope pack message" "unknown -scope pack 'py'" "$OUT"
[[ "$RC" == "2" ]] && report ok "unknown -scope pack exit 2" || report fail "unknown -scope pack exit 2 (got $RC)"

OUT=$("$BIN" -s "{\"scope\":\"py\",\"patterns\":[{\"id\":\"t\",\"regexp\":\"a\"}]}" "$SC_FIX/t2.c" 2>&1) ; RC=$?
expect_contains "script scope pack message" "unknown scope pack 'py'" "$OUT"
[[ "$RC" == "2" ]] && report ok "script scope pack exit 2" || report fail "script scope pack exit 2 (got $RC)"

rm -rf "$SC_FIX"

# ---------------------------------------------------------------------------
section "map_append / map_unique_append"
MA_FIX="$HERE/_tmp_mapappend"
mkdir -p "$MA_FIX"
printf 'tid=aa x\ntid=aa y\n' > "$MA_FIX/s1.log"
printf 'tid=aa z\ntid=bb q\n' > "$MA_FIX/s2.log"
cat > "$MA_FIX/ma.hpr" <<'EOF'
{"variables":{"m":{"type":"map"}},
 "patterns":[{"id":"t","regexp":"tid=(\\w+)","extract":["tid"],"on_match":[
   {"action":"map_unique_append","target":"m","key":"$EXTRACT_TID","value":"$FILE"}]}],
 "on_complete":[{"action":"for_each","var":"m","key_as":"k","as":"v","do":[
   {"action":"emit","data":{"tid":"$k","files":"$v"}}]}]}
EOF
OUT=$("$BIN" -script "$MA_FIX/ma.hpr" -- "$MA_FIX/s1.log" "$MA_FIX/s2.log")
expect_lines "one row per tid" 2 "$OUT"
expect_contains "files emitted as a JSON array" '"files":["' "$OUT"
N=$(printf '%s' "$OUT" | grep -o 's1\.log' | grep -c '^')
[[ "$N" == "1" ]] && report ok "unique_append dedupes repeat hits" || report fail "unique_append dedupes repeat hits (s1.log seen $N times)"
AA=$(printf '%s\n' "$OUT" | grep '"tid":"aa"')
case "$AA" in
    *s1.log*s2.log*) report ok "tid aa lists both files" ;;
    *) report fail "tid aa lists both files" ; printf '%s\n' "$AA" | sed 's/^/      | /' ;;
esac
rm -rf "$MA_FIX"

# ---------------------------------------------------------------------------
section "skip/limit paging"
PG_FIX="$HERE/_tmp_paging"
mkdir -p "$PG_FIX"
printf 'T one\nT two\nT three\nT four\nT five\n' > "$PG_FIX/t.txt"
OUT=$("$BIN" -s "{\"skip\":2,\"limit\":2,\"patterns\":[{\"id\":\"t\",\"regexp\":\"T \\\\w+\"}]}" "$PG_FIX/t.txt")
expect_lines "skip 2 limit 2 emits two records" 2 "$OUT"
expect_contains "page starts at record 3" '"line":3' "$OUT"
expect_contains "page ends at record 4" '"line":4' "$OUT"
expect_not_contains "record 5 stays unemitted" '"line":5' "$OUT"
rm -rf "$PG_FIX"

# ---------------------------------------------------------------------------
section "hidden paths: explicit base scanned, recursive skip"
HID_FIX="$HERE/_tmp_hidden"
mkdir -p "$HID_FIX/.github/workflows"
printf 'uses: x@main\n' > "$HID_FIX/.github/workflows/ci.yml"
OUT=$("$BIN" -p 'uses:' -f -glob "$HID_FIX/**/*.yml")
expect_lines "recursive walk skips hidden dirs" 0 "$OUT"
OUT=$("$BIN" -p 'uses:' -f -glob "$HID_FIX/.github/workflows/*.{yml,yaml}")
expect_lines "explicitly named hidden base is scanned" 1 "$OUT"
rm -rf "$HID_FIX"

# ---------------------------------------------------------------------------
section "file-list input (-files-from / -files0-from)"
FL_FIX="$HERE/_tmp_fileslist"
mkdir -p "$FL_FIX"
printf 'needle a\n' > "$FL_FIX/a.txt"
printf 'needle b\n' > "$FL_FIX/b.txt"
printf 'needle c\n' > "$FL_FIX/c.txt"
printf 'needle s\n' > "$FL_FIX/has space.txt"
printf 'needle br\n' > "$FL_FIX/lit{x}.txt"

printf '%s\n%s\n' "$FL_FIX/a.txt" "$FL_FIX/b.txt" > "$FL_FIX/list.txt"
OUT=$("$BIN" -p needle -f -files-from "$FL_FIX/list.txt")
expect_lines "newline list scans only listed files" 2 "$OUT"
expect_not_contains "unlisted file not scanned" "c.txt" "$OUT"

OUT=$(printf '%s\n' "$FL_FIX/a.txt" | "$BIN" -p needle -f -files-from -)
expect_lines "list read from stdin" 1 "$OUT"

OUT=$(printf '%s\0%s\0' "$FL_FIX/has space.txt" "$FL_FIX/lit{x}.txt" | "$BIN" -p needle -f -files0-from -)
expect_lines "NUL list: space + brace filenames" 2 "$OUT"
expect_contains "brace filename stays literal (no glob)" "lit{x}.txt" "$OUT"

printf '%s\r\n' "$FL_FIX/a.txt" > "$FL_FIX/crlf.txt"
OUT=$("$BIN" -p needle -f -files-from "$FL_FIX/crlf.txt")
expect_lines "CRLF list: trailing CR stripped" 1 "$OUT"

OUT=$("$BIN" -p needle -f -files-from "$FL_FIX/list.txt" -exclude 'b.txt')
expect_lines "-exclude applies to listed files" 1 "$OUT"

ERR=$(printf '%s\n%s\n' "$FL_FIX/a.txt" "$FL_FIX/missing.txt" | "$BIN" -p needle -f -files-from - 2>&1 >/dev/null)
OUT=$(printf '%s\n%s\n' "$FL_FIX/a.txt" "$FL_FIX/missing.txt" | "$BIN" -p needle -f -files-from - 2>/dev/null) ; RC=$?
expect_contains "missing listed path warns on stderr" "cannot access" "$ERR"
expect_lines "missing path skipped, scan continues" 1 "$OUT"
[[ "$RC" == "0" ]] && report ok "missing path doesn't affect exit code" || report fail "missing path exit (got $RC)"

# Script mode: the list overrides the script's scan, like positional paths.
cat > "$FL_FIX/s.hpr" <<'EOF'
{"scan":["/nonexistent/**/*.zz"],"patterns":[{"id":"n","regexp":"needle"}]}
EOF
OUT=$("$BIN" -script "$FL_FIX/s.hpr" -files-from "$FL_FIX/list.txt")
expect_lines "script mode: list overrides scan" 2 "$OUT"

# Two stdin lists can't both be read.
OUT=$(printf 'x' | "$BIN" -p needle -files-from - -files0-from - 2>&1) ; RC=$?
expect_contains "double stdin list rejected" "may read from stdin" "$OUT"
[[ "$RC" == "2" ]] && report ok "double stdin list exit 2" || report fail "double stdin list exit (got $RC)"

rm -rf "$FL_FIX"

# ---------------------------------------------------------------------------
section "named patterns (-name)"
NP_FIX="$HERE/_tmp_name"
mkdir -p "$NP_FIX"
printf 'WARN a\nERROR b\nx\nx\nWARN c\n' > "$NP_FIX/log.txt"

OUT=$("$BIN" -p WARN -name warn -p ERROR -name err -format '$PAT_ID:$LINE' "$NP_FIX/log.txt")
expect_contains "named id in \$PAT_ID" "warn:1" "$OUT"
expect_contains "second named id" "err:2" "$OUT"

OUT=$("$BIN" -p WARN -name warn -p ERROR -name err -near warn:err:1 -format '$PAT_ID:$LINE' "$NP_FIX/log.txt")
expect_contains "relation by name keeps warn:1" "warn:1" "$OUT"
expect_not_contains "relation by name drops warn:5" "warn:5" "$OUT"

OUT=$("$BIN" -p WARN -name x -p ERROR -name x "$NP_FIX/log.txt" 2>&1) ; RC=$?
expect_contains "duplicate name rejected" "duplicate pattern id" "$OUT"
[[ "$RC" == "2" ]] && report ok "duplicate name exit 2" || report fail "duplicate name exit (got $RC)"

OUT=$("$BIN" -p WARN -p ERROR -name p0 "$NP_FIX/log.txt" 2>&1) ; RC=$?
expect_contains "auto-id collision rejected" "duplicate pattern id 'p0'" "$OUT"

OUT=$("$BIN" -p WARN -name warn -p ERROR -name err -llm "$NP_FIX/log.txt")
expect_contains "-llm shows the name tag" "[warn]" "$OUT"
rm -rf "$NP_FIX"

# ---------------------------------------------------------------------------
section "informative -llm output (-desc legend / no-matches footer)"
LD_FIX="$HERE/_tmp_llmdesc"
mkdir -p "$LD_FIX"
printf 'WARN a\nERROR b\nx\nWARN c\n' > "$LD_FIX/log.txt"

# --- zero-match footer
OUT=$("$BIN" -p WARN -name warn -p NOPE_ZZZ -name ghost -llm "$LD_FIX/log.txt")
expect_contains "-llm zero-match footer names silent pattern" "--- no matches: ghost (1 of 2 patterns) ---" "$OUT"
OUT=$("$BIN" -p WARN -name warn -p ERROR -name err -llm "$LD_FIX/log.txt")
expect_not_contains "-llm no footer when all patterns match" "no matches" "$OUT"
OUT=$("$BIN" -p NOPE_A -p NOPE_B -llm "$LD_FIX/log.txt" ; echo "rc=$?")
expect_contains "-llm all-zero footer lists every pattern" "--- no matches: p0, p1 (2 of 2 patterns) ---" "$OUT"
expect_contains "-llm all-zero still exits 1" "rc=1" "$OUT"
OUT=$("$BIN" -p WARN -p NOPE_ZZZ -name ghost -llm -limit 1 "$LD_FIX/log.txt")
expect_contains "-llm footer qualified when limit stops scan" "no matches (scan stopped early): ghost" "$OUT"
OUT=$("$BIN" -p WARN -p NOPE_ZZZ -name ghost -elide "$LD_FIX/log.txt")
expect_contains "-elide zero-match footer" "--- no matches: ghost (1 of 2 patterns) ---" "$OUT"
OUT=$("$BIN" -p WARN -p NOPE_ZZZ "$LD_FIX/log.txt")
expect_not_contains "JSONL mode has no zero-match footer" "no matches" "$OUT"

# --- -desc query legend
OUT=$("$BIN" -p WARN -name warn -desc 'warning lines' -p ERROR -llm "$LD_FIX/log.txt")
expect_contains "-desc legend described pattern" "  warn — warning lines" "$OUT"
expect_contains "-desc legend regexp fallback" "  p1 — /ERROR/" "$OUT"
FIRST=$(printf '%s\n' "$OUT" | head -1)
expect_eq "-desc header is the first line" "query: 2 patterns over $LD_FIX/log.txt" "$FIRST"
OUT=$("$BIN" -p WARN -p ERROR -llm "$LD_FIX/log.txt")
expect_not_contains "no header without -desc" "query:" "$OUT"
OUT=$("$BIN" -p WARN -desc 'warning lines' "$LD_FIX/log.txt")
expect_not_contains "JSONL mode has no header" "query:" "$OUT"
OUT=$("$BIN" -p NOPE_ZZZ -desc 'missing thing' -llm "$LD_FIX/log.txt")
expect_contains "header still prints before all-zero footer" "query: 1 pattern over" "$OUT"
expect_contains "singular footer wording" "(1 of 1 pattern) ---" "$OUT"
OUT=$("$BIN" -p WARN -desc 'warning lines' -hotspots 1 -llm "$LD_FIX/log.txt")
expect_contains "-hotspots -llm prints header before rows" "query: 1 pattern over" "$OUT"

# --- -desc validation
OUT=$("$BIN" -desc x -p WARN "$LD_FIX/log.txt" 2>&1) ; RC=$?
expect_contains "-desc before pattern rejected" "-desc must follow" "$OUT"
[[ "$RC" == "2" ]] && report ok "-desc before pattern exit 2" || report fail "-desc before pattern exit (got $RC)"
OUT=$("$BIN" -p WARN -desc a -desc b "$LD_FIX/log.txt" 2>&1) ; RC=$?
expect_contains "-desc repeated rejected" "repeated for the same pattern" "$OUT"
OUT=$("$BIN" -p WARN -desc '' "$LD_FIX/log.txt" 2>&1) ; RC=$?
expect_contains "-desc empty rejected" "empty description" "$OUT"

# --- patterns-from description field
cat > "$LD_FIX/rules.jsonl" <<'EOF'
{"id":"warnish","regexp":"WARN","description":"warning lines"}
{"id":"errish","regexp":"ERROR"}
EOF
OUT=$("$BIN" -patterns-from "$LD_FIX/rules.jsonl" -llm "$LD_FIX/log.txt")
expect_contains "rule-file description in legend" "  warnish — warning lines" "$OUT"
expect_contains "rule-file undescribed pattern falls back to regexp" "  errish — /ERROR/" "$OUT"
printf '{"id":"x","regexp":"y","description":5}\n' > "$LD_FIX/bad.jsonl"
OUT=$("$BIN" -patterns-from "$LD_FIX/bad.jsonl" -llm "$LD_FIX/log.txt" 2>&1) ; RC=$?
expect_contains "rule-file description type-checked" "'description' must be a string" "$OUT"
[[ "$RC" == "2" ]] && report ok "rule-file bad description exit 2" || report fail "rule-file bad description exit (got $RC)"
rm -rf "$LD_FIX"

# ---------------------------------------------------------------------------
section "per-match role tags (def/comment/string/import)"
RT_FIX="$HERE/_tmp_roles"
mkdir -p "$RT_FIX"
cat > "$RT_FIX/a.go" <<'EOF'
package main

import "fmt"

// TODO comment hit
func Target() {
	s := "TODO in string"
	u := "http://x/TODO"
	fmt.Println(s, u) // trailing TODO note
}
EOF

OUT=$("$BIN" -p TODO -llm "$RT_FIX/a.go")
expect_contains "comment tag" "// TODO comment hit  [comment]" "$OUT"
expect_contains "string tag" '"TODO in string"  [string]' "$OUT"
expect_contains "trailing comment tag" "trailing TODO note  [comment]" "$OUT"
expect_contains "// inside a string is not a comment" 'http://x/TODO"  [string]' "$OUT"

OUT=$("$BIN" -p 'func Target' -llm -scope go "$RT_FIX/a.go")
expect_contains "def tag on signature line" "[def func Target]" "$OUT"
expect_not_contains "def replaces [in] on the signature" "[in func Target]" "$OUT"

OUT=$("$BIN" -p 'import' -llm "$RT_FIX/a.go")
expect_contains "import tag" 'import "fmt"  [import]' "$OUT"

# Position-accurate roles beat line-based ones: a trailing comment on a
# signature line is a comment, not a def.
cat > "$RT_FIX/b.go" <<'EOF'
package main

func Helper() { // TODO here
}
EOF
OUT=$("$BIN" -p TODO -llm -scope go "$RT_FIX/b.go")
expect_contains "comment beats def on signature line" "[comment]" "$OUT"
expect_not_contains "no def tag for trailing comment" "[def" "$OUT"

OUT=$("$BIN" -p TODO "$RT_FIX/a.go")
expect_contains "JSONL role field" '"role":"comment"' "$OUT"
OUT=$("$BIN" -p TODO -format '$LINE:$ROLE' "$RT_FIX/a.go")
expect_contains "\$ROLE format token" "5:comment" "$OUT"
OUT=$("$BIN" -p TODO -sample 2 -llm "$RT_FIX/a.go")
expect_contains "-sample keeps role tags" "[comment]" "$OUT"

OUT=$("$BIN" -p TODO -llm -no-roles "$RT_FIX/a.go")
expect_not_contains "-no-roles drops -llm tags" "[comment]" "$OUT"
OUT=$("$BIN" -p TODO -no-roles "$RT_FIX/a.go")
expect_not_contains "-no-roles drops the JSONL field" '"role"' "$OUT"

cat > "$RT_FIX/c.c" <<'EOF'
#include <stdio.h>
/* start
   TODO inside block
   end */
int x;
EOF
OUT=$("$BIN" -p TODO -llm "$RT_FIX/c.c")
expect_contains "block comment spans lines" "TODO inside block  [comment]" "$OUT"
OUT=$("$BIN" -p 'stdio' -llm "$RT_FIX/c.c")
expect_contains "#include line tagged import" "[import]" "$OUT"

cat > "$RT_FIX/p.py" <<'EOF'
import os
# TODO hash comment
s = "TODO quoted"
d = """
TODO in docstring
"""
EOF
OUT=$("$BIN" -p TODO -llm "$RT_FIX/p.py")
expect_contains "python hash comment" "# TODO hash comment  [comment]" "$OUT"
expect_contains "python string" '"TODO quoted"  [string]' "$OUT"
expect_contains "python triple-quote string" "TODO in docstring  [string]" "$OUT"
OUT=$("$BIN" -p 'import os' -llm "$RT_FIX/p.py")
expect_contains "python import tag" "[import]" "$OUT"

printf '// TODO not code\n' > "$RT_FIX/x.txt"
OUT=$("$BIN" -p TODO -llm "$RT_FIX/x.txt")
expect_not_contains "unknown extension: no tags" "[comment]" "$OUT"
rm -rf "$RT_FIX"

# ---------------------------------------------------------------------------
section "scope rollup (-rollup) and co-occurrence footer"
RU_FIX="$HERE/_tmp_rollup"
mkdir -p "$RU_FIX"
cat > "$RU_FIX/m.go" <<'EOF'
package main

func Alpha() {
	work()
	work()
	other()
}

func Beta() {
	work()
}
EOF

OUT=$("$BIN" -p 'work\(\)' -name w -p 'other\(\)' -name o -rollup "$RU_FIX/m.go")
expect_contains "rollup line with counts" "3-7 func Alpha — 3 hits (w×2, o×1)" "$OUT"
expect_contains "rollup second scope" "9-11 func Beta — 1 hit (w×1)" "$OUT"
expect_contains "rollup representative line" "    4: 	work()" "$OUT"
expect_contains "rollup file header" "$RU_FIX/m.go" "$OUT"

OUT=$("$BIN" -p 'work\(\)' -rollup "$RU_FIX/m.go")
expect_contains "single pattern: no breakdown" "3-7 func Alpha — 2 hits" "$OUT"
expect_not_contains "single pattern: no ×" "×" "$OUT"

printf 'TODO one\nx\nTODO two\n' > "$RU_FIX/notes.txt"
OUT=$("$BIN" -p TODO -rollup "$RU_FIX/notes.txt")
expect_contains "scopeless matches roll up as top level" "(top level) — 2 hits" "$OUT"

OUT=$("$BIN" -p 'zz_nope' -rollup "$RU_FIX/m.go" ; echo "rc=$?")
expect_contains "rollup no matches exit 1" "rc=1" "$OUT"
expect_contains "rollup zero-match footer" "--- no matches: p0 (1 of 1 pattern) ---" "$OUT"

OUT=$("$BIN" -p x -rollup -llm 2>&1) ; RC=$?
expect_contains "-rollup is an exclusive output mode" "mutually exclusive" "$OUT"
[[ "$RC" == "2" ]] && report ok "-rollup + -llm exit 2" || report fail "-rollup + -llm exit (got $RC)"
OUT=$("$BIN" -p x -rollup -sample 3 "$RU_FIX/m.go" 2>&1) ; RC=$?
expect_contains "-rollup + -sample rejected" "don't compose with -sample" "$OUT"

# --- co-occurrence footer
cat > "$RU_FIX/a.log" <<'EOF'
WARN x
ERROR y
EOF
printf 'WARN only\n' > "$RU_FIX/b.log"
printf 'ERROR only\n' > "$RU_FIX/c.log"
OUT=$("$BIN" -p WARN -name warn -p ERROR -name err -llm "$RU_FIX/a.log" "$RU_FIX/b.log" "$RU_FIX/c.log")
expect_contains "co-occurrence crosstab" "--- files: warn 2, err 2; both: 1 ($RU_FIX/a.log) ---" "$OUT"

OUT=$("$BIN" -p WARN -name warn -p ERROR -name err -llm "$RU_FIX/b.log" "$RU_FIX/c.log")
expect_contains "zero overlap stated explicitly" "both: 0" "$OUT"

OUT=$("$BIN" -p WARN -p 'zz_nope' -llm "$RU_FIX/b.log")
expect_not_contains "one active pattern: no co-occurrence footer" "--- files:" "$OUT"

OUT=$("$BIN" -p WARN -name warn -p ERROR -name err -p 'x$' -name xx -llm "$RU_FIX/a.log" "$RU_FIX/b.log" "$RU_FIX/c.log")
expect_contains ">2 patterns: multi-pattern form with sets" "multi-pattern: 1 ($RU_FIX/a.log warn+err+xx)" "$OUT"

OUT=$("$BIN" -p WARN -name warn -p ERROR -name err "$RU_FIX/a.log")
expect_not_contains "JSONL mode: no co-occurrence footer" "--- files:" "$OUT"

OUT=$("$BIN" -p 'work\(\)' -name w -p 'other\(\)' -name o -rollup "$RU_FIX/m.go")
expect_contains "rollup gets the co-occurrence footer" "--- files: w 1, o 1; both: 1" "$OUT"
rm -rf "$RU_FIX"

# ---------------------------------------------------------------------------
section "fixed strings (-F / -Fi / -patterns-from)"
FS_FIX="$HERE/_tmp_fixed"
mkdir -p "$FS_FIX"
printf 'call foo[0].bar() now\nWARN mixed\nwarn lower\n' > "$FS_FIX/src.txt"

OUT=$("$BIN" -F 'foo[0].bar()' -o "$FS_FIX/src.txt")
expect_eq "-F literal with regex metachars" "foo[0].bar()" "$OUT"
OUT=$("$BIN" -F 'f.o' "$FS_FIX/src.txt") ; RC=$?
[[ "$RC" == "1" ]] && report ok "-F stays literal ('f.o' no match)" || report fail "-F literal-ness (got rc $RC)"
OUT=$("$BIN" -Fi 'WARN' -o "$FS_FIX/src.txt")
expect_lines "-Fi case-insensitive: 2 hits" 2 "$OUT"

cat > "$FS_FIX/rules.jsonl" <<'EOF'
# comment line
{"id":"brackets","literal":"foo[0].bar()"}
{"id":"warnish","regexp":"\\bWARN\\b"}
EOF
OUT=$("$BIN" -patterns-from "$FS_FIX/rules.jsonl" -format '$PAT_ID:$LINE' "$FS_FIX/src.txt")
expect_contains "patterns-from literal entry" "brackets:1" "$OUT"
expect_contains "patterns-from regexp entry" "warnish:2" "$OUT"

printf '{"literal":"a","regexp":"b"}\n' > "$FS_FIX/bad.jsonl"
OUT=$("$BIN" -patterns-from "$FS_FIX/bad.jsonl" "$FS_FIX/src.txt" 2>&1) ; RC=$?
expect_contains "bad entry cites file:line" "bad.jsonl:1" "$OUT"
[[ "$RC" == "2" ]] && report ok "bad entry exit 2" || report fail "bad entry exit (got $RC)"

printf '{"literal":"a","bogus":1}\n' > "$FS_FIX/unk.jsonl"
OUT=$("$BIN" -patterns-from "$FS_FIX/unk.jsonl" "$FS_FIX/src.txt" 2>&1)
expect_contains "unknown field rejected" "unknown field 'bogus'" "$OUT"

OUT=$("$BIN" -patterns-from "$FS_FIX/rules.jsonl" -s '{"patterns":[{"id":"x","regexp":"y"}]}' 2>&1) ; RC=$?
expect_contains "patterns-from + -s rejected" "cannot be combined" "$OUT"
[[ "$RC" == "2" ]] && report ok "patterns-from + -s exit 2" || report fail "patterns-from + -s exit (got $RC)"
rm -rf "$FS_FIX"

# ---------------------------------------------------------------------------
section "per-file filter (-file-where)"
FW_FIX="$HERE/_tmp_where"
mkdir -p "$FW_FIX"
printf 'ERROR one\nrecovered\n' > "$FW_FIX/ok.log"
printf 'ERROR two\nstill bad\n' > "$FW_FIX/bad.log"
printf 'quiet\n' > "$FW_FIX/calm.log"

OUT=$("$BIN" -p ERROR -name err -pi recovered -name rec -file-where 'err AND NOT rec' -f "$FW_FIX"/*.log)
expect_lines "err AND NOT rec selects one file" 1 "$OUT"
expect_contains "the unrecovered file" "bad.log" "$OUT"

OUT=$("$BIN" -p ERROR -name err -pi recovered -name rec -file-where '(err) && !rec' -f "$FW_FIX"/*.log)
expect_contains "symbol operators work" "bad.log" "$OUT"

OUT=$("$BIN" -p ERROR -name err -pi recovered -name rec -file-where 'err OR rec' -f "$FW_FIX"/*.log)
expect_lines "OR selects both matching files" 2 "$OUT"

OUT=$("$BIN" -p ERROR -file-where 'zzz' "$FW_FIX/ok.log" 2>&1)
expect_contains "unknown id in -file-where" "unknown pattern 'zzz'" "$OUT"
OUT=$("$BIN" -p ERROR -file-where 'AND x' "$FW_FIX/ok.log" 2>&1) ; RC=$?
[[ "$RC" == "2" ]] && report ok "bad -file-where syntax exit 2" || report fail "bad -file-where syntax exit (got $RC)"
OUT=$("$BIN" -p ERROR -file-where 'p0' -absent "$FW_FIX/ok.log" 2>&1)
expect_contains "-file-where + -absent rejected" "cannot combine with -absent" "$OUT"
rm -rf "$FW_FIX"

# ---------------------------------------------------------------------------
section "scope relations (-same-scope / -not-same-scope)"
SR_FIX="$HERE/_tmp_scoperel"
mkdir -p "$SR_FIX"
printf 'void f() {\n  lock();\n  unlock();\n}\nvoid g() {\n  lock();\n}\n' > "$SR_FIX/locks.c"

OUT=$("$BIN" -p '\block\(\)' -name lk -p '\bunlock\(\)' -name ul -same-scope lk:ul -scope c -format '$PAT_ID:$ENCLOSING_NAME' "$SR_FIX/locks.c")
expect_contains "same-scope keeps lock in f" "lk:f" "$OUT"
expect_not_contains "same-scope drops lock in g" "lk:g" "$OUT"

OUT=$("$BIN" -p '\block\(\)' -name lk -p '\bunlock\(\)' -name ul -not-same-scope lk:ul -scope c -format '$PAT_ID:$ENCLOSING_NAME' "$SR_FIX/locks.c")
expect_contains "not-same-scope keeps lock in g" "lk:g" "$OUT"
expect_not_contains "not-same-scope drops lock in f" "lk:f" "$OUT"

printf 'void h() {\n  probe();\n  probe();\n}\nvoid i() {\n  probe();\n}\n' > "$SR_FIX/pair.c"
OUT=$("$BIN" -p '\bprobe\(\)' -name pr -same-scope pr:pr -scope c -format '$ENCLOSING_NAME:$LINE' "$SR_FIX/pair.c")
expect_lines "a==b needs a second occurrence" 2 "$OUT"
expect_not_contains "a==b drops the singleton scope" "i:" "$OUT"

OUT=$("$BIN" -p a -p b -same-scope p0:p1 "$SR_FIX/locks.c" 2>&1) ; RC=$?
expect_contains "scope relation without -scope rejected" "require an active -scope" "$OUT"
[[ "$RC" == "2" ]] && report ok "scope relation without -scope exit 2" || report fail "scope relation exit (got $RC)"
rm -rf "$SR_FIX"

# ---------------------------------------------------------------------------
section "record-level absence (-records line)"
RCD_FIX="$HERE/_tmp_records"
mkdir -p "$RCD_FIX"
printf '{"user_id":1}\n{"other":2}\n\n{"user_id":3}\n{"other":4}\n' > "$RCD_FIX/d.jsonl"

OUT=$("$BIN" -p '"user_id"' -absent -records line "$RCD_FIX/d.jsonl")
expect_lines "two records flagged" 2 "$OUT"
expect_contains "record text included" '"line":2,"record":"{\"other\":2}"' "$OUT"
expect_contains "second miss at line 5" '"line":5' "$OUT"
expect_not_contains "blank line skipped" '"line":3' "$OUT"

OUT=$("$BIN" -p x -records line "$RCD_FIX/d.jsonl" 2>&1) ; RC=$?
expect_contains "-records without -absent rejected" "requires -absent" "$OUT"
[[ "$RC" == "2" ]] && report ok "-records without -absent exit 2" || report fail "-records exit (got $RC)"

OUT=$("$BIN" -p x -p y -near p0:p1:1 -absent -records line "$RCD_FIX/d.jsonl" 2>&1)
expect_contains "-records + relations rejected" "cannot combine with" "$OUT"

OUT=$("$BIN" -p '"user_id"' -absent -records line -limit 1 "$RCD_FIX/d.jsonl")
expect_lines "-limit caps absent records" 1 "$OUT"
rm -rf "$RCD_FIX"

# ---------------------------------------------------------------------------
section "deterministic traversal order"
DT_FIX="$HERE/_tmp_order"
mkdir -p "$DT_FIX/bdir"
# Created deliberately out of order; output must be sorted pre-order.
printf 'needle\n' > "$DT_FIX/zz.txt"
printf 'needle\n' > "$DT_FIX/aa.txt"
printf 'needle\n' > "$DT_FIX/bdir/inner.txt"
printf 'needle\n' > "$DT_FIX/mm.txt"
OUT=$("$BIN" -p needle -f -glob "$DT_FIX/**/*.txt")
EXPECTED="$DT_FIX/aa.txt
$DT_FIX/bdir/inner.txt
$DT_FIX/mm.txt
$DT_FIX/zz.txt"
expect_eq "sorted lexicographic pre-order" "$EXPECTED" "$OUT"
OUT2=$("$BIN" -p needle -f -glob "$DT_FIX/**/*.txt")
expect_eq "identical order across runs" "$OUT" "$OUT2"
rm -rf "$DT_FIX"

# ---------------------------------------------------------------------------
section "scan accounting (-summary / -diagnostics / -require-complete)"
SA_FIX="$HERE/_tmp_accounting"
mkdir -p "$SA_FIX"
printf 'needle one\n' > "$SA_FIX/a.txt"
printf 'needle two\n' > "$SA_FIX/b.txt"
printf 'bin\x00needle\n' > "$SA_FIX/c.dat"

OUT=$("$BIN" -p needle -summary -glob "$SA_FIX/*" | tail -1)
expect_contains "summary record type" '"type":"summary"' "$OUT"
expect_contains "summary counts scanned files" '"files_scanned":2' "$OUT"
expect_contains "summary counts binary skip" '"files_skipped_binary":1' "$OUT"
expect_contains "summary counts matches" '"matches":2' "$OUT"
expect_contains "clean run is complete" '"complete":true' "$OUT"
expect_not_contains "no stop_reason when complete" '"stop_reason"' "$OUT"

OUT=$("$BIN" -p needle -summary -limit 1 -glob "$SA_FIX/*.txt" | tail -1)
expect_contains "limit run incomplete" '"complete":false' "$OUT"
expect_contains "limit stop_reason" '"stop_reason":"limit"' "$OUT"

OUT=$("$BIN" -p needle -diagnostics -f -glob "$SA_FIX/*")
expect_contains "binary_skip diagnostic on stdout" '"code":"binary_skip"' "$OUT"

ERR=$(printf '%s\n%s\n' "$SA_FIX/a.txt" "$SA_FIX/gone.txt" | "$BIN" -p needle -f -files-from - -require-complete 2>&1 >/dev/null) ; RC=$?
expect_contains "require-complete reports the gap" "incomplete scan" "$ERR"
[[ "$RC" == "2" ]] && report ok "require-complete exit 2 on missing path" || report fail "require-complete exit (got $RC)"

OUT=$("$BIN" -p needle -require-complete -f -glob "$SA_FIX/*.txt") ; RC=$?
[[ "$RC" == "0" ]] && report ok "require-complete clean run exit 0" || report fail "require-complete clean exit (got $RC)"

OUT=$(printf '%s\n%s\n' "$SA_FIX/a.txt" "$SA_FIX/gone.txt" | "$BIN" -p needle -diagnostics -f -files-from - 2>/dev/null)
expect_contains "missing_path diagnostic" '"code":"missing_path"' "$OUT"

if [[ "$EUID" -ne 0 ]]; then
    printf 'needle locked\n' > "$SA_FIX/locked.txt"
    chmod 000 "$SA_FIX/locked.txt"
    OUT=$("$BIN" -p needle -summary -glob "$SA_FIX/*.txt" 2>/dev/null | tail -1)
    expect_contains "read failure counted" '"files_failed":1' "$OUT"
    expect_contains "read failure breaks completeness" '"complete":false' "$OUT"
    OUT=$("$BIN" -p needle -diagnostics -f -glob "$SA_FIX/*.txt")
    expect_contains "read_error diagnostic" '"code":"read_error"' "$OUT"
    "$BIN" -p needle -require-complete -f -glob "$SA_FIX/*.txt" >/dev/null 2>&1 ; RC=$?
    [[ "$RC" == "2" ]] && report ok "require-complete exit 2 on read failure" || report fail "require-complete read-failure exit (got $RC)"
    chmod 644 "$SA_FIX/locked.txt"
fi

OUT=$("$BIN" -s "{\"summary\":true,\"scan\":[\"$SA_FIX/*.txt\"],\"patterns\":[{\"id\":\"n\",\"regexp\":\"needle\"}]}" | tail -1)
expect_contains "script-mode summary field" '"type":"summary"' "$OUT"
expect_contains "script-mode summary complete" '"complete":true' "$OUT"
rm -rf "$SA_FIX"

# ---------------------------------------------------------------------------
section "git-aware selection (-git-changed / -git-staged / -git-range / -git-added-lines)"
if command -v git >/dev/null 2>&1; then
    GA_FIX="$HERE/_tmp_gitaware"
    rm -rf "$GA_FIX"; mkdir -p "$GA_FIX"
    git -C "$GA_FIX" init -q -b main 2>/dev/null || git -C "$GA_FIX" init -q
    git -C "$GA_FIX" -c user.email=t@t -c user.name=t commit -q --allow-empty -m base --no-gpg-sign
    printf 'line one\nline two\nline three\n' > "$GA_FIX/committed.txt"
    git -C "$GA_FIX" add committed.txt
    git -C "$GA_FIX" -c user.email=t@t -c user.name=t commit -q -m add --no-gpg-sign
    printf 'line one\nline two TODO new\nline three\n' > "$GA_FIX/committed.txt"
    printf 'staged TODO\n' > "$GA_FIX/staged.txt"
    git -C "$GA_FIX" add staged.txt
    printf 'untracked TODO\n' > "$GA_FIX/untracked.txt"

    OUT=$(cd "$GA_FIX" && "$BIN" -p TODO -git-changed -f)
    expect_lines "git-changed: modified + staged" 2 "$OUT"
    expect_contains "git-changed sees the unstaged edit" "committed.txt" "$OUT"
    expect_contains "git-changed sees the staged file" "staged.txt" "$OUT"
    expect_not_contains "git-changed excludes untracked" "untracked.txt" "$OUT"

    OUT=$(cd "$GA_FIX" && "$BIN" -p TODO -git-staged -f)
    expect_eq "git-staged: staged file only" "staged.txt" "$OUT"

    OUT=$(cd "$GA_FIX" && "$BIN" -p TODO -git-untracked -f)
    expect_eq "git-untracked: untracked file only" "untracked.txt" "$OUT"

    OUT=$(cd "$GA_FIX" && "$BIN" -p 'line' -git-range 'HEAD~1..HEAD' -c)
    expect_eq "git-range scans the range's files" "committed.txt:3" "$OUT"

    OUT=$(cd "$GA_FIX" && "$BIN" -p 'line' -git-changed -git-added-lines -format '$FILE:$LINE')
    expect_eq "added-lines keeps only the modified line" "committed.txt:2" "$OUT"

    OUT=$(cd "$GA_FIX" && "$BIN" -p TODO -git-changed -git-untracked -git-added-lines -format '$FILE:$LINE')
    expect_lines "added-lines + untracked whole-file" 3 "$OUT"
    expect_contains "untracked line counts as added" "untracked.txt:1" "$OUT"

    OUT=$(cd "$GA_FIX" && "$BIN" -p x -git-untracked -git-added-lines 2>&1) ; RC=$?
    expect_contains "added-lines needs a diff-based flag" "requires -git-changed" "$OUT"
    [[ "$RC" == "2" ]] && report ok "added-lines flag validation exit 2" || report fail "added-lines validation exit (got $RC)"

    OUT=$(cd "$GA_FIX" && "$BIN" -p x -git-changed -git-added-lines -glob '*.txt' 2>&1) ; RC=$?
    expect_contains "added-lines rejects mixed inputs" "from git alone" "$OUT"

    OUT=$(cd "$GA_FIX" && "$BIN" -s '{"patterns":[{"id":"t","regexp":"TODO"}]}' -git-staged)
    expect_contains "script mode: git selection overrides scan" '"file":"staged.txt"' "$OUT"

    OUT=$(cd "$GA_FIX" && "$BIN" -s '{"patterns":[{"id":"t","regexp":"x"}]}' -git-changed -git-added-lines 2>&1) ; RC=$?
    expect_contains "script mode rejects added-lines" "quick mode" "$OUT"

    OUT=$(cd /tmp && "$BIN" -p x -git-changed 2>&1) ; RC=$?
    expect_contains "outside a repo: git's error surfaces" "not a git repository" "$OUT"
    [[ "$RC" == "2" ]] && report ok "outside a repo exit 2" || report fail "outside a repo exit (got $RC)"

    rm -rf "$GA_FIX"
else
    report ok "git not available — section skipped"
fi

# ---------------------------------------------------------------------------
section "edit subcommand: dispatch + validation (Phase 0)"

# Write-shaped flags stay unknown outside the edit subcommand — the bare
# tool's read-only promise is syntactic, not behavioral.
OUT=$("$BIN" -p x -write "$SAMPLE/a.go" 2>&1) ; RC=$?
expect_contains "-write outside edit is unknown" "unknown flag: -write" "$OUT"
[[ "$RC" == "2" ]] && report ok "-write outside edit exit 2" || report fail "-write outside edit exit (got $RC)"

# Help mentions edit mode; `edit -h` shows help instead of a usage error.
expect_contains "help documents edit mode" "Edit mode" "$("$BIN" --help)"
OUT=$("$BIN" edit -h 2>&1) ; RC=$?
expect_contains "edit -h prints help" "Usage:" "$OUT"
[[ "$RC" == "0" ]] && report ok "edit -h exit 0" || report fail "edit -h exit (got $RC)"

# Validation: every refusal is exit 2 with a targeted message.
OUT=$("$BIN" edit -p x -content y 2>&1) ; RC=$?
expect_contains "edit needs explicit inputs" "requires explicit input files" "$OUT"
[[ "$RC" == "2" ]] && report ok "edit no-input exit 2" || report fail "edit no-input exit (got $RC)"

OUT=$("$BIN" edit -p x -content y -span block "$SAMPLE/a.go" 2>&1)
expect_contains "span block needs delimiters" "requires -block-open and -block-close" "$OUT"

OUT=$("$BIN" edit -p x -content y -span scope "$SAMPLE/a.go" 2>&1)
expect_contains "span scope needs -scope" "requires an active -scope" "$OUT"

OUT=$("$BIN" edit -p x -content y -span nope "$SAMPLE/a.go" 2>&1)
expect_contains "unknown span rejected" "-span: unknown span 'nope'" "$OUT"

OUT=$("$BIN" edit -p x "$SAMPLE/a.go" 2>&1)
expect_contains "missing content source" "needs a content source" "$OUT"

OUT=$("$BIN" edit -p x -content a -content-stdin "$SAMPLE/a.go" 2>&1)
expect_contains "two content sources rejected" "exactly one of -content" "$OUT"

OUT=$("$BIN" edit -p x -delete -content a "$SAMPLE/a.go" 2>&1)
expect_contains "-delete rejects content" "-delete does not take" "$OUT"

OUT=$("$BIN" edit -p x -insert end -content a "$SAMPLE/a.go" 2>&1)
expect_contains "-insert end needs delimited span" "needs a delimited span" "$OUT"

OUT=$("$BIN" edit -p x -content a -insert nope "$SAMPLE/a.go" 2>&1)
expect_contains "unknown insert pos rejected" "-insert: unknown position" "$OUT"

OUT=$("$BIN" edit -p x -delete -insert end "$SAMPLE/a.go" 2>&1)
expect_contains "-insert after -delete rejected" "cannot combine with -delete" "$OUT"

OUT=$("$BIN" edit -p x -content a -expect 2x "$SAMPLE/a.go" 2>&1)
expect_contains "-expect strict integer" "not a non-negative integer" "$OUT"

OUT=$("$BIN" edit -p x -content a -max-span-lines -3 "$SAMPLE/a.go" 2>&1)
expect_contains "-max-span-lines strict integer" "not a non-negative integer" "$OUT"

OUT=$("$BIN" edit -p x -content a -o "$SAMPLE/a.go" 2>&1)
expect_contains "edit rejects -o output mode" "do not apply" "$OUT"

OUT=$("$BIN" edit -p x -content a -C 2 "$SAMPLE/a.go" 2>&1)
expect_contains "edit rejects context flags" "-A/-B/-C do not apply" "$OUT"

OUT=$("$BIN" edit -p x -content a -sample 3 "$SAMPLE/a.go" 2>&1)
expect_contains "edit rejects -sample" "cannot combine with -sample" "$OUT"

OUT=$("$BIN" edit -content a -s '{}' "$SAMPLE/a.go" 2>&1)
expect_contains "edit rejects -s script" "read-only" "$OUT"

OUT=$("$BIN" edit -content a "$SAMPLE/a.go" 2>&1) ; RC=$?
expect_contains "edit without -p" "edit: -p <pattern> required" "$OUT"
[[ "$RC" == "2" ]] && report ok "edit without -p exit 2" || report fail "edit without -p exit (got $RC)"

# Anchorless edits need -in-scope + a scope span; everything else refuses.
OUT=$("$BIN" edit -content y "$SAMPLE/a.go" 2>&1) ; RC=$?
expect_contains "anchorless needs in-scope+scope span" "anchorless" "$OUT"
[[ "$RC" == "2" ]] && report ok "anchorless misuse exit 2" || report fail "anchorless misuse exit (got $RC)"
OUT=$("$BIN" -in-scope f -s '{}' 2>&1) ; RC=$?
expect_contains "-in-scope rejected in script mode" "quick (-p) and edit modes" "$OUT"
OUT=$("$BIN" -lines 9:3 -p x "$SAMPLE/a.go" 2>&1) ; RC=$?
expect_contains "-lines rejects reversed range" "-lines: expected" "$OUT"
OUT=$("$BIN" -list-scopes -p x "$SAMPLE/a.go" 2>&1) ; RC=$?
expect_contains "-list-scopes takes no patterns" "takes no patterns" "$OUT"

# ---------------------------------------------------------------------------
section "edit engine: replace / guards / write (sandboxed)"

# Every test operates on fresh copies inside a temp sandbox — fixtures are
# never touched, and each case asserts BOTH the output contract and the
# on-disk bytes.
ED=$(mktemp -d)

# A small Go-ish file used across cases. No trailing oddities: 12 lines.
make_w() {
    cat > "$ED/w.go" <<'GOEOF'
package main

import "fmt"

func LoadData(path string) error {
	fmt.Println("loading", path)
	return nil
}

func main() {
	fmt.Println(LoadData("x"), "retry(3)")
}
GOEOF
}

# --- dry-run: exact diff golden, file untouched, exit 0
make_w
OUT=$("$BIN" edit -F 'retry(3)' -content 'retry(5)' "$ED/w.go") ; RC=$?
EXPECTED="--- a/$ED/w.go
+++ b/$ED/w.go
@@ -8,5 +8,5 @@
 }
 
 func main() {
-	fmt.Println(LoadData(\"x\"), \"retry(3)\")
+	fmt.Println(LoadData(\"x\"), \"retry(5)\")
 }
{\"type\":\"edit-summary\",\"sites\":1,\"changed\":1,\"noops\":0,\"files_changed\":1,\"dry_run\":true,\"applied\":false}"
expect_eq "dry-run: exact unified diff + summary" "$EXPECTED" "$OUT"
[[ "$RC" == "0" ]] && report ok "dry-run exit 0" || report fail "dry-run exit (got $RC)"
OUT=$("$BIN" -F 'retry(3)' -c "$ED/w.go")
expect_contains "dry-run leaves file untouched" ":1" "$OUT"

# --- -write with -expect: applied, records, exit 0
OUT=$("$BIN" edit -F 'retry(3)' -content 'retry(5)' -expect 1 -write "$ED/w.go") ; RC=$?
expect_contains "write: edit record"    '"status":"changed"' "$OUT"
expect_contains "write: summary applied" '"applied":true'    "$OUT"
[[ "$RC" == "0" ]] && report ok "write exit 0" || report fail "write exit (got $RC)"
OUT=$("$BIN" -F 'retry(5)' -c "$ED/w.go")
expect_contains "write: change on disk" ":1" "$OUT"

# --- idempotent rerun: noop, exit 0, file stable
OUT=$("$BIN" edit -F 'retry(5)' -content 'retry(5)' -write "$ED/w.go") ; RC=$?
expect_contains "noop rerun: status noop" '"status":"noop"' "$OUT"
expect_contains "noop rerun: 0 files changed" '"files_changed":0' "$OUT"
[[ "$RC" == "0" ]] && report ok "noop rerun exit 0" || report fail "noop rerun exit (got $RC)"

# --- -expect mismatch: exit 3, nothing written
make_w
OUT=$("$BIN" edit -F 'fmt' -content 'FMT' -expect 1 -write "$ED/w.go") ; RC=$?
expect_contains "expect mismatch: guard record" '"guard":"expect"' "$OUT"
[[ "$RC" == "3" ]] && report ok "expect mismatch exit 3" || report fail "expect mismatch exit (got $RC)"
OUT=$("$BIN" -F 'FMT' -f "$ED/w.go" ; true)
expect_lines "expect mismatch: nothing written" 0 "$OUT"

# --- no matches: exit 1, empty summary
OUT=$("$BIN" edit -F 'no_such_token' -content 'x' -write "$ED/w.go") ; RC=$?
expect_contains "no matches: sites 0" '"sites":0' "$OUT"
[[ "$RC" == "1" ]] && report ok "no matches exit 1" || report fail "no matches exit (got $RC)"

# --- block-full: whole-function swap from -content-file
make_w
cat > "$ED/newbody.txt" <<'GOEOF'
func LoadData(path string) error {
	return load(path)
}
GOEOF
printf '%s' "$(cat "$ED/newbody.txt")" > "$ED/newbody.txt"  # strip trailing NL: span ends at }
OUT=$("$BIN" edit -p 'func LoadData\b' -block-open '{' -block-close '}' \
      -span block-full -content-file "$ED/newbody.txt" -expect 1 -write "$ED/w.go") ; RC=$?
[[ "$RC" == "0" ]] && report ok "block-full swap exit 0" || report fail "block-full swap exit (got $RC)"
OUT=$("$BIN" -F 'return load(path)' -c "$ED/w.go")
expect_contains "block-full swap: new body on disk" ":1" "$OUT"
OUT=$("$BIN" -F 'loading' -f "$ED/w.go" ; true)
expect_lines "block-full swap: old body gone" 0 "$OUT"

# --- patch(1) round-trip: dry-run diff reproduces -write output exactly
if command -v patch >/dev/null 2>&1; then
    make_w
    "$BIN" edit -p 'func LoadData\b' -block-open '{' -block-close '}' \
        -span block-full -content-file "$ED/newbody.txt" "$ED/w.go" > "$ED/swap.diff"
    sed '/^{"type"/d' "$ED/swap.diff" | (cd / && patch -p1 -s -o "$ED/w.patched" "$ED/w.go")
    "$BIN" edit -p 'func LoadData\b' -block-open '{' -block-close '}' \
        -span block-full -content-file "$ED/newbody.txt" -write "$ED/w.go" > /dev/null
    if cmp -s "$ED/w.patched" "$ED/w.go"; then
        report ok "patch round-trip: diff == splice"
    else
        report fail "patch round-trip: diff == splice"
    fi
else
    report ok "patch not available — round-trip skipped"
fi

# --- insert end into a delimited block
printf 'import (\n\t"fmt"\n)\n' > "$ED/imp.go"
"$BIN" edit -p '^import \(' -block-open '(' -block-close ')' -span block \
    -insert end -content '\t"os"\n' -expect 1 -write "$ED/imp.go" > /dev/null
EXPECTED=$(printf 'import (\n\t"fmt"\n\t"os"\n)\n')
expect_eq "insert end inside block" "$EXPECTED" "$(cat "$ED/imp.go")"

# --- insert before, whole-line (pure insertion diff)
printf 'l1\nl2\nl3\n' > "$ED/ins.txt"
OUT=$("$BIN" edit -p '^l2' -span line -insert before -content 'NEW\n' "$ED/ins.txt")
expect_contains "pure line insert: diff +NEW" '+NEW' "$OUT"
expect_not_contains "pure line insert: no removals" '-l2' "$OUT"
"$BIN" edit -p '^l2' -span line -insert before -content 'NEW\n' -write "$ED/ins.txt" > /dev/null
expect_eq "insert before line on disk" "$(printf 'l1\nNEW\nl2\nl3\n')" "$(cat "$ED/ins.txt")"

# --- delete a full line (newline consumed, no blank left)
printf 'keep1\ndebugPrint(1)\nkeep2\n' > "$ED/d.txt"
"$BIN" edit -p '^debugPrint' -span line -delete -expect 1 -write "$ED/d.txt" > /dev/null
expect_eq "delete line consumes newline" "$(printf 'keep1\nkeep2\n')" "$(cat "$ED/d.txt")"

# --- template tokens: $EXTRACT_*, $MATCH, $$
printf 'log.Printf("hello %%s", name)\n' > "$ED/t.go"
"$BIN" edit -p 'log\.Printf\("([^"]*)"' -extract fmtstr \
    -content 'logger.Infof("$EXTRACT_FMTSTR"' -write "$ED/t.go" > /dev/null
expect_eq "extract token in template" 'logger.Infof("hello %s", name)' "$(cat "$ED/t.go")"
printf 'val\n' > "$ED/tok.txt"
"$BIN" edit -F 'val' -content '<$MATCH> costs $$5' -write "$ED/tok.txt" > /dev/null
expect_eq "MATCH and \$\$ tokens" '<val> costs $5' "$(cat "$ED/tok.txt")"

# --- overlap conflict: exit 3, file untouched
printf 'aabbcc\n' > "$ED/ov.txt"
OUT=$("$BIN" edit -p 'aabb' -p 'bbcc' -content 'X' -write "$ED/ov.txt") ; RC=$?
expect_contains "overlap: guard record" '"guard":"overlap"' "$OUT"
[[ "$RC" == "3" ]] && report ok "overlap exit 3" || report fail "overlap exit (got $RC)"
expect_eq "overlap: file untouched" 'aabbcc' "$(cat "$ED/ov.txt")"

# --- identical span+content from two patterns dedups to one site
printf 'target\n' > "$ED/dd.txt"
OUT=$("$BIN" edit -p 'target' -p 'tar get|target' -content 'done' -write "$ED/dd.txt")
expect_contains "identical edits dedup" '"sites":1' "$OUT"
expect_eq "dedup applied once" 'done' "$(cat "$ED/dd.txt")"

# --- -max-span-lines guard + explicit override
python3 - "$ED/big.go" <<'PYEOF'
import sys
with open(sys.argv[1], 'w') as f:
    f.write('func Big() {\n')
    for i in range(600):
        f.write('  line%d\n' % i)
    f.write('}\n')
PYEOF
OUT=$("$BIN" edit -p 'func Big' -block-open '{' -block-close '}' -span block-full \
      -content 'X' -write "$ED/big.go") ; RC=$?
expect_contains "max-span-lines: guard record" '"guard":"max-span-lines"' "$OUT"
[[ "$RC" == "3" ]] && report ok "max-span-lines exit 3" || report fail "max-span-lines exit (got $RC)"
OUT=$("$BIN" edit -p 'func Big' -block-open '{' -block-close '}' -span block-full \
      -content 'X' -max-span-lines 0 -write "$ED/big.go") ; RC=$?
[[ "$RC" == "0" ]] && report ok "max-span-lines 0 overrides" || report fail "max-span-lines 0 (got $RC)"
expect_eq "big replace applied" 'X' "$(cat "$ED/big.go")"

# --- -assert-contains: pass applies, fail refuses
printf 'func A() {\n  retry(3)\n}\n' > "$ED/ac.go"
OUT=$("$BIN" edit -p 'func A' -block-open '{' -block-close '}' -span block \
      -content '{ retry(5) }' -assert-contains 'retry' -write "$ED/ac.go") ; RC=$?
[[ "$RC" == "0" ]] && report ok "assert-contains pass applies" || report fail "assert-contains pass (got $RC)"
OUT=$("$BIN" edit -F 'retry(5)' -content 'retry(7)' -assert-contains 'nonexistent' -write "$ED/ac.go") ; RC=$?
expect_contains "assert-contains fail: guard" '"guard":"assert-contains"' "$OUT"
[[ "$RC" == "3" ]] && report ok "assert-contains fail exit 3" || report fail "assert-contains fail (got $RC)"

# --- block-not-found is a guard, not a silent skip
printf 'func Broken( {\n' > "$ED/broken.go"
OUT=$("$BIN" edit -p 'func Broken' -block-open '{' -block-close '}' -span block \
      -content 'X' -write "$ED/broken.go") ; RC=$?
expect_contains "unclosed block: guard record" '"guard":"block-not-found"' "$OUT"
[[ "$RC" == "3" ]] && report ok "unclosed block exit 3" || report fail "unclosed block exit (got $RC)"

# --- byte-exactness: CRLF and missing trailing newline survive
printf 'alpha\r\nBETA\r\ngamma\r\n' > "$ED/crlf.txt"
"$BIN" edit -F 'BETA' -content 'beta' -write "$ED/crlf.txt" > /dev/null
printf 'alpha\r\nbeta\r\ngamma\r\n' > "$ED/crlf.want"
if cmp -s "$ED/crlf.txt" "$ED/crlf.want"; then
    report ok "CRLF bytes preserved"
else
    report fail "CRLF bytes preserved"
fi
printf 'one\ntwo END' > "$ED/nonl.txt"
OUT=$("$BIN" edit -F 'END' -content 'FIN' "$ED/nonl.txt")
expect_contains "no-newline marker in diff" 'No newline at end of file' "$OUT"
"$BIN" edit -F 'END' -content 'FIN' -write "$ED/nonl.txt" > /dev/null
printf 'one\ntwo FIN' > "$ED/nonl.want"
if cmp -s "$ED/nonl.txt" "$ED/nonl.want"; then
    report ok "missing trailing newline preserved"
else
    report fail "missing trailing newline preserved"
fi

# --- executable bit survives the temp+rename write
printf '#!/bin/sh\necho hi\n' > "$ED/perm.sh" && chmod 755 "$ED/perm.sh"
"$BIN" edit -F 'hi' -content 'bye' -write "$ED/perm.sh" > /dev/null
MODE=$(stat -c '%a' "$ED/perm.sh" 2>/dev/null || stat -f '%Lp' "$ED/perm.sh")
expect_eq "permissions preserved" "755" "$MODE"

# --- multi-file edit in one pass
printf 'AAA\n' > "$ED/m1.txt" ; printf 'AAA\nAAA\n' > "$ED/m2.txt"
OUT=$("$BIN" edit -F 'AAA' -content 'BBB' -expect 3 -write "$ED/m1.txt" "$ED/m2.txt")
expect_contains "multi-file: 2 files changed" '"files_changed":2' "$OUT"
expect_eq "multi-file m1" 'BBB' "$(cat "$ED/m1.txt")"
expect_eq "multi-file m2" "$(printf 'BBB\nBBB\n')" "$(cat "$ED/m2.txt")"

# --- relations compose as edit qualifiers: -far + -ref skip allowlisted
# lines. The qualifier pattern MUST be -ref, or its own matches get edited.
printf 'p(1)\np(2) // keep\np(3)\n' > "$ED/rel.txt"
OUT=$("$BIN" edit -p 'p\([0-9]\)' -name hit -p 'keep' -name allow -ref \
      -far hit:allow:0 -content 'q()' -write "$ED/rel.txt")
expect_contains "relation-filtered edit: 2 sites" '"sites":2' "$OUT"
expect_eq "relation-filtered result" "$(printf 'q()\np(2) // keep\nq()\n')" "$(cat "$ED/rel.txt")"

# Without -ref the qualifier's own match is an edit site too — pinned so the
# semantic stays explicit.
printf 'p(1)\np(2) // keep\np(3)\n' > "$ED/rel2.txt"
OUT=$("$BIN" edit -p 'p\([0-9]\)' -name hit -p 'keep' -name allow \
      -far hit:allow:0 -content 'q()' "$ED/rel2.txt")
expect_contains "without -ref qualifier edits too" '"sites":3' "$OUT"

# All patterns -ref is a usage error.
OUT=$("$BIN" edit -p 'x' -ref -content 'y' "$ED/rel2.txt" 2>&1) ; RC=$?
expect_contains "all -ref rejected" "at least one pattern must produce" "$OUT"
[[ "$RC" == "2" ]] && report ok "all -ref exit 2" || report fail "all -ref exit (got $RC)"

# --- -j dry-run: records, no diff; -diff + -write: both
printf 'AAA\n' > "$ED/j.txt"
OUT=$("$BIN" edit -F 'AAA' -content 'BBB' -j "$ED/j.txt")
expect_contains "dry-run -j: edit record" '"type":"edit"' "$OUT"
expect_not_contains "dry-run -j: no diff" '+++' "$OUT"
OUT=$("$BIN" edit -F 'AAA' -content 'BBB' -diff -write "$ED/j.txt")
expect_contains "write -diff: prints diff" '+++' "$OUT"
expect_contains "write -diff: prints records" '"type":"edit"' "$OUT"

# --- -content-stdin
printf 'OLD\n' > "$ED/stdin.txt"
OUT=$(printf 'MULTI\nLINE' | "$BIN" edit -F 'OLD' -content-stdin -write "$ED/stdin.txt")
expect_eq "content from stdin" "$(printf 'MULTI\nLINE\n')" "$(cat "$ED/stdin.txt")"

rm -rf "$ED"

# ---------------------------------------------------------------------------
section "scoped targeting: -in-scope / -lines / scope spans / -list-scopes"

SC=$(mktemp -d)
make_p2() {
    cat > "$SC/p2.go" <<'GOEOF'
package main

func LoadData(path string) error {
	retry(3)
	return nil
}

func ProcessBatch(items []int) {
	retry(3)
	inner(func() {
		retry(3)
	})
}

func main() {
	retry(3)
}
GOEOF
}
make_p2

# --- -in-scope search filter: implies -scope auto, chain semantics include
# code inside anonymous closures nested in the named function.
OUT=$("$BIN" -F 'retry(3)' -in-scope 'ProcessBatch' -format '$LINE' "$SC/p2.go")
expect_eq "-in-scope keeps chain matches" "$(printf '9\n11')" "$OUT"
OUT=$("$BIN" -F 'retry(3)' -in-scope 'LoadData' -in-scope 'main' -format '$LINE' "$SC/p2.go")
expect_eq "-in-scope repeatable ORs" "$(printf '4\n16')" "$OUT"
OUT=$("$BIN" -F 'retry(3)' -in-scope 'NoSuchFunc' -f "$SC/p2.go" ; true)
expect_lines "-in-scope no match drops all" 0 "$OUT"

# --- -lines forms
OUT=$("$BIN" -F 'retry(3)' -lines 8:13 -format '$LINE' "$SC/p2.go")
expect_eq "-lines A:B" "$(printf '9\n11')" "$OUT"
OUT=$("$BIN" -F 'retry(3)' -lines 16 -format '$LINE' "$SC/p2.go")
expect_eq "-lines single N" "16" "$OUT"
OUT=$("$BIN" -F 'retry(3)' -lines :6 -lines 15: -format '$LINE' "$SC/p2.go")
expect_eq "-lines open forms OR" "$(printf '4\n16')" "$OUT"

# --- -list-scopes
OUT=$("$BIN" -list-scopes "$SC/p2.go")
expect_lines "list-scopes: 3 scopes" 3 "$OUT"
expect_contains "list-scopes record shape" '"type":"scope","file":"'"$SC"'/p2.go","name":"LoadData","kind":"func","line_start":3,"line_end":6' "$OUT"
OUT=$("$BIN" -list-scopes -llm -in-scope 'main' "$SC/p2.go")
expect_eq "list-scopes llm + filter" "$SC/p2.go:15-17 func main" "$OUT"
OUT=$("$BIN" -list-scopes -in-scope 'zzz' "$SC/p2.go" ; echo "rc=$?")
expect_contains "list-scopes none found exit 1" "rc=1" "$OUT"

# --- edit filtered by -in-scope
make_p2
OUT=$("$BIN" edit -F 'retry(3)' -content 'retry(9)' -in-scope 'ProcessBatch' \
      -expect 2 -write "$SC/p2.go")
expect_contains "in-scope edit: 2 sites" '"sites":2' "$OUT"
OUT=$("$BIN" -F 'retry(9)' -in-scope 'ProcessBatch' -c "$SC/p2.go")
expect_contains "in-scope edit applied inside" ":2" "$OUT"
OUT=$("$BIN" -F 'retry(3)' -c "$SC/p2.go")
expect_contains "in-scope edit untouched outside" ":2" "$OUT"

# --- -lines edit + -assert-contains staleness tripwire
make_p2
OUT=$("$BIN" edit -F 'retry(3)' -content 'retry(7)' -lines 1:6 \
      -assert-contains 'retry' -expect 1 -write "$SC/p2.go") ; RC=$?
[[ "$RC" == "0" ]] && report ok "-lines edit applies" || report fail "-lines edit (got $RC)"
OUT=$("$BIN" edit -F 'return nil' -content 'X' -lines 1:6 \
      -assert-contains 'no_longer_there' -write "$SC/p2.go") ; RC=$?
expect_contains "-lines stale assert refuses" '"guard":"assert-contains"' "$OUT"
[[ "$RC" == "3" ]] && report ok "-lines stale assert exit 3" || report fail "-lines stale assert (got $RC)"

# --- span scope: targets the NAMED scope (not the closure), dedups the two
# matches inside it to one site.
make_p2
OUT=$("$BIN" edit -F 'retry(3)' -in-scope 'ProcessBatch' -span scope \
      -content 'func ProcessBatch(items []int) {}' -j "$SC/p2.go")
expect_contains "span scope: single deduped site" '"sites":1' "$OUT"
expect_contains "span scope: whole-function lines" '"line_start":8,"line_end":13' "$OUT"

# --- span scope-body replace keeps the signature
make_p2
"$BIN" edit -F 'retry(3)' -in-scope '^main$' -span scope-body \
    -content '{ boot() }' -expect 1 -write "$SC/p2.go" > /dev/null
OUT=$("$BIN" -F 'func main() { boot() }' -c "$SC/p2.go")
expect_contains "scope-body keeps signature" ":1" "$OUT"

# --- scope-not-found guard: match outside any scope
printf 'retry(3)\n' > "$SC/bare.go"
OUT=$("$BIN" edit -F 'retry(3)' -span scope -scope go -content 'X' -write "$SC/bare.go") ; RC=$?
expect_contains "scope-not-found guard" '"guard":"scope-not-found"' "$OUT"
[[ "$RC" == "3" ]] && report ok "scope-not-found exit 3" || report fail "scope-not-found exit (got $RC)"

# --- anchorless: replace a function purely by name
make_p2
printf 'func LoadData(path string) error {\n\treturn load2(path)\n}' > "$SC/nb.txt"
OUT=$("$BIN" edit -in-scope '^LoadData$' -span scope -content-file "$SC/nb.txt" \
      -expect 1 -write "$SC/p2.go")
expect_contains "anchorless: pat is scope name" '"pat":"LoadData"' "$OUT"
OUT=$("$BIN" -F 'return load2(path)' -c "$SC/p2.go")
expect_contains "anchorless swap on disk" ":1" "$OUT"

# --- anchorless insert at end of a named function's body
make_p2
"$BIN" edit -in-scope '^main$' -span scope-body -insert end \
    -content '\tflush()\n' -expect 1 -write "$SC/p2.go" > /dev/null
OUT=$("$BIN" -F 'flush()' -in-scope '^main$' -c "$SC/p2.go")
expect_contains "anchorless insert end" ":1" "$OUT"

# --- -in-scope-kind with a custom scope pack
printf 'sub alpha {\n  x();\n}\nsub beta {\n  y();\n}\n' > "$SC/k.pl"
OUT=$("$BIN" -p '\w\(\)' -scope-pattern 'sub\s+(\w+)' -scope-open '{' \
      -scope-close '}' -scope-kind perl-sub -in-scope-kind perl-sub \
      -format '$LINE' "$SC/k.pl")
expect_eq "-in-scope-kind matching kind" "$(printf '2\n5')" "$OUT"
OUT=$("$BIN" -p '\w\(\)' -scope-pattern 'sub\s+(\w+)' -scope-open '{' \
      -scope-close '}' -scope-kind perl-sub -in-scope-kind other \
      -f "$SC/k.pl" ; true)
expect_lines "-in-scope-kind wrong kind drops all" 0 "$OUT"

# --- 'ref' field in -patterns-from rule packs
printf '{"id":"hit","regexp":"p\\\\([0-9]\\\\)"}\n{"id":"allow","regexp":"keep","ref":true}\n' > "$SC/pack.jsonl"
printf 'p(1)\np(2) // keep\np(3)\n' > "$SC/pk.txt"
OUT=$("$BIN" edit -patterns-from "$SC/pack.jsonl" -far hit:allow:0 \
      -content 'q()' -write "$SC/pk.txt")
expect_contains "patterns-from ref field: 2 sites" '"sites":2' "$OUT"
printf '{"id":"only","regexp":"x","ref":true}\n' > "$SC/allref.jsonl"
OUT=$("$BIN" edit -patterns-from "$SC/allref.jsonl" -content 'y' "$SC/pk.txt" 2>&1) ; RC=$?
expect_contains "all-ref rule pack rejected" "at least one pattern must produce" "$OUT"
[[ "$RC" == "2" ]] && report ok "all-ref rule pack exit 2" || report fail "all-ref rule pack exit (got $RC)"

# --- -list-scopes refuses silently-ignored flags
OUT=$("$BIN" -list-scopes -lines 1:2 "$SC/p2.go" 2>&1) ; RC=$?
expect_contains "-list-scopes rejects -lines" "does not apply" "$OUT"
[[ "$RC" == "2" ]] && report ok "-list-scopes -lines exit 2" || report fail "-list-scopes -lines exit (got $RC)"

rm -rf "$SC"

# ---------------------------------------------------------------------------
section "elided scope output (-elide)"
EL=$(mktemp -d)
cat > "$EL/el.go" <<'GOEOF'
package main

// target: top-level marker

func ShortFn() {
    target()
}

func LongFn() {
    target()
    step1()
    step2()
    step3()
    step4()
    step5()
    step6()
    step7()
    step8()
    target()
}
GOEOF

OUT=$("$BIN" -p 'target' -elide -scope go "$EL/el.go")
expect_contains "-elide: file header" "$EL/el.go" "$OUT"
expect_contains "-elide: scope header shows kind+name" "5-7 func ShortFn" "$OUT"
expect_contains "-elide: signature line kept" "func LongFn() {" "$OUT"
expect_contains "-elide: big gap elided" "… (+8 lines)" "$OUT"
expect_contains "-elide: closing line kept" "}" "$OUT"
expect_contains "-elide: orphan match outside any scope" "3: // target: top-level marker" "$OUT"
SHORT_BLOCK=$(printf '%s\n' "$OUT" | awk '/5-7 func ShortFn/{f=1} f{print} f && /^$/{exit}')
if [[ "$SHORT_BLOCK" != *"…"* ]]; then
    report ok "-elide: short function has no elision marker"
else
    report fail "-elide: short function has no elision marker"
fi

# --- implicit -scope auto: no -scope flag needed to trigger eliding
OUT=$("$BIN" -p 'target' -elide "$EL/el.go")
expect_contains "-elide implies -scope auto" "func ShortFn" "$OUT"

# --- mutually exclusive with other output modes and with -sample
OUT=$("$BIN" -p x -elide -llm 2>&1) ; RC=$?
expect_contains "-elide output-mode conflict" "mutually exclusive" "$OUT"
[[ "$RC" == "2" ]] && report ok "-elide output-mode conflict exit 2" || report fail "-elide output-mode conflict exit (got $RC)"
OUT=$("$BIN" -p x -elide -sample 5 2>&1) ; RC=$?
expect_contains "-elide + -sample rejected" "don't compose with -sample" "$OUT"
[[ "$RC" == "2" ]] && report ok "-elide + -sample exit 2" || report fail "-elide + -sample exit (got $RC)"

# --- -m caps how many matches participate in the render (pattern excludes
# the top-level orphan so match #1 is unambiguously the ShortFn hit)
OUT=$("$BIN" -p 'target\(\)' -elide -scope go -m 1 "$EL/el.go")
expect_contains "-elide -m 1 renders first match's scope" "func ShortFn" "$OUT"
if [[ "$OUT" != *"LongFn"* ]]; then
    report ok "-elide -m 1 excludes later matches"
else
    report fail "-elide -m 1 excludes later matches"
fi

rm -rf "$EL"

# ---------------------------------------------------------------------------
section "hotspot ranking (-hotspots)"
HS=$(mktemp -d)
cat > "$HS/hot.go" <<'GOEOF'
package main

func Busy() {
    alpha()
    beta()
}
GOEOF
cat > "$HS/cold.go" <<'GOEOF'
package main

func Quiet() {
    alpha()
}
GOEOF

# hot.go matches both patterns (full coverage + a proximity cluster) so it
# must outrank cold.go, which matches only one.
OUT=$("$BIN" -p 'alpha\(\)' -p 'beta\(\)' -hotspots 2 "$HS/hot.go" "$HS/cold.go")
FIRST_LINE=$(printf '%s\n' "$OUT" | head -n1)
expect_contains "-hotspots ranks fuller-coverage file first" "hot.go" "$FIRST_LINE"
expect_lines "-hotspots emits one row per file" 2 "$OUT"
expect_contains "-hotspots JSONL shape" '"type":"hotspot"' "$OUT"

OUT=$("$BIN" -p 'alpha\(\)' -p 'beta\(\)' -hotspots 1 -llm "$HS/hot.go" "$HS/cold.go")
expect_contains "-hotspots -llm flat line" "hot.go:" "$OUT"
expect_contains "-hotspots -llm shows patterns" "patterns=" "$OUT"

OUT=$("$BIN" -p 'alpha\(\)' -p 'beta\(\)' -hotspots 1 -elide -scope go "$HS/hot.go" "$HS/cold.go")
expect_contains "-hotspots -elide renders top file's scope" "func Busy" "$OUT"
if [[ "$OUT" != *"Quiet"* ]]; then
    report ok "-hotspots 1 -elide excludes lower-ranked file"
else
    report fail "-hotspots 1 -elide excludes lower-ranked file"
fi

# --- validation
OUT=$("$BIN" -p x -hotspots 5 -sample 3 2>&1) ; RC=$?
expect_contains "-hotspots + -sample rejected" "cannot combine with -sample" "$OUT"
[[ "$RC" == "2" ]] && report ok "-hotspots + -sample exit 2" || report fail "-hotspots + -sample exit (got $RC)"
OUT=$("$BIN" -p x -hotspots 5 -f 2>&1) ; RC=$?
expect_contains "-hotspots rejects -f output mode" "JSONL (default), -llm, or -elide" "$OUT"
[[ "$RC" == "2" ]] && report ok "-hotspots -f exit 2" || report fail "-hotspots -f exit (got $RC)"
OUT=$("$BIN" edit -p x -hotspots 5 -content y "$HS/hot.go" 2>&1) ; RC=$?
expect_contains "edit mode rejects -hotspots" "cannot combine with -hotspots" "$OUT"
[[ "$RC" == "2" ]] && report ok "edit mode -hotspots exit 2" || report fail "edit mode -hotspots exit (got $RC)"
OUT=$("$BIN" -p 'zzz_no_such_token_zzz' -hotspots 5 "$HS/hot.go" ; echo "rc=$?")
expect_contains "-hotspots no matches exit 1" "rc=1" "$OUT"

rm -rf "$HS"

# ---------------------------------------------------------------------------
section "budget-packed context (-budget)"
BG=$(mktemp -d)
cat > "$BG/hot.go" <<'GOEOF'
package main

func Busy() {
    alpha()
    beta()
}
GOEOF
cat > "$BG/cold.go" <<'GOEOF'
package main

func Quiet() {
    alpha()
}
GOEOF

# Measure each file's standalone full-render size so budget thresholds are
# derived, not hardcoded — robust to any future change in -elide's shape.
HOT_ELIDE=$("$BIN" -p 'alpha\(\)' -p 'beta\(\)' -elide -scope go "$BG/hot.go")
HOT_LEN=$(printf '%s' "$HOT_ELIDE" | wc -c)
COLD_ELIDE=$("$BIN" -p 'alpha\(\)' -p 'beta\(\)' -elide -scope go "$BG/cold.go")
COLD_LEN=$(printf '%s' "$COLD_ELIDE" | wc -c)

# --- huge budget: both files render in full, no footer at all
OUT=$("$BIN" -p 'alpha\(\)' -p 'beta\(\)' -budget $((HOT_LEN + COLD_LEN + 1000)) "$BG/hot.go" "$BG/cold.go")
expect_contains "-budget huge: hot.go in full" "func Busy" "$OUT"
expect_contains "-budget huge: cold.go in full" "func Quiet" "$OUT"
if [[ "$OUT" != *"--- budget:"* ]]; then
    report ok "-budget huge: no footer when nothing degraded"
else
    report fail "-budget huge: no footer when nothing degraded"
fi

# --- mid budget: only room for the top-ranked file (hot.go: full coverage +
# proximity cluster outranks cold.go), cold.go can't fit in any form
OUT=$("$BIN" -p 'alpha\(\)' -p 'beta\(\)' -budget $((HOT_LEN + 3)) "$BG/hot.go" "$BG/cold.go")
expect_contains "-budget mid: top file rendered in full" "func Busy" "$OUT"
if [[ "$OUT" != *"Quiet"* ]]; then
    report ok "-budget mid: lower-ranked file not shown in any form"
else
    report fail "-budget mid: lower-ranked file not shown in any form"
fi
expect_contains "-budget mid: footer counts one file in full" "1 file(s) in full" "$OUT"
expect_contains "-budget mid: footer names the dropped file" "cold.go" "$OUT"

# --- a budget below the full render but above the compact line degrades to
# the one-line compact summary instead of the source text. A single tiny
# function's full render can be as small as (or smaller than) its own
# compact line, especially under a long mktemp path, so use a file with
# several matched functions — its multi-block full render is reliably
# bigger than one compact line regardless of path length — and probe a
# range rather than assume a fixed threshold.
cat > "$BG/multi.go" <<'GOEOF'
package main

func Busy1() {
    alpha()
    beta()
}

func Busy2() {
    alpha()
    beta()
}

func Busy3() {
    alpha()
    beta()
}

func Busy4() {
    alpha()
    beta()
}
GOEOF
MULTI_ELIDE=$("$BIN" -p 'alpha\(\)' -p 'beta\(\)' -elide -scope go "$BG/multi.go")
MULTI_LEN=$(printf '%s' "$MULTI_ELIDE" | wc -c)
COMPACT_OUT=""
for b in $(seq 20 10 "$MULTI_LEN"); do
    PROBE=$("$BIN" -p 'alpha\(\)' -p 'beta\(\)' -budget "$b" "$BG/multi.go")
    if [[ "$PROBE" == *"(compact"* ]]; then COMPACT_OUT="$PROBE"; break; fi
done
expect_contains "-budget: compact summary reachable" "(compact — full render didn't fit the budget)" "$COMPACT_OUT"
if [[ "$COMPACT_OUT" != *"func Busy"* ]]; then
    report ok "-budget: compact summary omits source text"
else
    report fail "-budget: compact summary omits source text"
fi

# --- essentially zero budget: everything dropped, exit 1
OUT=$("$BIN" -p 'alpha\(\)' -p 'beta\(\)' -budget 1 "$BG/hot.go" "$BG/cold.go" ; echo "rc=$?")
expect_contains "-budget ~0: both dropped" "2 dropped" "$OUT"
expect_contains "-budget ~0: exit 1" "rc=1" "$OUT"

# --- validation
OUT=$("$BIN" -p x -budget 1000 -llm 2>&1) ; RC=$?
expect_contains "-budget + explicit output mode rejected" "defines its own output shape" "$OUT"
[[ "$RC" == "2" ]] && report ok "-budget + -llm exit 2" || report fail "-budget + -llm exit (got $RC)"
OUT=$("$BIN" -p x -budget 1000 -sample 5 2>&1) ; RC=$?
expect_contains "-budget + -sample rejected" "cannot combine with -sample/-hotspots" "$OUT"
[[ "$RC" == "2" ]] && report ok "-budget + -sample exit 2" || report fail "-budget + -sample exit (got $RC)"
OUT=$("$BIN" edit -p x -budget 1000 -content y "$BG/hot.go" 2>&1) ; RC=$?
expect_contains "edit mode rejects -budget" "cannot combine with -budget" "$OUT"
[[ "$RC" == "2" ]] && report ok "edit mode -budget exit 2" || report fail "edit mode -budget exit (got $RC)"

rm -rf "$BG"

# ---------------------------------------------------------------------------
section "identifier matching (-ident)"
ID=$(mktemp -d)
cat > "$ID/id.go" <<'GOEOF'
package main

func parseConfig() {}
func ParseConfig() {}
func parse_config() {}
func ConfigParser() {}
func HTTPServerConfig() {}
func utf8Decoder() {}
func unrelated() {}
func MAX_RETRY_COUNT() {}
GOEOF

# --- casing/separator-agnostic AND-within-group matching
OUT=$("$BIN" -ident 'parse config' -o "$ID/id.go")
expect_lines "-ident: matches all casing variants" 4 "$OUT"
expect_contains "-ident: camelCase" "parseConfig" "$OUT"
expect_contains "-ident: PascalCase" "ParseConfig" "$OUT"
expect_contains "-ident: snake_case" "parse_config" "$OUT"
expect_contains "-ident: reversed word order still matches (AND, not sequence)" "ConfigParser" "$OUT"
if [[ "$OUT" != *"unrelated"* && "$OUT" != *"HTTPServerConfig"* ]]; then
    report ok "-ident: non-matching identifiers excluded"
else
    report fail "-ident: non-matching identifiers excluded"
fi

# --- acronym and digit-boundary splitting
OUT=$("$BIN" -ident 'http server' -o "$ID/id.go")
expect_eq "-ident: acronym split (HTTPServer)" "HTTPServerConfig" "$OUT"
OUT=$("$BIN" -ident 'utf8' -o "$ID/id.go")
expect_eq "-ident: digit-spanning term" "utf8Decoder" "$OUT"
OUT=$("$BIN" -ident 'max retry' -o "$ID/id.go")
expect_eq "-ident: underscore-separated all-caps" "MAX_RETRY_COUNT" "$OUT"

# --- OR across repeated -ident groups
OUT=$("$BIN" -ident 'parse config' -ident 'unrelated' -f "$ID/id.go")
expect_contains "-ident: repeated groups OR together" "id.go" "$OUT"
COUNT=$("$BIN" -ident 'parse config' -ident 'unrelated' -o "$ID/id.go" | wc -l)
[[ "$COUNT" -eq 5 ]] && report ok "-ident: OR group adds its own matches" || report fail "-ident: OR group adds its own matches (got $COUNT)"

# --- synthetic pattern id: auto ident0/ident1 and explicit -name
OUT=$("$BIN" -ident 'parse config' -format '$PAT_ID' "$ID/id.go")
expect_eq "-ident: auto id is ident0" "ident0" "$(printf '%s\n' "$OUT" | head -n1)"
OUT=$("$BIN" -ident 'parse config' -name myid -format '$PAT_ID' "$ID/id.go")
expect_eq "-ident: -name overrides auto id" "myid" "$(printf '%s\n' "$OUT" | head -n1)"
OUT=$("$BIN" -p 'func' -ident 'parse config' -name pc -format '$PAT_ID' "$ID/id.go")
if [[ "$OUT" == *"p0"* && "$OUT" == *"pc"* ]]; then
    report ok "-ident: coexists with -p's independent p<i> numbering"
else
    report fail "-ident: coexists with -p's independent p<i> numbering"
fi

# --- composes with relations and -file-where
cat > "$ID/rel.go" <<'GOEOF'
package main

func Handler() {
    parseConfig()
    doWork()
}

func Other() {
    doWork()
}
GOEOF
OUT=$("$BIN" -p 'doWork' -name work -ident 'parse config' -name pc -near work:pc:2 -format '$LINE $PAT_ID' "$ID/rel.go")
expect_eq "-ident: participates in -near" "$(printf '4 pc\n5 work')" "$OUT"
OUT=$("$BIN" -p 'Other' -name has_other -ident 'parse config' -name pc -file-where 'NOT has_other' -f "$ID/rel.go" ; true)
expect_lines "-ident: participates in -file-where" 0 "$OUT"

# --- validation
OUT=$("$BIN" -ident 'parse config' -extract x "$ID/id.go" 2>&1) ; RC=$?
expect_contains "-extract after -ident rejected" "cannot follow -ident" "$OUT"
[[ "$RC" == "2" ]] && report ok "-extract after -ident exit 2" || report fail "-extract after -ident exit (got $RC)"
OUT=$("$BIN" -ident '   ' "$ID/id.go" 2>&1) ; RC=$?
expect_contains "-ident: empty terms rejected" "at least one term required" "$OUT"
[[ "$RC" == "2" ]] && report ok "-ident empty terms exit 2" || report fail "-ident empty terms exit (got $RC)"
OUT=$("$BIN" edit -ident 'parse config' -content x "$ID/id.go" 2>&1) ; RC=$?
expect_contains "edit mode rejects -ident" "cannot combine with -ident" "$OUT"
[[ "$RC" == "2" ]] && report ok "edit mode -ident exit 2" || report fail "edit mode -ident exit (got $RC)"
OUT=$("$BIN" -ident 'zzz nonexistent' "$ID/id.go" ; echo "rc=$?")
expect_contains "-ident: no matches exit 1" "rc=1" "$OUT"

rm -rf "$ID"

# ---------------------------------------------------------------------------
section "-file-where metadata conditions (count/churn/lang)"
FW=$(mktemp -d)
printf 'foo\nfoo\nfoo\n' > "$FW/hot.txt"
printf 'foo\n' > "$FW/cold.txt"
printf 'package main\n' > "$FW/a.go"

# --- count(pat)
OUT=$("$BIN" -p foo -name f -file-where 'count(f) >= 2' -f "$FW/hot.txt" "$FW/cold.txt")
expect_eq "-file-where count(): threshold met" "$FW/hot.txt" "$OUT"
OUT=$("$BIN" -p foo -name f -file-where 'count(f) >= 5' -f "$FW/hot.txt" "$FW/cold.txt" ; true)
expect_lines "-file-where count(): threshold not met" 0 "$OUT"

# --- lang
OUT=$("$BIN" -p 'package' -file-where 'lang == go' -f "$FW/a.go" "$FW/hot.txt")
expect_eq "-file-where lang ==" "$FW/a.go" "$OUT"
OUT=$("$BIN" -p 'foo' -file-where 'lang != go' -f "$FW/a.go" "$FW/hot.txt")
expect_eq "-file-where lang !=" "$FW/hot.txt" "$OUT"

# --- compound
OUT=$("$BIN" -p foo -name f -file-where 'count(f) >= 2 AND lang != go' -f "$FW/hot.txt" "$FW/cold.txt" "$FW/a.go")
expect_eq "-file-where compound count+lang" "$FW/hot.txt" "$OUT"

# --- churn(): fresh repo, commit hot.txt 3x and cold.txt once, all "now" —
# well within any day-window regardless of when the test runs.
GITREPO=$(mktemp -d)
(
    cd "$GITREPO"
    git init -q
    git config user.email test@example.com
    git config user.name Test
    printf 'foo\n' > hot.txt
    printf 'foo\n' > cold.txt
    git add -A && git commit -q -m 1
    printf 'foo\nfoo\n' > hot.txt
    git add -A && git commit -q -m 2
    printf 'foo\nfoo\nfoo\n' > hot.txt
    git add -A && git commit -q -m 3
)
OUT=$(cd "$GITREPO" && "$BIN" -p foo -file-where 'churn(1) > 1' -f hot.txt cold.txt)
expect_eq "-file-where churn(): more-committed file selected" "hot.txt" "$OUT"
OUT=$(cd "$GITREPO" && "$BIN" -p foo -file-where 'churn(1) > 5' -f hot.txt cold.txt ; true)
expect_lines "-file-where churn(): threshold not met" 0 "$OUT"

# --- edit mode: same predicates work for targeting
OUT=$(cd "$GITREPO" && "$BIN" edit -p foo -file-where 'churn(1) > 1' -content bar -j hot.txt)
expect_contains "edit mode: churn() predicate targets correctly" '"file":"hot.txt"' "$OUT"
OUT=$(cd "$GITREPO" && "$BIN" edit -p foo -file-where 'churn(1) > 1' -content bar -j cold.txt 2>&1) ; RC=$?
[[ "$RC" == "1" ]] && report ok "edit mode: churn() predicate excludes non-matching file" || report fail "edit mode: churn() exclude (got $RC)"

rm -rf "$GITREPO"

# --- validation
OUT=$("$BIN" -p x -file-where 'churn(abc) > 2' 2>&1) ; RC=$?
expect_contains "-file-where: non-numeric churn arg rejected" "positive integer day-window" "$OUT"
[[ "$RC" == "2" ]] && report ok "-file-where churn(abc) exit 2" || report fail "-file-where churn(abc) exit (got $RC)"
OUT=$("$BIN" -p x -file-where 'lang > go' 2>&1) ; RC=$?
expect_contains "-file-where: lang ordering operator rejected" "only supports == and !=" "$OUT"
[[ "$RC" == "2" ]] && report ok "-file-where lang> exit 2" || report fail "-file-where lang> exit (got $RC)"
OUT=$("$BIN" -p x -file-where 'foo(bar) > 2' 2>&1) ; RC=$?
expect_contains "-file-where: unknown condition function rejected" "unknown condition" "$OUT"
[[ "$RC" == "2" ]] && report ok "-file-where foo(bar) exit 2" || report fail "-file-where foo(bar) exit (got $RC)"
OUT=$("$BIN" -p x -name f -file-where 'count(zzz) > 2' 2>&1) ; RC=$?
expect_contains "-file-where: unknown pattern in count() rejected" "unknown pattern" "$OUT"
[[ "$RC" == "2" ]] && report ok "-file-where count(zzz) exit 2" || report fail "-file-where count(zzz) exit (got $RC)"

rm -rf "$FW"

# ---------------------------------------------------------------------------
section "sorting file-grouped output (-order-by)"
OB=$(mktemp -d)
printf 'foo\nfoo\nfoo\n' > "$OB/c_three.txt"
printf 'foo\n' > "$OB/a_one.txt"
printf 'foo\nfoo\n' > "$OB/b_two.txt"

OUT=$("$BIN" -p foo -c -order-by path "$OB/c_three.txt" "$OB/a_one.txt" "$OB/b_two.txt")
expect_eq "-order-by path" "$(printf '%s:1\n%s:2\n%s:3' "$OB/a_one.txt" "$OB/b_two.txt" "$OB/c_three.txt")" "$OUT"

OUT=$("$BIN" -p foo -c -order-by count "$OB/c_three.txt" "$OB/a_one.txt" "$OB/b_two.txt")
FIRST_LINE=$(printf '%s\n' "$OUT" | head -n1)
expect_eq "-order-by count: highest first" "$OB/c_three.txt:3" "$FIRST_LINE"

OUT=$("$BIN" -p foo -f -order-by score "$OB/c_three.txt" "$OB/a_one.txt" "$OB/b_two.txt")
expect_eq "-order-by score: densest file first" "$OB/c_three.txt" "$(printf '%s\n' "$OUT" | head -n1)"

# --- -c lists every file (including :0) even when ordered
printf 'nothing\n' > "$OB/d_zero.txt"
OUT=$("$BIN" -p foo -c -order-by path "$OB/d_zero.txt" "$OB/a_one.txt" ; echo "rc=$?")
expect_contains "-order-by: -c still lists 0-match files" "d_zero.txt:0" "$OUT"
expect_contains "-order-by: exit code reflects matches, not rows" "rc=0" "$OUT"
OUT=$("$BIN" -p foo -c -order-by path "$OB/d_zero.txt" ; echo "rc=$?")
expect_contains "-order-by: exit 1 when the only row has 0 matches" "rc=1" "$OUT"

# --- validation
OUT=$("$BIN" -p foo -order-by path 2>&1) ; RC=$?
expect_contains "-order-by requires -f/-c" "requires -f or -c output" "$OUT"
[[ "$RC" == "2" ]] && report ok "-order-by without -f/-c exit 2" || report fail "-order-by without -f/-c exit (got $RC)"
OUT=$("$BIN" -p foo -order-by bogus 2>&1) ; RC=$?
expect_contains "-order-by: unknown field rejected" "unknown field" "$OUT"
[[ "$RC" == "2" ]] && report ok "-order-by bogus field exit 2" || report fail "-order-by bogus field exit (got $RC)"
OUT=$("$BIN" edit -p foo -order-by path -content bar "$OB/a_one.txt" 2>&1) ; RC=$?
expect_contains "edit mode rejects -order-by" "cannot combine with -order-by" "$OUT"
[[ "$RC" == "2" ]] && report ok "edit mode -order-by exit 2" || report fail "edit mode -order-by exit (got $RC)"

rm -rf "$OB"

# ---------------------------------------------------------------------------
section "cross-invocation dedup (-seen)"
SN=$(mktemp -d)
cat > "$SN/f.go" <<'GOEOF'
package main

func Alpha() {
    target()
}

func Beta() {
    target()
}
GOEOF

# --- first run: full render, state file written in the documented format
OUT=$("$BIN" -p target -elide -scope go -seen "$SN/state.txt" "$SN/f.go")
expect_contains "-seen first run: full render" "func Alpha() {" "$OUT"
expect_contains "-seen first run: full render (2nd fn)" "func Beta() {" "$OUT"
[[ -f "$SN/state.txt" ]] && report ok "-seen: state file created" || report fail "-seen: state file created"
STATE_LINES=$(wc -l < "$SN/state.txt")
[[ "$STATE_LINES" -eq 2 ]] && report ok "-seen: one state line per scope" || report fail "-seen: one state line per scope (got $STATE_LINES)"

# --- second run, nothing changed: both collapse
OUT=$("$BIN" -p target -elide -scope go -seen "$SN/state.txt" "$SN/f.go")
expect_contains "-seen: unchanged Alpha collapses" "func Alpha (unchanged, already shown)" "$OUT"
expect_contains "-seen: unchanged Beta collapses" "func Beta (unchanged, already shown)" "$OUT"
if [[ "$OUT" != *"func Alpha() {"* && "$OUT" != *"func Beta() {"* ]]; then
    report ok "-seen: collapsed chunks omit source text"
else
    report fail "-seen: collapsed chunks omit source text"
fi

# --- change one function's body (same line count/range): hash mismatch
# still forces a full re-render even though the range didn't move.
cat > "$SN/f.go" <<'GOEOF'
package main

func Alpha() {
    target2()
}

func Beta() {
    target()
}
GOEOF
OUT=$("$BIN" -p 'target2|target' -elide -scope go -seen "$SN/state.txt" "$SN/f.go")
expect_contains "-seen: content change at same range re-renders" "func Alpha() {" "$OUT"
expect_contains "-seen: untouched function still collapses" "func Beta (unchanged, already shown)" "$OUT"

# --- -budget: a render measured to decide fit, then degraded/dropped, must
# NOT be recorded as seen — a later run with enough budget renders in full.
rm -f "$SN/bstate.txt"
"$BIN" -p target -budget 10 -seen "$SN/bstate.txt" "$SN/f.go" > /dev/null
if [[ ! -s "$SN/bstate.txt" ]]; then
    report ok "-seen + -budget: dropped render isn't marked seen"
else
    report fail "-seen + -budget: dropped render isn't marked seen"
fi
OUT=$("$BIN" -p target -elide -scope go -seen "$SN/bstate.txt" "$SN/f.go")
expect_contains "-seen + -budget: later full run still shows real content" "func Beta() {" "$OUT"

# --- missing state file on first use: no error, just an empty store
rm -f "$SN/fresh.txt"
OUT=$("$BIN" -p target -elide -scope go -seen "$SN/fresh.txt" "$SN/f.go" 2>&1) ; RC=$?
[[ "$RC" == "0" ]] && report ok "-seen: missing state file is not an error" || report fail "-seen: missing state file (got rc=$RC)"

# --- validation
OUT=$("$BIN" -p target -seen "$SN/state.txt" "$SN/f.go" 2>&1) ; RC=$?
expect_contains "-seen requires -elide/-budget" "requires -elide or -budget" "$OUT"
[[ "$RC" == "2" ]] && report ok "-seen without -elide/-budget exit 2" || report fail "-seen without -elide/-budget exit (got $RC)"
OUT=$("$BIN" edit -p target -seen "$SN/state.txt" -content x "$SN/f.go" 2>&1) ; RC=$?
expect_contains "edit mode rejects -seen" "cannot combine with -seen" "$OUT"
[[ "$RC" == "2" ]] && report ok "edit mode -seen exit 2" || report fail "edit mode -seen exit (got $RC)"

rm -rf "$SN"

# ---------------------------------------------------------------------------
section "summary"
TOTAL=$((PASS + FAIL))
if [[ "$FAIL" -eq 0 ]]; then
    printf "\n\033[32mAll %d tests passed.\033[0m\n" "$TOTAL"
    exit 0
else
    printf "\n\033[31m%d/%d FAILED:\033[0m\n" "$FAIL" "$TOTAL"
    for n in "${FAILED_NAMES[@]}"; do printf "  - %s\n" "$n"; done
    exit 1
fi
