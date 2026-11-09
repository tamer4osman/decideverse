<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# Golden regression corpus

`golden/` freezes the library's exact outputs on 2,013 inputs so that any
change to a decision, confidence, distribution, metadata field or error
message shows up as a byte difference. It is the strongest guard against an
accidental behavior change in a deterministic library.

```sh
make golden-check          # re-runs every case; exit 0 only if all are byte-identical
```

The v2.0.0 spine engines have their own corpus: `golden/spine/` (35 cases,
checked with `make spine-golden-check`), documented in
[golden/spine/README.md](../golden/spine/README.md).

## 1. Files

- `golden/inputs.jsonl`: one JSON object per case, fully describing one API call
  (no external state).
- `golden/expected.jsonl`: one object per case with the exact expected output.
- `tests/golden_tool.c`: the generator (`gen`) and checker (`check`):
  `build/golden_tool gen|check [dir]`. `check` parses the inputs with the
  library's own strict JSON parser, so the corpus is also a parser round-trip
  test. The inputs file is about 11 MB because contexts are stored as base64.

## 2. Determinism

- No clock, locale, environment or random source influences a case. The
  hostile and binary cases come from a fixed linear congruential generator,
  so they replay byte for byte.
- The client pipeline's wall-clock `execution_time_ms` is the only volatile
  field and is stripped from client expectations. Its `library_version` is
  masked as `<version>`, so a release does not force a golden update; every
  other byte, including the decisions, stays checked.

## 3. What is frozen

Each input records the primitive, the strings, the exact context bytes
(`context_b64`, so invalid UTF-8 survives), the text mode, profile, language,
over-limit strategy, any non-default limits, per-call options, output caps,
`top_k` and an optional synthetic image (`gradient24`, `half24`, `solid64`).
Each expected record holds the `odk_status`, the exact five-key result JSON,
and for the kernel cases **every** `odk_result_meta` field (doubles printed
with `%.17g`). Error messages are deterministic functions of the input and
are frozen too.

## 4. Coverage

| Kind | Cases | What it exercises |
|---|---|---|
| `base` | 1,728 | every language (12 including `und` and `auto`) × text mode × primitive × input profile, with a native-language sentence in the context |
| `kern` | 269 | a length ladder up to 90 KB with the decisive sentence at the end; over-limit strategies; forced truncation and work-budget limits; invalid UTF-8 and a binary blob; images; per-call language and morphology options; output caps; edge cases (one option for rank, three for compare, zero options, empty contexts, 60 options, duplicates, `top_k` clamping, odd rubric sizes) |
| `cli` | 16 | the client pipeline: simple, 64-level JSON nesting, markdown minification, multi-query batches, invalid and empty queries, Unicode and hostile text |

## 5. Changing the corpus

The corpus was generated from the last pre-release build and then updated twice, both times only where the output genuinely
changed:

- the result metadata gained two appended fields (`context_truncated`,
  `context_units_dropped`);
- at 1.0.0 the library version string changed, which changes the 14 client
  pipeline cases that report it. Thirteen differ only in the `library_version`
  text; one differs in a confidence that moved from 0.503 to 0.5029 because the
  scored evidence stamp text changed with the version at that moment. From 1.0.0
  on the stamp is a fixed constant, so later releases do not shift these cases.

1,999 of the 2,013 cases are byte-identical to the pre-release corpus, and no
decision changed anywhere. For every case the pre-release sources reproduce the
stored output except for those documented differences.

Treat the files as read-only. If a change is *meant* to alter an output,
regenerate with `build/golden_tool gen golden`, review the diff line by line,
and explain each changed output in the CHANGELOG in the same commit. The
checks must pass byte for byte on gcc and clang at -O0, -O1, -O2 and -O3.
