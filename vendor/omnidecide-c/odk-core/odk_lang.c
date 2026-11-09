/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * src/odk_lang.c - language layer implementation.
 *
 * UND anchor: every entry point here must be an exact no-op /
 * baseline behavior when lang == ODK_LANG_UND.
 *
 * Data provenance: the presentation-form mapping and the stopword
 * packs live in src/odk_lang_data.c, GENERATED from the pinned,
 * checksum-verified Unicode Character Database 15.1.0 by
 * the project's table generator - never typed from memory.  Hand-written
 * ranges below cite their UCD sections in comments.
 */
#include "odk_lang.h"
#include "odk_unicode_internal.h"
#include "odk_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the single table-driven language definition */
#include "odk_languages.def"

/* --- generated data (odk_lang_data.c) -------------------------------- */
extern const uint32_t odk_lang_presform_keys[];
extern const uint32_t *const odk_lang_presform_vals[];
extern const uint8_t odk_lang_presform_len[];
extern const size_t odk_lang_presform_count;

#define ODK_LANG_PACK_DECL(code)                                          \
    extern const char *const odk_lang_stopwords_##code[];                 \
    extern const size_t odk_lang_stopwords_##code##_count;
ODK_LANG_PACK_DECL(en)
ODK_LANG_PACK_DECL(zh)
ODK_LANG_PACK_DECL(hi)
ODK_LANG_PACK_DECL(es)
ODK_LANG_PACK_DECL(ar)
ODK_LANG_PACK_DECL(fr)
ODK_LANG_PACK_DECL(bn)
ODK_LANG_PACK_DECL(pt)
ODK_LANG_PACK_DECL(id)
ODK_LANG_PACK_DECL(ur)

/* --- table-generated metadata ---------------------------------------- */

typedef struct {
    const char *code;
    const char *name;
    unsigned char script;
    int rtl;
    unsigned flags;
} odk_lang_row;

#define X_(code, suffix, name, script, rtl, flags)                         \
    { #code, name, script, rtl, flags },
static const odk_lang_row kLangTable[] = {
ODK_LANGUAGE_TABLE(X_)
};
#undef X_

#define ODK_LANG_N (sizeof(kLangTable) / sizeof(kLangTable[0]))

/* enum value == table index for UND..UR (asserted below at runtime in
 * tests; the header enum and this table are generated from the same
 * list).  AUTO and DEFAULT are request markers, not rows. */

unsigned odk_lang_count(void)
{
    return (unsigned)ODK_LANG_N;
}

const char *odk_lang_code(unsigned idx)
{
    return idx < ODK_LANG_N ? kLangTable[idx].code : "und";
}

const char *odk_lang_name(unsigned idx)
{
    return idx < ODK_LANG_N ? kLangTable[idx].name : "undetermined";
}

unsigned char odk_lang_script(unsigned idx)
{
    return idx < ODK_LANG_N ? kLangTable[idx].script : ODK_S_WORD;
}

int odk_lang_rtl(unsigned idx)
{
    return idx < ODK_LANG_N ? kLangTable[idx].rtl : 0;
}

unsigned odk_lang_flags(unsigned idx)
{
    return idx < ODK_LANG_N ? kLangTable[idx].flags : 0u;
}

odk_language odk_lang_from_code(const char *code, int *ok)
{
    unsigned i;
    if (ok != NULL) {
        *ok = 0;
    }
    if (code == NULL) {
        return ODK_LANG_UND;
    }
    if (strcmp(code, "auto") == 0) {
        if (ok != NULL) {
            *ok = 1;
        }
        return ODK_LANG_AUTO;
    }
    if (strcmp(code, "default") == 0) {
        if (ok != NULL) {
            *ok = 1;
        }
        return ODK_LANG_DEFAULT;
    }
    for (i = 0; i < ODK_LANG_N; i++) {
        if (strcmp(code, kLangTable[i].code) == 0) {
            if (ok != NULL) {
                *ok = 1;
            }
            return (odk_language)i;
        }
    }
    return ODK_LANG_UND;
}

const char *odk_lang_to_code(odk_language lang)
{
    if ((unsigned)lang < ODK_LANG_N) {
        return kLangTable[lang].code;
    }
    if (lang == ODK_LANG_AUTO) {
        return "auto";
    }
    return "default";
}

const char *odk_lang_display_name(odk_language lang)
{
    if ((unsigned)lang < ODK_LANG_N) {
        return kLangTable[lang].name;
    }
    if (lang == ODK_LANG_AUTO) {
        return "auto-detect";
    }
    return "default";
}

unsigned odk_lang_index(odk_language lang)
{
    if ((unsigned)lang < ODK_LANG_N) {
        return (unsigned)lang;
    }
    if (lang == ODK_LANG_AUTO || lang == ODK_LANG_DEFAULT) {
        return 0; /* request markers resolve like UND for lookups */
    }
    return (unsigned)-1; /* SIZE_MAX */
}

/* --- flag helpers ----------------------------------------------------- */

static unsigned lang_flags_of(odk_language lang)
{
    unsigned idx = odk_lang_index(lang);
    return idx == (unsigned)-1 ? 0u : kLangTable[idx].flags;
}

int odk_lang_has_pack(odk_language lang)
{
    return (lang_flags_of(lang) & LF_HAVE_PACK) != 0;
}

int odk_lang_arabic_fold(odk_language lang)
{
    return (lang_flags_of(lang) & LF_ARABIC_FOLD) != 0;
}

int odk_lang_zwj_join(odk_language lang)
{
#if defined(ODK_ABLATE_NO_LANG_ZWJ)
    /* dev-only ablation toggle (build flag, never a
     * shipped default): joiner stripping off - the baseline matching
     * behavior.  Every strip_joiners consumer derives from this
     * gate, so the whole ZWJ/ZWNJ channel switches at once. */
    (void)lang;
    return 0;
#else
    return (lang_flags_of(lang) & LF_ZWJ_JOIN) != 0;
#endif
}

int odk_lang_is_latin(odk_language lang)
{
    return (lang_flags_of(lang) & LF_LATIN) != 0;
}

int odk_lang_is_zh(odk_language lang)
{
    return (lang_flags_of(lang) & LF_ZH) != 0;
}

/* --- helpers ----------------------------------------------------------- */

static int cp_is_ascii_digit(uint32_t cp)
{
    return cp >= 0x30u && cp <= 0x39u;
}

/* Arabic-Indic 0660-0669, Extended 06F0-06F9, Devanagari 0966-096F,
 * Bengali 09E6-09EF, fullwidth FF10-FF19 (all Nd per UCD). */
static uint32_t arabic_indic_digit_value(uint32_t cp)
{
    if (cp >= 0x0660u && cp <= 0x0669u) {
        return 0x30u + (cp - 0x0660u);
    }
    if (cp >= 0x06F0u && cp <= 0x06F9u) {
        return 0x30u + (cp - 0x06F0u);
    }
    if (cp >= 0x0966u && cp <= 0x096Fu) {
        return 0x30u + (cp - 0x0966u);
    }
    if (cp >= 0x09E6u && cp <= 0x09EFu) {
        return 0x30u + (cp - 0x09E6u);
    }
    if (cp >= 0xFF10u && cp <= 0xFF19u) {
        return 0x30u + (cp - 0xFF10u);
    }
    return 0;
}

/* --- normalization ----------------------------------------------------- */

/* Binary search in the generated presentation-form table.  Returns
 * the index or (size_t)-1. */
static size_t presform_find(uint32_t cp)
{
    size_t lo = 0;
    size_t hi = odk_lang_presform_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (cp < odk_lang_presform_keys[mid]) {
            hi = mid;
        } else if (cp > odk_lang_presform_keys[mid]) {
            lo = mid + 1;
        } else {
            return mid;
        }
    }
    return (size_t)-1;
}

