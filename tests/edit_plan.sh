#!/usr/bin/env bash
set -u

BIN=${HPRSCRIPT_BIN:?HPRSCRIPT_BIN must name the built binary}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/hpr-edit-plan.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

PASS=0
FAIL=0

pass() { printf '  \033[32mPASS\033[0m  %s\n' "$1"; PASS=$((PASS + 1)); }
fail() { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; FAIL=$((FAIL + 1)); }

expect_eq() {
    local want=$1 got=$2 name=$3
    if [[ $got == "$want" ]]; then pass "$name"; else
        fail "$name (expected '$want', got '$got')"
    fi
}

expect_has() {
    local text=$1 needle=$2 name=$3
    if [[ $text == *"$needle"* ]]; then pass "$name"; else
        fail "$name (missing '$needle')"
    fi
}

run_rc() {
    set +e
    RUN_OUT=$("$@" 2>&1)
    RUN_RC=$?
    set -e
}

set -e
printf '\n\033[1mimmutable edit plans and apply\033[0m\n'

mkdir "$TMP/basic"
printf 'retry(3)\n' > "$TMP/basic/worker.go"
(
    cd "$TMP/basic"
    "$BIN" edit -F 'retry(3)' -content 'retry(5)' \
        -plan-out retry.plan.json worker.go >/dev/null
)
expect_eq 'retry(3)' "$(tr -d '\n' < "$TMP/basic/worker.go")" \
    'plan creation writes no target files'
expect_has "$(<"$TMP/basic/retry.plan.json")" '"schema": "hprscript-edit-plan"' \
    'plan schema is versioned'
(
    cd "$TMP/basic"
    "$BIN" apply retry.plan.json -j > apply.out
)
expect_eq 'retry(5)' "$(tr -d '\n' < "$TMP/basic/worker.go")" \
    'apply changes exact stored site'
expect_has "$(<"$TMP/basic/apply.out")" '"status":"complete"' \
    'apply emits complete receipt'

mkdir "$TMP/human"
printf 'old\n' > "$TMP/human/a.txt"
(
    cd "$TMP/human"
    "$BIN" edit -F old -content new -plan-out p.json a.txt >/dev/null
    "$BIN" apply p.json -receipt human > human.out
)
expect_has "$(<"$TMP/human/human.out")" 'apply complete:' \
    'human receipt mode is explicit'

mkdir "$TMP/root-one" "$TMP/root-two"
printf 'old\n' > "$TMP/root-one/a.txt"
printf 'old\n' > "$TMP/root-two/a.txt"
(
    cd "$TMP/root-one"
    "$BIN" edit -F old -content new -plan-out p.json a.txt >/dev/null
)
cp "$TMP/root-one/p.json" "$TMP/root-two/p.json"
run_rc bash -c 'cd "$1" && "$2" apply p.json -j' _ "$TMP/root-two" "$BIN"
expect_eq 3 "$RUN_RC" 'plan root mismatch is refused by default'
expect_eq old "$(tr -d '\n' < "$TMP/root-two/a.txt")" \
    'root mismatch refusal leaves relocated target untouched'
(
    cd "$TMP/root-two"
    "$BIN" apply p.json -allow-root-mismatch -j >/dev/null
)
expect_eq new "$(tr -d '\n' < "$TMP/root-two/a.txt")" \
    'reviewed root relocation applies only beneath the new root'

mkdir "$TMP/root-guard" "$TMP/outside"
printf 'old\n' > "$TMP/outside/a.txt"
run_rc bash -c 'cd "$1" && "$2" edit -F old -content new -plan-out p.json ../outside/a.txt' \
    _ "$TMP/root-guard" "$BIN"
expect_eq 3 "$RUN_RC" 'persistent plan refuses targets outside its root'
expect_eq old "$(tr -d '\n' < "$TMP/outside/a.txt")" \
    'outside-root planning refusal never writes the target'

