#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tamer Osman
#
# fetch-kernel.sh - kernel source acquisition for the OmniDecide WASM
# integration.
#
# Resolution order (documented in docs/BUILDING.md):
#   1. vendor/omnidecide-c/        (default; the pinned snapshot ships
#                                   inside this repository)
#   2. $OMNIDECIDE_C               (developer-provided local checkout;
#                                   strictly verified before use)
#
# This script NEVER downloads from the network and NEVER updates the
# kernel silently.  A checkout whose version or commit does not match
# scripts/kernel-pin.env produces a hard error, not a warning.
#
# Usage:
#   ./fetch-kernel.sh                 resolve + verify, print summary
#   ./fetch-kernel.sh --verify-only   verify without any changes
#   ./fetch-kernel.sh --print-path    print the resolved kernel path
#
# Exit codes: 0 resolved and verified, 1 verification failure,
#             2 no usable kernel source.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR"
PIN_ENV="$SCRIPT_DIR/scripts/kernel-pin.env"
VENDOR="$ROOT/vendor/omnidecide-c"

VERIFY_ONLY=0
PRINT_PATH=0
for arg in "$@"; do
    case "$arg" in
        --verify-only) VERIFY_ONLY=1 ;;
        --print-path) PRINT_PATH=1 ;;
        *) echo "fetch-kernel: unknown option $arg" >&2; exit 2 ;;
    esac
done

fail() { echo "fetch-kernel: ERROR: $*" >&2; exit 1; }
ok()   { echo "fetch-kernel: OK: $*"; }

[ -f "$PIN_ENV" ] || fail "scripts/kernel-pin.env missing - checkout incomplete"
# shellcheck disable=SC1090
. "$PIN_ENV"

verify_source() {
    local src="$1"
    if ! bash "$SCRIPT_DIR/scripts/verify-kernel-source.sh" "$src"; then
        fail "kernel source at $src failed verification"
    fi
}

resolved=""

# ---------------------------------------------------------------------
# 1. Vendored snapshot (preferred, default)
# ---------------------------------------------------------------------
if [ -f "$VENDOR/include/omnidecide.h" ]; then
    verify_source "$VENDOR"
    ok "vendored pinned kernel snapshot verified at $VENDOR"
    resolved="$VENDOR"
else
    echo "fetch-kernel: no vendored kernel at $VENDOR"
fi

# ---------------------------------------------------------------------
# 2. OMNIDECIDE_C external local checkout (strict verification)
# ---------------------------------------------------------------------
if [ -z "$resolved" ] && [ -n "${OMNIDECIDE_C:-}" ]; then
    EXT="$OMNIDECIDE_C"
    [ -d "$EXT" ] || fail "OMNIDECIDE_C=$EXT is not a directory"
    [ -f "$EXT/include/omnidecide.h" ] || fail "OMNIDECIDE_C=$EXT does not look like an OmniDecide C checkout (missing include/omnidecide.h)"

    # Version must match exactly.
    if grep -q "#define ODK_LIBRARY_VERSION \"$ODK_EXPECTED_VERSION\"" \
           "$EXT/include/omnidecide.h"; then
        ok "external checkout declares kernel version $ODK_EXPECTED_VERSION"
    else
        fail "external checkout is not kernel version $ODK_EXPECTED_VERSION (refusing to build from a foreign revision)"
    fi

    # Commit must match the recorded snapshot when discoverable.
    if [ -d "$EXT/.git" ] && command -v git >/dev/null 2>&1; then
        COMMIT="$(git -C "$EXT" rev-parse HEAD 2>/dev/null || echo unknown)"
        if [ "$COMMIT" = "$ODK_SNAPSHOT_COMMIT" ]; then
            ok "external checkout is at the pinned snapshot commit $COMMIT"
        else
            fail "external checkout commit $COMMIT does not match the pinned snapshot commit $ODK_SNAPSHOT_COMMIT.
A build must never silently combine files from different kernel revisions.
If you explicitly intend to build from a different revision, set
ODK_ALLOW_FOREIGN_COMMIT=1 and rerun; the mismatch is then recorded in the
build log but remains unsupported."
        fi
        if [ "${ODK_ALLOW_FOREIGN_COMMIT:-0}" = "1" ] && [ "$COMMIT" != "$ODK_SNAPSHOT_COMMIT" ]; then
            echo "fetch-kernel: WARNING: ODK_ALLOW_FOREIGN_COMMIT=1 - building from foreign commit $COMMIT (unsupported)"
        fi
    else
        echo "fetch-kernel: external checkout has no git metadata; version check only"
    fi

    verify_source "$EXT"
    ok "external local checkout verified at $EXT"
    resolved="$EXT"
fi

# ---------------------------------------------------------------------
# Nothing usable: explicit failure with recovery instructions
# ---------------------------------------------------------------------
if [ -z "$resolved" ]; then
    if [ $VERIFY_ONLY = 1 ]; then
        fail "no kernel source found (verify-only mode)"
    fi
    fail "no kernel source available.
Recovery options:
  1. Restore vendor/omnidecide-c/ from the delivery archive (preferred;
     the pinned snapshot ships inside the repository).
  2. Provide a local checkout of omnidecide-c at the pinned snapshot
     commit $ODK_SNAPSHOT_COMMIT (version $ODK_EXPECTED_VERSION) and set:
       export OMNIDECIDE_C=/path/to/omnidecide-c
     The checkout is verified against the pin before any build.
This project deliberately does NOT download the kernel at build time and
does NOT fall back to the upstream default branch."
fi

if [ $PRINT_PATH = 1 ]; then
    echo "$resolved"
    exit 0
fi

echo "fetch-kernel: resolved kernel source: $resolved"
echo "fetch-kernel: pinned snapshot commit: $ODK_SNAPSHOT_COMMIT"
echo "fetch-kernel: source manifest fingerprint: $ODK_VENDOR_FINGERPRINT"
echo "fetch-kernel: OK"
