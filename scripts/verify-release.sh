#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tamer Osman
#
# verify-release.sh - single entry point for reproducing the full
# release verification locally.
#
#   ./scripts/verify-release.sh              full verification
#   ./scripts/verify-release.sh --fast       reduced mode (skips browser
#                                              tests and benchmarks for
#                                              developers without browser
#                                              dependencies; NEVER present
#                                              the result as full release
#                                              verification)
#   ./scripts/verify-release.sh --help
#
# Stages (each logged to build/verify-logs/<stage>.log):
#   1.  prerequisites
#   2.  kernel source pin + manifest verification
#   3.  native build (kernel + integration wrapper)
#   4.  native wrapper smoke + ASan/LSan leak pass
#   5.  upstream kernel regression gates (test, golden-check,
#       spine-golden-check, hostile, stress, determinism, sanitize,
#       docs-check, version-check, v2-gates aggregate)
#   6.  WASM build from the same pinned source
#   7.  Node functional suite
#   8.  native/WASM parity (byte-identical envelopes, no silent skips)
#   9.  browser Playwright verification (fresh screenshots)
#   10. standard Node + browser benchmarks (fresh JSON)
#   11. verification summary
#
# Exit codes: 0 all mandatory checks passed, 1 any mandatory failure.
# There are no swallowed errors and no silent skips anywhere in this
# script: a stage that cannot run is a failure with a printed reason.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
cd "$ROOT"

LOGS="$ROOT/build/verify-logs"
mkdir -p "$LOGS"

FAST=0
case "${1:-}" in
    --fast) FAST=1 ;;
    --help|-h)
        sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
        exit 0 ;;
    "") ;;
    *) echo "verify-release: unknown option '$1' (try --help)" >&2; exit 2 ;;
esac

declare -a RESULTS
declare -a NAMES
FAILED_TOTAL=0

banner() {
    echo ""
    echo "==================================================================="
    echo "[$1] $2"
    echo "==================================================================="
}

run_stage() {
    local idx="$1" name="$2"; shift 2
    banner "$idx/11" "$name"
    local log="$LOGS/$(printf '%02d' "$idx")-$(echo "$name" | tr ' /:' '___' | tr -cd 'a-zA-Z0-9_-').log"
    if "$@" > "$log" 2>&1; then
        tail -n 3 "$log"
        echo "[$idx/11] PASS ($name) - full log: $log"
        RESULTS+=("PASS"); NAMES+=("$name")
    else
        local rc=$?
        tail -n 15 "$log"
        echo "[$idx/11] FAIL ($name) exit=$rc - log: $log"
        RESULTS+=("FAIL"); NAMES+=("$name")
        FAILED_TOTAL=$((FAILED_TOTAL + 1))
    fi
}

# ---------------------------------------------------------------------
banner "0/11" "verify-release starting (mode: $([ $FAST = 1 ] && echo REDUCED-FAST || echo FULL))"
echo "project root: $ROOT"
if [ $FAST = 1 ]; then
    echo "REDUCED MODE: browser tests and benchmarks are skipped."
    echo "A reduced run must NEVER be presented as full release verification."
fi

# 1. prerequisites
run_stage 1 "prerequisites" bash -c '
    set -e
    command -v cc >/dev/null    || { echo "cc not found"; exit 1; }
    command -v make >/dev/null  || { echo "make not found"; exit 1; }
    command -v cmake >/dev/null || { echo "cmake not found (pip install cmake)"; exit 1; }
    command -v node >/dev/null  || { echo "node not found"; exit 1; }
    command -v emcc >/dev/null  || { echo "emcc not found (install Emscripten SDK, see docs/BUILDING.md)"; exit 1; }
    [ -f tests/package-lock.json ] || { echo "tests/package-lock.json missing"; exit 1; }
    [ -d tests/node_modules/playwright ] || { echo "tests/node_modules/playwright missing (cd tests && npm ci)"; exit 1; }
    cc --version | head -1
    cmake --version | head -1
    node --version
    emcc --version | head -1
    echo "prerequisites: OK"'

# 2. source pin + manifest
run_stage 2 "kernel source pin and manifest verification" ./fetch-kernel.sh

# 3. native build
run_stage 3 "native build (kernel + integration wrapper)" ./build-native.sh --build-only

# 4. native smoke + ASan/LSan
run_stage 4 "native wrapper smoke + ASan/LSan leak pass" bash -c '
    set -e
    build/native/odk_smoke
    build/native/odk_parity_runner tests/requests.jsonl build/native/native_envelopes.tsv
    ./build-native.sh --asan'

# 5. upstream regression gates
run_stage 5 "upstream kernel regression gates (incl. v2-gates aggregate)" bash -c '
    set -e
    cd vendor/omnidecide-c
    make -j4 >/dev/null 2>&1
    make test
    make golden-check
    make spine-golden-check
    make hostile
    make stress
    make determinism
    make sanitize
    make docs-check
    make version-check
    make v2-gates'

# 6. WASM build + fresh-build byte-identity check
run_stage 6 "WASM build + fresh-build byte-identity check" bash -c '
    set -e
    ./build.sh
    scripts/ci-verify-dist.sh dist
    ./build.sh --output-dir build/fresh-wasm
    scripts/ci-verify-dist.sh build/fresh-wasm
    cmp dist/omnidecide.wasm build/fresh-wasm/omnidecide.wasm
    cmp dist/omnidecide.js build/fresh-wasm/omnidecide.js
    rm -rf build/fresh-wasm
    echo "fresh-build check: dist/ rebuild is byte-identical (wasm + js)"'

# 7. Node functional suite
run_stage 7 "Node functional suite" node tests/omnidecide.test.mjs

# 8. parity
run_stage 8 "native/WASM envelope parity" node tests/parity_check.mjs

# 9. browser (skipped in --fast)
if [ $FAST = 1 ]; then
    banner "9/11" "browser verification SKIPPED (--fast reduced mode)"
    RESULTS+=("SKIPPED"); NAMES+=("browser verification")
    FAILED_TOTAL=$((FAILED_TOTAL + 1))   # reduced mode is never "fully green"
else
    run_stage 9 "browser Playwright verification" node tests/browser_check.mjs
fi

# 10. benchmarks (skipped in --fast)
if [ $FAST = 1 ]; then
    banner "10/11" "benchmarks SKIPPED (--fast reduced mode)"
    RESULTS+=("SKIPPED"); NAMES+=("benchmarks")
    FAILED_TOTAL=$((FAILED_TOTAL + 1))
else
    run_stage 10 "standard benchmarks (Node + browser)" bash -c '
        set -e
        node tests/node_bench.mjs --scale standard
        node tests/browser_bench.mjs --scale standard'
fi

# 11. summary
banner "11/11" "verification summary"
for i in "${!NAMES[@]}"; do
    printf '  %-59s %s\n' "${NAMES[$i]}" "${RESULTS[$i]}"
done
echo ""

SUMMARY="build/verify-logs/summary.txt"
{
    echo "verify-release summary - $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "mode: $([ $FAST = 1 ] && echo REDUCED-FAST || echo FULL)"
    for i in "${!NAMES[@]}"; do
        printf '%-50s %s\n' "${NAMES[$i]}" "${RESULTS[$i]}"
    done
} > "$SUMMARY"
echo "summary written to $SUMMARY"

if [ $FAILED_TOTAL -gt 0 ]; then
    echo "verify-release: FAILED ($FAILED_TOTAL mandatory stage(s) failed/skipped)"
    exit 1
fi
echo "verify-release: ALL STAGES PASSED"
exit 0