mkdir "$TMP/drift"
printf 'alpha alpha\n' > "$TMP/drift/a.txt"
(
    cd "$TMP/drift"
    "$BIN" edit -F alpha -content beta -plan-out plan.json a.txt >/dev/null
)
printf 'alpha  alpha\n' > "$TMP/drift/a.txt"
run_rc bash -c 'cd "$1" && "$2" apply plan.json -j' _ "$TMP/drift" "$BIN"
expect_eq 3 "$RUN_RC" 'file modified after planning exits 3'
expect_eq 'alpha  alpha' "$(tr -d '\n' < "$TMP/drift/a.txt")" \
    'verification refusal writes nothing'
expect_has "$RUN_OUT" 'whole-file SHA-256 or size changed' \
    'same-match-count drift is hash-detected'

mkdir "$TMP/same-size"
printf 'old\n' > "$TMP/same-size/a.txt"
(
    cd "$TMP/same-size"
    "$BIN" edit -F old -content new -plan-out p.json a.txt >/dev/null
)
printf 'odd\n' > "$TMP/same-size/a.txt"
run_rc bash -c 'cd "$1" && "$2" apply p.json -j' _ "$TMP/same-size" "$BIN"
expect_eq 3 "$RUN_RC" 'same-size old-span mutation exits 3'
expect_eq odd "$(tr -d '\n' < "$TMP/same-size/a.txt")" \
    'same-size verification refusal writes nothing'

mkdir "$TMP/all-preflight"
printf 'old\n' > "$TMP/all-preflight/a.txt"
printf 'old\n' > "$TMP/all-preflight/b.txt"
(
    cd "$TMP/all-preflight"
    "$BIN" edit -F old -content new -plan-out p.json a.txt b.txt >/dev/null
)
printf 'odd\n' > "$TMP/all-preflight/b.txt"
run_rc bash -c 'cd "$1" && "$2" apply p.json -j' _ "$TMP/all-preflight" "$BIN"
expect_eq 3 "$RUN_RC" 'one stale file refuses the entire multi-file plan'
expect_eq old "$(tr -d '\n' < "$TMP/all-preflight/a.txt")" \
    'all-file verification precedes every staged write'

mkdir "$TMP/overlap"
printf 'alpha beta\n' > "$TMP/overlap/a.txt"
run_rc bash -c 'cd "$1" && "$2" edit -F alpha -F "alpha beta" -content changed -plan-out p.json a.txt' \
    _ "$TMP/overlap" "$BIN"
expect_eq 3 "$RUN_RC" 'overlapping edit sites are rejected during planning'
expect_eq 'alpha beta' "$(tr -d '\n' < "$TMP/overlap/a.txt")" \
    'overlap refusal leaves the target untouched'

mkdir "$TMP/noop"
printf 'same\n' > "$TMP/noop/a.txt"
(
    cd "$TMP/noop"
    "$BIN" edit -F same -content same -plan-out p.json a.txt >/dev/null
    "$BIN" apply p.json -j > receipt.out
)
expect_has "$(<"$TMP/noop/p.json")" '"site_count": 1' \
    'no-op sites remain represented in the immutable plan'
expect_has "$(<"$TMP/noop/receipt.out")" '"status":"noop"' \
    'no-op apply status is represented consistently'

mkdir "$TMP/order"
printf 'old\n' > "$TMP/order/z.txt"
printf 'old\n' > "$TMP/order/a.txt"
(
    cd "$TMP/order"
    "$BIN" edit -F old -content new -plan-out p.json z.txt a.txt >/dev/null
)
ORDER_PLAN=$(<"$TMP/order/p.json")
ORDER_PREFIX=${ORDER_PLAN%%'"path": "z.txt"'*}
expect_has "$ORDER_PREFIX" '"path": "a.txt"' \
    'plan files are ordered deterministically by normalized path'
