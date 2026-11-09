# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Tamer Osman
#
# OmniDecide C v2.0.0 - builds libomnidecide.a from the v2 module tree:
#
#   odk-core/   the deterministic kernel, six primitives, text machinery
#   odk-brain/  the empirical odk_brain store (opt-in, never in the kernel)
#   odk-spine/  observations, registries, extension API, pins, replay,
#               calibration (the generic integration spine)
#
#   make               build build/libomnidecide.a
#   make examples      build the example programs into build/
#   make test          build and run the smoke test
#   make install       install the library and headers (PREFIX=/usr/local)
#   make clean
#
# Override the compiler or flags as usual, e.g.
#   make CC=clang CFLAGS="-O2 -Wall -Wextra -Werror -pedantic"
#
# -std=c11 and -ffp-contract=off are ALWAYS applied: disabling floating
# point contraction is what keeps results bit-identical across compilers,
# optimization levels and machines.

CC      ?= cc
AR      ?= ar
STRINGS ?= strings
CFLAGS  ?= -O2 -Wall -Wextra -pedantic
PREFIX  ?= /usr/local
BUILD   ?= build

ODK_CFLAGS := -std=c11 -ffp-contract=off -MMD -MP
ODK_IFLAGS := -Iinclude -Iodk-core -Iodk-brain -Iodk-spine

