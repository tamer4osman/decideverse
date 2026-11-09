#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tamer Osman
#
# Determinism gate (I1): every determinism-sensitive runner executes at
# least three times; the SHA-256 of the output must match across runs.
# Also runs the v1 golden corpus (byte-identical by construction).

set -e
cd "$(dirname "$0")/.." || exit 2

RUNS=${RUNS:-3}
fail=0

check() { # check <label> <command...>
    label=$1
    shift
    hash_prev=""
    i=1
    while [ "$i" -le "$RUNS" ]; do
        hash_now=$("$@" 2>/dev/null | sha256sum | cut -d' ' -f1)
        if [ -z "$hash_now" ]; then
            echo "determinism: $label run $i produced no output"
            fail=1
            return
        fi
        if [ -n "$hash_prev" ] && [ "$hash_now" != "$hash_prev" ]; then
            echo "determinism: $label run $i hash differs from run $((i-1))"
            fail=1
            return
        fi
        hash_prev=$hash_now
        i=$((i + 1))
    done
    echo "determinism: $label identical across $RUNS runs ($hash_prev)"
}

check "spine-golden" ./build/golden_spine check
check "synthetic-example" ./build/synthetic_example
check "smoke-test" ./build/smoke_test

[ "$fail" = 0 ] && echo "determinism: OK"
exit $fail
