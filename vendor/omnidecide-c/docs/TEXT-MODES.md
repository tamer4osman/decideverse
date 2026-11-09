<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# Text modes, tokenization and input profiles

Before scoring anything, the kernel turns text into **tokens** and splits the
context into **chunks**. This page explains how, and what to configure.

## Two text modes

| | `ODK_TEXT_ASCII_COMPAT` (default) | `ODK_TEXT_UNICODE` |
|---|---|---|
| Works on | bytes | Unicode code points |
| Words | ASCII letters and digits | letters, marks and numbers of any script |
| Case folding | ASCII only | full Unicode default case folding |
| Limits measured in | bytes | code points (truncation only on code-point boundaries) |
| Non-ASCII text | acts as separators, so it carries no evidence | tokenized properly |

**Use `ODK_TEXT_UNICODE` for any text that is not plain ASCII.** In the default
mode, Arabic text produces no signal at all. In our check, two Arabic options
scored an exact 0.5000 / 0.5000 tie (so the first option "wins"), while Unicode
mode scored the correct option 0.8708 against 0.1292. Accented Latin text (for
example Spanish) mostly still works in ASCII mode, because the unaccented
fragments of its words remain visible, but you lose the accented letters.

For pure ASCII input, both modes give identical results (we checked eight
cases across two profiles), so switching to Unicode mode is safe for mixed
workloads. The ASCII mode exists to keep legacy byte-for-byte behavior.

## The Unicode pipeline

1. **UTF-8 decoding is strict.** Overlong forms, surrogates, values above
   U+10FFFF, truncated sequences and stray continuation bytes are invalid.
   Each invalid byte is treated as a single separator and decoding resumes at
   the next byte; the library never reads out of bounds and never fails on bad
   UTF-8. A NUL (U+0000) is a separator, too.
2. **Normalization:** NFC, then Unicode default case folding, using tables
   derived from the Unicode Character Database 15.1.0 (`ODK_UNICODE_VERSION`).
   `odk_text_normalize_alloc` exposes exactly this step.
3. **Language folding** (only when a language pack is selected): width folding,
   stripping of bidi controls and soft hyphens, script-specific rules such as
   Arabic letter unification. See [LANGUAGES.md](LANGUAGES.md).
4. **Classification and segmentation.** Each code point is classified as a
   letter, mark, number or separator, and given a script class. Text is split
   into runs of token characters, then at every change of script class (for
   example Latin|Han). Marks and digits never split a run by themselves
   (`café2024` is one token).
5. **Token emission.** Space-delimited scripts emit one token per word.
   Han and kana emit overlapping **bigrams** of code points (a single
   character emits itself). Hangul emits words.
6. **Filtering.** Tokens of a single code point are dropped (except a single
   Han character, which is a word), then stopwords are removed.

Stopwords come from the active pack: `cfg.stopwords` (`EN` by default, or `DE`,
`FR`, `ES`) for the generic mode, or the selected language's own pack when a
language is chosen (see [LANGUAGES.md](LANGUAGES.md)).

## Chunking and input profiles

The context is split into chunks that are scored separately against the
question. **The profile decides the chunk boundaries.** Set it in the kernel
config (`cfg.profile`) or use `ODK_PROFILE_AUTO`.

| Profile | Chunk unit | Notes |
|---|---|---|
| `PLAIN` | sentences | the default |
| `MARKDOWN` | blank-line blocks; headings start new blocks | the minifier runs first |
| `CHAT` | non-empty lines | one message per line |
| `LOG` | non-empty lines | keeps timestamps and levels; over-limit default is head+tail |
| `JSON` / `JSONL` | top-level array elements / one object per line | falls back to plain lines if unparseable |
| `CSV` / `TSV` | data rows | each chunk is the header row plus one row |
| `CODE` | blank-line blocks | no markdown minification |
| `HTML_TEXT` | sentences | **your app must strip the tags first** |
| `EXTRACTED_PDF` | blank-line blocks after de-hyphenation | joins `word-\nword` |

`ODK_PROFILE_AUTO` runs `odk_detect_input_profile`, a conservative deterministic
detector; call it yourself to see its verdict and reason. When in doubt, set the
profile explicitly: a wrong guess changes how the evidence is chunked.

## Limits and what happens past them

Limits (context length, chunk text, number of chunks, snippet length, work
budget) have defaults per mode and hard ceilings; see the table in
[API.md](API.md#the-kernel). When the context is longer than the limits,
`over_limit` chooses what to keep:

- `HEAD`: the beginning (the legacy behavior; the default for most profiles);
- `HEAD_TAIL`: the beginning and the end (the default for logs);
- `SELECT`: the chunks most relevant to the question.

**Long contexts: the cut is now visible.** The `_opts` evaluators fill
`odk_result_meta` with `context_truncated` (1 when part of the context never
entered the evidence view) and `context_units_dropped` (bytes in ASCII mode,
code points in Unicode mode). Under the default `HEAD` strategy a decisive
sentence at the end of a 90 KB context was invisible to the default ASCII
configuration (an exact 50/50 tie) - the meta now reports it as 50,000 bytes
dropped. The sentence was found with `HEAD_TAIL`, with raised limits
(`context_max`, `chunk_source_max` and `max_chunks`), or in Unicode mode with
its larger defaults. The base (no-meta) evaluators still report nothing, and
the report never changes a decision. Compare your input length with the limits
before trusting a decision on long text.

## Input formats the library does not handle

It reads UTF-8 text in memory. It does not open files, decode PDF, Word, Excel
or HTML, convert other encodings (Latin-1, UTF-16, …) or decode images. Do
that in your application, then pass text (and, for images, raw RGB pixels).
