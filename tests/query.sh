#!/usr/bin/env bash
set -euo pipefail

BIN=${HPRSCRIPT_BIN:-./hprscript}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
PASS=0

ok() { PASS=$((PASS + 1)); printf 'ok %d - %s\n' "$PASS" "$1"; }
has() { [[ $1 == *"$2"* ]] || { printf 'not ok - %s\n%s\n' "$3" "$1"; exit 1; }; ok "$3"; }
run_rc() { set +e; "$@"; local rc=$?; set -e; return "$rc"; }

printf '%s\n' 'DEF foo' 'DEF orphan' 'CALL foo' 'CALL missing' 'CALL foo' 'KEY token' >"$TMP/a.txt"
printf '%s\n' 'CALL foo' 'token is used here' >"$TMP/b.txt"
printf '%s\n' 'VALUE a+b' 'VALUE a+b' 'VALUE' >"$TMP/derived-source.txt"
printf '%s\n' 'literal a+b occurs here' >"$TMP/derived-target.txt"
printf '%s\n' \
  'void Alpha() {' \
  'SOURCE item' \
  'SANITIZE item' \
  'SINK item' \
  '}' \
  'void Beta() {' \
  'SOURCE raw' \
  'SINK raw' \
  '}' >"$TMP/flow.cpp"

BASE='{"version":1,"sets":[{"id":"defs","scan":["'"$TMP"'/*.txt"],"patterns":[{"id":"def","regexp":"^DEF ([A-Za-z]+)","extract":["name"]}]},{"id":"uses","scan":["'"$TMP"'/*.txt"],"patterns":[{"id":"call","regexp":"^CALL ([A-Za-z]+)","extract":["name"]}]}],"query":{"from":{"set":"uses","as":"use"},"joins":[{"type":"inner","set":"defs","as":"def","on":[{"op":"eq","left":"use.capture.name","right":"def.capture.name"}]}],"select":{"symbol":"use.capture.name","file":"use.file","definition":"def.file"},"order_by":[{"field":"file","direction":"asc"}]}}'
OUT=$($BIN query -q "$BASE" -summary)
has "$OUT" '"symbol":"foo"' 'equality join projects matching capture values'
[[ $OUT != *'"symbol":"missing"'* ]] || { printf 'not ok - inner join leaked missing symbol\n'; exit 1; }
ok 'inner join excludes rows without a partner'
has "$OUT" '"scan_stages":1' 'compatible sets share one scan stage'
has "$OUT" '"patterns_compiled":2' 'combined matcher accounting is explicit'

ANTI='{"version":1,"sets":[{"id":"defs","scan":["'"$TMP"'/*.txt"],"patterns":[{"id":"def","regexp":"^DEF ([A-Za-z]+)","extract":["name"]}]},{"id":"uses","scan":["'"$TMP"'/*.txt"],"patterns":[{"id":"call","regexp":"^CALL ([A-Za-z]+)","extract":["name"]}]}],"query":{"from":{"set":"defs","as":"def"},"joins":[{"type":"anti","set":"uses","as":"use","on":[{"op":"eq","left":"def.capture.name","right":"use.capture.name"}]}],"select":{"symbol":"def.capture.name","file":"def.file"}}}'
ANTI_OUT=$($BIN query -q "$ANTI")
has "$ANTI_OUT" '"symbol":"orphan"' 'anti-join finds declarations without references'
[[ $ANTI_OUT != *'"symbol":"foo"'* ]] || { printf 'not ok - anti join retained used declaration\n'; exit 1; }
ok 'anti-join removes declarations with references'

GROUP='{"version":1,"sets":[{"id":"uses","scan":["'"$TMP"'/*.txt"],"patterns":[{"id":"call","regexp":"^CALL ([A-Za-z]+)","extract":["name"]}]}],"query":{"from":{"set":"uses","as":"use"},"group_by":["use.capture.name"],"select":{"symbol":"use.capture.name","uses":{"count":"*"},"files":{"collect_distinct":"use.file"}},"having":{"op":"gt","left":"uses","right":1},"order_by":[{"field":"uses","direction":"desc"}]}}'
GROUP_OUT=$($BIN query -q "$GROUP")
has "$GROUP_OUT" '"uses":3' 'grouping and count aggregation preserve integers'
has "$GROUP_OUT" '"files":[' 'collect_distinct aggregation emits a list'

