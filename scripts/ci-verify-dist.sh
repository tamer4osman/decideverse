#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tamer Osman
#
# ci-verify-dist.sh - verify a WASM distribution directory.
#
# Used by CI (Jobs C, D, E, F) immediately after a build or an artifact
# download so that a missing, empty or corrupted file fails the job with
# a clear message instead of surfacing later as a confusing test error.
#
# Checks:
#   1. <dir>/omnidecide.js  exists and is non-empty
#   2. <dir>/omnidecide.wasm exists and is non-empty
#   3. <dir>/omnidecide.wasm starts with the WASM magic bytes "\0asm"
#
# Usage:
#   scripts/ci-verify-dist.sh <dist-dir>
#
# Exit codes: 0 verified, 1 verification failure.

set -euo pipefail

DIR="${1:-}"

fail() {
    echo "ci-verify-dist: ERROR: $*" >&2
    exit 1
}

[ -n "$DIR" ] || fail "usage: ci-verify-dist.sh <dist-dir>"
[ -d "$DIR" ] || fail "distribution directory not found: $DIR"

for f in omnidecide.js omnidecide.wasm; do
    [ -f "$DIR/$f" ] || fail "$DIR/$f is missing"
    [ -s "$DIR/$f" ] || fail "$DIR/$f is empty"
done

# WASM magic bytes: "\0asm" == 00 61 73 6d
MAGIC="$(head -c 4 "$DIR/omnidecide.wasm" | od -An -tx1 | tr -d ' \n')"
if [ "$MAGIC" != "0061736d" ]; then
    fail "$DIR/omnidecide.wasm has wrong magic bytes: got $MAGIC, expected 0061736d ('\\0asm')"
fi

echo "ci-verify-dist: OK: $DIR contains omnidecide.js + omnidecide.wasm (magic 0061736d)"
sha256sum "$DIR/omnidecide.wasm" "$DIR/omnidecide.js"
