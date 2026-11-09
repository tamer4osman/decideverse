/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * src/odk_lang.h - language layer (internal API).
 *
 * Owns: the language definition table (odk_languages.def), the
 * language-aware normalization stages applied AFTER the baseline
 * NFC+case-fold pipeline (UNICODE mode only, language != UND), the
 * per-script sentence-ender sets, the ten stopword packs, the light
 * morphology rules and the deterministic language auto-detector.
 *
 * Compatibility anchor: ODK_LANG_UND must never trigger any of
 * this - odk_lang_* entry points are no-ops (or exact baseline
 * behavior) for UND.
 */
#ifndef ODK_LANG_H
#define ODK_LANG_H

#include "omnidecide.h"

#include <stddef.h>
#include <stdint.h>

/* --- table-generated metadata --------------------------------------- */

/* Count of table rows (excluding AUTO). */
unsigned odk_lang_count(void);

/* Row access by table index (0 = und). */
const char *odk_lang_code(unsigned idx);
const char *odk_lang_name(unsigned idx);
unsigned char odk_lang_script(unsigned idx);
int odk_lang_rtl(unsigned idx);
unsigned odk_lang_flags(unsigned idx);

/* Language code string ("en", "ur", "und", ...) <-> enum.
 * odk_lang_from_code returns ODK_LANG_UND and sets *ok = 0 for an
 * unknown code (ok may be NULL).  "auto" is NOT accepted here (it is
 * a request value, not a language); use odk_lang_resolve. */
odk_language odk_lang_from_code(const char *code, int *ok);
const char *odk_lang_to_code(odk_language lang);

/* Index of lang in the table (0 for UND, SIZE_MAX for invalid). */
unsigned odk_lang_index(odk_language lang);

/* Flag helpers (compile-time drops fold to 0 automatically). */
int odk_lang_has_pack(odk_language lang);
int odk_lang_arabic_fold(odk_language lang);
int odk_lang_zwj_join(odk_language lang);
int odk_lang_is_latin(odk_language lang);
int odk_lang_is_zh(odk_language lang);

/* --- normalization (code-point level, post NFC+fold) -----------------
 * Input: cps[0..count) already NFC + default-case-folded (the baseline
 * pipeline).  Applies, for lang != UND:
 *   generic: width fold (FF01-FF5E -> ASCII, 3000 -> space), NBSP/
 *            NNBSP -> space, bidi controls + soft hyphen strip,
 *            typographic apostrophes/quotes -> ASCII (Latin script)
 *   arabic-fold (ar/ur): tatweel/tashkeel strip, alef/yeh/kaf/heh
 *            unification per LANGUAGES.md policy, presentation
 *            forms -> base letters
 *   latin: French elision apostrophe normalization (U+2019 -> ')
 * Writes into *out (caller-allocated, >= count entries is always
 * enough; every rule replaces 1:1 except presentation-form ligatures
 * which expand 1->2).  Returns the output length, or (size_t)-1 when
 * out_cap is too small.  digit_folding is handled by the caller via
 * odk_lang_fold_digit below.
 */
size_t odk_lang_normalize(const uint32_t *cps, size_t count,
                          odk_language lang, uint32_t *out,
                          size_t out_cap);

/* Digit folding: if enabled for the pack (runtime flag), map every
 * Nd code point to its ASCII digit; else identity.  Works on a
 * single cp; returns the ASCII digit char or 0 when not foldable. */
uint32_t odk_lang_fold_digit(uint32_t cp, int digit_folding);

/* Whether the pack enables digit folding by default (all off
 * today, but the knob stays data-driven). */
int odk_lang_digit_folding_default(odk_language lang);

/* --- sentence enders (gap 1 extension) ---------------------
 * UND: exactly the baseline set (byte-identity anchor).  hi/bn add
 * danda U+0964/0965; ar adds U+061F/U+061B; ur adds U+06D4/U+061F/
 * U+061B; zh/Latin unchanged. */
int odk_lang_is_sentence_ender(uint32_t cp, odk_language lang);