FLOW='{"version":1,"sets":[{"id":"source","scan":["'"$TMP"'/flow.cpp"],"scope":"auto","patterns":[{"id":"source","regexp":"SOURCE ([A-Za-z]+)","extract":["name"]}]},{"id":"sink","scan":["'"$TMP"'/flow.cpp"],"scope":"auto","patterns":[{"id":"sink","regexp":"SINK ([A-Za-z]+)","extract":["name"]}]},{"id":"san","scan":["'"$TMP"'/flow.cpp"],"scope":"auto","patterns":[{"id":"san","regexp":"SANITIZE ([A-Za-z]+)","extract":["name"]}]}],"query":{"from":{"set":"source","as":"source"},"joins":[{"type":"inner","set":"sink","as":"sink","on":[{"op":"eq","left":"source.capture.name","right":"sink.capture.name"},{"op":"same_scope","left":"source","right":"sink"},{"op":"before","left":"source","right":"sink"}]},{"type":"anti","set":"san","as":"san","on":[{"op":"eq","left":"source.capture.name","right":"san.capture.name"},{"op":"same_scope","left":"source","right":"san"}]}],"select":{"name":"source.capture.name","scope":"source.enclosing.name"}}}'
FLOW_OUT=$($BIN query -q "$FLOW" -explain-plan)
has "$FLOW_OUT" '"op":"hash-inner-join"' 'composite joins use their equality key as a hash index'
has "$FLOW_OUT" '"name":"raw"' 'source-before-sink without sanitizer is found'
[[ $FLOW_OUT != *'"name":"item"'* ]] || { printf 'not ok - sanitized source leaked through anti join\n'; exit 1; }
ok 'same-scope sanitizer anti-join removes the sanitized flow'

LEFT='{"version":1,"sets":[{"id":"defs","scan":["'"$TMP"'/a.txt"],"patterns":[{"id":"def","regexp":"^DEF ([A-Za-z]+)","extract":["name"]}]},{"id":"uses","scan":["'"$TMP"'/a.txt"],"patterns":[{"id":"call","regexp":"^CALL ([A-Za-z]+)","extract":["name"]}]}],"query":{"from":{"set":"defs","as":"def"},"joins":[{"type":"left","set":"uses","as":"use","on":[{"op":"eq","left":"def.capture.name","right":"use.capture.name"}]}],"select":{"symbol":"def.capture.name","use_file":"use.file"}}}'
LEFT_OUT=$($BIN query -q "$LEFT")
has "$LEFT_OUT" '"symbol":"orphan","use_file":null' 'left join preserves unmatched rows with typed nulls'

SAME_FILE='{"version":1,"sets":[{"id":"keys","scan":["'"$TMP"'/*.txt"],"patterns":[{"id":"key","regexp":"^KEY ([A-Za-z]+)","extract":["name"]}]},{"id":"mentions","scan":["'"$TMP"'/*.txt"],"patterns":[{"id":"mention","regexp":"token"}]}],"query":{"from":{"set":"keys","as":"key"},"joins":[{"type":"inner","set":"mentions","as":"mention","on":[{"op":"same_file","left":"key","right":"mention"}]}],"select":{"key_file":"key.file","mention_file":"mention.file"}}}'
SAME_FILE_OUT=$($BIN query -q "$SAME_FILE" -explain-plan)
has "$SAME_FILE_OUT" '"op":"interval-inner-join"' 'pure location joins expose an interval strategy'
[[ $SAME_FILE_OUT != *'"mention_file":"'"$TMP"'/b.txt"'* ]] || {
  printf 'not ok - same-file join crossed file boundaries\n'; exit 1;
}
ok 'same-file join partitions candidates by file'

SAME_SCOPE='{"version":1,"sets":[{"id":"source","scan":["'"$TMP"'/flow.cpp"],"scope":"auto","patterns":[{"id":"source","regexp":"SOURCE"}]},{"id":"sink","scan":["'"$TMP"'/flow.cpp"],"scope":"auto","patterns":[{"id":"sink","regexp":"SINK"}]}],"query":{"from":{"set":"source","as":"source"},"joins":[{"type":"inner","set":"sink","as":"sink","on":[{"op":"same_scope","left":"source","right":"sink"}]}],"select":{"source_scope":"source.enclosing.name","sink_scope":"sink.enclosing.name"}}}'
SCOPE_OUT=$($BIN query -q "$SAME_SCOPE")
has "$SCOPE_OUT" '"sink_scope":"Alpha","source_scope":"Alpha"' 'same-scope join correlates Alpha rows'
has "$SCOPE_OUT" '"sink_scope":"Beta","source_scope":"Beta"' 'same-scope join correlates Beta rows'
[[ $SCOPE_OUT != *'"sink_scope":"Beta","source_scope":"Alpha"'* ]] || {
  printf 'not ok - same-scope join crossed scopes\n'; exit 1;
}
ok 'same-scope join partitions candidates by enclosing scope'

