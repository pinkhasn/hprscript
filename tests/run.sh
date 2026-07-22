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
