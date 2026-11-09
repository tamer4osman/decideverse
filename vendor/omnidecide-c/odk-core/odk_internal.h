/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_internal.h - shared internals of the OmniDecide C library.
 *
 * Memory-safety conventions used across every source file:
 *   - every malloc/calloc/realloc return value is checked; on exhaustion
 *     the failing operation returns -1 / NULL / ODK_ERR_MEMORY and the
 *     caller unwinds, freeing everything it owns.
 *   - no VLAs, no alloca: all dynamically sized buffers live on the heap
 *     through sb_t / str_list_t / cmap_t.
 *   - all formatting goes through snprintf into pre-sized buffers.
 *
 */

#ifndef ODK_INTERNAL_H
#define ODK_INTERNAL_H

/* POSIX.1-2008: strdup(), clock_gettime(), CLOCK_MONOTONIC. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "omnidecide.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

/* Evidence stamp embedded in the text the client pipeline evaluates
 * ("OmniDecide <stamp>").  It is part of the scored evidence (the
 * character-trigram channel), so it is a FIXED constant that identifies the
 * scoring semantics, not the library version: it stayed "1.0.0" through the
 * whole 1.x series and is deliberately NOT bumped in 2.0.0, so that 1.x-era
 * decision bytes keep replaying bit-identically (docs/UPGRADE-2.0.md).  New
 * v2 records carry kernel_version "2.0.0" instead.  A build
 * may override it with -DODK_STAMP_VERSION='"x.y.z"'. */
#ifndef ODK_STAMP_VERSION
#define ODK_STAMP_VERSION "1.0.0"
#endif

