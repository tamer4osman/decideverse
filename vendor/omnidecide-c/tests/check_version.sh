#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tamer Osman
#
# Checks that the version is declared consistently: the header macros, the
# CMake project version and the newest numbered CHANGELOG entry must agree, and
# the evidence stamp of the 1.x series must still be "1.0.0".
cd "$(dirname "$0")/.." || exit 2
major=$(sed -n 's/^#define ODK_VERSION_MAJOR \([0-9][0-9]*\).*/\1/p' include/omnidecide.h)
minor=$(sed -n 's/^#define ODK_VERSION_MINOR \([0-9][0-9]*\).*/\1/p' include/omnidecide.h)
patch=$(sed -n 's/^#define ODK_VERSION_PATCH \([0-9][0-9]*\).*/\1/p' include/omnidecide.h)
str=$(sed -n 's/^#define ODK_LIBRARY_VERSION "\(.*\)".*/\1/p' include/omnidecide.h)
cmake=$(sed -n 's/^project(omnidecide VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt)
log=$(sed -n 's/^## \([0-9][0-9.]*\)\([^0-9.].*\)\{0,1\}$/\1/p' CHANGELOG.md | head -1)
fail=0
check() { # check <label> <got> <want>
    if [ "$2" != "$3" ]; then echo "version-check: $1 is '$2', expected '$3'"; fail=1; fi
}
want="$major.$minor.$patch"
check "ODK_LIBRARY_VERSION" "$str" "$want"
check "CMakeLists.txt project VERSION" "$cmake" "$want"
check "newest CHANGELOG entry" "$log" "$want"
if [ "$major" = "1" ]; then
    grep -q '^#define ODK_STAMP_VERSION "1.0.0"' src/odk_internal.h || {
        echo "version-check: the evidence stamp must stay \"1.0.0\" throughout 1.x"; fail=1; }
fi
[ "$fail" = 0 ] && echo "version-check: OK (version $want)"
exit $fail