/* Arabic/Urdu letter folding (matching form).  Policies per
 * docs/LANGUAGES.md; range facts from the pinned UCD. */
static uint32_t arabic_fold_cp(uint32_t cp, odk_language lang)
{
    /* tatweel U+0640 (Lm) - stripped for matching */
    if (cp == 0x0640u) {
        return 0; /* 0 = drop */
    }
    /* tashkeel: combining marks U+064B-065F, superscript alef U+0670,
     * and Quranic annotation marks U+08D3-08E1 / U+08E2-08FF - all
     * stripped for matching */
    if ((cp >= 0x064Bu && cp <= 0x065Fu) || cp == 0x0670u ||
        (cp >= 0x08D3u && cp <= 0x08FFu)) {
        return 0; /* drop */
    }
    /* alef variants -> bare alef */
    if (cp == 0x0622u || cp == 0x0623u || cp == 0x0625u || cp == 0x0671u) {
        return 0x0627u;
    }
    if (lang == ODK_LANG_AR) {
        /* yeh variants unified for Arabic: alef maksura + yeh */
        if (cp == 0x0649u || cp == 0x064Au) {
            return 0x064Au;
        }
        /* heh variants unified for Arabic */
        if (cp == 0x06C1u || cp == 0x06BEu) {
            return 0x0647u;
        }
    } else if (lang == ODK_LANG_UR) {
        /* Urdu: Farsi yeh is the canonical yeh; alef maksura and the
         * dotted yeh map TO it for matching.  Baari yeh U+06D2 stays
         * distinct (word-final heuristic marker). */
        if (cp == 0x0649u || cp == 0x064Au) {
            return 0x06CCu;
        }
        /* kaf: Urdu keheh is canonical */
        if (cp == 0x0643u) {
            return 0x06A9u;
        }
        /* heh: gol heh U+06C1 is canonical for Urdu; do-chashmi heh
         * U+06BE stays DISTINCT (meaning-bearing in Urdu spelling);
         * Arabic heh maps to gol heh. */
        if (cp == 0x0647u) {
            return 0x06C1u;
        }
    }
    return cp; /* identity */
}

/* Generic width folding: U+FF01-FF5E -> ASCII (cp - 0xFEE0).  The
 * fullwidth range FF00/FF5F is excluded (no ASCII counterpart). */
static uint32_t width_fold_cp(uint32_t cp)
{
    if (cp >= 0xFF01u && cp <= 0xFF5Eu) {
        return cp - 0xFEE0u;
    }
    return cp;
}

/* Bidi controls + soft hyphen, stripped in the generic layer. */
static int is_bidi_or_shy(uint32_t cp)
{
    switch (cp) {
    case 0x00ADu: /* soft hyphen */
    case 0x061Cu: /* arabic letter mark */
    case 0x200Eu: /* LRM */
    case 0x200Fu: /* RLM */
    case 0x202Au: case 0x202Bu: case 0x202Cu: case 0x202Du: case 0x202Eu:
    case 0x2066u: case 0x2067u: case 0x2068u: case 0x2069u:
        return 1;
    default:
        return 0;
    }
}

/* Zh variants folding: OPT-IN compile flag
 * -DODK_ZH_VARIANTS=1 (default OFF = baseline token identity).  The
 * table is authored + unreviewed (generated by the project's zh-variants generator). */
#if defined(ODK_NO_ZH_VARIANTS)
#undef ODK_ZH_VARIANTS
#define ODK_ZH_VARIANTS 0
#elif !defined(ODK_ZH_VARIANTS)
#define ODK_ZH_VARIANTS 0
#endif

#if ODK_ZH_VARIANTS
extern const uint32_t odk_zh_variant_keys[];
extern const uint32_t odk_zh_variant_vals[];
extern const size_t odk_zh_variant_count;