WITHIN='{"version":1,"sets":[{"id":"source","scan":["'"$TMP"'/flow.cpp"],"patterns":[{"id":"source","regexp":"SOURCE ([A-Za-z]+)","extract":["name"]}]},{"id":"san","scan":["'"$TMP"'/flow.cpp"],"patterns":[{"id":"san","regexp":"SANITIZE ([A-Za-z]+)","extract":["name"]}]}],"query":{"from":{"set":"source","as":"source"},"joins":[{"type":"inner","set":"san","as":"san","on":[{"op":"within_lines","left":"source","right":"san","lines":1}]}],"select":{"name":"source.capture.name"}}}'
WITHIN_OUT=$($BIN query -q "$WITHIN")
has "$WITHIN_OUT" '"name":"item"' 'within-lines join finds nearby rows'
[[ $WITHIN_OUT != *'"name":"raw"'* ]] || { printf 'not ok - within-lines leaked distant row\n'; exit 1; }
ok 'within-lines join excludes distant rows'

PROJECT='{"version":1,"sets":[{"id":"uses","scan":["'"$TMP"'/a.txt"],"patterns":[{"id":"call","regexp":"^CALL ([A-Za-z]+)","extract":["name"]}]}],"query":{"from":{"set":"uses","as":"use"},"where":{"op":"starts_with","left":"use.capture.name","right":"f"},"select":{"lower":{"lower":"use.capture.name"},"upper":{"upper":"use.capture.name"},"base":{"basename":"use.file"},"dir":{"dirname":"use.file"},"label":{"concat":["use.capture.name",{"literal":"!"}]},"fallback":{"coalesce":["use.enclosing.name",{"literal":"none"}]}}}}'
PROJECT_OUT=$($BIN query -q "$PROJECT")
has "$PROJECT_OUT" '"base":"a.txt"' 'projection basename expression works'
has "$PROJECT_OUT" '"fallback":"none"' 'projection coalesce preserves typed null behavior'
has "$PROJECT_OUT" '"label":"foo!"' 'projection concat expression works'
has "$PROJECT_OUT" '"lower":"foo","upper":"FOO"' 'projection case functions work'

GROUP_MEMBER='{"version":1,"sets":[{"id":"uses","scan":["'"$TMP"'/*.txt"],"patterns":[{"id":"call","regexp":"^CALL ([A-Za-z]+)","extract":["name"]}]}],"query":{"from":{"set":"uses","as":"use"},"group_by":["use.capture.name"],"select":{"symbol":"use.capture.name","uses":{"count":"*"},"files":{"collect_distinct":"use.file"}},"having":{"op":"contains","left":"files","right":"'"$TMP"'/b.txt"}}}'
GROUP_MEMBER_OUT=$($BIN query -q "$GROUP_MEMBER")
has "$GROUP_MEMBER_OUT" '"symbol":"foo"' 'grouped-list membership filters aggregate results'

ADAPT='{"version":1,"sets":[{"id":"keys","scan":["'"$TMP"'/derived-source.txt"],"patterns":[{"id":"key","regexp":"^VALUE ?(.*)$","extract":["name"]}]},{"id":"hits","scan":["'"$TMP"'/derived-target.txt"],"derive_patterns":{"from_set":"keys","field":"capture.name","mode":"literal","deduplicate":true,"max_patterns":10}}],"query":{"from":{"set":"hits","as":"hit"},"select":{"key":"hit.derived.value","source_rows":"hit.derived.source_rows","file":"hit.file"}}}'
ADAPT_OUT=$($BIN query -q "$ADAPT" -summary 2>&1)
has "$ADAPT_OUT" '"key":"a+b"' 'literal adaptive patterns escape regex metacharacters'
has "$ADAPT_OUT" '"source_rows":[' 'adaptive matcher preserves source-row identity'
has "$ADAPT_OUT" '"scan_stages":2' 'adaptive query reports two scan stages'
has "$ADAPT_OUT" '"patterns_compiled":2' 'duplicate derived values compile only once'
has "$ADAPT_OUT" 'skipped 1 empty' 'empty adaptive values are reported explicitly'

