#!/usr/bin/env bash
set -euo pipefail

BIN=${HPRSCRIPT_BIN:-./hprscript}
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
PASS=0

ok() { PASS=$((PASS + 1)); printf 'ok %d - %s\n' "$PASS" "$1"; }
has() { [[ $1 == *"$2"* ]] || { printf 'not ok - %s\n%s\n' "$3" "$1"; exit 1; }; ok "$3"; }

printf 'alpha beta\n' >"$TMP/input.txt"

OUT=$($BIN -F alpha -F beta -j -summary "$TMP/input.txt")
SUMMARY=${OUT##*$'\n'}
has "$SUMMARY" '"bytes_scanned":11' 'summary accounts exact input bytes'
has "$SUMMARY" '"scan_stages":1' 'quick search reports one scan stage'
has "$SUMMARY" '"matcher_compilations":1' 'batched patterns report one matcher compilation'
has "$SUMMARY" '"patterns_compiled":2' 'summary reports compiled pattern count'
has "$SUMMARY" '"rows_materialized":2' 'summary reports materialized rows'
has "$SUMMARY" '"rows_output":2' 'summary reports output rows'
has "$SUMMARY" '"rows_truncated":0' 'summary explicitly reports no truncation'
has "$SUMMARY" '"buffered_bytes_peak":0' 'streaming search discloses zero buffered peak'
has "$SUMMARY" '"complete":true' 'complete scan is explicit'

SCRIPT="{\"phases\":[{\"id\":\"one\",\"scan\":[\"$TMP/input.txt\"],\"patterns\":[{\"id\":\"a\",\"regexp\":\"alpha\"}]},{\"id\":\"two\",\"scan\":[\"$TMP/input.txt\"],\"patterns\":[{\"id\":\"b\",\"regexp\":\"beta\"}]}]}"
OUT=$($BIN -s "$SCRIPT" -summary)
SUMMARY=${OUT##*$'\n'}
has "$SUMMARY" '"bytes_scanned":22' 'phase accounting includes repeated scans'
has "$SUMMARY" '"scan_stages":2' 'phase program reports two scan stages'
has "$SUMMARY" '"matcher_compilations":2' 'phase program reports per-stage compilations'
has "$SUMMARY" '"patterns_compiled":2' 'phase program reports all compiled patterns'

PLAN=$($BIN -F alpha -F beta -explain-plan -plan-only "$TMP/does-not-exist")
has "$PLAN" '"type":"execution-plan"' 'plan-only emits a machine-readable plan'
has "$PLAN" '"patterns":2' 'plan exposes the batched pattern count'
has "$PLAN" '"scan_stages"' 'plan exposes planned scan stages'

printf '1..%d\n' "$PASS"