static uint32_t zh_variant_fold(uint32_t cp)
{
    size_t lo = 0;
    size_t hi = odk_zh_variant_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (odk_zh_variant_keys[mid] == cp) {
            return odk_zh_variant_vals[mid];
        }
        if (odk_zh_variant_keys[mid] < cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return cp;
}
#endif

size_t odk_lang_normalize(const uint32_t *cps, size_t count,
                          odk_language lang, uint32_t *out,
                          size_t out_cap)
{
    size_t i;
    size_t n = 0;
    const int arabic = odk_lang_arabic_fold(lang);

#if defined(ODK_ABLATE_NO_LANG_NORM)
    /* dev-only ablation toggle: per-language
     * normalization off - the UND identity-copy path (baseline
     * behavior for the normalization channel). */
    (void)arabic;
    lang = ODK_LANG_UND;
#endif
    if (lang == ODK_LANG_UND || lang == ODK_LANG_AUTO ||
        lang == ODK_LANG_DEFAULT) {
        /* UND anchor: identity copy (callers keep baseline behavior). */
        if (count > out_cap) {
            return (size_t)-1;
        }
        memcpy(out, cps, count * sizeof(uint32_t));
        return count;
    }

    for (i = 0; i < count; i++) {
        uint32_t cp = cps[i];
        size_t pf;

        /* generic layer first: bidi/SHY strip */
        if (is_bidi_or_shy(cp)) {
            continue;
        }
        /* spaces: NBSP/NNBSP/ideographic -> ASCII space */
        if (cp == 0x00A0u || cp == 0x202Fu || cp == 0x3000u) {
            cp = 0x20u;
        }
        /* width folding (letters, digits, punctuation) */
        cp = width_fold_cp(cp);
        /* fullwidth digits handled above; other digit systems only
         * with digit_folding (caller applies odk_lang_fold_digit) */

#if ODK_ZH_VARIANTS
        /* zh variants folding (traditional -> simplified) for
         * MATCHING; opt-in compile flag, LANGUAGES.md policy */
        if (lang == ODK_LANG_ZH) {
            cp = zh_variant_fold(cp);
        }
#endif

        if (arabic) {
            uint32_t folded = arabic_fold_cp(cp, lang);
            if (folded == 0u) {
                continue; /* tatweel/tashkeel: dropped */
            }
            cp = folded;
            /* presentation forms -> base letters (generated table,
             * may expand 1 -> 2 for lam-alef ligatures) */
            pf = presform_find(cp);
            if (pf != (size_t)-1) {
                size_t k;
                uint32_t m;
                for (k = 0; k < odk_lang_presform_len[pf]; k++) {
                    uint32_t v = odk_lang_presform_vals[pf][k];
                    /* fold the expanded letters too (e.g. FEFB ->
                     * 0644 0627; alef variants already folded) */
                    v = arabic_fold_cp(v, lang);
                    if (v == 0u) {
                        continue;
                    }
                    if (n == out_cap) {
                        return (size_t)-1;
                    }
                    out[n++] = v;
                }
                (void)m;
                continue;
            }
        } else {
            /* Latin-script layer: typographic apostrophes/quotes ->
             * ASCII (CJK quotes untouched - they are not in these
             * ranges). */
            switch (cp) {
            case 0x2018u: /* left single quote */
            case 0x2019u: /* right single quote */
            case 0x201Bu: /* single high-reversed-9 */
            case 0x2032u: /* prime */
                cp = 0x27u; /* ' */
                break;
            case 0x201Cu: /* left double quote */
            case 0x201Du: /* right double quote */
            case 0x2033u: /* double prime */
                cp = 0x22u; /* " */
                break;
            default:
                break;
            }
            /* French ligature folding (LANGUAGES.md Latin table:
             * oe/ae -> two ASCII letters; fr policy only).  The
             * uppercase code points normally never reach this point
             * (stage-2 default case folding lowercases them); they
             * are handled anyway so the function is idempotent and
             * order-independent.  1 -> 2 expansion, like presforms. */
            if (lang == ODK_LANG_FR) {
                uint32_t l0 = 0, l1 = 0;
                if (cp == 0x0153u || cp == 0x0152u) { /* oe ligature */
                    l0 = 0x6Fu; /* o */
                    l1 = 0x65u; /* e */
                } else if (cp == 0x00E6u || cp == 0x00C6u) { /* ae */
                    l0 = 0x61u; /* a */
                    l1 = 0x65u; /* e */
                }
                if (l1 != 0u) {
                    if (n + 1 >= out_cap) {
                        return (size_t)-1;
                    }
                    out[n++] = l0;
                    out[n++] = l1;
                    continue;
                }
            }
        }

        if (n == out_cap) {
            return (size_t)-1;
        }
        out[n++] = cp;
    }
    return n;
}

uint32_t odk_lang_fold_digit(uint32_t cp, int digit_folding)
{
    uint32_t d;
    if (!digit_folding || cp_is_ascii_digit(cp)) {
        return cp;
    }
    d = arabic_indic_digit_value(cp);
    return d != 0 ? d : cp;
}

int odk_lang_digit_folding_default(odk_language lang)
{
    /* off for every pack (conservative; measured impact was
     * inside the Wilson interval).  The knob stays data-driven via
     * this single function. */
#if defined(ODK_ABLATE_FORCE_DIGIT)
    /* dev-only ablation toggle: digit folding ON for
     * every language (measures the "digit folding only" pack
     * component). */
    (void)lang;
    return 1;
#else
    (void)lang;
    return 0;
#endif
}

/* --- sentence enders ----------------------------------------- */

int odk_lang_is_sentence_ender(uint32_t cp, odk_language lang)
{
    /* Set (UNICODE chunker) - the UND anchor */
    switch (cp) {
    case 0x2Eu:   /* . */
    case 0x21u:   /* ! */
    case 0x3Fu:   /* ? */
    case 0x3002u: /* 。 */
    case 0xFF01u: /* ！ */
    case 0xFF1Fu: /* ？ */
    case 0x2026u: /* … */
    case 0xFF1Bu: /* ； */
        return 1;
    default:
        break;
    }
#if defined(ODK_ABLATE_NO_LANG_SENT)
    /* dev-only ablation toggle: language sentence
     * enders off; only the base set above splits. */
    (void)lang;
    return 0;
#else
    /* language-specific extensions */
    switch (lang) {
    case ODK_LANG_HI:
    case ODK_LANG_BN:
#if defined(ODK_ABLATE_NO_DANDA)
        /* dev-only: danda/double-danda enders off, the
         * remaining hi/bn extensions (if any) stay. */
        return 0;
#else
        /* danda / double danda */
        return cp == 0x0964u || cp == 0x0965u;
#endif
    case ODK_LANG_AR:
        /* Arabic question mark + Arabic semicolon (Arabic full stop
         * is the ASCII '.', already handled) */
        return cp == 0x061Fu || cp == 0x061Bu;
    case ODK_LANG_UR:
        /* Urdu full stop + Arabic question mark + semicolon */
        return cp == 0x06D4u || cp == 0x061Fu || cp == 0x061Bu;
    default:
        return 0;
    }
#endif /* !ODK_ABLATE_NO_LANG_SENT */
}

/* --- stopword packs ---------------------------------------------------- */

static const char *const *pack_array(odk_language lang, size_t *count)
{
    *count = 0;
    switch (lang) {
    case ODK_LANG_EN:
#ifndef ODK_NO_LANG_EN
        *count = odk_lang_stopwords_en_count;
        return odk_lang_stopwords_en;
#endif
        return NULL;
    case ODK_LANG_ZH:
#ifndef ODK_NO_LANG_ZH
        *count = odk_lang_stopwords_zh_count;
        return odk_lang_stopwords_zh;
#endif
        return NULL;
    case ODK_LANG_HI:
#ifndef ODK_NO_LANG_HI
        *count = odk_lang_stopwords_hi_count;
        return odk_lang_stopwords_hi;
#endif
        return NULL;
    case ODK_LANG_ES:
#ifndef ODK_NO_LANG_ES
        *count = odk_lang_stopwords_es_count;
        return odk_lang_stopwords_es;
#endif
        return NULL;
    case ODK_LANG_AR:
#ifndef ODK_NO_LANG_AR
        *count = odk_lang_stopwords_ar_count;
        return odk_lang_stopwords_ar;
#endif
        return NULL;
    case ODK_LANG_FR:
#ifndef ODK_NO_LANG_FR
        *count = odk_lang_stopwords_fr_count;
        return odk_lang_stopwords_fr;
#endif
        return NULL;
    case ODK_LANG_BN:
#ifndef ODK_NO_LANG_BN
        *count = odk_lang_stopwords_bn_count;
        return odk_lang_stopwords_bn;
#endif
        return NULL;
    case ODK_LANG_PT:
#ifndef ODK_NO_LANG_PT
        *count = odk_lang_stopwords_pt_count;
        return odk_lang_stopwords_pt;
#endif
        return NULL;
    case ODK_LANG_ID:
#ifndef ODK_NO_LANG_ID
        *count = odk_lang_stopwords_id_count;
        return odk_lang_stopwords_id;
#endif
        return NULL;
    case ODK_LANG_UR:
#ifndef ODK_NO_LANG_UR
        *count = odk_lang_stopwords_ur_count;
        return odk_lang_stopwords_ur;
#endif
        return NULL;
    default:
        return NULL;
    }
}

int odk_lang_is_stop(odk_language lang, const char *token)
{
    const char *const *words;
    size_t count;
    size_t lo;
    size_t hi;

    words = pack_array(lang, &count);
    if (words == NULL || token == NULL) {
        return 0;
    }
    lo = 0;
    hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(token, words[mid]);
        if (cmp == 0) {
            return 1;
        }
        if (cmp < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return 0;
}

size_t odk_lang_pack_size(odk_language lang)
{
    size_t count;
    (void)pack_array(lang, &count);
    return count;
}

/* --- light morphology (off by default) --------------------
 * Operates on UTF-8 token bytes; conservative suffix/proclitic
 * stripping with minimum-stem guards.  Rules authored for this
 * project; over-stemming counter-examples live in the vectors. */

static size_t utf8_len_of(uint32_t cp, char *buf)
{
    if (cp < 0x80u) {
        buf[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        buf[0] = (char)(0xC0u | (cp >> 6));
        buf[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        buf[0] = (char)(0xE0u | (cp >> 12));
        buf[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        buf[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    buf[0] = (char)(0xF0u | (cp >> 18));
    buf[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
    buf[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
    buf[3] = (char)(0x80u | (cp & 0x3Fu));
    return 4;
}

/* does the token end with the given UTF-8 suffix? */
static int ends_with(const char *tok, size_t len, const char *suffix,
                     size_t suflen)
{
    return len >= suflen && memcmp(tok + len - suflen, suffix, suflen) == 0;
}

static size_t strip_if_stem_ok(char *tok, size_t len, size_t suflen,
                               size_t min_stem)
{
    (void)tok; /* in-place caller shortens after we return the length */
    if (len - suflen >= min_stem && len - suflen >= 2) {
        return len - suflen;
    }
    return len;
}

size_t odk_lang_morph_light(odk_language lang, char *token, size_t len)
{
    if (token == NULL || len < 4) {
        return len; /* never touch short words (< 4 letters/bytes) */
    }

    switch (lang) {
    case ODK_LANG_EN:
        /* English regular plurals: -es only after s/x/z/h (boxes,
         * churches), plain -s otherwise (services -> service). */
        if (ends_with(token, len, "es", 2)) {
            char prev = token[len - 3];
            if ((prev == 's' || prev == 'x' || prev == 'z' ||
                 prev == 'h') && len - 2 >= 3) {
                return len - 2;
            }
            if (len - 1 >= 3) {
                return len - 1;
            }
            return len;
        }
        if (ends_with(token, len, "s", 1) && len - 1 >= 3) {
            return len - 1;
        }
        return len;
    case ODK_LANG_ES:
    case ODK_LANG_PT:
        /* consonant-final plurals take -es (errores -> error) with a
         * 4-letter stem guard (lunes stays); vowel-final plurals take
         * -s (servicios -> servicio); gender -a with a 3-letter stem
         * guard (fallida -> fallid, fila -> fil).  One affix per call. */
        if (ends_with(token, len, "es", 2)) {
            return strip_if_stem_ok(token, len, 2, 4);
        }
        if (ends_with(token, len, "s", 1)) {
            return strip_if_stem_ok(token, len, 1, 3);
        }
        if (ends_with(token, len, "a", 1)) {
            return strip_if_stem_ok(token, len, 1, 3);
        }
        return len;
    case ODK_LANG_FR:
        /* French: plural -s only (services -> service, never -es:
         * "service" ends in -e); mute -e with a 4-letter stem guard
         * (latence -> latenc).  One affix per call. */
        if (ends_with(token, len, "s", 1)) {
            return strip_if_stem_ok(token, len, 1, 4);
        }
        if (ends_with(token, len, "e", 1)) {
            return strip_if_stem_ok(token, len, 1, 4);
        }
        return len;
    case ODK_LANG_HI:
    case ODK_LANG_BN: {
        /* inflectional suffixes (UTF-8), stem >= 2 letters (>= 6
         * bytes in these scripts).  Hindi oblique plural -o.n
         * (e0 a5 8b e0 a4 82) and direct plural -e.candrabindu
         * (e0 a4 8f e0 a4 81); Bengali genitive plural -der
         * (e0 a6 a6 e0 a7 87 e0 a6 b0).  One affix per call. */
        static const char hi_plural[] = "\xe0\xa5\x8b\xe0\xa4\x82"; /* o+n */
        static const char hi_plural_e[] = "\xe0\xa4\x8f\xe0\xa4\x81";
        static const char bn_genitive[] = "\xe0\xa6\xa6\xe0\xa7\x87"
                                          "\xe0\xa6\xb0";         /* der */
        if (lang == ODK_LANG_HI) {
            if (ends_with(token, len, hi_plural, sizeof(hi_plural) - 1)) {
                return strip_if_stem_ok(token, len,
                                        sizeof(hi_plural) - 1, 6);
            }
            if (ends_with(token, len, hi_plural_e,
                          sizeof(hi_plural_e) - 1)) {
                return strip_if_stem_ok(token, len,
                                        sizeof(hi_plural_e) - 1, 6);
            }
            return len;
        }
        if (ends_with(token, len, bn_genitive, sizeof(bn_genitive) - 1)) {
            return strip_if_stem_ok(token, len, sizeof(bn_genitive) - 1, 6);
        }
        return len;
    }
    case ODK_LANG_ID: {
        /* Indonesian light affix handling, one affix per call:
         * 1. -lah / -kah (teruslah -> terus, apakah -> apa);
         * 2. -kan only when the word does NOT start with a known
         *    prefix (bayarkan -> bayar; kerusakan/dilakukan fall
         *    through to prefix stripping instead);
         * 3. prefixes me-/ber-/di-/ke-/ter- with a 3-letter stem
         *    guard (memulai -> mulai, berjalan -> jalan, dilakukan ->
         *    akukan, kerusakan -> rusakan, terjadi -> jadi).
         * pe- and -nya/-i are deliberately NOT handled (over-stemming
         * risk on authored data; pembayaran/antreannya stay). */
        static const char *const prefixes[] = {"me", "ber", "di", "ke",
                                               "ter"};
        static const size_t prefix_lens[] = {2, 3, 2, 2, 3};
        size_t np = sizeof(prefixes) / sizeof(prefixes[0]);
        size_t p;
        int starts_prefix = 0;

        for (p = 0; p < np; p++) {
            if (len > prefix_lens[p] &&
                memcmp(token, prefixes[p], prefix_lens[p]) == 0) {
                starts_prefix = 1;
                break;
            }
        }
        if (ends_with(token, len, "lah", 3) || ends_with(token, len, "kah", 3)) {
            return strip_if_stem_ok(token, len, 3, 3);
        }
        if (!starts_prefix && ends_with(token, len, "kan", 3)) {
            return strip_if_stem_ok(token, len, 3, 3);
        }
        if (starts_prefix) {
            if (len - prefix_lens[p] >= 3) {
                memmove(token, token + prefix_lens[p],
                        len - prefix_lens[p]);
                return len - prefix_lens[p];
            }
        }
        return len;
    }
    case ODK_LANG_AR:
    case ODK_LANG_UR: {
        /* definite article / proclitics: al- (d8 a7 d9 84, 4 bytes),
         * w- (d9 88), b- (d8 a8), l- (d9 84), k- (d9 83), f- (d9 81),
         * each 2 bytes.  Strip only when the stem stays >= 3 LETTERS
         * = >= 6 bytes of Arabic script (ash-shabaka -> shabaka;
         * wamh stays).  One affix per call. */
        static const char al[] = "\xd8\xa7\xd9\x84"; /* al- */
        static const char waw[] = "\xd9\x88";         /* w- */
        static const char b[] = "\xd8\xa8";           /* b- */
        static const char lam[] = "\xd9\x84";         /* l- */
        static const char kaf[] = "\xd9\x83";         /* k- */
        static const char fa[] = "\xd9\x81";         /* f- */
        size_t suflen = 0;

        if (len > 4 && memcmp(token, al, 4) == 0 &&
            !ends_with(token, len, al, 4)) {
            suflen = 4;
        } else if (len > 2 && memcmp(token, waw, 2) == 0) {
            suflen = 2;
        } else if (len > 2 && memcmp(token, b, 2) == 0) {
            suflen = 2;
        } else if (len > 2 && memcmp(token, lam, 2) == 0) {
            suflen = 2;
        } else if (len > 2 && memcmp(token, kaf, 2) == 0) {
            suflen = 2;
        } else if (len > 2 && memcmp(token, fa, 2) == 0) {
            suflen = 2;
        }
        if (suflen != 0 && len - suflen >= 6) {
            memmove(token, token + suflen, len - suflen);
            return len - suflen;
        }
        return len;
    }
    default:
        return len;
    }
}

/* --- deterministic auto-detector --------------------------------------- */

odk_language odk_lang_detect(const char *text, double *confidence,
                             char *signal, size_t signal_cap)
{
    /* Script histogram over letters.  Deterministic, conservative:
     * - dominant non-Latin script with >= 30% of letters decides
     *   (Han->zh, Devanagari->hi, Bengali->bn, Arabic block with
     *   Urdu-specific letters (U+06CC/06D2/06BE/06C1/06D4/06C3 present
     *   or Arabic otherwise) -> ur / ar);
     * - Latin: stopword hit-rate ranking over en/es/fr/pt/id
     *   (>= 20% hit rate and a 1.2x margin over the runner-up);
     * - else UND.
     * RTL/bidi marks bias ar/ur. */
    uint32_t *cps = NULL;
    size_t count = 0;
    size_t i;
    size_t script_counts[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
    size_t letters = 0;
    size_t words_total = 0;
    size_t hits[5] = {0, 0, 0, 0, 0};
    odk_language out = ODK_LANG_UND;
    double conf = 0.0;
    char sig[64] = "no signal";

    if (text == NULL || text[0] == '\0') {
        goto done;
    }
    cps = odk_u_decode_all(text, strlen(text), &count);
    if (cps == NULL) {
        goto done;
    }
    for (i = 0; i < count; i++) {
        unsigned char sc = odk_u_script(cps[i]);
        if ((odk_u_class(cps[i]) & ODK_U_LETTER) != 0) {
            letters++;
            if (sc <= ODK_S_MYAN) {
                script_counts[sc]++;
            }
            if (cps[i] == 0x06CCu || cps[i] == 0x06D2u || cps[i] == 0x06BEu ||
                cps[i] == 0x06C1u || cps[i] == 0x06C3u || cps[i] == 0x06D4u) {
                script_counts[8]++; /* urdu-specific marker */
            }
        }
    }
    if (letters >= 8) {
        /* non-Latin scripts first */
        if (script_counts[ODK_S_HAN] * 3 >= letters) {
            out = ODK_LANG_ZH;
            conf = (double)script_counts[ODK_S_HAN] / (double)letters;
            snprintf(sig, sizeof(sig), "han %.0f%%", conf * 100.0);
            goto done;
        }
        if (script_counts[8] >= 2) {
            out = ODK_LANG_UR;
            conf = 0.7;
            snprintf(sig, sizeof(sig), "urdu letters %u",
                     (unsigned)script_counts[8]);
            goto done;
        }
        /* Devanagari/Bengali/Arabic vs Latin word-script histogram:
         * the unicode layer's ODK_S_WORD lumps them; use ranges. */
        {
            size_t dev = 0, ben = 0, ar = 0;
            for (i = 0; i < count; i++) {
                uint32_t cp = cps[i];
                if ((odk_u_class(cp) & ODK_U_LETTER) == 0) {
                    continue;
                }
                if (cp >= 0x0900u && cp <= 0x097Fu) {
                    dev++;
                } else if (cp >= 0x0980u && cp <= 0x09FFu) {
                    ben++;
                } else if (cp >= 0x0600u && cp <= 0x06FFu) {
                    ar++;
                }
            }
            if (dev * 3 >= letters) {
                out = ODK_LANG_HI;
                conf = (double)dev / (double)letters;
                snprintf(sig, sizeof(sig), "devanagari %.0f%%", conf * 100.0);
                goto done;
            }
            if (ben * 3 >= letters) {
                out = ODK_LANG_BN;
                conf = (double)ben / (double)letters;
                snprintf(sig, sizeof(sig), "bengali %.0f%%", conf * 100.0);
                goto done;
            }
            if (ar * 3 >= letters) {
                out = ODK_LANG_AR;
                conf = (double)ar / (double)letters;
                snprintf(sig, sizeof(sig), "arabic %.0f%%", conf * 100.0);
                goto done;
            }
        }
    }

    /* Latin-script ranking by stopword hit rate */
    {
        /* reuse the tokenizer for word boundaries: letters run */
        size_t word_start = (size_t)-1;
        char wbuf[64];
        size_t wlen = 0;
        for (i = 0; i <= count; i++) {
            int is_letter =
                i < count && (odk_u_class(cps[i]) & ODK_U_LETTER) != 0;
            if (is_letter && wlen + 4 < sizeof(wbuf)) {
                if (word_start == (size_t)-1) {
                    word_start = i;
                }
                wlen += utf8_len_of(cps[i], wbuf + wlen);
            } else {
                if (wlen > 0) {
                    size_t li;
                    wbuf[wlen] = '\0';
                    words_total++;
                    /* lowercase ASCII in place for pack compare */
                    for (li = 0; li < wlen; li++) {
                        if (wbuf[li] >= 'A' && wbuf[li] <= 'Z') {
                            wbuf[li] = (char)(wbuf[li] - 'A' + 'a');
                        }
                    }
                    if (odk_lang_is_stop(ODK_LANG_EN, wbuf)) {
                        hits[0]++;
                    }
                    if (odk_lang_is_stop(ODK_LANG_ES, wbuf)) {
                        hits[1]++;
                    }
                    if (odk_lang_is_stop(ODK_LANG_FR, wbuf)) {
                        hits[2]++;
                    }
                    if (odk_lang_is_stop(ODK_LANG_PT, wbuf)) {
                        hits[3]++;
                    }
                    if (odk_lang_is_stop(ODK_LANG_ID, wbuf)) {
                        hits[4]++;
                    }
                }
                word_start = (size_t)-1;
                wlen = 0;
            }
        }
        if (words_total >= 6) {
            static const odk_language langs[5] = {
                ODK_LANG_EN, ODK_LANG_ES, ODK_LANG_FR, ODK_LANG_PT,
                ODK_LANG_ID
            };
            size_t best = 0;
            size_t second = 0;
            int best_i = -1;
            int k;
            for (k = 0; k < 5; k++) {
                if (hits[k] > best) {
                    second = best;
                    best = hits[k];
                    best_i = k;
                } else if (hits[k] > second) {
                    second = hits[k];
                }
            }
            if (best_i >= 0 && best * 5 >= words_total &&
                (second == 0 || best * 5 >= second * 6)) {
                out = langs[best_i];
                conf = (double)best / (double)words_total;
                snprintf(sig, sizeof(sig), "stopwords %u/%u %s",
                         (unsigned)best, (unsigned)words_total,
                         odk_lang_to_code(out));
            }
        }
    }

done:
    free(cps);
    if (confidence != NULL) {
        *confidence = conf;
    }
    if (signal != NULL && signal_cap > 0) {
        snprintf(signal, signal_cap, "%s", sig);
    }
    return out;
}

odk_language odk_lang_detect_text(const char *text, double *confidence,
                                   char *signal_buf, size_t signal_cap)
{
    return odk_lang_detect(text, confidence, signal_buf, signal_cap);
}

odk_language odk_lang_resolve(odk_language requested, const char *text)
{
    if (requested == ODK_LANG_AUTO) {
        return odk_lang_detect(text, NULL, NULL, 0);
    }
    if (requested == ODK_LANG_DEFAULT) {
        return ODK_LANG_UND;
    }
    return requested;
}

/* ------------------------------------------------------------------ */
/* Runtime-loaded text data                          */
/*                                                                    */
/* Strict, hostile-input-safe loaders shared by                       */
/* odk_kernel_load_stopwords / odk_kernel_load_lexicon.  Caps: file    */
/* <= 1 MB; <= 5,000 stopwords; <= 200 concepts; <= 5,000 zh words;   */
/* entry <= 64 bytes (zh words additionally >= 2 code points).        */
/* Deterministic output: sorted (strcmp) + deduped lists.             */
/* ------------------------------------------------------------------ */

#define ODK_RT_FILE_MAX   (1024u * 1024u) /* 1 MB                    */
#define ODK_RT_ENTRY_MAX  64u            /* bytes per string entry  */
#define ODK_RT_STOP_MAX   5000u
#define ODK_RT_CONCEPT_MAX 200u
#define ODK_RT_WORDS_MAX  5000u
#define ODK_RT_CWORDS_MAX 64u            /* words per concept       */

int odk_lang_rt_is_stop(const odk_rt_text *rt, const char *token)
{
    size_t lo;
    size_t hi;

    if (rt == NULL || rt->stopwords == NULL || token == NULL) {
        return 0;
    }
    lo = 0;
    hi = rt->stopword_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(token, rt->stopwords[mid]);
        if (cmp == 0) {
            return 1;
        }
        if (cmp < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return 0;
}

/* local strict UTF-8 encoder (mirrors odk_unicode.c cp_encode;
 * kept local so the lang layer stays independent of statics) */
static int rt_encode_cp(uint32_t cp, char out[4])
{
    if (cp < 0x80u) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800u) {
        out[0] = (char)(0xC0u | (cp >> 6));
        out[1] = (char)(0x80u | (cp & 0x3Fu));
        return 2;
    }
    if (cp < 0x10000u) {
        out[0] = (char)(0xE0u | (cp >> 12));
        out[1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[2] = (char)(0x80u | (cp & 0x3Fu));
        return 3;
    }
    if (cp <= 0x10FFFFu) {
        out[0] = (char)(0xF0u | (cp >> 18));
        out[1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        out[2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[3] = (char)(0x80u | (cp & 0x3Fu));
        return 4;
    }
    return 0;
}

int odk_lang_zh_word_match(const odk_rt_text *rt, const uint32_t *cps,
                           size_t n)
{
    char buf[ODK_RT_ENTRY_MAX + 8];
    size_t off = 0;
    size_t i;
    size_t lo;
    size_t hi;

    if (rt == NULL || rt->zh_words == NULL || cps == NULL || n == 0) {
        return 0;
    }
    for (i = 0; i < n; i++) {
        char tmp[4];
        int cl = rt_encode_cp(cps[i], tmp);
        if (cl <= 0 || off + (size_t)cl >= sizeof(buf)) {
            return 0; /* word longer than the entry cap cannot exist */
        }
        memcpy(buf + off, tmp, (size_t)cl);
        off += (size_t)cl;
    }
    buf[off] = '\0';
    lo = 0;
    hi = rt->zh_word_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(buf, rt->zh_words[mid]);
        if (cmp == 0) {
            return 1;
        }
        if (cmp < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return 0;
}

static int rt_cmp_str(const void *a, const void *b)
{
    const char *const *pa = a;
    const char *const *pb = b;
    return strcmp(*pa, *pb);
}

/* strdup-or-fail helper that also validates: non-NULL, non-empty,
 * <= cap bytes, no control bytes (all c >= 0x20; the JSON layer
 * cannot produce NUL inside a C string, but be strict anyway). */
static char *rt_dup_entry(const char *s, size_t cap)
{
    size_t len;
    size_t i;
    char *out;

    if (s == NULL) {
        return NULL;
    }
    len = strlen(s);
    if (len == 0 || len > cap) {
        return NULL;
    }
    for (i = 0; i < len; i++) {
        if ((unsigned char)s[i] < 0x20u) {
            return NULL;
        }
    }
    out = malloc(len + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

/* sort + dedup a loaded string list in place; returns the new count */
static size_t rt_sort_dedup(char **list, size_t count)
{
    size_t w;
    size_t i;

    if (count > 1) {
        qsort(list, count, sizeof(char *), rt_cmp_str);
    }
    w = 0;
    for (i = 0; i < count; i++) {
        if (w == 0 || strcmp(list[w - 1], list[i]) != 0) {
            list[w++] = list[i];
        } else {
            free(list[i]);
        }
    }
    return w;
}

static void rt_fail(odk_error *err, const char *msg)
{
    if (err != NULL) {
        snprintf(err->text, sizeof(err->text), "%s", msg);
    }
}

static char *rt_read_file(const char *path, odk_error *err)
{
    FILE *fp;
    long file_len;
    size_t len;
    char *text;

    fp = fopen(path, "rb");
    if (fp == NULL) {
        rt_fail(err, "runtime pack: cannot read file");
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (file_len = ftell(fp)) < 0 ||
        fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        rt_fail(err, "runtime pack: cannot read file");
        return NULL;
    }
    len = (size_t)file_len;
    if (len > ODK_RT_FILE_MAX) {
        fclose(fp);
        rt_fail(err, "runtime pack: file exceeds the 1 MB cap");
        return NULL;
    }
    text = malloc(len + 1);
    if (text == NULL) {
        fclose(fp);
        rt_fail(err, "runtime pack: out of memory");
        return NULL;
    }
    if (fread(text, 1, len, fp) != len) {
        fclose(fp);
        free(text);
        rt_fail(err, "runtime pack: cannot read file");
        return NULL;
    }
    fclose(fp);
    text[len] = '\0';
    return text;
}

/* parse a JSON array of strings into a malloc'd (list, count);
 * enforces the per-entry cap and the total cap.  Returns 0 on
 * success (list may be NULL when the array is empty). */
static int rt_parse_string_array(const odk_json_value *arr, size_t cap,
                                 size_t total_cap, char ***list_out,
                                 size_t *count_out, odk_error *err)
{
    char **list = NULL;
    size_t count = 0;
    size_t i;

    *list_out = NULL;
    *count_out = 0;
    if (arr == NULL) {
        return 0;
    }
    if (arr->type != ODK_JSON_ARRAY) {
        rt_fail(err, "runtime pack: expected an array of strings");
        return -1;
    }
    if (arr->item_count > total_cap) {
        rt_fail(err, "runtime pack: too many entries");
        return -1;
    }
    if (arr->item_count > 0) {
        list = calloc(arr->item_count, sizeof(char *));
        if (list == NULL) {
            rt_fail(err, "runtime pack: out of memory");
            return -1;
        }
    }
    for (i = 0; i < arr->item_count; i++) {
        const odk_json_value *item = arr->items[i];
        if (item == NULL || item->type != ODK_JSON_STRING) {
            rt_fail(err, "runtime pack: entries must be strings");
            goto bad;
        }
        list[count] = rt_dup_entry(item->string, cap);
        if (list[count] == NULL) {
            rt_fail(err, "runtime pack: entry is empty, too long or "
                         "contains control bytes");
            goto bad;
        }
        count++;
    }
    *list_out = list;
    *count_out = count;
    return 0;

bad:
    for (i = 0; i < count; i++) {
        free(list[i]);
    }
    free(list);
    return -1;
}

/* parse a zh word list: same as rt_parse_string_array but each entry
 * must also be >= 2 code points (FMM singles are the fallback). */
static int rt_parse_zh_words(const odk_json_value *arr, char ***list_out,
                             size_t *count_out, size_t *max_cps_out,
                             odk_error *err)
{
    char **list = NULL;
    size_t count = 0;
    size_t max_cps = 0;
    size_t i;

    *list_out = NULL;
    *count_out = 0;
    *max_cps_out = 0;
    if (arr == NULL) {
        return 0;
    }
    if (arr->type != ODK_JSON_ARRAY) {
        rt_fail(err, "runtime pack: 'words' must be an array of strings");
        return -1;
    }
    if (arr->item_count > ODK_RT_WORDS_MAX) {
        rt_fail(err, "runtime pack: too many 'words' entries");
        return -1;
    }
    if (arr->item_count > 0) {
        list = calloc(arr->item_count, sizeof(char *));
        if (list == NULL) {
            rt_fail(err, "runtime pack: out of memory");
            return -1;
        }
    }
    for (i = 0; i < arr->item_count; i++) {
        const odk_json_value *item = arr->items[i];
        size_t cps;
        if (item == NULL || item->type != ODK_JSON_STRING) {
            rt_fail(err, "runtime pack: 'words' entries must be strings");
            goto bad;
        }
        cps = odk_utf8_codepoints(item->string, strlen(item->string));
        if (cps < 2) {
            rt_fail(err, "runtime pack: 'words' entries must be at least "
                         "2 code points");
            goto bad;
        }
        list[count] = rt_dup_entry(item->string, ODK_RT_ENTRY_MAX);
        if (list[count] == NULL) {
            rt_fail(err, "runtime pack: 'words' entry too long");
            goto bad;
        }
        if (cps > max_cps) {
            max_cps = cps;
        }
        count++;
    }
    *list_out = list;
    *count_out = count;
    *max_cps_out = max_cps;
    return 0;

bad:
    for (i = 0; i < count; i++) {
        free(list[i]);
    }
    free(list);
    return -1;
}

/* concepts: {"name": ["w1", ...]} -> flat arrays */
static int rt_parse_concepts(const odk_json_value *obj,
                             char ***names_out, size_t *name_count_out,
                             char ***words_out, size_t **starts_out,
                             size_t *word_total_out, odk_error *err)
{
    char **names = NULL;
    char **words = NULL;
    size_t *starts = NULL;
    size_t name_count = 0;
    size_t word_total = 0;
    size_t i;
    size_t w;

    *names_out = NULL;
    *name_count_out = 0;
    *words_out = NULL;
    *starts_out = NULL;
    *word_total_out = 0;
    if (obj == NULL) {
        return 0;
    }
    if (obj->type != ODK_JSON_OBJECT) {
        rt_fail(err, "runtime pack: 'concepts' must be an object");
        return -1;
    }
    if (obj->member_count > ODK_RT_CONCEPT_MAX) {
        rt_fail(err, "runtime pack: too many concepts");
        return -1;
    }
    if (obj->member_count > 0) {
        names = calloc(obj->member_count, sizeof(char *));
        starts = calloc(obj->member_count + 1, sizeof(size_t));
        if (names == NULL || starts == NULL) {
            rt_fail(err, "runtime pack: out of memory");
            goto bad;
        }
    }
    for (i = 0; i < obj->member_count; i++) {
        const odk_json_value *val = obj->members[i];
        const char *name = obj->keys[i];
        char **cw = NULL;
        size_t cw_count = 0;
        if (val == NULL || val->type != ODK_JSON_ARRAY) {
            rt_fail(err, "runtime pack: concept values must be arrays");
            goto bad;
        }
        if (val->item_count > ODK_RT_CWORDS_MAX) {
            rt_fail(err, "runtime pack: too many words in one concept");
            goto bad;
        }
        if (rt_parse_string_array(val, ODK_RT_ENTRY_MAX, ODK_RT_CWORDS_MAX,
                                  &cw, &cw_count, err) != 0) {
            goto bad;
        }
        names[name_count] = rt_dup_entry(name, ODK_RT_ENTRY_MAX);
        if (names[name_count] == NULL) {
            size_t k;
            for (k = 0; k < cw_count; k++) {
                free(cw[k]);
            }
            free(cw);
            rt_fail(err, "runtime pack: concept name is invalid");
            goto bad;
        }
        /* starts[i] is the BEGIN offset of concept i's words
         * (odk_lang.h: "concept i owns words[starts[i]..starts[i+1])"),
         * recorded BEFORE the words are appended; the array has
         * member_count+1 slots, so starts[member_count] is the END
         * sentinel = word_total (set after the loop).  The original
         * code stored the cumulative END in starts[i] and left the
         * sentinel at 0, which made
         * odk_kernel_lexicon_seed_links read an empty range. */
        starts[name_count] = word_total;
        if (cw_count > 0) {
            char **grown = realloc(words,
                                   (word_total + cw_count) * sizeof(char *));
            if (grown == NULL) {
                size_t k;
                free(names[name_count]);
                for (k = 0; k < cw_count; k++) {
                    free(cw[k]);
                }
                free(cw);
                rt_fail(err, "runtime pack: out of memory");
                goto bad;
            }
            words = grown;
            for (w = 0; w < cw_count; w++) {
                words[word_total + w] = cw[w];
            }
            word_total += cw_count;
        }
        free(cw);
        name_count++;
    }
    /* END sentinel: starts[member_count] == word_total (the last
     * concept's words run up to the total). */
    if (name_count > 0) {
        starts[name_count] = word_total;
    }
    *names_out = names;
    *name_count_out = name_count;
    *words_out = words;
    *starts_out = starts;
    *word_total_out = word_total;
    return 0;

bad:
    for (i = 0; i < name_count; i++) {
        free(names[i]);
    }
    free(names);
    free(starts);
    if (words != NULL) {
        for (w = 0; w < word_total; w++) {
            free(words[w]);
        }
    }
    free(words);
    return -1;
}

static odk_rt_text *rt_load(const char *path, int want_lexicon,
                            odk_error *err)
{
    char *text = NULL;
    odk_json_value *root = NULL;
    const odk_json_value *stop_arr = NULL;
    const odk_json_value *concept_obj = NULL;
    const odk_json_value *words_arr = NULL;
    odk_rt_text *rt;

    text = rt_read_file(path, err);
    if (text == NULL) {
        return NULL;
    }
    root = odk_json_parse(text, strlen(text), err);
    free(text);
    if (root == NULL) {
        rt_fail(err, "runtime pack: not valid JSON");
        return NULL;
    }
    if (root->type != ODK_JSON_OBJECT) {
        odk_json_free(root);
        rt_fail(err, "runtime pack: root must be a JSON object");
        return NULL;
    }
    stop_arr = odk_json_object_get(root, "stopwords");
    concept_obj = want_lexicon ? odk_json_object_get(root, "concepts")
                               : NULL;
    words_arr = want_lexicon ? odk_json_object_get(root, "words") : NULL;
    if (stop_arr == NULL && concept_obj == NULL && words_arr == NULL) {
        odk_json_free(root);
        rt_fail(err, want_lexicon
                       ? "runtime pack: no stopwords/concepts/words "
                         "section found"
                       : "runtime pack: no 'stopwords' section found");
        return NULL;
    }
    if (stop_arr != NULL && stop_arr->type != ODK_JSON_ARRAY) {
        odk_json_free(root);
        rt_fail(err, "runtime pack: 'stopwords' must be an array");
        return NULL;
    }
    rt = calloc(1, sizeof(*rt));
    if (rt == NULL) {
        odk_json_free(root);
        rt_fail(err, "runtime pack: out of memory");
        return NULL;
    }
    if (rt_parse_string_array(stop_arr, ODK_RT_ENTRY_MAX, ODK_RT_STOP_MAX,
                              &rt->stopwords, &rt->stopword_count,
                              err) != 0) {
        goto bad;
    }
    if (want_lexicon) {
        if (rt_parse_concepts(concept_obj, &rt->concept_names,
                              &rt->concept_count, &rt->concept_words,
                              &rt->concept_starts,
                              &rt->concept_word_total, err) != 0) {
            goto bad;
        }
        if (rt_parse_zh_words(words_arr, &rt->zh_words,
                              &rt->zh_word_count, &rt->zh_word_max_cps,
                              err) != 0) {
            goto bad;
        }
        if (rt->stopword_count == 0 && rt->concept_count == 0
            && rt->zh_word_count == 0) {
            rt_fail(err, "runtime pack: all sections are empty");
            goto bad;
        }
    } else if (rt->stopword_count == 0) {
        rt_fail(err, "runtime pack: 'stopwords' is empty");
        goto bad;
    }
    odk_json_free(root);
    rt->stopword_count = rt_sort_dedup(rt->stopwords, rt->stopword_count);
    rt->zh_word_count = rt_sort_dedup(rt->zh_words, rt->zh_word_count);
    return rt;

bad:
    odk_json_free(root);
    odk_lang_rt_free(rt);
    return NULL;
}

odk_rt_text *odk_lang_rt_load_stopwords(const char *path, odk_error *err)
{
    return rt_load(path, 0, err);
}

odk_rt_text *odk_lang_rt_load_lexicon(const char *path, odk_error *err)
{
    return rt_load(path, 1, err);
}

void odk_lang_rt_free(odk_rt_text *rt)
{
    size_t i;

    if (rt == NULL) {
        return;
    }
    if (rt->stopwords != NULL) {
        for (i = 0; i < rt->stopword_count; i++) {
            free(rt->stopwords[i]);
        }
        free(rt->stopwords);
    }
    if (rt->zh_words != NULL) {
        for (i = 0; i < rt->zh_word_count; i++) {
            free(rt->zh_words[i]);
        }
        free(rt->zh_words);
    }
    if (rt->concept_names != NULL) {
        for (i = 0; i < rt->concept_count; i++) {
            free(rt->concept_names[i]);
        }
        free(rt->concept_names);
    }
    if (rt->concept_words != NULL) {
        for (i = 0; i < rt->concept_word_total; i++) {
            free(rt->concept_words[i]);
        }
        free(rt->concept_words);
    }
    free(rt->concept_starts);
    free(rt);
}