/* --- stopword packs ----------------------------------------------------
 * Token-level binary search in the compiled-in pack for lang.
 * Tokens and pack entries both passed through the language pipeline
 * (caller normalizes; entries are pre-normalized at build time). */
int odk_lang_is_stop(odk_language lang, const char *token);

/* Pack size (for ping/summary and guards). */
size_t odk_lang_pack_size(odk_language lang);

/* --- light morphology (off by default) ------------------
 * Normalizes ONE token in place for matching.  Returns the new
 * length (<= len).  Deterministic, dictionary-free rules with
 * minimum-stem guards; a no-op for morph = off (callers gate). */
size_t odk_lang_morph_light(odk_language lang, char *token, size_t len);

/* --- deterministic auto-detector ---------------------------------------
 * Conservative: returns ODK_LANG_UND when unsure.  Fills lang_out
 * (concrete language or UND), confidence in [0,1] and a short signal
 * string when the pointers are non-NULL. */
odk_language odk_lang_detect(const char *text, double *confidence,
                              char *signal, size_t signal_cap);

/* Resolve a REQUESTED language (AUTO -> detect on this text) to the
 * concrete language used by the text layer. */
odk_language odk_lang_resolve(odk_language requested, const char *text);

/* --- runtime-loaded text data -----------------------
 * Owned by an odk_kernel (per-kernel state, no globals; the loaders
 * are odk_kernel_load_stopwords / odk_kernel_load_lexicon).  Every
 * member is owned by the struct; NULL/0 = not loaded.  Hard caps:
 * file <= 1 MB, <= 5,000 stopwords, <= 200 concepts, <= 5,000 zh
 * words, entry <= 64 bytes.  Strict parsing: hostile input fails. */
typedef struct odk_rt_text {
    char **stopwords;        /* sorted UTF-8 (strcmp order), owned   */
    size_t stopword_count;
    char **zh_words;         /* sorted UTF-8 word list for zh
                                lexicon-mode FMM, owned              */
    size_t zh_word_count;
    size_t zh_word_max_cps;  /* longest zh word in code points       */
    /* concepts: name -> flat word list (seed PINNED links via
     * odk_kernel_lexicon_seed_links; link_weight 0 keeps them
     * inactive until rewarded). */
    char **concept_names;    /* owned, count = concept_count         */
    size_t concept_count;
    char **concept_words;    /* owned flat list                      */
    size_t *concept_starts;  /* concept i owns words[starts[i]..     */
                              /* starts[i+1])                        */
    size_t concept_word_total;
} odk_rt_text;

/* Stopword lookup in a runtime list (binary search; token must be a
 * NUL-terminated UTF-8 string).  0 when rt is empty/NULL. */
int odk_lang_rt_is_stop(const odk_rt_text *rt, const char *token);

/* Load + parse a runtime STOPWORD file ({"stopwords":[...]}).
 * Returns a malloc'd odk_rt_text with the stopwords populated (or
 * NULL with a diagnostic in err).  Deterministic: sorted, deduped. */
odk_rt_text *odk_lang_rt_load_stopwords(const char *path, odk_error *err);

/* Load + parse a runtime LEXICON file ({"stopwords":[...],
 * "concepts":{name:[...]}, "words":[...]} - all sections optional,
 * at least one must be present).  Caps as per the struct comment;
 * duplicate concept names are collapsed (first wins); stopwords and
 * words are sorted + deduped. */
odk_rt_text *odk_lang_rt_load_lexicon(const char *path, odk_error *err);

/* Free every member + the struct (NULL-safe). */
void odk_lang_rt_free(odk_rt_text *rt);

/* Longest-match helper for zh lexicon mode: searches rt->zh_words
 * for a word equal to cps[0..n) encoded as UTF-8.  Returns 1 when
 * the exact word exists. */
int odk_lang_zh_word_match(const odk_rt_text *rt, const uint32_t *cps,
                           size_t n);

#endif /* ODK_LANG_H */
