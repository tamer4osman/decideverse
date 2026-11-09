<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# Contributing

Thanks for helping. This is a small library with one overriding rule:
**results must stay deterministic and bit-for-bit reproducible.**

## Build and test

```sh
make CFLAGS="-O2 -Wall -Wextra -Werror -pedantic" all examples
make CFLAGS="-O2 -Wall -Wextra -Werror -pedantic" test golden-check spine-golden-check \
     hostile thread-probe stress docs-check version-check
make v2-gates   # the full 2.0.0 aggregate: tests, corpora, hostile, stress,
                # determinism, sanitizers, seam, schema, default build, bench
```

Please also try `CC=clang`, and, for memory safety, run `make sanitize` and
`make stress-asan` (ASan + UBSan over the spine and stress suites). A change
is ready when:

1. it compiles without warnings under gcc and clang with the flags above;
2. `make test`, `make golden-check`, `make spine-golden-check`, `make hostile`,
   `make thread-probe`, `make stress`, `make docs-check` and
   `make version-check` all pass (each exits non-zero on failure), or simply
   `make v2-gates`;
3. `make golden-check` stays byte-identical, unless the change is *meant* to
   alter an output; in that case follow [docs/GOLDEN.md](docs/GOLDEN.md)
   section 5 and explain every changed output in the CHANGELOG;
4. you added or updated a check in `tests/smoke_test.c` (kernel) or
   `tests/spine_test.c` (spine) for the new behavior.

## Rules that protect determinism

- Keep `-ffp-contract=off`. Do not add `-ffast-math` or other options that
  change floating-point results.
- No randomness, no clock reads, no locale or environment lookups in anything
  that influences a decision. (The client's `execution_time_ms` metadata is the
  single exception.)
- No new dependencies beyond libc and libm.
- No mutable global state: every object (`odk_kernel`, `odk_client`, `odk_brain`)
  must own its state.
- A change that alters a numeric result for existing inputs is a behavior
  change. Open an issue first, explain why, and show before/after outputs on a
  set of inputs. Pure refactors must leave outputs byte-identical.

## Versioning and releases

The project uses [semantic versioning](README.md#versioning). To cut a release,
change all three places and run `make version-check`:

1. `ODK_VERSION_MAJOR`, `ODK_VERSION_MINOR`, `ODK_VERSION_PATCH` and
   `ODK_LIBRARY_VERSION` in `include/omnidecide.h`;
2. `project(omnidecide VERSION ...)` in `CMakeLists.txt`;
3. a new top entry `## x.y.z - title` in `CHANGELOG.md`.

Do **not** change the evidence stamp in `odk-core/odk_internal.h` within a major
version: it is part of the scored evidence, so changing it shifts client scores.
Patch and minor releases must keep `make golden-check` byte-identical; a change
that alters an output belongs in a major release (see
[docs/GOLDEN.md](docs/GOLDEN.md)).

## Style

- C11, no compiler extensions; the code must pass `-pedantic`.
- Source files are UTF-8, with the SPDX header used throughout:
  `SPDX-License-Identifier: Apache-2.0` and the copyright line.
- Every allocation is checked, every public function validates its arguments,
  and input is never trusted (text, JSON and snapshots may be hostile).
- Explain *why* in comments, keep functions short, match the surrounding style.

## Language data

Changes to stopwords, normalization tables or language rules should come from a
native speaker where possible, with examples. See [docs/LANGUAGES.md](docs/LANGUAGES.md).
The Unicode tables in `odk-core/odk_unicode_data.c` are generated from the Unicode
Character Database; do not edit them by hand.

## License

By submitting a contribution you agree that it is licensed under the Apache
License 2.0, as described in section 5 of [LICENSE](LICENSE).
