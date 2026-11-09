#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tamer Osman
#
# verify-kernel-source.sh - verify the kernel source in place.
#
# Checks, with explicit pass/fail output (no silent skips):
#   1. required upstream files are present
#   2. the kernel version is exactly 2.0.0 (header macro + version-check)
#   3. LICENSE, NOTICE and THIRD_PARTY_NOTICES.md are present
#   4. the vendored source has NOT been modified (manifest fingerprint
#      equals the recorded value in scripts/kernel-pin.env)
#
# Usage:
#   scripts/verify-kernel-source.sh [kernel-root]
#     default kernel-root: vendor/omnidecide-c inside this checkout
#
# Exit codes: 0 verified, 1 verification failure, 2 usage error.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"
PIN_ENV="$SCRIPT_DIR/kernel-pin.env"
KERNEL="${1:-$ROOT/vendor/omnidecide-c}"

fail() { echo "kernel-verify: FAIL: $*" >&2; exit 1; }
ok()   { echo "kernel-verify: OK: $*"; }

[ -f "$PIN_ENV" ] || fail "scripts/kernel-pin.env missing - checkout incomplete"
# shellcheck disable=SC1090
. "$PIN_ENV"

# 1. required files ----------------------------------------------------
for f in Makefile CMakeLists.txt include/omnidecide.h include/odk_spine.h \
         odk-core odk-brain odk-spine examples tests golden docs \
         LICENSE NOTICE THIRD_PARTY_NOTICES.md README.md; do
    [ -e "$KERNEL/$f" ] || fail "required upstream file missing: $KERNEL/$f"
done
ok "all required upstream files present"

# 2. kernel version ----------------------------------------------------
grep -q '#define ODK_LIBRARY_VERSION "2.0.0"' "$KERNEL/include/omnidecide.h" \
    || fail "include/omnidecide.h does not declare version 2.0.0"
ok "header declares ODK_LIBRARY_VERSION 2.0.0"

if command -v make >/dev/null 2>&1 && command -v sh >/dev/null 2>&1; then
    if ( cd "$KERNEL" && sh tests/check_version.sh >/dev/null 2>&1 ); then
        ok "upstream version-check passed (header, CMake and CHANGELOG agree)"
    else
        fail "upstream tests/check_version.sh failed"
    fi
else
    fail "make/sh unavailable - cannot run the upstream version check"
fi

# 3. license files -----------------------------------------------------
for f in LICENSE NOTICE THIRD_PARTY_NOTICES.md; do
    [ -s "$KERNEL/$f" ] || fail "$f is missing or empty"
done
ok "LICENSE, NOTICE and THIRD_PARTY_NOTICES.md present and non-empty"

# 4. manifest fingerprint ----------------------------------------------
command -v sha256sum >/dev/null 2>&1 || fail "sha256sum not available"
ACTUAL_FP="$(bash "$SCRIPT_DIR/source-manifest.sh" "$KERNEL")"
if [ "$ACTUAL_FP" = "$ODK_VENDOR_FINGERPRINT" ]; then
    ok "source manifest fingerprint matches the recorded pin ($ACTUAL_FP)"
else
    fail "source manifest fingerprint mismatch:
  expected: $ODK_VENDOR_FINGERPRINT
  actual:   $ACTUAL_FP
The vendored kernel source was modified. Restore vendor/omnidecide-c/ from
the delivery archive; WASM-specific changes belong in the integration layer
(src/wrappers, build scripts), never in the vendored tree."
fi

echo "kernel-verify: ALL CHECKS PASSED"