(
    cd "$TMP/order"
    "$BIN" apply p.json -j > receipt.out
)
ORDER_RECEIPT=$(<"$TMP/order/receipt.out")
ORDER_FIRST=${ORDER_RECEIPT%%$'\n'*}
expect_has "$ORDER_FIRST" '"file":"a.txt"' \
    'per-file receipts preserve deterministic plan order'

mkdir "$TMP/symlink"
printf 'old\n' > "$TMP/symlink/target.txt"
ln -s target.txt "$TMP/symlink/link.txt"
run_rc bash -c 'cd "$1" && "$2" edit -F old -content new -plan-out p.json link.txt' \
    _ "$TMP/symlink" "$BIN"
expect_eq 3 "$RUN_RC" 'symlink planning is refused by default'
expect_has "$RUN_OUT" '"guard":"symlink"' 'symlink refusal is explicit'
(
    cd "$TMP/symlink"
    "$BIN" edit -F old -content new -follow-symlinks \
        -plan-out p.json link.txt >/dev/null
)
run_rc bash -c 'cd "$1" && "$2" apply p.json -j' _ "$TMP/symlink" "$BIN"
expect_eq 3 "$RUN_RC" 'apply requires -follow-symlinks again'
(
    cd "$TMP/symlink"
    "$BIN" apply p.json -follow-symlinks -j >/dev/null
)
expect_eq new "$(tr -d '\n' < "$TMP/symlink/target.txt")" \
    'explicit symlink plan verifies and applies target'

mkdir "$TMP/bytes"
printf 'old\r\nlast' > "$TMP/bytes/crlf.txt"
chmod 0640 "$TMP/bytes/crlf.txt"
(
    cd "$TMP/bytes"
    "$BIN" edit -F old -content new -plan-out p.json crlf.txt >/dev/null
    "$BIN" apply p.json -j >/dev/null
)
expect_eq $'new\r\nlast' "$(<"$TMP/bytes/crlf.txt")" \
    'CRLF and missing final newline are preserved'
expect_eq 640 "$(stat -c '%a' "$TMP/bytes/crlf.txt")" \
    'file permissions are preserved'

mkdir "$TMP/corrupt"
printf 'old\n' > "$TMP/corrupt/a.txt"
(
    cd "$TMP/corrupt"
    "$BIN" edit -F old -content new -plan-out valid.json a.txt >/dev/null
)
valid=$(<"$TMP/corrupt/valid.json")
printf '%s' "${valid/\"version\": 1/\"version\": 99}" > "$TMP/corrupt/version.json"
run_rc bash -c 'cd "$1" && "$2" apply version.json -j' _ "$TMP/corrupt" "$BIN"
expect_eq 2 "$RUN_RC" 'unknown plan version exits 2'
printf '%s' "${valid/old_base64\":\"b2xk/old_base64\":\"!!!!}" > \
    "$TMP/corrupt/base64.json"
run_rc bash -c 'cd "$1" && "$2" apply base64.json -j' _ "$TMP/corrupt" "$BIN"
expect_eq 2 "$RUN_RC" 'corrupted base64 exits 2'
expect_eq old "$(tr -d '\n' < "$TMP/corrupt/a.txt")" \
    'corrupted plans do not write targets'

mkdir "$TMP/stage"
printf 'old\n' > "$TMP/stage/a.txt"
printf 'old\n' > "$TMP/stage/b.txt"
(
    cd "$TMP/stage"
    "$BIN" edit -F old -content new -plan-out p.json a.txt b.txt >/dev/null
)
run_rc bash -c 'cd "$1" && HPRSCRIPT_ENABLE_FAULT_INJECTION=1 HPRSCRIPT_TEST_FAIL_STAGE_N=2 "$2" apply p.json -j' \
    _ "$TMP/stage" "$BIN"
expect_eq 2 "$RUN_RC" 'staging failure exits 2'
expect_has "$RUN_OUT" '"files_not_applied":2' \
    'staging failure receipt accounts for every unchanged target'
