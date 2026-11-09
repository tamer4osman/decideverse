<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# Upgrading from 1.x to 2.0

2.0.0 is the semver-major release that carries the platform API. The
kernel's decision behavior is **unchanged**: all six primitives pass the
1.0.0 golden corpus byte-identically.

## Breaking changes, itemized

### 1. Source layout

`src/` is gone. The tree is now:

```
odk-core/    the kernel, six primitives, text machinery, Tier A
             classify/decompose engines
odk-brain/   the odk_brain store and the Tier B classifier machinery
odk-spine/   observations, registries, extension API, pins, replay,
             calibration; generated record types
schema/      omnidecide.schema — the single source of truth
```

**Migration.**

- Makefile builds: `src/*.c` → `odk-core/*.c odk-brain/*.c odk-spine/*.c`
  with `-Iodk-core -Iodk-brain -Iodk-spine` (or keep `make`).
- CMake: nothing changes (`add_subdirectory` still works).
- Drop-in source builds must add the three module directories.

### 2. Version macros

`ODK_VERSION_MAJOR` is 2 and `ODK_LIBRARY_VERSION` is `"2.0.0"`.

The client-pipeline **evidence stamp stays `"1.0.0"` on purpose**. The
stamp is part of the scored evidence (the character-trigram channel), so
changing it would change every 1.x decision's output bytes. Keeping it
means 1.x-era decisions replay bit-identically. New v2 records (pins,
observations, calibration reports) carry `kernel_version: "2.0.0"`
instead. If your deployment re-stamps deliberately, build with
`-DODK_STAMP_VERSION='"x.y.z"'` as before.

### 3. Zero application-domain content

The kernel and spine contain no domain vocabulary, scheme sets,
templates or corpora. If you had hardcoded any of that in 1.x (the
library never did, so most integrations are unaffected), it now enters
through the extension API — see [EXTENSION.md](EXTENSION.md).

### 4. New public header

`include/odk_spine.h` (plus `include/odk_spine_base.h` and the generated
`odk_spine_types.h`). `include/omnidecide.h` keeps its full 1.x API.
Installing now copies both headers (`make install`).

### 5. Build-id / stamping

`ODK_SPINE_SCHEMA_FINGERPRINT` identifies the schema version a build was
generated from; pins record it. Regenerating types after changing
`schema/omnidecide.schema` changes the fingerprint by design.

## Non-breaking additions (quick reference)

- `odk_classify` (F1) with two labeled tiers; Tier B disableable
- temporal validity with as-of queries (F2)
- `odk_decompose` parameterized templates (F3)
- calibration reports (F4)
- schema-first generated types + fingerprint (F5)
- sidecar (JSONL stdio + HTTP), worldState contract, pinning + replay
  with `exact` / `semantic` / `foreign` classes (F6)
- the F7 extension API and the synthetic in-repo example
- new gates: determinism, seam-grep, schema-dup, default-build,
  benchmarks, sanitizer aggregate — `make v2-gates`

## Verification you can rerun

```sh
make test            # 373 kernel checks + 210 spine checks
make golden-check    # 2013 v1 corpus cases, byte-identical
make spine-golden-check
make v2-gates        # everything, including sanitizers and benchmarks
```
