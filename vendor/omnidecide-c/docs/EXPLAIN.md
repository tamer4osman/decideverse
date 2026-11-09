<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# The explain API

The explain API is read-only introspection: it returns every intermediate the
decision kernel computes, per option, with the exact values the internal path
uses. It never changes a decision (the golden corpus stays byte-identical).

## 1. Structs

```c
typedef struct {
    size_t index;        /* 0-based slot (option or rubric level)      */
    char *label;         /* owned copy of the option / level text      */
    /* channels of the pair (label, anchor) where the anchor is the
     * whole-context evidence view the kernel actually scored against   */
    double tf_cosine;    /* tf-weighted token cosine                   */
    double jaccard;      /* distinct-token Jaccard                     */
    double trigram;      /* character-trigram cosine                   */
    double whole;        /* 0.35*cos + 0.35*jac + 0.30*trig, clamped   */
    double best_chunk;   /* blended similarity of the best chunk
                            (0.0 when the context has no chunks)      */
    double contextual;   /* clamp01(0.4*whole + 0.6*best_chunk)        */
    double directional;  /* pair similarity (label, instruction);
                            0.0 when the instruction is empty         */
    double fused;        /* clamp01(0.7*contextual + 0.3*directional)  */
    double probability;  /* softmax(logits = 6*(fused-0.5)), scale 6;
                            the calibrated distribution entry          */
} odk_explain_option;

typedef struct {
    char primitive[16];  /* "choice", "score" or "noul"                */
    odk_explain_option *options; /* option_count entries               */
    size_t option_count; /* choice/rank/compare: options; score: rubric
                            levels (defaults included); noul: 0        */
    size_t chunk_count;  /* chunks built from the context              */
    /* score only */
    double score_normalized; /* result number: sum p_i * i/(n-1)       */
    double score_peak;       /* max level probability                  */
    /* noul only (options == NULL; use noul_* fields)                  */
    double noul_whole;   /* pair similarity (statement, anchor)        */
    double noul_best_chunk;
    double noul_evidence;    /* max(whole, best_chunk)                 */
    double noul_affirm;      /* sigmoid(8*(evidence-0.5))              */
} odk_explain;
```

## 2. Functions

- `odk_explain_choice(kernel, instruction, options, option_count, context,
  image, out, err)` — per-option intermediates for choice (rank and compare
  use the same fused scores; call this with the same options to explain them).
- `odk_explain_score(kernel, instruction, rubric, rubric_count, context,
  image, out, err)` — per-level intermediates; with an empty rubric the five
  default levels are explained.
- `odk_explain_noul(kernel, statement, context, image, out, err)` — the
  affirmation path: whole, best chunk, evidence, affirm (sigmoid scale 8).
- `odk_explain_to_json(const odk_explain *)` — malloc'd JSON rendering.
- `odk_explain_release(odk_explain *)` — frees labels, options and zeroes.

Errors mirror the primitives (e.g. rank arity rules apply through the caller;
noul requires a non-empty statement). `out` is always zero-initialised on
entry; release on every path.

## 3. JSON shape

```json
{"primitive":"choice","chunk_count":3,"options":[
  {"index":0,"label":"annual plan","tf_cosine":0.1,"jaccard":0.05,
   "trigram":0.3,"whole":0.15,"best_chunk":0.44,"contextual":0.32,
   "directional":0.05,"fused":0.24,"probability":0.40},
  {"index":1,"label":"monthly plan", ...}]}
```

`score` adds `"score":{"normalized":...,"peak":...}`; `noul` uses
`"noul":{"whole":...,"best_chunk":...,"evidence":...,"affirm":...}`. All
doubles print `%.17g` so the JSON round-trips the exact binary values.

## 3b. Model-dependent constants

The blend coefficients quoted above (0.35/0.35/0.30, 0.4/0.6, 0.7/0.3,
scales 6 and 8) are the BUILT-IN DEFAULT model's values. When a model
file is loaded they come from the model (docs/MODEL.md); the explain
fields always reflect the arithmetic actually performed. Feature-set-1
models additionally fill the append-only `extra[8]`/`extra_count`
fields per option, and the JSON adds `"extra":[...],"extra_count":N`.

## 4. Exactness contract

The capture is threaded through the REAL scoring functions
(`score_against_context_l`, `lexical_similarity_l`, `calibrated_distribution`)
behind optional out-parameters — the explain runner is not a parallel
reimplementation. Writing the capture changes no arithmetic, so:

- `explain.fused[i]` equals the internal fused score bit-for-bit;
- recomputing `softmax(6*(fused-0.5))` from the explained values reproduces
  the primitive's `distribution` and `probability` (rounded to 4 decimals
  like the five-key result) — this is tested in the smoke test;
- the golden corpus stays byte-identical (the decision path is untouched).

## 5. Scope notes

- v1 explains kernel-level configuration. Per-call language overrides
  (`_call`) are not part of the explain surface yet; use kernel-level
  settings.
- Word links (brain) are off in the explain path (`links = NULL`), matching
  every no-brain decision path.
