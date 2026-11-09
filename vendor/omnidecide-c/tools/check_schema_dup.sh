#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tamer Osman
#
# Schema gate (F5): no hand-maintained duplicates of the generated
# record types. The generated files are odk-spine/odk_spine_types.[ch];
# any other definition of a generated type name outside them is a bug.

set -e
cd "$(dirname "$0")/.." || exit 2

TYPES_H=odk-spine/odk_spine_types.h
[ -f "$TYPES_H" ] || { echo "schema-check: $TYPES_H missing (run: python3 tools/gen_types.py)"; exit 2; }

fail=0

# 1. no struct/typedef DEFINITION of a generated record outside the
#    generated header (variable declarations of generated types are
#    exactly the point of the schema-first flow and are fine)
defs=$(grep -h "^} odk_spine_[a-z_]*;" "$TYPES_H" | sed 's/^} odk_spine_//;s/;$//' | sort)
dup=""
for t in $defs; do
    hits=$(grep -R -n "^} odk_spine_$t;" \
           --include="*.h" --include="*.c" include odk-core odk-brain odk-spine \
           2>/dev/null | grep -v odk_spine_types || true)
    [ -n "$hits" ] && dup="$dup$hits\n"
done
if [ -n "$dup" ]; then
    echo "schema-check: hand-written spine record definitions outside generated files:"
    printf '%b' "$dup" | head -5
    fail=1
fi

# 2. the generated header declares every type exactly once
total=$(printf '%s\n' "$defs" | grep -c . || true)
unique=$(printf '%s\n' "$defs" | sort -u | grep -c . || true)
if [ "$total" != "$unique" ]; then
    echo "schema-check: duplicate type definitions inside generated header"
    fail=1
fi

# 3. the fingerprint macro in the build matches the schema file bytes
fp_file=$(python3 tools/gen_types.py --fingerprint 2>/dev/null || true)
if [ -z "$fp_file" ]; then
    echo "schema-check: could not compute schema fingerprint"
    fail=1
elif ! grep -q "\"$fp_file\"" "$TYPES_H"; then
    echo "schema-check: generated header fingerprint does not match schema (regenerate!)"
    fail=1
fi

echo "schema-check: $unique record types verified against the generated header"
[ "$fail" = 0 ] && echo "schema-check: OK"
exit $fail
