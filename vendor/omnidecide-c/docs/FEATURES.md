<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# Feature set 1 (experimental)

Feature set 1 adds six extra per-option features to the score. It is
**experimental and off by default**. Only a model file in format v2
(`feature_set_id = 1`, see [MODEL.md](MODEL.md)) enables it; with the built-in
default model, or a v1 model file, this code never runs and every output is
byte-identical to a build without it. No model that uses these features has
been validated on real labeled data, so treat them as a research tool.

## How they enter the score

```
fused = clamp01(w_ctx * contextual + w_dir * directional + sum_k v_k * F_k)
```

`v_k` are the extra weights stored in the model file (one to eight values).
Six features exist, so values 7 and 8 multiply zero. A model that supplies
fewer than six weights uses only the first ones.

## The six features

All are pure functions of the option, the evidence text the kernel scored
against (the "anchor"), the question, and the other options. Tokens are the
kernel's own tokens: the active text mode, language and stopword list apply.
Features 3 to 5 apply only when the option's token sequence occurs in order
in the anchor (stopwords are already removed, so the sequence is compared
after filtering).

| # | Name | Definition |
|---|---|---|
| F1 | contrastive overlap | Of the option's tokens that also occur in the anchor, the fraction that occur in **no other option** (at most 64 other options are considered). 0 when no token overlaps. It rewards the words that distinguish an option. |
| F2 | question overlap | The fraction of the option's tokens that also occur in the question. A high value means the option mostly repeats the question. |
| F3 | cue proximity | `1 / (1 + d)` where `d` is the token distance from the option's first occurrence in the anchor to the nearest decision cue word; 0 when there is no cue word. |
| F4 | negation flag | 1.0 when a negation word occurs within the 3 tokens before the option's first occurrence in the anchor. |
| F5 | exact phrase | 1.0 when the option's whole token sequence occurs contiguously in the anchor. |
| F6 | number match | Every maximal run of ASCII digits in the option is one number. It is matched when the anchor contains an identical maximal run (same length, same digits). F6 = matched runs / runs in the option; 0 when the option has no digits. |

**Cue words** (F3): chose, chosen, selected, approved, picked, decided,
rejected, declined, vetoed, ratified, endorsed, opted; Chinese 批准 选择 决定
拍板 认可; Arabic اعتمد اختار قرر; Spanish aprobó seleccionó eligió ratificó
avaló. The list includes negative decisions (rejected, declined, vetoed), so
F3 means "a decision verb is nearby", not "the option was approved".

**Negation words** (F4): not, never, none, no, cannot, without; Chinese 不 未
别 无; Arabic لم لن لا; Spanish nunca tampoco.

## Known limits

- F6 compares digit runs, not numbers. A decimal or thousands separator splits
  a number: "1.5" is the runs "1" and "5", and "1,250" is "1" and "250". Other
  numeral systems and units are not handled. (Earlier builds matched digit
  strings as substrings, so "5" matched inside "15"; this is fixed, and any
  model evaluated with the old behavior should be re-evaluated.)
- The cue and negation lists cover English, Chinese, Arabic and Spanish only.
- F4 looks only before the option, and only 3 tokens back.
- Features are lexical. They do not understand meaning, and they inherit the
  limits of the kernel described in the [README](../README.md).
- `odk_explain_*` reports the values (`extra[]`, `extra_count`) so you can see
  what each feature contributed; see [EXPLAIN.md](EXPLAIN.md).
