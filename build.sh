#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tamer Osman
#
# build.sh - WebAssembly build of the OmniDecide integration.
#
# Compiles the vendored, pinned kernel source plus the C interop
# wrapper with Emscripten into dist/omnidecide.js + dist/omnidecide.wasm.
#
# The build is always performed from source: a compilation failure never
# leaves a stale binary in place (the output directory is written only
# after a successful link).
#
# Usage:
#   ./build.sh                        build into dist/ (default)
#   ./build.sh --output-dir <dir>     build into <dir> instead (relative
#                                     paths resolve against the project
#                                     root, not the caller's cwd); dist/
#                                     is left untouched, which lets CI
#                                     fresh-build into an isolated
#                                     directory for byte-level comparison
#                                     against downloaded artifacts
#
# Environment:
#   EMCC      override the emcc binary (default: emcc on PATH)
#   ODK_OPT   override optimization level (default: -O2)
#
# The script is independent of the caller's working directory.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
ROOT="$SCRIPT_DIR"
VENDOR="$ROOT/vendor/omnidecide-c"
OUT_DIR="$ROOT/build/wasm"
DIST="$ROOT/dist"

EMCC="${EMCC:-emcc}"
ODK_OPT="${ODK_OPT:--O2}"

fail() {
    echo "build.sh: ERROR: $*" >&2
    exit 1
}

# Output target: dist/ by default; an isolated directory when
# --output-dir is given (used by CI to compare a fresh build against
# the downloaded artifact without ever touching the downloaded copy).
TARGET="$DIST"
while [ $# -gt 0 ]; do
    case "$1" in
        --output-dir)
            [ $# -ge 2 ] || fail "--output-dir requires a directory argument"
            TARGET="$ROOT/$2"; shift 2 ;;
        *) fail "unknown option '$1' (supported: --output-dir <dir>)" ;;
    esac
done

command -v "$EMCC" >/dev/null 2>&1 || fail "Emscripten compiler ('$EMCC') not found on PATH. Install the Emscripten SDK (see docs/BUILDING.md), e.g.: git clone https://github.com/emscripten-core/emsdk && ./emsdk install latest && ./emsdk activate latest"
[ -f "$VENDOR/include/omnidecide.h" ] || fail "vendored kernel source missing at $VENDOR. Run ./fetch-kernel.sh or restore vendor/ from the delivery archive (docs/BUILDING.md)."

EMCC_VERSION="$("$EMCC" --version 2>/dev/null | head -1 || true)"
echo "build.sh: $EMCC_VERSION"

# Compile into an intermediate directory first: dist/ is only touched
# after the link succeeds, so a failed build can never leave a stale or
# half-written artifact behind.
rm -rf "$OUT_DIR"
mkdir -p "$OUT_DIR"
mkdir -p "$TARGET"

EXPORTED_FUNCTIONS='["_odkw_kernel_create","_odkw_kernel_free","_odkw_kernel_count","_odkw_evaluate","_odkw_version","_odkw_backend","_odkw_last_status","_odkw_last_error","_odkw_clear_error","_odkw_last_error_envelope","_odkw_free","_malloc","_free"]'

# shellcheck disable=SC2086
"$EMCC" $ODK_OPT -std=c11 -ffp-contract=off \
    -I"$VENDOR/include" -I"$VENDOR/odk-core" -I"$VENDOR/odk-brain" -I"$VENDOR/odk-spine" \
    -I"$ROOT/src/wrappers" \
    "$VENDOR"/odk-core/*.c "$VENDOR"/odk-brain/*.c "$VENDOR"/odk-spine/*.c \
    "$ROOT/src/wrappers/odk_wasm.c" \
    -s WASM=1 \
    -s "EXPORTED_FUNCTIONS=$EXPORTED_FUNCTIONS" \
    -s "EXPORTED_RUNTIME_METHODS=[\"UTF8ToString\",\"stringToUTF8\",\"lengthBytesUTF8\",\"HEAPU32\"]" \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s ENVIRONMENT=web,node \
    -s MODULARIZE=1 \
    -s EXPORT_NAME=createOmniDecideModule \
    --no-entry \
    -o "$OUT_DIR/omnidecide.js" \
    || fail "emcc compilation failed"

[ -f "$OUT_DIR/omnidecide.wasm" ] || fail "compiler did not produce $OUT_DIR/omnidecide.wasm"
[ -s "$OUT_DIR/omnidecide.wasm" ] || fail "$OUT_DIR/omnidecide.wasm is empty"

mv "$OUT_DIR/omnidecide.js" "$TARGET/omnidecide.js"
mv "$OUT_DIR/omnidecide.wasm" "$TARGET/omnidecide.wasm"
rm -rf "$OUT_DIR"

WASM_BYTES="$(wc -c < "$TARGET/omnidecide.wasm" | tr -d ' ')"
echo "build.sh: wrote ${TARGET#"$ROOT"/}/omnidecide.js + ${TARGET#"$ROOT"/}/omnidecide.wasm ($WASM_BYTES bytes)"
echo "build.sh: artifact checksums:"
sha256sum "$TARGET/omnidecide.wasm" "$TARGET/omnidecide.js" | sed 's/^/build.sh:   /'
echo "build.sh: OK"
