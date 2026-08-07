#!/usr/bin/env bash
set -euo pipefail

BIN=${HPRSCRIPT_BIN:-./hprscript}
HERE=$(cd "$(dirname "$0")" && pwd)
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
PASS=0

check() {
    local name=$1 actual=$2
    if diff -u "$HERE/golden/$name" "$actual"; then
        PASS=$((PASS + 1)); printf 'ok %d - golden %s\n' "$PASS" "$name"
    else
        printf 'not ok - golden %s\n' "$name"; exit 1
    fi
}

normalize() {
    local input=$1 output=$2
    sed -E \
      -e "s#$TMP#<ROOT>#g" \
      -e "s#$BIN#<BIN>#g" \
      -e 's/"created_at": "[^"]*"/"created_at": "<TIMESTAMP>"/' \
      -e 's/"tool_version": "[^"]*"/"tool_version": "<VERSION>"/' \
      -e 's#<ROOT>/partial/\.hpr-apply\.[^"]+#<TEMP>#g' \
      "$input" >"$output"
}

mkdir -p "$TMP/src" "$TMP/edit" "$TMP/partial"
printf 'func target() {\n  helper()\n}\n' >"$TMP/src/x.go"
printf 'old\n' >"$TMP/edit/a.txt"
printf 'old\n' >"$TMP/partial/a.txt"
printf 'old\n' >"$TMP/partial/b.txt"
chmod 0644 "$TMP/edit/a.txt" "$TMP/partial/a.txt" "$TMP/partial/b.txt"

"$BIN" investigate -F target -profile symbol -followup-scan never \
  -top-files 1 -top-scopes 1 -related 0 -examples 1 -evidence-budget 0 \
  "$TMP/src" >"$TMP/investigation.raw"
normalize "$TMP/investigation.raw" "$TMP/investigation.jsonl"
check investigation.jsonl "$TMP/investigation.jsonl"

"$BIN" investigate -F target -profile symbol -followup-scan never \
  -top-files 1 -top-scopes 1 -related 0 -examples 1 -evidence-budget 0 \
  -llm "$TMP/src" >"$TMP/investigation-llm.raw"
normalize "$TMP/investigation-llm.raw" "$TMP/investigation.llm"
check investigation.llm "$TMP/investigation.llm"

QUERY='{"version":1,"sets":[{"id":"symbols","scan":["'"$TMP"'/src/x.go"],"scope":"auto","patterns":[{"id":"symbol","regexp":"(target|helper)","extract":["name"]}]}],"query":{"from":{"set":"symbols","as":"symbol"},"select":{"file":"symbol.file","line":"symbol.line","name":"symbol.capture.name"},"order_by":[{"field":"line","direction":"asc"}]}}'
"$BIN" query -q "$QUERY" >"$TMP/query.raw"
normalize "$TMP/query.raw" "$TMP/query.jsonl"
check query.jsonl "$TMP/query.jsonl"

"$BIN" query -q "$QUERY" -plan-only >"$TMP/query-plan.raw"
normalize "$TMP/query-plan.raw" "$TMP/query-plan.jsonl"
check query-plan.jsonl "$TMP/query-plan.jsonl"

(
    cd "$TMP/edit"
    "$BIN" edit -F old -content new -plan-out p.json a.txt >/dev/null
)
normalize "$TMP/edit/p.json" "$TMP/edit-plan.json"
check edit-plan.json "$TMP/edit-plan.json"
(
    cd "$TMP/edit"
    "$BIN" apply p.json -j >"$TMP/apply.raw"
)
normalize "$TMP/apply.raw" "$TMP/apply.jsonl"
check apply.jsonl "$TMP/apply.jsonl"

(
    cd "$TMP/partial"
    "$BIN" edit -F old -content new -plan-out p.json a.txt b.txt >/dev/null
)
set +e
(
    cd "$TMP/partial"
    HPRSCRIPT_ENABLE_FAULT_INJECTION=1 HPRSCRIPT_TEST_FAIL_RENAME_N=2 \
      "$BIN" apply p.json -j >"$TMP/partial.raw" 2>/dev/null
)
RC=$?
set -e
[[ $RC -eq 4 ]] || { printf 'not ok - golden partial apply expected exit 4\n'; exit 1; }
normalize "$TMP/partial.raw" "$TMP/partial.jsonl"
check partial-apply.jsonl "$TMP/partial.jsonl"

printf '1..%d\n' "$PASS"