expect_eq old "$(tr -d '\n' < "$TMP/stage/a.txt")" \
    'staging failure leaves first target untouched'
expect_eq old "$(tr -d '\n' < "$TMP/stage/b.txt")" \
    'staging failure leaves second target untouched'

stage_fault() {
    local suffix=$1 variable=$2 description=$3
    mkdir "$TMP/fault-$suffix"
    printf 'old\n' > "$TMP/fault-$suffix/a.txt"
    (
        cd "$TMP/fault-$suffix"
        "$BIN" edit -F old -content new -plan-out p.json a.txt >/dev/null
    )
    run_rc bash -c 'cd "$1" && env HPRSCRIPT_ENABLE_FAULT_INJECTION=1 "$3=1" "$2" apply p.json -j' \
        _ "$TMP/fault-$suffix" "$BIN" "$variable"
    expect_eq 2 "$RUN_RC" "$description exits 2 before commit"
    expect_eq old "$(tr -d '\n' < "$TMP/fault-$suffix/a.txt")" \
        "$description leaves target untouched"
}

stage_fault open HPRSCRIPT_TEST_FAIL_OPEN_N 'temporary open fault'
stage_fault write HPRSCRIPT_TEST_FAIL_WRITE_N 'temporary write fault'
stage_fault chmod HPRSCRIPT_TEST_FAIL_CHMOD_N 'temporary chmod fault'
stage_fault fsync HPRSCRIPT_TEST_FAIL_FSYNC_N 'temporary fsync fault'

mkdir "$TMP/partial"
printf 'old\n' > "$TMP/partial/a.txt"
printf 'old\n' > "$TMP/partial/b.txt"
(
    cd "$TMP/partial"
    "$BIN" edit -F old -content new -plan-out p.json a.txt b.txt >/dev/null
)
run_rc bash -c 'cd "$1" && HPRSCRIPT_ENABLE_FAULT_INJECTION=1 HPRSCRIPT_TEST_FAIL_RENAME_N=2 "$2" apply p.json -j' \
    _ "$TMP/partial" "$BIN"
expect_eq 4 "$RUN_RC" 'rename failure after first commit exits 4'
expect_eq new "$(tr -d '\n' < "$TMP/partial/a.txt")" \
    'partial receipt corresponds to committed file'
expect_eq old "$(tr -d '\n' < "$TMP/partial/b.txt")" \
    'partial commit leaves later target unchanged'
expect_has "$RUN_OUT" '"status":"partial"' 'partial receipt is explicit'
expect_has "$RUN_OUT" '"temporary_files":[' 'partial receipt lists recovery files'

mkdir "$TMP/dirsync"
printf 'old\n' > "$TMP/dirsync/a.txt"
(
    cd "$TMP/dirsync"
    "$BIN" edit -F old -content new -plan-out p.json a.txt >/dev/null
)
run_rc bash -c 'cd "$1" && HPRSCRIPT_ENABLE_FAULT_INJECTION=1 HPRSCRIPT_TEST_FAIL_DIR_FSYNC_N=1 "$2" apply p.json -j' \
    _ "$TMP/dirsync" "$BIN"
expect_eq 4 "$RUN_RC" 'directory fsync uncertainty exits 4'
expect_eq new "$(tr -d '\n' < "$TMP/dirsync/a.txt")" \
    'directory fsync fault occurs after target rename'
expect_has "$RUN_OUT" '"status":"durability-uncertain"' \
    'directory fsync receipt reports durability uncertainty'

printf '\n\033[1medit-plan summary\033[0m\n'
if (( FAIL > 0 )); then
    printf '\n\033[31m%d/%d FAILED\033[0m\n' "$FAIL" "$((PASS + FAIL))"
    exit 1
fi
printf '\n\033[32mAll %d edit-plan tests passed.\033[0m\n' "$PASS"
