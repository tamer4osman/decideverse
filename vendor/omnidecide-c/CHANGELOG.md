<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# Changelog

This project follows [semantic versioning](README.md#versioning) from 1.0.0.

## 2.0.0 - the platform core

OmniDecide 2.0.0 converts the project from a decision *library* into the
deterministic core of a decision *platform*: multi-scheme classification,
temporal validity, parameterized decomposition, calibration reporting,
schema-first types, and an integration surface for a larger
observation-and-decision stack.

### Breaking changes (all itemized for the 1.x → 2.0 upgrade)

- **Source layout.** `src/` is gone; the tree is now `odk-core/` (kernel,
  six primitives, text machinery), `odk-brain/` (the `odk_brain` store) and
  `odk-spine/` (observations, registries, extension API, pins, replay,
  calibration). Drop-in builds change from `src/*.c` to `odk-core/*.c
  odk-brain/*.c odk-spine/*.c` with `-Iodk-core -Iodk-brain -Iodk-spine`.
  The public headers are `include/omnidecide.h` (kernel, unchanged API) and
  the new `include/odk_spine.h`.
- **`ODK_LIBRARY_VERSION` is "2.0.0"** and `ODK_VERSION_MAJOR` is 2. The
  client-pipeline *evidence stamp* stays "1.0.0" on purpose: 1.x output
  bytes must keep replaying bit-identically (see docs/UPGRADE-2.0.md).
  New v2 records carry `kernel_version: "2.0.0"`.
- **The kernel and spine contain zero application-domain content.**
  Classifications, decompositions, vocabularies and corpora enter only as
  versioned data through the new extension API; the shipped build contains
  zero registered customizations.

### What is included

- **`odk_classify`** (F1): one call, N schemes, one result document. Schemes
  are declarative, versioned data. Two clearly labeled tiers: Tier A
  (deterministic lexical/rule matching, bit-level replayable) and Tier B
  (brain-assisted synonym/paraphrase resilience, labeled with the brain
  snapshot that produced it, fully disableable by configuration).
- **Temporal validity** (F2): every classification assignment carries
  `valid_from` / `valid_to`; reclassification appends, never overwrites;
  as-of classification queries answer from stored intervals without
  recomputing.
- **`odk_decompose`** (F3): parameterized, versioned, temporally valid
  decomposition templates; components are the contract between perception
  and decision layers.
- **Calibration reports** (F4): per primitive, per scheme, per tier
  reliability tables, binned, with sample counts and per-bin calibration
  error; byte-identical for the same decision log.
- **Schema-first types** (F5): `schema/omnidecide.schema` is the single
  source of truth; C types and JSON (de)serialization are generated from it
  (`odk-spine/odk_spine_types.[ch]`, do not edit); every pin records the
  schema fingerprint.
- **Integration surface** (F6): a sidecar runtime speaking JSONL over stdio
  or HTTP with the same binary and guarantees; the `worldState(variables,
  scope, t)` context contract (validated, scope opaque); append-only
  decision pins; replay with `exact` / `semantic` / `foreign` classes.
- **Application extension API** (F7): registration of scheme sets,
  decomposition templates, vocabularies, golden corpora and brain snapshots
  from files or provider callbacks; register → validate → activate
  atomically; re-registration is a versioned append. The in-repo synthetic
  example exercises every extension point and is explicitly non-normative.
- **Golden corpus expansion**: classification (including paraphrase),
  reclassification lineages, decomposition, calibration, pin→replay
  round-trips.
- **CI gates**: determinism (≥3 runs, hash-equal), ASan/UBSan clean,
  benchmark (Tier A decision ≤ 1 ms @ 2 KB context), customization-seam
  grep gate with a configurable prohibited-words list, schema-duplicate
  gate, and the default build check for zero registered customizations.

### Verification performed (rerun with `make v2-gates`)

- All six v1 primitives pass the existing 2,013-case golden corpus
  **byte-identically** (the 1.0.0 evidence stamp is preserved for this).
- The 373-check smoke test, the 5,443-check hostile-input runner, the
  thread probe and the documentation link check all pass unchanged.
- New `make stress` suite (6,135 deterministic checks): contract-boundary
  abuse, a seeded fuzz marathon proving two-kernel byte-identical
  transcripts, truncation/caps/work-budget sweeps with the
  caps-never-touch-the-numbers guarantee, an 8-thread concurrency storm
  over all six primitives (shared and per-thread kernels), spine
  lineage/as-of/pin/replay/calibration endurance, and a lifecycle churn
  soak with an RSS growth watch. Clean under ASan/UBSan and TSan.
- The new engines pass a 210-check spine test suite and a 35-case spine
  golden corpus (`golden/spine/`), both byte-identical across runs.
- Clean under AddressSanitizer and UndefinedBehaviorSanitizer; the
  determinism gate re-runs the corpus runners three times with hash
  comparison; the seam, schema and default-build gates are green.
- Benchmarks at 2.0.0: Tier A decision 0.28 ms @ 2 KB context (budget
  ≤ 1 ms), classification throughput ≈ 22,000/s @ 2 KB, pin+replay
  round-trip ≈ 0.7 ms.

## 1.0.0 - first public release

OmniDecide C is released as a standalone, dependency-free C11 library.

### What is included

- A deterministic decision kernel with six primitives (`choice`, `score`,
  `noul`, `rank`, `compare`, `extract`), each returning a result, a confidence
  and a probability distribution.
- Two text modes: ASCII compatibility (the default) and Unicode (NFC, default
  case folding, script-aware segmentation, Unicode 15.1.0 data).
- Ten language packs (English, Chinese, Hindi, Spanish, Arabic, French, Bengali,
  Portuguese, Indonesian, Urdu), a generic mode and a deterministic
  auto-detector. All non-English packs are unreviewed by native speakers.
- Input profiles (plain, markdown, chat, log, JSON, JSONL, CSV, TSV, code, HTML
  text, extracted PDF text, auto-detect), configurable limits, over-limit
  strategies, and **truncation reporting** (`context_truncated`,
  `context_units_dropped` in `odk_result_meta`, filled by the `_opts` and
  `_call` evaluators; the plain evaluators still cut long contexts without a
  signal).
- A client pipeline: raw text plus JSON queries in, five-key JSON out.
- An image-fingerprint evidence channel (raw RGB pixels).
- The **explain API** (`docs/EXPLAIN.md`): every intermediate behind a decision.
- Optional **scoring model files** (`docs/MODEL.md`) and an experimental extra
  feature set (`docs/FEATURES.md`). With no model loaded the built-in default
  is used, whose values are the kernel's original constants.
- Opt-in outcome learning: an experience brain, learned word links and JSON
  snapshots (format v3; v1 and v2 snapshots still load, whatever library
  version wrote them).
- Makefile and CMake builds, seven examples, and the test suite: a 373-check
  smoke test, a 2,013-case golden regression corpus (`docs/GOLDEN.md`), a
  5,443-check hostile-input runner, a ThreadSanitizer probe, a documentation
  link check and a version-consistency check.

### Verification performed

- Warning-free under `-Wall -Wextra -Werror -pedantic` with gcc, clang and
  musl-gcc; CMake builds and `ctest` pass with gcc and clang.
- The golden corpus is byte-identical on gcc and clang at -O0, -O1, -O2 and -O3,
  and on musl.
- Clean under AddressSanitizer, UndefinedBehaviorSanitizer, LeakSanitizer,
  valgrind and ThreadSanitizer (separate objects per thread, and one kernel
  shared by 8 threads, with and without a loaded model).
- Extreme-value model files (1e308, negative, denormal, zero) load and evaluate
  without crashes, NaN or infinity.

### Known limitations

- The engine is lexical, not semantic: it can pick the wrong option when a
  distractor shares a word with the question, when the context mentions both
  options, or when the answer is a paraphrase. Validate it on your own data.
- Contexts longer than the configured limits are cut. Use the `_opts`
  evaluators to see it, raise the limits, use head+tail, or use Unicode mode.
- The language packs and the experimental features have not been validated on
  real labeled data. No trained model ships with the library.
- No independent third-party audit. macOS is covered by the CI workflow but has
  not been run. Windows and MSVC are not supported.
- Model files are trusted configuration: a valid hash detects corruption, not
  tampering.

### Compatibility and numbering

- Earlier internal builds were numbered 3.x and 4.x and were never released.
  1.0.0 is the first public version; source comments no longer refer to the
  old numbers.
- The client pipeline scores a **fixed evidence stamp** (`"1.0.0"` for the whole
  1.x series), not the library version, so client decisions do not change
  between 1.x releases. The golden corpus masks `library_version`, so a patch
  release does not require regenerating it (verified by building 1.0.1 against
  the 1.0.0 corpus).
- The public structs may grow in minor releases (new fields are appended at the
  end). There is no ABI guarantee: link statically or rebuild when you upgrade.
