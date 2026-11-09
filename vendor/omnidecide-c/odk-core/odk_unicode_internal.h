/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_unicode_internal.h - Unicode text layer internals.
 *
 * All tables live in odk_unicode_data.c (GENERATED from UCD 15.1.0 by
 * the project's table generator - dev-only, output committed).
 *
 * Nothing in this header is public API; see omnidecide.h for the public
 * surface (odk_utf8_decode_next, odk_text_normalize_alloc, ...).
 *
 */

#ifndef ODK_UNICODE_INTERNAL_H
#define ODK_UNICODE_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

/* Pinned Unicode version of the committed tables. */
#define ODK_UNICODE_VERSION "15.1.0"

/* ------------------------------------------------------------------ */
/* Class byte layout (odk_class lookup)                                */
/* ------------------------------------------------------------------ */

#define ODK_U_LETTER 0x01 /* L* general categories                    */
#define ODK_U_MARK 0x02   /* M*                                       */
#define ODK_U_NUMBER 0x04 /* N*                                       */
#define ODK_U_SPACE 0x08  /* White_Space property (PropList.txt)      */

/* Script nibble (bits 4-7), coarse classes for segmentation. */
#define ODK_S_WORD 0     /* space-delimited scripts incl. Hangul     */
#define ODK_S_HAN 1      /* bigram                                   */
#define ODK_S_HIRA 2     /* bigram                                   */
#define ODK_S_KATA 3     /* bigram                                   */
#define ODK_S_THAI 4     /* bigram                                   */
#define ODK_S_LAO 5      /* bigram                                   */
#define ODK_S_KHMR 6     /* bigram                                   */
#define ODK_S_MYAN 7     /* bigram                                   */
#define ODK_S_INHERIT 8  /* Common / Inherited / unassigned          */

#define ODK_U_SCRIPT_MASK 0xF0
#define ODK_U_CLASS_MASK 0x0F

typedef struct {
    unsigned int first; /* first cp of the range (inclusive)  */
    unsigned int last;  /* last cp of the range (inclusive)   */
    unsigned short block; /* index into odk_class_blocks      */
} odk_supra_range;

/* Generated tables (odk_unicode_data.c; external linkage, internal
 * namespace - not part of the public API). */
extern const unsigned char odk_class_blocks[][128];
extern const unsigned short odk_class_stage1_bmp[];
extern const odk_supra_range odk_class_supra[];
extern const unsigned short odk_class_supra_count;
extern const unsigned int odk_fold_cp[];
extern const unsigned char odk_fold_len[];
extern const unsigned int odk_fold_data[];
extern const unsigned int odk_fold_cp_count;
extern const unsigned int odk_dec_cp[];
extern const unsigned char odk_dec_len[];
extern const unsigned int odk_dec_data[];
extern const unsigned int odk_dec_cp_count;
extern const unsigned int odk_ccc_cp[];
extern const unsigned char odk_ccc_val[];
extern const unsigned int odk_ccc_cp_count;
extern const unsigned int odk_compose_a[];
extern const unsigned int odk_compose_b[];
extern const unsigned int odk_compose_c[];
extern const unsigned int odk_compose_count;
extern const unsigned char odk_needs_nfc[];
/* Dense per-cp participation bitmaps (cp < limit): a set
 * bit says the cp may hit the corresponding table; a clear bit proves
 * it cannot, so the runtime skips the search and uses the identity
 * result.  Bit-identical by construction (derived from the same
 * generated tables). */
extern const unsigned char odk_nfc_dense_bmp[];
extern const unsigned int odk_nfc_dense_limit;
extern const unsigned char odk_fold_dense_bmp[];
extern const unsigned int odk_fold_dense_limit;

/* Stopword packs (odk_unicode_data.c, original-authored). */
extern const char *const odk_stopwords_de[];
extern const size_t odk_stopwords_de_count;
extern const char *const odk_stopwords_fr[];
extern const size_t odk_stopwords_fr_count;
extern const char *const odk_stopwords_es[];
extern const size_t odk_stopwords_es_count;

/* English pack = the legacy kStopwords[] in odk_util.c (unchanged). */

/* ------------------------------------------------------------------ */
/* Runtime (odk_unicode.c)                                             */
/* ------------------------------------------------------------------ */

/* Strict UTF-8 decode of one sequence.  Returns consumed byte count
 * (1-4) and stores the code point in *out_cp; returns 0 for an invalid
 * sequence (caller treats s[0] as a separator and advances 1 byte). */
int odk_u_decode_next(const char *s, size_t len, uint32_t *out_cp);

/* Combined class+script byte for a code point. */
unsigned char odk_u_class(uint32_t cp);

/* General-category/property predicates. */
int odk_u_is_letter(uint32_t cp);
int odk_u_is_mark(uint32_t cp);
int odk_u_is_number(uint32_t cp);
int odk_u_is_space(uint32_t cp); /* White_Space property */
int odk_u_is_bigram_script(unsigned char script_nibble);
unsigned char odk_u_script(uint32_t cp);

/* Canonical decomposition of cp (NULL when none). *out_len is 1 or 2
 * (Hangul syllables are decomposed by the caller's algorithmic path). */
const unsigned int *odk_u_decomp(uint32_t cp, int *out_len);

/* Canonical combining class (0 when absent). */
int odk_u_ccc(uint32_t cp);

/* Composition: (first, second) -> composite, or 0 when no pair. */
uint32_t odk_u_compose(uint32_t first, uint32_t second);

/* Default case folding: mapping length (0 = identity). */
int odk_u_fold(uint32_t cp, uint32_t out[3]);

/* True when the code point can trigger any NFC change. */
int odk_u_needs_nfc(uint32_t cp);

/* Decode the whole buffer to code points (invalid bytes -> 0xFFFFFFFF).
 * Returns a malloc'd array of exactly byte-based code point count? No:
 * of decoded units (one per valid sequence OR per invalid byte) and
 * writes the count to *out_count.  NULL on OOM. */
uint32_t *odk_u_decode_all(const char *s, size_t len, size_t *out_count);

/* In-place NFC normalization of a code point array.  Returns the new
 * length; sets *out_ok = 0 on allocation failure (contents undefined). */
size_t odk_u_nfc_array(const uint32_t *cps, size_t count,
                        uint32_t **out_result, int *out_ok);

/* fold(NFC(text)) for a NUL-terminated string: malloc'd UTF-8 result
 * (NULL on OOM).  Pure-ASCII input is returned unchanged (fast path). */
char *odk_u_normalize(const char *text);

/* Same, for an explicit length. */
char *odk_u_normalize_len(const char *text, size_t len);

/* Unicode White_Space-aware strip bounds: first/last non-space byte
 * indexes of the trimmed region (byte offsets, whole UTF-8 sequences
 * are skipped as single units). */
void odk_u_strip_bounds(const char *s, size_t len, size_t *start,
                        size_t *end);

/* Truncate to at most n_cps code points, cutting only on sequence
 * boundaries; returns the kept byte length. */
size_t odk_u_truncate_cp(const char *s, size_t len, size_t n_cps);

#endif /* ODK_UNICODE_INTERNAL_H */
