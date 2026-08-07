#!/usr/bin/env bash
set -euo pipefail

BIN=${HPRSCRIPT_BIN:-./hprscript}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
PASS=0

ok() { PASS=$((PASS + 1)); printf 'ok %d - %s\n' "$PASS" "$1"; }
has() { [[ $1 == *"$2"* ]] || { printf 'not ok - %s\n%s\n' "$3" "$1"; exit 1; }; ok "$3"; }

mkdir -p "$TMP/src" "$TMP/tests" "$TMP/config" "$TMP/docs"
printf '%s\n' \
  'package auth' \
  'func validateToken(raw string) error {' \
  '    token := parseToken(raw)' \
  '    return ErrInvalidToken(token)' \
  '}' >"$TMP/src/token.go"
printf '%s\n' \
  'package auth' \
  'func TestValidateToken() {' \
  '    validateToken("bad")' \
  '}' >"$TMP/tests/token_test.go"
printf '%s\n' 'validator: validateToken' >"$TMP/config/auth.yaml"
printf '%s\n' 'The validateToken flow rejects invalid credentials.' >"$TMP/docs/auth.md"

OUT=$($BIN investigate -F validateToken -profile symbol -followup-scan always \
  -top-files 8 -top-scopes 8 -related 8 -examples 8 -summary "$TMP")
FIRST=${OUT%%$'\n'*}
has "$FIRST" '"type":"investigation-summary"' 'summary is the first investigation record'
has "$FIRST" '"profile":"symbol"' 'selected profile is explicit'
has "$OUT" '"type":"investigation-file"' 'ranked file records are emitted'
has "$OUT" '"roles":["test","source"]' 'test role is path-heuristic evidence'
has "$OUT" '"roles":["documentation"]' 'concept evidence includes documentation roles'
has "$OUT" '"type":"investigation-scope"' 'scope evidence is emitted'
has "$OUT" '"type":"investigation-related"' 'related identifiers are emitted'
has "$OUT" '"type":"investigation-evidence"' 'representative evidence is emitted'
has "$OUT" '"method":"lexical-go-pack"' 'lexical classification method is explicit'
has "$OUT" '"scan_stages":2' 'one optional follow-up scan is accounted'
has "$OUT" '"matcher_compilations":2' 'matcher compilations are accounted'
has "$OUT" '"type":"investigation-footer"' 'investigation footer is emitted'

LLM=$($BIN investigate -F validateToken -profile symbol -followup-scan never \
  -llm "$TMP")
has "$LLM" 'scans=1' 'followup-scan never guarantees one stage'
has "$LLM" 'TOP FILES' 'LLM report has stable top-files section'
has "$LLM" 'PROBABLE DEFINITIONS / DECLARATIONS' 'LLM report separates definition evidence'
has "$LLM" 'RELATED IDENTIFIERS' 'LLM report has stable related section'
has "$LLM" 'REPRESENTATIVE EVIDENCE' 'LLM report has stable evidence section'
has "$LLM" 'LIMITS' 'LLM report discloses limits'

BUDGET=$($BIN investigate -F validateToken -profile symbol -followup-scan never \
  -top-files 8 -top-scopes 8 -related 8 -examples 8 -evidence-budget 500 "$TMP")
has "$BUDGET" '"classification":"probable_definition"' \
  'evidence budget reserves probable definitions before rankings'
has "$BUDGET" '"omitted_files":' 'budget footer discloses omitted file rows'
[[ $BUDGET == *'"omitted_files":0'* ]] && {
  printf 'not ok - constrained budget should omit a lower-priority file row\n'; exit 1;
}
ok 'constrained budget truncates lower-priority file rows first'

printf '%s\n' \
  'func validateToken() {' \
  '  alphaName betaName gammaName deltaName epsilonName' \
  '  zetaName etaName thetaName iotaName kappaName' \
  '}' >"$TMP/src/many.go"
CAP=$($BIN investigate -F validateToken -followup-scan always \
  -max-related-patterns 2 -related 2 "$TMP")
has "$CAP" '"candidate_patterns_omitted":' 'candidate cap is disclosed in the footer'
[[ $CAP == *'"candidate_patterns_omitted":0'* ]] && {
  printf 'not ok - constrained candidate cap should omit derived patterns\n'; exit 1;
}
ok 'adaptive candidate truncation is counted'
has "$CAP" '"stop_reason":"adaptive_pattern_cap"' 'adaptive cap marks the package incomplete'

MEM=$($BIN investigate -F validateToken -followup-scan never \
  -max-memory-bytes 1 "$TMP")
has "$MEM" '"stop_reason":"memory_cap"' 'configurable evidence-memory cap is explicit'

REPEAT=$($BIN investigate -F validateToken -profile symbol -followup-scan never "$TMP")
REPEAT2=$($BIN investigate -F validateToken -profile symbol -followup-scan never "$TMP")
[[ $REPEAT == "$REPEAT2" ]] || { printf 'not ok - deterministic repeated output\n'; exit 1; }
ok 'unchanged inputs produce deterministic output'

PLAN=$($BIN investigate -F validateToken -plan-only "$TMP")
has "$PLAN" '"type":"execution-plan"' 'investigation plan-only emits a plan'
has "$PLAN" '"adaptive":true' 'plan exposes the conditional adaptive stage'

set +e
$BIN investigate -F validateToken -sample 2 "$TMP" >/dev/null 2>&1
RC=$?
set -e
[[ $RC -eq 2 ]] || { printf 'not ok - incompatible options return 2\n'; exit 1; }
ok 'incompatible quick-search output options are rejected'

set +e
$BIN investigate -F definitelyAbsent -followup-scan never "$TMP" >/dev/null 2>&1
RC=$?
set -e
[[ $RC -eq 1 ]] || { printf 'not ok - no seed matches returns 1\n'; exit 1; }
ok 'no seed matches returns exit 1'

set +e
printf '%s\n' "$TMP/missing" >"$TMP/missing.list"
$BIN investigate -F validateToken -files-from "$TMP/missing.list" >/dev/null 2>&1
RC=$?
set -e
[[ $RC -eq 1 ]] || { printf 'not ok - incomplete optional scan returns match status\n'; exit 1; }
ok 'unreadable input without completeness requirement returns match status'

set +e
$BIN investigate -F validateToken -require-complete \
  -files-from "$TMP/missing.list" >/dev/null 2>&1
RC=$?
set -e
[[ $RC -eq 2 ]] || { printf 'not ok - incomplete required scan returns 2\n'; exit 1; }
ok 'unreadable required input returns exit 2'

printf '1..%d\n' "$PASS"
