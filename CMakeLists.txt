# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tamer Osman
#
# OmniDecide WASM integration - native build.
#
# Builds the vendored, pinned kernel source plus the C interoperability
# wrapper into a static library, then the native parity runner and the
# native wrapper smoke test.  The WASM build (build.sh) compiles exactly
# the same sources with Emscripten.
#
# Floating-point contract: -ffp-contract=off is mandatory.  It is what
# keeps decisions bit-identical across compilers, optimization levels,
# native and WASM hosts.  Do not remove it to fix a build problem.

cmake_minimum_required(VERSION 3.16)
project(omnidecide-wasm VERSION 1.0.0 LANGUAGES C)

set(ODK_VENDOR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/omnidecide-c")

if(NOT EXISTS "${ODK_VENDOR}/include/omnidecide.h")
    message(FATAL_ERROR
        "Vendored kernel source not found at ${ODK_VENDOR}.\n"
        "Restore vendor/omnidecide-c/ from the delivery archive or run "
        "./fetch-kernel.sh (see docs/BUILDING.md).")
endif()

file(GLOB ODK_KERNEL_SOURCES
    "${ODK_VENDOR}/odk-core/*.c"
    "${ODK_VENDOR}/odk-brain/*.c"
    "${ODK_VENDOR}/odk-spine/*.c")

add_library(omnidecide STATIC
    ${ODK_KERNEL_SOURCES}
    src/wrappers/odk_wasm.c)

target_include_directories(omnidecide PUBLIC
    "${ODK_VENDOR}/include"
    "${ODK_VENDOR}/odk-core"
    "${ODK_VENDOR}/odk-brain"
    "${ODK_VENDOR}/odk-spine"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/wrappers")

target_compile_features(omnidecide PRIVATE c_std_11)
target_compile_options(omnidecide PRIVATE
    -O2 -Wall -Wextra -pedantic -ffp-contract=off)

# Native parity runner: reads tests/requests.jsonl, writes one
# "id\tstatus\tenvelope" line per request (see docs/TESTING.md).
add_executable(odk_parity_runner tests/native_parity.c tests/minijson.c)
target_link_libraries(odk_parity_runner PRIVATE omnidecide m)

# Native wrapper smoke test: exercises lifecycle, the six primitives,
# error envelopes and handle bookkeeping outside a browser.
add_executable(odk_smoke tests/odk_smoke.c)
target_link_libraries(odk_smoke PRIVATE omnidecide m)

enable_testing()
add_test(NAME wrapper_smoke COMMAND odk_smoke)
add_test(NAME parity_runner_smoke
         COMMAND odk_parity_runner
                 "${CMAKE_CURRENT_SOURCE_DIR}/tests/requests.jsonl"
                 "${CMAKE_CURRENT_BINARY_DIR}/native_envelopes.tsv")
