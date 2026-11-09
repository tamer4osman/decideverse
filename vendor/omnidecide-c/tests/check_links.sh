#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tamer Osman
#
# Checks that every relative markdown link in the repository resolves to an
# existing file or directory.  External (http, https, mailto) links and
# in-page anchors are not checked.  Exit status 1 if any link is broken.
cd "$(dirname "$0")/.." || exit 2
bad=$(mktemp) || exit 2
count=$(mktemp) || exit 2
find . \( -name node_modules -o -name build -o -name out -o -name .git \) -prune \
       -o -name '*.md' -print | sort | while read -r file; do
    dir=$(dirname "$file")
    grep -o '\]([^)]*)' "$file" | sed 's/^](//; s/)$//' | while read -r link; do
        case "$link" in
            http://*|https://*|mailto:*|'#'*|'') continue ;;
        esac
        target=${link%%#*}
        [ -n "$target" ] || continue
        echo x >> "$count"
        [ -e "$dir/$target" ] || echo "BROKEN LINK in $file: $link" >> "$bad"
    done
done
n=$(wc -l < "$count" | tr -d ' ')
if [ -s "$bad" ]; then
    cat "$bad"
    rm -f "$bad" "$count"
    exit 1
fi
rm -f "$bad" "$count"
echo "docs-check: OK ($n relative links resolve)"