/* ------------------------------------------------------------------ */
/* Dynamic string buffer                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} sb_t;

int sb_init(sb_t *sb);
void sb_free(sb_t *sb);
/* Ensure room for `extra` more bytes + NUL terminator; 0 ok, -1 OOM. */
int sb_reserve(sb_t *sb, size_t extra);
int sb_append(sb_t *sb, const char *s, size_t n);
int sb_puts(sb_t *sb, const char *s);
int sb_putc(sb_t *sb, char c);
int sb_printf(sb_t *sb, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;
/* Hand the NUL-terminated heap buffer to the caller (sb becomes empty). */
char *sb_detach(sb_t *sb);

/* ------------------------------------------------------------------ */
/* String list                                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    char **items;
    size_t count;
    size_t cap;
} str_list_t;

void str_list_init(str_list_t *list);
void str_list_free(str_list_t *list);
/* Copies `s` (length n). 0 ok, -1 OOM. */
int str_list_push(str_list_t *list, const char *s, size_t n);

/* ------------------------------------------------------------------ */
/* Insertion-ordered string -> count map                               */
/* ------------------------------------------------------------------ */

typedef struct {
    char *key;
    size_t count;
} cmap_entry;

typedef struct {
    cmap_entry *entries; /* insertion order */
    size_t count;
    size_t cap;
    size_t *buckets; /* entry index + 1; 0 = empty */
    size_t bucket_count;
} cmap_t;

int cmap_init(cmap_t *map);
void cmap_free(cmap_t *map);
/* Insert or increment; the key is copied. 0 ok, -1 OOM. */
int cmap_increment(cmap_t *map, const char *key, size_t key_len);
/* Returns the count (0 when absent). */
size_t cmap_get(const cmap_t *map, const char *key, size_t key_len);

/* ------------------------------------------------------------------ */
/* Text helpers (tokenizer, stopwords, similarity primitives)          */
/* ------------------------------------------------------------------ */

/* isspace() for the ASCII subset used here. */
int odk_ascii_isspace(unsigned char c);
int odk_ascii_islower_token(unsigned char c); /* [a-z0-9] after lowering */

/* Extract [a-z0-9]+ runs from the ASCII-lowercased text.  When `filtered`
 * is non-zero the token post-filter is applied (len > 1 and
 * not a stopword).  0 ok, -1 OOM. */
int odk_extract_tokens(const char *text, int filtered, str_list_t *out);

/* Mode-aware tokenizer: ASCII_COMPAT reproduces
 * odk_extract_tokens exactly for the EN pack; UNICODE applies NFC +
 * default case folding + script-aware segmentation.  0 ok, -1 OOM. */
int odk_extract_tokens_ex(const char *text, odk_text_mode mode,
                          odk_stopword_pack pack, int filtered,
                          str_list_t *out);

/* Single-pass variant producing BOTH token lists (the
 * unfiltered and the filtered) from one tokenization - byte-identical
 * to the two odk_extract_tokens_ex calls it replaces.  0 ok, -1 OOM. */
int odk_extract_tokens_both(const char *text, odk_text_mode mode,
                            odk_stopword_pack pack,
                            str_list_t *unfiltered, str_list_t *filtered);

/* Language-aware tokenizer variants.  AUTO is
 * resolved per text by the conservative detector; UND (and the
 * legacy odk_extract_tokens_* entry points) keep the exact baseline
 * semantics. */
/* Rt = per-kernel runtime-loaded text data;
 * NULL = compiled-in packs only (the baseline semantics). */
struct odk_rt_text;
int odk_extract_tokens_lang(const char *text, odk_text_mode mode,
                            odk_stopword_pack pack, odk_language lang,
                            const struct odk_rt_text *rt, int filtered,
                            str_list_t *out);
int odk_extract_tokens_both_lang(const char *text, odk_text_mode mode,
                                 odk_stopword_pack pack, odk_language lang,
                                 const struct odk_rt_text *rt,
                                 str_list_t *unfiltered,
                                 str_list_t *filtered);

/* The single-pass analysis with an explicit light-
 * morphology knob for the MATCHING channel (0 = off = the _lang
 * semantics above; brain memory keys always use the off form). */
int odk_extract_tokens_both_morph(const char *text, odk_text_mode mode,
                                  odk_stopword_pack pack, odk_language lang,
                                  const struct odk_rt_text *rt,
                                  int morph_light,
                                  str_list_t *unfiltered,
                                  str_list_t *filtered);

/* Stopword pack membership (exact byte match, binary search). */
int odk_stopword_is_stop(odk_stopword_pack pack, const char *token);

/* English stopword lookup bridge (odk_util.c kStopwords, legacy list). */
int odk_util_stopword_en_find(const char *s);

/* Unicode runtime helpers (odk_unicode.c). */
int odk_u_decode_next(const char *s, size_t len, uint32_t *out_cp);
unsigned char odk_u_class(uint32_t cp);
int odk_u_is_space(uint32_t cp);
char *odk_u_normalize(const char *text);
/* Language-aware normalization; UND/AUTO/DEFAULT
 * are the exact baseline pipeline. */
char *odk_u_normalize_lang(const char *text, odk_language lang);
char *odk_u_normalize_len(const char *text, size_t len);
void odk_u_strip_bounds(const char *s, size_t len, size_t *start,
                        size_t *end);
size_t odk_u_truncate_cp(const char *s, size_t len, size_t n_cps);
int odk_u_space_span(const char *s, size_t len);
int odk_u_trigram_counter(const char *text, cmap_t *map);

/* Input-profile chunking + detector (odk_profiles.c).  Returns 0 ok,
 * -1 OOM, -2 "no dedicated chunker for this profile" (caller falls
 * back to the classic sentence chunker). */
int odk_profile_chunks(odk_text_mode mode, odk_input_profile profile,
                        size_t chunk_source_max, size_t max_chunks,
                        const char *context, str_list_t *out);

/* Dot / norms helper: cosine over two count maps. */
double odk_counter_cosine(const cmap_t *left, const cmap_t *right);

/* join(items, sep) into a malloc'd string; NULL on OOM. */
char *odk_str_join(const char *const *items, size_t count, const char *sep);

/* Bounded strdup helpers (checked allocation). */
char *odk_strdup_range(const char *s, size_t len);
char *odk_strdup(const char *s);

/* Strip on the ASCII whitespace subset; returns the first
 * non-space index and the trimmed length. */
void odk_strip_bounds(const char *s, size_t len, size_t *start, size_t *end);

/* Round half-even to 4 / 3 / 2 decimals and to an integer.  No exact-tie case exists for the decimal cases (a midpoint
 * k + 0.5 in scaled space would require a non-dyadic rational to be exact
 * in binary, which is impossible), and rint() honours round-half-even for
 * the integer case, so the results are bit-exact and reproducible. */
double odk_round4(double x);
double odk_round3(double x);
double odk_round2(double x);
double odk_round_half_even(double x);
double odk_clamp01(double x);

/* UTF-8 code-point count (used where characters are counted). */
size_t odk_utf8_codepoints(const char *s, size_t len);

/* ------------------------------------------------------------------ */
/* Internal scoring hooks shared with the learning module (odk_learn.c)*/
/* ------------------------------------------------------------------ */

/* Calibrated-distribution logit scale: logits = 6*(f(s)-0.5). */
#define ODK_LOGIT_SCALE 6.0

/* ------------------------------------------------------------------ */
/* Model file v1 (docs/MODEL.md).  The built-in default     */
/* equals today's constants; a loaded file only swaps these values.    */
/* ------------------------------------------------------------------ */
#define ODK_MODEL_EXTRA_MAX 8

typedef struct {
    uint32_t feature_set_id; /* 0 = built-in six-channel scoring       */
    double w_cos;
    double w_jaccard;
    double w_trigram;
    double w_whole;
    double w_best;
    double w_ctx;
    double w_dir;
    double logit_scale;      /* choice/score/rank calibration          */
    double noul_logit_scale; /* noul calibration                       */
    /* feature-set-1 extra weights (docs/FEATURES.md); the extra
     * FEATURE VALUES are computed in the kernel, the weights come from
     * the model file (v2 format). */
    int extra_count;
    double extra[ODK_MODEL_EXTRA_MAX];
} odk_model;

/* The built-in default model: byte-for-byte today's behaviour. */
extern const odk_model odk_default_model;

/* Strict 128-byte model-file parser (all MODEL.md rejection rules). */
odk_status odk_model_parse_file(const char *path, odk_model *out,
                                odk_error *err);

/* SHA-256 helper (lowercase hex, 65 bytes incl. NUL) — used by the model
 * loader and available for diagnostics. */
void odk_sha256_hex(const unsigned char *data, size_t len, char out[65]);

/* Shared internal error setter (vsnprintf into err->text; NULL-safe). */
void odk_set_error(odk_error *err, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

/* Thin wrappers over the kernel's static scoring path, exposed so the
 * opt-in experience layer can blend bonuses into exactly the same
 * evidence scores that choice/rank/compare use.  Behaviour of every
 * existing kernel path is unchanged.  0 ok, -1 on allocation failure.
 * odk_internal_score_options_k is the legacy mode-aware variant. */
int odk_internal_score_options(const char *const *options, size_t option_count,
                               const char *instruction, const char *context,
                               const odk_image *image, double *scores);
int odk_internal_score_options_k(const odk_kernel *kernel,
                                 const char *const *options,
                                 size_t option_count,
                                 const char *instruction, const char *context,
                                 const odk_image *image, double *scores);

/* Link-aware scoring (odk_links.h defines odk_link_store; declared here
 * to avoid a include cycle: odk_links.h includes this header). */
struct odk_link_store;
int odk_internal_score_options_linked(const odk_kernel *kernel,
                                      const struct odk_link_store *links,
                                      const char *const *options,
                                      size_t option_count,
                                      const char *instruction,
                                      const char *context,
                                      const odk_image *image,
                                      double *scores, sb_t *explain);

/* ---- brain test hooks (internal, no global state) ---- */

/* Raw insert-or-fetch keyed by an explicit (text, khi, klo) triple:
 * increments visits and returns the new count, or -1 on OOM / cap /
 * bad arguments.  Tests use it to force hash-lane collisions (same
 * lanes, different text) and to prove entry equality compares BOTH
 * the stored text and both lanes. */
struct odk_brain;
long odk_internal_brain_insert_raw(struct odk_brain *brain, const char *key,
                                   uint64_t khi, uint64_t klo);
/* Raw fetch by the explicit triple: the entry's visits, or -1 when the
 * (text, khi, klo) triple has no entry. */
long odk_internal_brain_find_raw(const struct odk_brain *brain,
                                 const char *key, uint64_t khi,
                                 uint64_t klo);
/* Key builder with the per-part truncation report: builds the stored
 * key text for (primitive, instruction, option) under the brain's text
 * config and key_part_max.  khi/klo (out) receive the 128-bit full-text
 * hash lanes; *part_mask receives the clipped-part bitmask (bit0=prim,
 * bit1=instr, bit2=opt).  Returns a malloc'd key the caller frees, or
 * NULL on OOM / bad arguments. */
char *odk_internal_brain_build_key_raw(const struct odk_brain *brain,
                                       const char *primitive,
                                       const char *instruction,
                                       const char *option, uint64_t *khi,
                                       uint64_t *klo,
                                       unsigned *part_mask);

/* Calibrated distribution: logits = 6*(clamp01(s)-0.5), then a
 * numerically stable softmax.  0 ok, -1 on allocation failure. */
int odk_internal_calibrated_distribution(const double *scores, size_t count,
                                         double *probs);

#endif /* ODK_INTERNAL_H */
