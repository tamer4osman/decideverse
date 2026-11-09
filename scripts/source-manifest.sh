#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tamer Osman
#
# source-manifest.sh - deterministic source manifest fingerprint.
#
# Algorithm (pinned before the first public release; every future fingerprint of this
# project is computed with exactly these steps):
#   1. Walk all files under the given root, sorted by relative path
#      (byte order).
#   2. For each file, emit "<sha256(file)>  <relative-path>\n"
#      (sha256sum output format).
#   3. Concatenate those lines in sorted order and SHA-256 the result.
#
# Usage:
#   scripts/source-manifest.sh <root>            print the fingerprint
#   scripts/source-manifest.sh <root> --manifest print the manifest text
#
# The fingerprint changes when ANY byte of ANY file changes, so it pins
# the vendored kernel snapshot verifiably.

set -euo pipefail

ROOT="${1:-}"
[ -n "$ROOT" ] || { echo "usage: source-manifest.sh <root> [--manifest]" >&2; exit 2; }
ROOT="$(cd "$ROOT" && pwd)"

MANIFEST="$(mktemp)"
trap 'rm -f "$MANIFEST"' EXIT

( cd "$ROOT" &&
    find . -type f ! -path './build/*' | sed 's|^\./||' | LC_ALL=C sort |
    while IFS= read -r rel; do
        sha256sum "$rel" || exit 1
    done
) > "$MANIFEST"

if [ "${2:-}" = "--manifest" ]; then
    cat "$MANIFEST"
else
    sha256sum "$MANIFEST" | cut -d' ' -f1
fi
