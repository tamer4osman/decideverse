<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# API guide

Everything public is declared in [`include/omnidecide.h`](../include/omnidecide.h),
which is safe to include from C and C++. This guide explains the function
families and how they fit together; the header comments are the detailed
reference.

The v2.0.0 integration spine (`odk_spine*`: multi-scheme classification,
temporal validity, decomposition, calibration, pinning and replay, the
sidecar runtime) is declared in `include/odk_spine.h` and documented
separately in [SPINE-API.md](SPINE-API.md). The kernel API below is
unchanged in 2.0.0.

Contents: [Conventions](#conventions) · [Kernel](#the-kernel) ·
[Primitives](#the-six-primitives) · [Results](#results) ·
[Per-call options](#per-call-options) · [Queries and the dispatcher](#queries-and-the-dispatcher) ·
[Client pipeline](#the-client-pipeline-raw-text--json) · [Limits](#limits) ·
[Images](#the-image-channel) · [Languages](#languages) · [Minifier](#the-minifier) ·
[Learning](#learning) · [Compile-time options](#compile-time-options)

## Version

`ODK_VERSION_MAJOR`, `ODK_VERSION_MINOR` and `ODK_VERSION_PATCH` are integers,
and `ODK_LIBRARY_VERSION` is the matching string `"MAJOR.MINOR.PATCH"`. The
client pipeline reports it as `metadata.library_version`, and brain snapshots
record it as `semantics_version` (informational: snapshots written by any
earlier build still load). The versioning policy is in the
[README](../README.md#versioning).

## Conventions

**Status codes.** Fallible functions return an `odk_status`:

| Code | Meaning |
|---|---|
| `ODK_OK` | success |
| `ODK_ERR_ARGUMENT` | misuse: NULL pointer, wrong option count, bad JSON, unknown primitive |
| `ODK_ERR_INFERENCE` | evaluation failure |
| `ODK_ERR_MEMORY` | allocation failure |
| `ODK_ERR_INTERNAL` | unexpected internal condition |

`odk_status_message(status)` returns a short description (never NULL).

**Error text.** Most calls take an optional `odk_error *err` (a 256-byte buffer
with a `text` field). Pass `NULL` to discard the message. Messages that embed
caller data are bounded: a 120-byte excerpt cut on a UTF-8 boundary plus a
visible `... [truncated]` marker. The `odk_error_ex` variants carry an
unbounded heap message; release them with `odk_error_ex_release`.

**Ownership.**

- Objects you create (`odk_kernel`, `odk_client`, `odk_brain`) are released with
  their `*_free` function.
- An `odk_result` you receive from an evaluator owns its memory: call
  `odk_result_release(&result)` when done.
- Functions that return `char *` (`odk_result_to_json`, `odk_minify`,
  `odk_output_to_json`, `odk_text_normalize_alloc`, `odk_brain_summary_json`, …)
  return `malloc`'d strings: release them with `free()`.
- Input strings are never retained; you may free them after the call returns.

**Text encoding.** All text is UTF-8. Invalid UTF-8 never crashes the library:
each invalid byte is treated as a separator (see [TEXT-MODES.md](TEXT-MODES.md)).

**Determinism.** Evaluation has no randomness and reads no clock or
environment. The same inputs give bit-identical results on a given platform.
The client pipeline's `metadata.execution_time_ms` is the one wall-clock field.

**Threads.** Tested under ThreadSanitizer: independent kernels, brains and
clients in separate threads, and one `odk_kernel` shared across threads for
concurrent evaluation, both without data races. Sharing a brain or a client
across threads is untested; guard it with your own lock. `odk_brain_advise` is
`const` and documented as safe beside concurrent evaluations.

## The kernel

```c
odk_kernel *odk_kernel_create(void);                       /* defaults */
odk_kernel *odk_kernel_create_ex(const odk_kernel_config *cfg);
void        odk_kernel_free(odk_kernel *kernel);
```

`odk_kernel_create_ex(NULL)` equals `odk_kernel_create()`. It returns `NULL`
only on allocation failure. **A zero-initialised `odk_kernel_config` selects
every documented default**, so set only what you need:

```c
odk_kernel_config cfg = {0};
cfg.text_mode = ODK_TEXT_UNICODE;
cfg.language  = ODK_LANG_AUTO;
odk_kernel *kernel = odk_kernel_create_ex(&cfg);
```

| Field | Default (0) | Meaning |
|---|---|---|
| `text_mode` | `ODK_TEXT_ASCII_COMPAT` | `ODK_TEXT_UNICODE` for any non-ASCII text |
| `stopwords` | `ODK_STOPWORDS_EN` | stopword pack for the ASCII mode (`EN`, `DE`, `FR`, `ES`) |
| `profile` | `ODK_PROFILE_PLAIN` | how the context is chunked (see [TEXT-MODES.md](TEXT-MODES.md)) |
| `language` | `ODK_LANG_UND` | generic mode; or a language pack, or `ODK_LANG_AUTO` |
| `morph_light` | 0 (off) | light morphology in the matching channel |
| `context_max` | 20,000 bytes (ASCII) / 100,000 code points | cap on the context length |
| `chunk_source_max` | 40,000 B / 200,000 cp | cap on the text that is chunked |
| `max_chunks` | 2,000 / 10,000 | cap on chunks scored |
| `snippet_max` | 240 B / 1,000 cp | cap on one chunk |
| `over_limit` | per profile | what to do with an over-long context (`HEAD`, `HEAD_TAIL`, `SELECT`) |
| `max_ranked`, `max_snippet_chars`, `max_total_chars` | all options / `snippet_max` / 10,000 cp | caps on the *rendered* output only |
| `max_work` | derived (never binds by default) | work budget for the scan |

Hard ceilings are clamped for you (for example `context_max` ≤ 10,000,000).
Accessors: `odk_kernel_text_mode`, `odk_kernel_stopwords`, `odk_kernel_profile`,
`odk_kernel_language`, `odk_kernel_backend` (always `"embedded-fallback"`: the
deterministic engine is the only backend).

## The six primitives

All evaluators take the kernel, the question or statement, the options (where
relevant), the `context` text, an optional `const odk_image *image` (`NULL` for
none), and return an `odk_result` through `odk_result *out`.

```c
odk_status odk_evaluate_choice (kernel, instruction, options, option_count, context, image, out, err);
odk_status odk_evaluate_score  (kernel, instruction, rubric,  rubric_count,  context, image, out, err);
odk_status odk_evaluate_noul   (kernel, statement,                          context, image, out, err);
odk_status odk_evaluate_rank   (kernel, instruction, options, option_count, context, image, out, err);
odk_status odk_evaluate_compare(kernel, instruction, options, 2,            context, image, out, err);
odk_status odk_evaluate_extract(kernel, instruction,                        context, image, top_k, out, err);
```

| Primitive | Result `kind` | Notes |
|---|---|---|
| `choice` | `ODK_VALUE_STRING` | needs at least one option; the winner |
| `score` | `ODK_VALUE_NUMBER` | a number from 0 to 1 (the expected rubric position); the distribution is over the rubric labels |
| `noul` | `ODK_VALUE_BOOL` | is the statement supported by the context; distribution keys `affirm` / `deny` |
| `rank` | `ODK_VALUE_STRING` | `"a > b > c"`; needs at least 2 options; full array via `odk_result_rank_items` |
| `compare` | `ODK_VALUE_STRING` | **exactly 2 options**, otherwise `ODK_ERR_ARGUMENT` |
| `extract` | `ODK_VALUE_LIST` | up to `top_k` best-supported sentences, best first |

Real output for one context ("The service stopped responding. Restarting the
service restored normal operation within seconds. Logs show the disk was full."):

```text
choice  {"primitive": "choice", "result": "restart the service", "confidence": 0.6136, "probability": 0.6136, "distribution": {"restart the service":0.6136,"reinstall the operating system":0.208,"wait for the weekend":0.1783}}
score   {"primitive": "score", "result": 0.5271, "confidence": 0.3536, "probability": 0.3536, "distribution": {"fails":0.2961,"partly works":0.3536,"works":0.3502}}
noul    {"primitive": "noul", "result": true, "confidence": 0.7754, "probability": 0.7754, "distribution": {"affirm":0.7754,"deny":0.2246}}
rank    {"primitive": "rank", "result": "restart the service > reinstall the operating system > wait for the weekend", "confidence": 0.225, "probability": 0.6235, "distribution": {...}}
compare {"primitive": "compare", "result": "restart the service", "confidence": 0.7514, "probability": 0.7514, "distribution": {...}}
extract {"primitive": "extract", "result": ["Logs show the disk was full.","The service stopped responding."], "confidence": 0.5586, "probability": 0.5586, "distribution": {"snippet_1":0.5586,"snippet_2":0.0}}
```

These are lexical evidence scores. See the limits of that approach in the
[README](../README.md#read-this-first-what-it-is-and-what-it-is-not).

## Results

A simplified view of `odk_result` (see the header for the exact layout):

```c
kind                /* ODK_VALUE_NONE / BOOL / NUMBER / STRING / LIST */
boolean | number | string | list   /* the value that kind selects */
confidence          /* 0..1 */
probability         /* 0..1 */
distribution        /* .count and .entries[i].key / .entries[i].value */
```

`odk_result_to_json(&result)` serialises the five-key payload shown above
(`primitive`, `result`, `confidence`, `probability`, `distribution`); free the
returned string with `free()`. Numbers are rounded to 4 decimals (half-even).
Release the result with `odk_result_release`.

For `rank`, `odk_result_rank_items(&result, options, option_count, &items)`
returns the **full** ranked array (label, original index, score), independent of
`max_ranked`; release it with `odk_result_rank_items_free`.

## Per-call options

Three families extend the base evaluators without changing them:

- **`_opts`** adds an `odk_output_caps *` (cap the *rendered* output) and an
  `odk_result_meta *` (truncation, work and language reporting). Caps never
  change the numeric decision: result, confidence and distribution are
  bit-identical under every cap.
- **`_call`** adds an `odk_call_config *` that can change the decision for that
  call: for example an explicit `language` hint that overrides the kernel's.
  `ODK_LANG_DEFAULT` or a `NULL` pointer keeps the kernel setting.
- **Zero-initialise** `odk_result_meta` before the call. What it reports:
  `language` and, with `ODK_LANG_AUTO`, the detected language, confidence and
  signal; `total_options`, `emitted` and `dropped` for rank and extract;
  `truncated` (a cap limited the **rendered output**); and, for the SELECT
  over-limit strategy only, `chunks_kept` and `chunks_dropped`, with
  `work_used`, `work_budget` and `work_exceeded` (the work budget stopped the
  scan); and the context truncation report: `context_truncated` is 1 when part
  of the caller's context never entered the evidence view, and
  `context_units_dropped` is how much (bytes in ASCII mode, code points in
  Unicode mode). These two fields are pure accounting - they never change a
  decision, and the base (no-meta) evaluators behave exactly as before. See
  [Limits](#limits).

## Queries and the dispatcher

A query is a validated, owned description of one primitive call:

```c
odk_query_from_spec(key, primitive, instruction, statement,
                    options, option_count, rubric, rubric_count, top_k, &q, err);
odk_query_from_json("{\"primitive\":\"choice\",\"question\":\"Where?\","
                    "\"options\":[\"Berlin\",\"Madrid\"]}", &q, err);
odk_evaluate_query(kernel, &q, context, image, &result, err);
odk_query_release(&q);
```

JSON fields: `primitive`, `instruction` (or `question`), `statement`, `options`,
`rubric`, `top_k`. The primitive name is matched case-insensitively and trimmed;
`top_k` is clamped to 1..10; `rank` needs at least 2 options and `compare`
exactly 2. `odk_evaluate_query_opts` and `_call` exist too.

## The client pipeline (raw text → JSON)

The client turns raw text plus a set of queries into one five-key document. It
is the easiest way to drive the library from another process or language.

```c
odk_client *client = odk_client_create("auto");      /* device preference */
char *json = NULL;
odk_client_evaluate_json(client, text,
    "{\"fix\":{\"primitive\":\"choice\",\"question\":\"How should we fix the outage?\","
    "\"options\":[\"restart the service\",\"wait for the weekend\"]}}",
    "hybrid", &json, err);
/* ... use json ... */
free(json);
odk_client_free(client);
```

The queries JSON is an object mapping a key you choose to a query spec. The
analysis mode is `"hybrid"`, `"kernel_only"` or `"parser_only"`; unknown values
fall back to hybrid and record a warning. The device preference is accepted for
compatibility; the library has no GPU backends, so the plan is always CPU.

The output has exactly five top-level keys:

```json
{"file_name": "direct_string_input", "file_type": "text",
 "metadata": { "analysis_mode": "hybrid", "kernel_backend": "embedded-fallback",
               "library_version": "2.0.0", "raw_tokens": 82, "compressed_tokens": 80,
               "token_reduction_pct": 2.44, "execution_time_ms": 0.33, "...": "..." },
 "extracted_markdown": "---\nfile_name: direct_string_input\n...",
 "decisions": {"fix": {"primitive": "choice", "result": "restart the service",
                       "confidence": 0.7512, "probability": 0.7512,
                       "distribution": {"restart the service": 0.7512, "wait for the weekend": 0.2488}}}}
```

Variants: `odk_client_evaluate` takes `odk_query` objects instead of JSON and fills an
`odk_output` (release with `odk_output_release`, read metadata with
`odk_output_meta_get`, serialise with `odk_output_to_json`); the `_ex` and `_lang`
variants add an `odk_output_policy` (what the markdown echo contains) and an
`odk_call_config` (language). `odk_client_attach_kernel` makes the client use a
kernel you configured. Direct strings only: the client does not read files.

## Limits

Context limits are measured in **bytes in ASCII mode** and **code points in
Unicode mode**; truncation in Unicode mode happens only on code-point
boundaries. When the context exceeds the limits, `over_limit` decides what is
kept: `ODK_OVERLIMIT_HEAD` (the start; the legacy behavior),
`ODK_OVERLIMIT_HEAD_TAIL` (start and end; the default for logs) or
`ODK_OVERLIMIT_SELECT` (the most relevant chunks).

> **Long contexts: check `context_truncated`.** With the default limits
> (ASCII mode: 20,000 bytes of context, 40,000 bytes of chunk source, 2,000
> chunks) and the HEAD strategy, text beyond the limit is discarded from the
> evidence view. The `_opts` evaluators now REPORT this: `meta.context_truncated`
> is 1 and `meta.context_units_dropped` counts the bytes (ASCII) or code points
> (Unicode) that never entered the evidence scan. In our test, a decisive
> sentence at the end of a 90 KB context was still not scored (the decision was
> an exact 50/50 tie) - but the meta now says exactly why: 50,000 bytes dropped.
> The base (no-meta) evaluators do not report it; only `_opts`/`_call` variants
> fill the meta. To actually keep the end of long inputs, raise `context_max`,
> `chunk_source_max` and `max_chunks`, use `ODK_OVERLIMIT_HEAD_TAIL`, or use
> `ODK_TEXT_UNICODE` (whose defaults are 100,000 code points).
> `ODK_OVERLIMIT_SELECT` can miss evidence too: in the same test its work
> budget ran out (`work_exceeded = 1`) before it reached the sentence.

## The image channel

```c
typedef struct { int width, height; const unsigned char *rgb; } odk_image;
```

`rgb` is `width * height * 3` bytes, row-major RGB. The image is area-averaged
onto a 24×24 grid and described as short text (`image <brightness> <colour>
<aspect> <W>x<H>`), which is added to the evidence. `odk_describe_image` exposes
the description directly; a NULL or invalid image degrades to the bare marker
`"image"` and never fails. The image feeds `choice`, `score`, `noul`, `rank` and `compare`;
`extract` ignores it (text-only). Decode PNG/JPEG yourself first.

## Languages

```c
odk_language odk_lang_from_code("es", &ok);   /* en zh hi es ar fr bn pt id ur, und, auto */
const char  *odk_lang_to_code(lang);          /* "es" */
const char  *odk_lang_display_name(lang);     /* "spanish" */
odk_language odk_lang_detect_text(text, &confidence, signal_buf, signal_cap);
```

`odk_lang_detect_text` is the deterministic detector used by `ODK_LANG_AUTO`; it
returns `ODK_LANG_UND` when unsure. Details and advice: [LANGUAGES.md](LANGUAGES.md).
Optional runtime loaders: `odk_kernel_load_stopwords(path)` and
`odk_kernel_load_lexicon(path)` (strict JSON, size-capped, safe on hostile input).

## The minifier

`odk_minify(text)` / `odk_minify_ex(text, mode)` return a compacted copy of
Markdown text (free with `free()`); `odk_estimate_tokens(text)` estimates its
token count and `odk_token_report` compares before and after.
`odk_detect_input_profile(text, reason_buf, cap)` guesses the input profile.
`odk_utf8_decode_next` and `odk_text_normalize_alloc` expose the UTF-8 decoder and
the Unicode normalizer.

## Learning

The brain API (`odk_brain_*`) is covered in [LEARNING.md](LEARNING.md).

## Compile-time options

| Flag | Effect |
|---|---|
| `-ffp-contract=off` | **required** for bit-identical floating-point results (applied by the bundled Makefile and CMakeLists) |
| `-DODK_NO_LANG_<CODE>` | omit one language pack, for example `-DODK_NO_LANG_UR`. A language built without its pack falls back to the generic behavior |
| `-DODK_ZH_VARIANTS=1` | enable the opt-in simplified/traditional Chinese folding table (off by default; unreviewed) |
| `-DODK_STAMP_VERSION='"x.y.z"'` | override the fixed evidence stamp that the **client pipeline** embeds in the text it scores. The stamp is a constant ("1.0.0" for the whole 1.x series, unchanged in 2.0.0), so client decisions do not shift between releases; override it only to reproduce scores from a build that used another stamp. Direct `odk_evaluate_*` calls never use it |
