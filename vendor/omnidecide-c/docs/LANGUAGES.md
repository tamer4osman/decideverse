<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# Languages

OmniDecide supports ten languages plus a generic mode. This page explains what
"support" means, how to choose, and, honestly, how well it works.

## The ten languages and the generic mode

| Code | Language | Script | Direction |
|---|---|---|---|
| `en` | English | Latin | LTR |
| `zh` | Chinese | Han | LTR |
| `hi` | Hindi | Devanagari | LTR |
| `es` | Spanish | Latin | LTR |
| `ar` | Arabic (Modern Standard) | Arabic | RTL |
| `fr` | French | Latin | LTR |
| `bn` | Bengali | Bengali | LTR |
| `pt` | Portuguese | Latin | LTR |
| `id` | Indonesian | Latin | LTR |
| `ur` | Urdu | Arabic (Nastaliq-style letters) | RTL |
| `und` | **generic** (the default) | any | n/a |
| `auto` | deterministic auto-detection | any | n/a |

```c
odk_kernel_config cfg = {0};
cfg.text_mode = ODK_TEXT_UNICODE;   /* required for non-ASCII text */
cfg.language  = ODK_LANG_ES;        /* or ODK_LANG_UND, ODK_LANG_AUTO */
```

`odk_lang_from_code("es", &ok)` and `odk_lang_to_code(lang)` convert between
codes and the enum. You can also override the language for a single call with
the `_call` evaluators (`odk_call_config.language`).

## What a language pack adds

The generic mode (`UND`) already tokenizes any script (see
[TEXT-MODES.md](TEXT-MODES.md)). A pack adds, for its language:

- **Stopwords** (at most 400 entries, authored for this project). When a pack is
  selected its stopwords replace the `cfg.stopwords` setting.
- **Sentence enders** specific to the script (Arabic `؟ ؛ ۔`, the Devanagari and
  Bengali danda `। ॥`, Chinese `。！？；…`, and so on; Spanish `¿ ¡` are openers,
  never enders).
- **Normalization for matching**:
  - all packs: bidi controls and soft hyphens are stripped, typographic
    apostrophes map to `'`, no-break and ideographic spaces map to a space,
    full-width forms map to ASCII;
  - Arabic and Urdu: tatweel and diacritics (tashkeel) are removed, alef
    variants unified, presentation forms mapped to base letters. Urdu keeps
    letters that are distinct in Urdu spelling (such as U+06CC, U+06D2, U+06C1,
    U+06BE) separate;
  - Hindi and Bengali: combining marks, virama and nukta are **never** stripped
    (they carry meaning); zero-width joiners are kept in tokens but ignored for
    matching;
  - French: elision is detached (`l'incident` gives the token `incident`);
  - Indonesian: reduplication `anak-anak` keeps one form.
- **Optional light morphology**, off by default: set `cfg.morph_light = 1` to
  strip a small set of inflections in the *matching* channel (for example
  Arabic definite article and proclitics, a few Hindi and Bengali suffixes,
  regular Latin-script plurals and common verb endings).
- **Chinese**: the default is bigram tokenization. An optional runtime
  lexicon (`odk_kernel_load_lexicon(path)`) enables forward maximum matching;
  no dictionary is bundled. An opt-in simplified/traditional folding table is
  available with `-DODK_ZH_VARIANTS=1`.

Digit folding (mapping other numeral systems to 0-9) and Latin diacritic
folding exist but are **off** for every pack by default.

## The auto-detector

`ODK_LANG_AUTO` (or `odk_lang_detect_text`) is deterministic and conservative.
It looks at scripts first (Han, Devanagari, Bengali, Arabic-block with
Urdu-specific letters, Latin), and within Latin ranks `en`, `es`, `fr`, `pt`, `id`
by stopword hit rates and letter frequencies. When unsure it returns `UND`. It
reports its confidence and the signals it used.

In our checks it identified all ten languages on one clear sentence each, but
confidence is high only for non-Latin scripts (1.00 for Chinese, Hindi, Arabic
and Bengali; 0.70 for Urdu) and low for short Latin-script text (for example
0.25 for Indonesian and 0.40 for Portuguese). **When you know the language, set
it explicitly.**

## How well does it work? Measured results

We compared each pack with the generic mode on 720 paired decisions per
language, pooled over three corpora. Positive gain means the pack answered more
cases correctly than the generic mode.

| Language | Gain over generic (percentage points) | Verdict |
|---|---|---|
| en | +0.00 | identical answers (no difference) |
| zh | -0.14 | not significant after correction |
| hi | +4.72 | **inconsistent across corpora** |
| es | +2.92 | not significant after correction (raw p = 0.048, adjusted p = 0.34) |
| ar | -0.56 | **inconsistent across corpora** |
| fr | +4.17 | **inconsistent across corpora** |
| bn | +1.39 | **inconsistent across corpora** |
| pt | +1.25 | not significant after correction |
| id | +0.28 | not significant after correction |
| ur | +5.14 | **inconsistent across corpora** |

"Inconsistent" means the size or direction of the gain differed significantly
from one corpus to another, so the pooled number cannot be trusted as a general
expectation. For the other five, the gain was not statistically significant
after correcting for multiple comparisons. **No pack showed a consistent,
statistically reliable improvement**, which is why the generic mode is the
default and why packs are opt-in.

For scale: on these synthetic sets the generic mode itself answered only about
53% to 68% of the English cases correctly (162, 141 and 128 of 240 on the three
corpora). The kernel is a lexical engine, and absolute accuracy depends heavily
on how well the options and the context share words.

Important limits of these numbers:

- All three corpora are **synthetic**, and **no native speaker has reviewed
  them**. They show relative behavior on that data, not accuracy on
  real documents.
- They were produced with the original development tooling, not with this
  repository, so you cannot re-run them from here.
- Every non-English stopword list, normalization table and test corpus is
  **unreviewed by native speakers**.

## Recommendations

1. Start with `ODK_TEXT_UNICODE` and `ODK_LANG_UND` (generic). It works for every
   script.
2. Try a pack (or `AUTO`) only if you can measure the difference on **your own
   data**; native-speaker review of a pack would make this much safer.
3. For Arabic and Urdu, Hindi and Bengali, remember that packs only normalize
   and filter. There is no stemming by default and no dictionary.

## Out of scope

Regional or spoken varieties (Egyptian and other Arabic dialects, Cantonese),
romanized or transliterated text (Hinglish, Arabizi), machine translation,
cross-lingual or semantic understanding, speech and OCR. The kernel remains a
lexical engine: a question in one language will not match evidence written in
another.

## Smaller builds

Each pack can be omitted at compile time with `-DODK_NO_LANG_<CODE>` (for
example `-DODK_NO_LANG_UR`). A language built without its pack behaves like the
generic mode. The saving is modest (about 47 KB of a roughly 0.5 MB static
library when all nine non-English packs are dropped).
