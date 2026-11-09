#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tamer Osman
#
# build-native.sh - native build of the OmniDecide WASM integration.
#
# Builds the vendored kernel plus the C interop wrapper into a static
# library, then the native parity runner and the native wrapper smoke
# test.  Outputs live under build/native/ (never inside vendor/).
#
# Usage:
#   ./build-native.sh                configure + build + run smoke tests
#   ./build-native.sh --build-only   build without running the smoke tests
#   ./build-native.sh --asan         additionally run the smoke test under
#                                    ASan + LSan + UBSan
#   ./build-native.sh --clean        remove build/native entirely first
#
# The script is independent of the caller's working directory: it always
# operates on the checkout that contains it.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

ROOT="$SCRIPT_DIR"
BUILD_DIR="$ROOT/build/native"
VENDOR="$ROOT/vendor/omnidecide-c"
REQUESTS="$ROOT/tests/requests.jsonl"
ASAN=0
BUILD_ONLY=0

for arg in "$@"; do
    case "$arg" in
        --asan) ASAN=1 ;;
        --build-only) BUILD_ONLY=1 ;;
        --clean) rm -rf "$BUILD_DIR"; echo "build-native: removed $BUILD_DIR"; exit 0 ;;
        *) echo "build-native: unknown option $arg" >&2; exit 2 ;;
    esac
done

fail() {
    echo "build-native: ERROR: $*" >&2
    exit 1
}

# ---------------------------------------------------------------------
# Prerequisites (explicit errors, no silent fallbacks)
# ---------------------------------------------------------------------
command -v cmake >/dev/null 2>&1 || fail "cmake not found on PATH. Install cmake (>= 3.16), e.g. 'pip install cmake'."
command -v cc >/dev/null 2>&1 || fail "no C compiler (cc) found on PATH."

[ -f "$VENDOR/include/omnidecide.h" ] || fail "vendored kernel source missing at $VENDOR. Run ./fetch-kernel.sh or restore vendor/ from the delivery archive (docs/BUILDING.md)."
[ -f "$REQUESTS" ] || fail "tests/requests.jsonl missing - checkout is incomplete."

C_COMPILER="${ODK_CC:-cc}"
CMAKE_VERSION="$(cmake --version 2>/dev/null | head -1 || true)"
CC_VERSION="$("$C_COMPILER" --version 2>/dev/null | head -1 || true)"
echo "build-native: $CMAKE_VERSION"
echo "build-native: $CC_VERSION"

mkdir -p "$BUILD_DIR"

# ---------------------------------------------------------------------
# Configure + build
# ---------------------------------------------------------------------
cmake -S "$ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$C_COMPILER" \
    -DCMAKE_C_FLAGS="-ffp-contract=off" \
    -DCMAKE_EXE_LINKER_FLAGS="" >/dev/null

cmake --build "$BUILD_DIR" --parallel "$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"

PARITY="$BUILD_DIR/odk_parity_runner"
SMOKE="$BUILD_DIR/odk_smoke"
[ -x "$PARITY" ] || fail "parity runner was not produced by the build"
[ -x "$SMOKE" ] || fail "smoke test binary was not produced by the build"

echo "build-native: built $PARITY"
echo "build-native: built $SMOKE"

if [ "$BUILD_ONLY" = "1" ]; then
    echo "build-native: --build-only requested, skipping smoke runs"
    exit 0
fi

# ---------------------------------------------------------------------
# Smoke tests
# ---------------------------------------------------------------------
echo "build-native: running wrapper smoke test..."
"$SMOKE"

echo "build-native: running parity runner over tests/requests.jsonl..."
"$PARITY" "$REQUESTS" "$BUILD_DIR/native_envelopes.tsv"
echo "build-native: envelopes written to $BUILD_DIR/native_envelopes.tsv"

# ---------------------------------------------------------------------
# ASan + LSan pass over the wrapper (leak-free error paths)
# ---------------------------------------------------------------------
if [ "$ASAN" = "1" ]; then
    echo "build-native: building ASan+LSan+UBSan variant..."
    ASAN_DIR="$BUILD_DIR/asan"
    mkdir -p "$ASAN_DIR"
    cc -std=c11 -O1 -g -ffp-contract=off -fsanitize=address,leak,undefined \
       -I"$VENDOR/include" -I"$VENDOR/odk-core" -I"$VENDOR/odk-brain" \
       -I"$VENDOR/odk-spine" -I"$ROOT/src/wrappers" \
       "$ROOT/tests/odk_smoke.c" "$ROOT/src/wrappers/odk_wasm.c" \
       "$VENDOR"/odk-core/*.c "$VENDOR"/odk-brain/*.c "$VENDOR"/odk-spine/*.c \
       -lm -o "$ASAN_DIR/odk_smoke_asan" \
       || fail "ASan build failed"
    echo "build-native: running wrapper smoke test under ASan+LSan..."
    ASAN_OPTIONS=detect_leaks=1 "$ASAN_DIR/odk_smoke_asan"
    echo "build-native: ASan+LSan pass complete (0 leaks)"
fi

echo "build-native: OK"
