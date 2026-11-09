<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# Scoring model files

A **model file** replaces the nine hand-set scoring constants of the kernel
(and, in format v2, adds weights for the extra features of
[FEATURES.md](FEATURES.md)). With no model loaded the kernel uses the built-in
default model, whose values are exactly the constants the library has always
used, so **output is byte-identical to a build without model support**.

> **Trust.** A model file is configuration, like code. Its SHA-256 checks
> integrity (it catches corruption), but it is **not authentication**: anyone
> who can write a file can also write a valid hash, and a model file can change
> which option wins. Load model files only from sources you trust.

## 1. Format v1 (128 bytes, little-endian)

| offset | size | field |
| --- | --- | --- |
| 0 | 8 | magic `"ODKMODL1"` |
| 8 | 4 | `format_version` (uint32, must be 1) |
| 12 | 4 | `feature_set_id` (uint32, must be 0 in v1) |
| 16 | 4 | reserved (0) |
| 20 | 8 | `w_cos` (default 0.35) |
| 28 | 8 | `w_jaccard` (0.35) |
| 36 | 8 | `w_trigram` (0.30) |
| 44 | 8 | `w_whole` (0.40) |
| 52 | 8 | `w_best` (0.60) |
| 60 | 8 | `w_ctx` (0.70) |
| 68 | 8 | `w_dir` (0.30) |
| 76 | 8 | `logit_scale` (6.0) |
| 84 | 8 | `noul_logit_scale` (8.0) |
| 92 | 4 | reserved (0) |
| 96 | 32 | SHA-256 of bytes 0 to 95 |

## 2. Format v2 (feature set 1)

The same layout through byte 95, with these differences:

- magic `"ODKMODL2"`, `format_version = 2`, `feature_set_id = 1`;
- bytes 16 to 19 hold `extra_count` M (1 to 8);
- after byte 95 come M extra weights (doubles, little-endian), then the SHA-256
  of everything before it. Total size: 128 + 8·M bytes.

## 3. Scoring semantics

```
pair    = w_cos * tf_cosine + w_jaccard * jaccard + w_trigram * trigram   (clamped to [0, 1])
whole   = pair(option, anchor)
best    = pair of the best chunk (stops early at 0.95)
context = clamp01(w_whole * whole + w_best * best)
fused   = clamp01(w_ctx * context + w_dir * directional [+ sum_k v_k * F_k in v2])
logits  = logit_scale * (clamp01(fused) - 0.5)          (choice, score, rank)
noul    = sigmoid(noul_logit_scale * (evidence - 0.5))
```

## 4. Loader rules (`odk_kernel_load_model`)

The loader rejects, with `ODK_ERR_ARGUMENT` and a diagnostic: a wrong file
size (v1: exactly 128 bytes; v2: exactly 128 + 8·M), a bad magic, a wrong
version or feature-set id for the magic, nonzero reserved bytes (16 to 19 and
92 to 95 in v1, 92 to 95 in v2), any value that
is NaN or infinite, a negative `logit_scale` or `noul_logit_scale`, and a
SHA-256 mismatch. I/O failures return `ODK_ERR_INFERENCE`. On any failure the
kernel's model is left **unchanged**. A successful load replaces the kernel's
model; the model is read-only afterwards, so a loaded kernel can be shared
between threads for evaluation like any other.

**Magnitudes are not limited.** Any finite value is accepted, including
negative weights and values near 1e308. We tested extreme but validly hashed
files (huge, negative, denormal, zero and mixed weights, in v1 and v2) under
AddressSanitizer, UndefinedBehaviorSanitizer and the float checks: they load
and evaluate without crashes, NaN or infinity, and every probability stays in
[0, 1] because intermediate values are clamped. Such models do produce
meaningless or saturated decisions, so validate a trained model on held-out
data before you use it.

## 5. Usage

```c
odk_kernel *kernel = odk_kernel_create();
odk_error err;
if (odk_kernel_load_model(kernel, "project.odkmodel", &err) != ODK_OK) {
    /* err.text says why; the kernel keeps its built-in default model */
    fprintf(stderr, "model rejected: %s\n", err.text);
}
/* evaluate as usual; every decision now uses the loaded constants */
```

`examples/model.c` runs this end to end. To write a file, build and run the
packer: `make build/model_pack`, then

```sh
build/model_pack out.odkmodel                       # default constants (v1)
build/model_pack out.odkmodel W1..W9                # v1 with nine values
build/model_pack out.odkmodel --fs1 W1..W9 E1..E6   # v2; one to eight extras
```

The format stores no text. If you keep provenance for a trained model (data
hash, tool version, settings), store it in a file next to the model; the loader
does not read it.

## 6. Robustness

`make hostile` feeds the loader every truncation, bit flip, NaN, infinity,
oversized and random file; each is rejected with the kernel's model unchanged.
`make thread-probe` evaluates a loaded model from 8 threads sharing one kernel
and requires identical results. Fixtures are described in
[`tests/fixtures/README.md`](../tests/fixtures/README.md).
