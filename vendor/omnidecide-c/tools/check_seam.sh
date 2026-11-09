#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tamer Osman
#
# Customization-seam gate (I6): no application-domain vocabulary may
# appear in the kernel, brain or spine. The PROHIBITED WORDS LIST is
# configurable data that the consuming application supplies
# (tests/seam/prohibited_words.txt); this script ships the mechanism,
# never the list's content. The synthetic example fixtures under
# examples/ are explicitly non-normative test content and are not
# scanned.
#
#   sh tools/check_seam.sh [wordlist]

set -e
cd "$(dirname "$0")/.." || exit 2

WORDS=${1:-tests/seam/prohibited_words.txt}
[ -f "$WORDS" ] || { echo "seam-check: word list '$WORDS' missing"; exit 2; }

fail=0
count=0
while IFS= read -r word; do
    case "$word" in ''|'#'*) continue ;; esac
    count=$((count + 1))
    hits=$(grep -R -n -i -F -- "$word" \
        odk-core odk-brain odk-spine include schema tools/gen_types.py \
        2>/dev/null | grep -v -F "ODK_SEAM_ALLOW" || true)
    if [ -n "$hits" ]; then
        echo "seam-check: PROHIBITED WORD '$word' found in core/spine:"
        echo "$hits" | head -5
        fail=1
    fi
done < "$WORDS"

echo "seam-check: scanned core, brain, spine, headers and schema against $count prohibited words"
[ "$fail" = 0 ] && echo "seam-check: OK"
exit $fail