ADAPT_CAP=${ADAPT/\"max_patterns\":10/\"max_patterns\":1}
printf '%s\n' 'VALUE second' >>"$TMP/derived-source.txt"
set +e
$BIN query -q "$ADAPT_CAP" >/dev/null 2>&1
RC=$?
set -e
[[ $RC -eq 2 ]] || { printf 'not ok - adaptive pattern cap did not fail\n'; exit 1; }
ok 'adaptive max-pattern cap fails explicitly'

LIMIT='{"version":1,"sets":[{"id":"uses","scan":["'"$TMP"'/*.txt"],"patterns":[{"id":"call","regexp":"^CALL ([A-Za-z]+)","extract":["name"]}]}],"limits":{"max_rows_per_set":1},"query":{"from":{"set":"uses","as":"use"},"select":{"name":"use.capture.name"}}}'
set +e
$BIN query -q "$LIMIT" >/dev/null 2>&1
RC=$?
set -e
[[ $RC -eq 2 ]] || { printf 'not ok - hard row limit did not fail with exit 2\n'; exit 1; }
ok 'hard resource limit fails instead of returning partial-looking results'

PARTIAL=${LIMIT/\"version\":1/\"version\":1,\"on_limit\":\"partial\"}
PARTIAL_OUT=$($BIN query -q "$PARTIAL")
has "$PARTIAL_OUT" '"type":"query-footer"' 'partial mode emits an explicit footer'
has "$PARTIAL_OUT" '"complete":false' 'partial footer marks incompleteness'

BAD='{"version":1,"sets":[],"query":{"from":{"set":"x","as":"x"},"select":{},"mystery":true}}'
set +e
$BIN query -q "$BAD" >/dev/null 2>&1
RC=$?
set -e
[[ $RC -eq 2 ]] || { printf 'not ok - unknown query field did not fail\n'; exit 1; }
ok 'unknown version-1 query fields fail strictly'

PLAN=$($BIN query -q "$BASE" -plan-only)
has "$PLAN" '"type":"execution-plan"' 'query plan-only validates and plans without scanning'
has "$PLAN" '"op":"hash-inner-join"' 'query plan exposes hash join selection'

DIFFERENT='{"version":1,"sets":[{"id":"defs","scan":["'"$TMP"'/a.txt"],"patterns":[{"id":"def","regexp":"^DEF"}]},{"id":"uses","scan":["'"$TMP"'/b.txt"],"patterns":[{"id":"use","regexp":"^CALL"}]}],"query":{"from":{"set":"defs","as":"def"},"select":{"file":"def.file"}}}'
DIFFERENT_PLAN=$($BIN query -q "$DIFFERENT" -plan-only)
has "$DIFFERENT_PLAN" '"id":"scan1"' 'different scan configurations use separate stages'

REPEAT=$($BIN query -q "$BASE")
REPEAT2=$($BIN query -q "$BASE")
[[ $REPEAT == "$REPEAT2" ]] || { printf 'not ok - query output is not deterministic\n'; exit 1; }
ok 'unchanged query inputs produce deterministic ordering'

EMPTY='{"version":1,"sets":[{"id":"none","scan":["'"$TMP"'/a.txt"],"patterns":[{"id":"none","regexp":"definitely-absent"}]}],"query":{"from":{"set":"none","as":"none"},"select":{"file":"none.file"}}}'
set +e
$BIN query -q "$EMPTY" >/dev/null
RC=$?
set -e
[[ $RC -eq 1 ]] || { printf 'not ok - empty query result did not return 1\n'; exit 1; }
ok 'query with no rows returns exit 1'

CARTESIAN='{"version":1,"sets":[{"id":"defs","scan":["'"$TMP"'/a.txt"],"patterns":[{"id":"def","regexp":"^DEF"}]},{"id":"uses","scan":["'"$TMP"'/a.txt"],"patterns":[{"id":"use","regexp":"^CALL"}]}],"limits":{"max_cartesian_rows":1},"query":{"from":{"set":"defs","as":"def"},"joins":[{"type":"inner","set":"uses","as":"use","on":[{"op":"ne","left":"def.line","right":"use.line"}]}],"select":{"def":"def.line","use":"use.line"}}}'
set +e
CART_OUT=$($BIN query -q "$CARTESIAN" 2>&1)
RC=$?
set -e
[[ $RC -eq 2 ]] || { printf 'not ok - predicted Cartesian join did not fail\n'; exit 1; }
has "$CART_OUT" 'predicted Cartesian product exceeds' 'Cartesian guard fails before execution with guidance'

CART_ALLOWED=${CARTESIAN/\"on\":[/\"allow_cartesian\":true,\"on\":[}
CART_ALLOWED_OUT=$($BIN query -q "$CART_ALLOWED")
has "$CART_ALLOWED_OUT" '"def":1' 'explicitly allowed small Cartesian join executes'

printf '1..%d\n' "$PASS"