CORE_SRCS  := $(sort $(wildcard odk-core/*.c))
BRAIN_SRCS := $(sort $(wildcard odk-brain/*.c))
SPINE_SRCS := $(sort $(wildcard odk-spine/*.c))
SRCS       := $(CORE_SRCS) $(BRAIN_SRCS) $(SPINE_SRCS)

CORE_OBJS  := $(CORE_SRCS:odk-core/%.c=$(BUILD)/core/%.o)
BRAIN_OBJS := $(BRAIN_SRCS:odk-brain/%.c=$(BUILD)/brain/%.o)
SPINE_OBJS := $(SPINE_SRCS:odk-spine/%.c=$(BUILD)/spine/%.o)
OBJS       := $(CORE_OBJS) $(BRAIN_OBJS) $(SPINE_OBJS)

LIB        := $(BUILD)/libomnidecide.a
EXAMPLES   := odk_demo quickstart languages learning long_context explain model
EXAMPLE_BINS := $(EXAMPLES:%=$(BUILD)/%)

.PHONY: all examples test install uninstall clean

all: $(LIB)

$(BUILD) $(BUILD)/core $(BUILD)/brain $(BUILD)/spine:
	mkdir -p $@

$(BUILD)/core/%.o: odk-core/%.c | $(BUILD)/core
	$(CC) $(ODK_CFLAGS) $(CFLAGS) $(ODK_IFLAGS) -c $< -o $@

$(BUILD)/brain/%.o: odk-brain/%.c | $(BUILD)/brain
	$(CC) $(ODK_CFLAGS) $(CFLAGS) $(ODK_IFLAGS) -c $< -o $@

$(BUILD)/spine/%.o: odk-spine/%.c | $(BUILD)/spine
	$(CC) $(ODK_CFLAGS) $(CFLAGS) $(ODK_IFLAGS) -c $< -o $@

$(LIB): $(OBJS)
	$(AR) rcs $@ $(OBJS)

examples: $(EXAMPLE_BINS) $(BUILD)/synthetic_example

$(BUILD)/synthetic_example: examples/synthetic/synthetic_example.c $(LIB)
	$(CC) $(ODK_CFLAGS) $(CFLAGS) $(ODK_IFLAGS) $< $(LIB) -lm -o $@

$(BUILD)/odk_demo: examples/odk_demo.c $(LIB)
$(BUILD)/quickstart: examples/quickstart.c $(LIB)
$(BUILD)/languages: examples/languages.c $(LIB)
$(BUILD)/learning: examples/learning.c $(LIB)
$(BUILD)/long_context: examples/long_context.c $(LIB)
$(BUILD)/explain: examples/explain.c $(LIB)
$(BUILD)/model: examples/model.c $(LIB)

$(EXAMPLE_BINS):
	$(CC) $(ODK_CFLAGS) $(CFLAGS) -Iinclude $< $(LIB) -lm -o $@

$(BUILD)/smoke_test: tests/smoke_test.c $(LIB)
	$(CC) $(ODK_CFLAGS) $(CFLAGS) $(ODK_IFLAGS) $< $(LIB) -lm -o $@

$(BUILD)/spine_test: tests/spine_test.c $(LIB)
	$(CC) $(ODK_CFLAGS) $(CFLAGS) $(ODK_IFLAGS) $< $(LIB) -lm -o $@

# The test's exit status is the make target's exit status.
test: $(BUILD)/smoke_test $(BUILD)/spine_test
	$(BUILD)/smoke_test
	$(BUILD)/spine_test

install: $(LIB)
	install -d $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include
	install -m 644 $(LIB) $(DESTDIR)$(PREFIX)/lib/libomnidecide.a
	install -m 644 include/omnidecide.h $(DESTDIR)$(PREFIX)/include/omnidecide.h
	install -m 644 include/odk_spine.h $(DESTDIR)$(PREFIX)/include/odk_spine.h

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/lib/libomnidecide.a \
	$(DESTDIR)$(PREFIX)/include/omnidecide.h \
	$(DESTDIR)$(PREFIX)/include/odk_spine.h

clean:
	rm -rf $(BUILD)

-include $(OBJS:.o=.d)

# ---- Golden corpus (docs/GOLDEN.md) ----
GOLDEN ?= golden

.PHONY: golden golden-check spine-golden spine-golden-check spine-golden-update \
         hostile thread-probe stress stress-asan docs-check version-check bench sanitize \
         seam-check schema-check determinism check-default e2e v2-gates
golden: $(BUILD)/golden_tool
$(BUILD)/model_pack: tools/model_pack.c $(LIB)
	$(CC) $(ODK_CFLAGS) $(CFLAGS) $(ODK_IFLAGS) tools/model_pack.c $(LIB) -lm -o $@
golden-check: $(BUILD)/golden_tool
	$(BUILD)/golden_tool check $(GOLDEN)
$(BUILD)/golden_tool: tests/golden_tool.c tests/golden_lang.h $(LIB)
	$(CC) $(ODK_CFLAGS) $(CFLAGS) $(ODK_IFLAGS) tests/golden_tool.c $(LIB) -lm -o $@
$(BUILD)/odk_sidecar: tools/odk_sidecar.c $(LIB)
	$(CC) $(ODK_CFLAGS) $(CFLAGS) $(ODK_IFLAGS) tools/odk_sidecar.c $(LIB) -lm -o $@

$(BUILD)/golden_spine: tests/golden_spine.c $(LIB)
	$(CC) $(ODK_CFLAGS) $(CFLAGS) $(ODK_IFLAGS) tests/golden_spine.c $(LIB) -lm -o $@
spine-golden: $(BUILD)/golden_spine
spine-golden-check: $(BUILD)/golden_spine
	$(BUILD)/golden_spine check
spine-golden-update: $(BUILD)/golden_spine
	$(BUILD)/golden_spine update
$(BUILD)/hostile_tool: tests/hostile_tool.c $(LIB)
	$(CC) $(ODK_CFLAGS) $(CFLAGS) $(ODK_IFLAGS) tests/hostile_tool.c $(LIB) -lm -o $@
hostile: $(BUILD)/hostile_tool
	$(BUILD)/hostile_tool
$(BUILD)/thread_probe: tests/thread_probe.c $(LIB)
	$(CC) $(ODK_CFLAGS) $(CFLAGS) $(ODK_IFLAGS) tests/thread_probe.c $(LIB) -lm -lpthread -o $@
thread-probe: $(BUILD)/thread_probe
	$(BUILD)/thread_probe

# ---- Stress suite (tests/stress_test.c) ----
# Endurance + invariants under load: boundary abuse, seeded fuzz marathon
# with two-kernel byte determinism, truncation/caps sweeps, a concurrency
# storm over all six primitives, spine lineage/pin/replay endurance and a
# lifecycle churn soak.  Deterministic (fixed seeds); exits nonzero on
# any failure.  `make stress-asan` runs it under ASan + UBSan.
$(BUILD)/stress_test: tests/stress_test.c $(LIB)
	$(CC) $(ODK_CFLAGS) $(CFLAGS) $(ODK_IFLAGS) tests/stress_test.c $(LIB) -lm -lpthread -o $@
stress: $(BUILD)/stress_test
	$(BUILD)/stress_test
stress-asan:
	$(CC) $(ODK_CFLAGS) $(CFLAGS) -O1 -g -fsanitize=address,undefined $(ODK_IFLAGS) tests/stress_test.c $(LIB) -lm -lpthread -o $(BUILD)/stress_test_asan
	$(BUILD)/stress_test_asan

$(BUILD)/bench_spine: tools/bench_spine.c $(LIB)
	$(CC) $(ODK_CFLAGS) $(CFLAGS) $(ODK_IFLAGS) tools/bench_spine.c $(LIB) -lm -o $@
bench: $(BUILD)/bench_spine
	$(BUILD)/bench_spine

# Default-build check: the shipped library contains zero registered
# customizations (no application-domain vocabulary in the binary).
check-default: $(LIB)
	@hits=$$($(STRINGS) $(LIB) | grep -i -c -E "frobnication|fixture-" || true); \
	if [ "$$hits" != "0" ]; then \
		echo "default-build: FAIL - application vocabulary found in the binary"; \
		exit 1; \
	fi; \
	echo "default-build: OK (zero registered customizations in the shipped library)"

# Sanitizer gate: ASan + UBSan over the spine test, corpus runner and
# the synthetic example.
sanitize:
	$(CC) $(ODK_CFLAGS) $(CFLAGS) -O1 -g -fsanitize=address,undefined $(ODK_IFLAGS) tests/spine_test.c $(LIB) -lm -o $(BUILD)/spine_test_asan
	$(CC) $(ODK_CFLAGS) $(CFLAGS) -O1 -g -fsanitize=address,undefined $(ODK_IFLAGS) tests/golden_spine.c $(LIB) -lm -o $(BUILD)/golden_spine_asan
	$(CC) $(ODK_CFLAGS) $(CFLAGS) -O1 -g -fsanitize=address,undefined $(ODK_IFLAGS) examples/synthetic/synthetic_example.c $(LIB) -lm -o $(BUILD)/synthetic_example_asan
	$(BUILD)/spine_test_asan
	$(BUILD)/golden_spine_asan check
	$(BUILD)/synthetic_example_asan

# Customization-seam gate (I6) and schema gate (F5).
seam-check:
	sh tools/check_seam.sh
schema-check:
	sh tools/check_schema_dup.sh

# Determinism gate (I1): >= 3 runs, hash-equal outputs.
determinism: $(BUILD)/golden_spine $(BUILD)/synthetic_example $(BUILD)/smoke_test
	sh tests/determinism_gate.sh

# Browser E2E: Playwright + Chromium driving the sidecar's HTTP surface
# and the browser harness page (first run: cd e2e && npm install).
e2e: $(BUILD)/odk_sidecar
	cd e2e && npx playwright test

# The v2.0.0 acceptance aggregate.
v2-gates: test golden-check spine-golden-check hostile stress determinism sanitize seam-check schema-check check-default bench
	@echo "v2-gates: ALL GREEN"

# Relative markdown links must resolve (POSIX shell; no other tools needed).
docs-check:
	sh tests/check_links.sh

# Header macros, CMake version and CHANGELOG must agree.
version-check:
	sh tests/check_version.sh

# ---- WASM target (F6) ----
# A build profile producing a WASM module for browser and embedded hosts,
# with the same primitive surface. Requires emscripten (emcc).
# Documented host-imposed limits:
#   - single-threaded (no pthreads in the shipped profile)
#   - no filesystem: brain snapshots move through memory exports
#   - fp-contract stays off, so decisions match native bit for bit
#   - the sidecar surface (stdio/HTTP) is host-side only, not in the module
EMCC ?= emcc
WASM_EXPORTS := _odk_kernel_create,_odk_kernel_free,_odk_evaluate_choice,\
 _odk_evaluate_score,_odk_evaluate_noul,_odk_evaluate_rank,\
 _odk_evaluate_compare,_odk_evaluate_extract,_odk_result_release,\
 _odk_result_to_json,_odk_classify_scheme_tier_a,_odk_free,_odk_malloc

wasm: $(LIB)
	@command -v $(EMCC) >/dev/null 2>&1 || { \
		echo "wasm: $(EMCC) not found - install emscripten to produce build/omnidecide.wasm"; \
		echo "wasm: this host cannot run the WASM profile; limits are documented in docs/API.md"; \
		exit 1; }
	$(EMCC) -O2 -ffp-contract=off $(ODK_IFLAGS) -s WASM=1 -s EXPORTED_FUNCTIONS='[$(WASM_EXPORTS)]' -s EXPORTED_RUNTIME_METHODS='["UTF8ToString","stringToUTF8","lengthBytesUTF8"]' -s ALLOW_MEMORY_GROWTH=1 -s ENVIRONMENT=web,node --no-entry odk-core/*.c odk-brain/*.c odk-spine/*.c -o build/omnidecide.js
	@echo "wasm: wrote build/omnidecide.wasm (+ build/omnidecide.js loader)"
