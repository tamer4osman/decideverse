/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * omnidecide.h - Public C API of the OmniDecide deterministic decision
 * kernel.
 *
 * Owner and author: Tamer Osman
 *   GitHub:   https://github.com/tamer4osman
 *   LinkedIn: https://www.linkedin.com/in/tamer4osman
 *
 * Modules:
 *   - the embedded deterministic decision engine (all six decision
 *     primitives)
 *   - the deterministic image fingerprint
 *   - the Markdown minifier (minification + token estimation)
 *   - the evaluate() pipeline (five-key output contract, direct-string
 *     ingestion path)
 *
 * The library has no neural-network backbone and loads no model or
 * weight files; the deterministic engine is the only backend
 * (backend = "embedded-fallback").
 *
 * Design goals:
 *   - deterministic: identical inputs always produce identical outputs
 *   - zero dependencies beyond libc + libm
 *   - leak-free: every allocation has an owner and a release function
 *     (verified with AddressSanitizer / LeakSanitizer / UBSan)
 *   - thread-friendly: no global mutable state; kernel and client handles
 *     are read-only after creation, so concurrent evaluate() calls on a
 *     shared handle are safe.
 *
 * Build:  see Makefile (C11, -Wall -Wextra -Werror -pedantic).
 *
 */

#ifndef OMNIDECIDE_H
#define OMNIDECIDE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Library version (release history: CHANGELOG.md).
 *
 * What the library provides:
 *   - a deterministic decision kernel with six primitives (choice, score,
 *     noul, rank, compare, extract) and the five-key evaluate() contract;
 *   - two text modes: ASCII compatibility (the default; legacy byte
 *     semantics) and UNICODE (NFC, default case folding and script-aware
 *     segmentation over pinned Unicode 15.1.0 data);
 *   - ten language packs (en zh hi es ar fr bn pt id ur), a generic
 *     language-neutral mode (UND) and a deterministic auto-detector
 *     (ODK_LANG_AUTO);
 *   - input profiles, configurable limits with truncation reporting,
 *     over-limit strategies and output caps;
 *   - an image-fingerprint evidence channel;
 *   - opt-in outcome learning: an experience brain, learned word links
 *     and persistent snapshots (schema v3; v1/v2 snapshots still load).
 *
 * The client pipeline scores a FIXED evidence stamp ("1.0.0", unchanged in
 * 2.0.0 so that 1.x-era decision bytes replay bit-identically; see
 * docs/UPGRADE-2.0.md). New v2.0.0 records (pins, observations, calibration
 * reports) carry kernel_version "2.0.0" from ODK_LIBRARY_VERSION instead. */

/* Semantic version (see README, "Versioning").  ODK_LIBRARY_VERSION must
 * equal "MAJOR.MINOR.PATCH"; `make version-check` enforces it. */
#define ODK_VERSION_MAJOR 2
#define ODK_VERSION_MINOR 0
#define ODK_VERSION_PATCH 0
#define ODK_LIBRARY_VERSION "2.0.0"

/* Unicode version of the committed UCD tables (see THIRD_PARTY_NOTICES.md). */
#define ODK_UNICODE_VERSION "15.1.0"

/* ------------------------------------------------------------------ */
/* Status codes and error reporting                                    */
/* ------------------------------------------------------------------ */

typedef enum odk_status {
    ODK_OK = 0,           /* success                                          */
    ODK_ERR_ARGUMENT = 1, /* API misuse: bad primitive, arity, JSON, NULL ...  */
    ODK_ERR_INFERENCE = 2,/* evaluation failure                               */
    ODK_ERR_MEMORY = 3,   /* allocation failure                               */
    ODK_ERR_INTERNAL = 4  /* unexpected internal condition                    */
} odk_status;

/* Human-readable one-liner for a status code (never NULL). */
const char *odk_status_message(odk_status status);

/* Fixed-capacity error buffer a caller may pass to any fallible call.
 * Passing NULL simply discards the diagnostic text.  Messages that
 * embed caller-controlled data are bounded by construction (120-byte
 * excerpt cut on a UTF-8 boundary plus a visible "... [truncated]"
 * marker), so the buffer never truncates a diagnostic silently; the
 * struct size is ABI-frozen. */
#define ODK_ERROR_MAX 256
typedef struct {
    char text[ODK_ERROR_MAX];
} odk_error;

/* Full-copy diagnostic: returns a malloc'd clone of the error text
 * (caller frees), or NULL when err is NULL or empty.   Messages that embed caller-controlled data
 * are bounded by construction (120-byte excerpt, UTF-8 boundary cut,
 * visible "... [truncated]" marker), so the buffer text is the complete
 * diagnostic; use the odk_error_ex variants for the unbounded original
 * data. */
char *odk_error_message_alloc(const odk_error *err);

/* Extended diagnostics with a heap message that is
 * never truncated and the status code.  Release with
 * odk_error_ex_release (frees the message and zeroes the struct). */
typedef struct {
    odk_status code;   /* status of the failing call (ODK_OK when unused) */
    char *message;     /* owned, malloc'd, full text; NULL when unused    */
} odk_error_ex;

void odk_error_ex_release(odk_error_ex *err);

/* ------------------------------------------------------------------ */
/* Result values                                                       */
/* ------------------------------------------------------------------ */

typedef enum odk_value_kind {
    ODK_VALUE_NONE = 0,   /* result is null (only used for recorded errors) */
    ODK_VALUE_BOOL,       /* result is 0/1                                  */
    ODK_VALUE_NUMBER,     /* result is a double                             */
    ODK_VALUE_STRING,     /* result is a NUL-terminated UTF-8 string        */
    ODK_VALUE_LIST        /* result is a list of strings                    */
} odk_value_kind;

typedef struct {
    char *key;    /* owned */
    double value; /* rounded half-even to 4 decimals */
} odk_distribution_entry;

typedef struct {
    odk_distribution_entry *entries; /* insertion-ordered */
    size_t count;
} odk_distribution;

typedef struct {
    char *primitive;      /* owned, e.g. "choice"                    */
    odk_value_kind kind;
    int boolean;          /* ODK_VALUE_BOOL                          */
    double number;        /* ODK_VALUE_NUMBER                        */
    char *string;         /* ODK_VALUE_STRING (owned)                */
    char **items;         /* ODK_VALUE_LIST: owned array             */
    size_t item_count;    /* ODK_VALUE_LIST                          */
    double confidence;
    double probability;
    odk_distribution distribution;
} odk_result;

/* Release every allocation owned by *result and zero the struct. */
void odk_result_release(odk_result *result);

/* Serialise to the exact five-key decision payload:
 * {"primitive":..., "result":..., "confidence":...,
 * "probability":..., "distribution":{...}}.  Returns a malloc'd string
 * the caller must free, or NULL on allocation failure. */
char *odk_result_to_json(const odk_result *result);

/* ------------------------------------------------------------------ */
/* Query specification                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    char *query_key;   /* owned, may be ""                              */
    char *primitive;   /* owned canonical name: choice|score|noul|rank|compare|extract */
    char *instruction; /* owned, may be ""                              */
    char *statement;   /* owned, may be ""                              */
    char **options;    /* owned array of owned strings                  */
    size_t option_count;
    char **rubric;     /* owned array of owned strings                  */
    size_t rubric_count;
    int top_k;         /* clamped to [1, 10]                            */
} odk_query;

/* Validate + normalise a raw query spec.
 * - primitive must be one of the six names (case-insensitive, trimmed)
 * - statement falls back to instruction when empty
 * - top_k is clamped to [1, 10]
 * - "rank" requires >= 2 options, "compare" exactly 2
 * On success *out owns its copies; release with odk_query_release(). */
odk_status odk_query_from_spec(const char *query_key,
                               const char *primitive,
                               const char *instruction_or_question,
                               const char *statement,
                               const char *const *options, size_t option_count,
                               const char *const *rubric, size_t rubric_count,
                               int top_k,
                               odk_query *out, odk_error *err);

/* Parse one query spec from a JSON object string (fields:
 * primitive, instruction/question, statement, options,
 * rubric, top_k).  On success *out owns its copies. */
odk_status odk_query_from_json(const char *json_spec,
                               odk_query *out, odk_error *err);

void odk_query_release(odk_query *query);

/* ------------------------------------------------------------------ */
/* Image evidence channel                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    int width;                 /* pixels, >= 1 for a usable image        */
    int height;                /* pixels, >= 1 for a usable image        */
    const unsigned char *rgb;  /* width*height*3 bytes, row-major, RGB   */
} odk_image;

/* Deterministic textual fingerprint:
 *   "image <brightness> <color> <aspect> <W>x<H>"
 * brightness/color are computed on a fixed 24x24 area-averaged downsample
 * (BT.601 luma thresholds 85/170, neutral spread threshold 24); aspect
 * comes from the full-resolution dimensions.  Invalid or empty input
 * degrades to the bare marker "image" - the call never fails.  buf needs
 * at least 64 bytes. */
odk_status odk_describe_image(const odk_image *image, char *buf, size_t cap,
                              odk_error *err);

/* ------------------------------------------------------------------ */
/* Decision kernel                                                     */
/* ------------------------------------------------------------------ */

typedef struct odk_kernel odk_kernel;
typedef struct odk_brain odk_brain; /* brain sections below (forward declaration) */

/* Text layer mode.
 *
 * ASCII_COMPAT is the default: legacy byte-for-byte text semantics.
 * UNICODE applies NFC + Unicode default case folding + script-aware
 * segmentation over the pinned UCD 15.1.0 tables; for pure-ASCII input
 * with the English stopword pack it produces identical output. */
typedef enum {
    ODK_TEXT_ASCII_COMPAT = 0,
    ODK_TEXT_UNICODE = 1
} odk_text_mode;

/* Stopword pack selection.  ODK_STOPWORDS_EN is the
 * 120-word legacy list; de/fr/es are original-authored minimal packs. */
typedef enum {
    ODK_STOPWORDS_EN = 0,
    ODK_STOPWORDS_DE = 1,
    ODK_STOPWORDS_FR = 2,
    ODK_STOPWORDS_ES = 3,
    ODK_STOPWORDS_NONE = 4
} odk_stopword_pack;

/* ------------------------------------------------------------------ */
/* Language selection (see docs/LANGUAGES.md)         */
/* ------------------------------------------------------------------ */

/* Language packs for the ten most widely spoken languages
 * (Ethnologue 2026 total speakers).  The list is generated from the
 * single table-driven definition file src/odk_languages.def so the
 * owner can change the set without touching logic.  ABI: values are
 * stable; new languages append.  ODK_LANG_UND (default) reproduces
 * the baseline generic Unicode behavior BYTE-IDENTICALLY (differential
 * test); every language-specific behavior below is opt-in by setting
 * a language (or ODK_LANG_AUTO).  All non-English packs are UNREVIEWED
 * until a native speaker signs them off (see docs/LANGUAGES.md). */
typedef enum {
    ODK_LANG_UND = 0,   /* undetermined: baseline generic Unicode        */
    ODK_LANG_EN = 1,    /* English                                     */
    ODK_LANG_ZH = 2,    /* Mandarin Chinese (simplified default)      */
    ODK_LANG_HI = 3,    /* Hindi                                       */
    ODK_LANG_ES = 4,    /* Spanish                                     */
    ODK_LANG_AR = 5,    /* Modern Standard Arabic                      */
    ODK_LANG_FR = 6,    /* French                                      */
    ODK_LANG_BN = 7,    /* Bengali                                     */
    ODK_LANG_PT = 8,    /* Portuguese (pt-BR and pt-PT)               */
    ODK_LANG_ID = 9,    /* Indonesian (Bahasa Indonesia)              */
    ODK_LANG_UR = 10,   /* Urdu                                        */
    ODK_LANG_AUTO = 11, /* deterministic detector (falls back to UND) */
    ODK_LANG_DEFAULT = 12 /* per-call config only: use kernel setting */
} odk_language;

/* Language code string ("und", "en", ..., "auto") <-> enum.  Unknown
 * codes leave *ok = 0 (ok may be NULL) and return ODK_LANG_UND.
 * "default" maps to ODK_LANG_DEFAULT (per-call marker only). */
odk_language odk_lang_from_code(const char *code, int *ok);
const char *odk_lang_to_code(odk_language lang);
const char *odk_lang_display_name(odk_language lang);

/* ------------------------------------------------------------------ */
/* Input profiles                              */
/* ------------------------------------------------------------------ */

/* Content profile of the extracted text: selects the chunking unit and
 * profile-specific preprocessing.  ODK_PROFILE_AUTO runs the
 * conservative deterministic detector on the actual context text at
 * evaluation time (the kernel stays read-only: detection is a pure
 * function of the input). */
typedef enum {
    ODK_PROFILE_PLAIN = 0,
    ODK_PROFILE_MARKDOWN = 1,
    ODK_PROFILE_CHAT = 2,
    ODK_PROFILE_LOG = 3,
    ODK_PROFILE_JSON = 4,
    ODK_PROFILE_JSONL = 5,
    ODK_PROFILE_CSV = 6,
    ODK_PROFILE_TSV = 7,
    ODK_PROFILE_CODE = 8,
    ODK_PROFILE_HTML_TEXT = 9,
    ODK_PROFILE_EXTRACTED_PDF = 10,
    ODK_PROFILE_AUTO = 11
} odk_input_profile;


/* Over-limit strategy: what happens when the context
 * exceeds the effective limits.  PROFILE_DEFAULT resolves to head for
 * every profile except LOG (head+tail); HEAD is the legacy behavior
 * and keeps default evaluations byte-identical. */
typedef enum {
    ODK_OVERLIMIT_PROFILE_DEFAULT = 0,
    ODK_OVERLIMIT_HEAD = 1,
    ODK_OVERLIMIT_HEAD_TAIL = 2,
    ODK_OVERLIMIT_SELECT = 3
} odk_over_limit_strategy;

/* Kernel configuration (all fields have documented defaults; zero/0
 * selects the default for that field; see docs/API.md).
 *
 * In ASCII_COMPAT mode the defaults are the legacy values, so
 * legacy output stays byte-identical.  In UNICODE mode limits are measured in code
 * points and truncation happens only on code-point boundaries.
 *
 * Hard ceilings (clamped by odk_kernel_create_ex): context_max
 * 10,000,000; chunk_source_max 20,000,000; max_chunks 100,000;
 * snippet_max 10,000; max_ranked 10,000; max_snippet_chars 10,000;
 * max_total_chars 100,000; max_work 2,000,000,000.
 *
 * New fields are appended at the END of the struct: a zeroed struct (or an
 * initializer with fewer fields) selects every default, so older configs
 * keep their behavior. */
typedef struct {
    odk_text_mode text_mode;        /* default ODK_TEXT_ASCII_COMPAT    */
    odk_stopword_pack stopwords;    /* default ODK_STOPWORDS_EN         */
    odk_input_profile profile;      /* default ODK_PROFILE_PLAIN        */
    size_t context_max;             /* 0 = 20,000 B (ASCII) / 100,000 cp */
    size_t chunk_source_max;        /* 0 = 40,000 B / 200,000 cp         */
    size_t max_chunks;              /* 0 = 2,000 / 10,000               */
    size_t snippet_max;             /* 0 = 240 B / 1,000 cp             */
    odk_over_limit_strategy over_limit;  /* 0 = per-profile  */
    size_t max_ranked;              /* 0 = all options; cap 10,000      */
    size_t max_snippet_chars;       /* 0 = snippet_max; cap 10,000      */
    size_t max_total_chars;         /* 0 = 10,000 cps; cap 100,000      */
    size_t max_work;                /* 0 = derived (never
                                       binds by default)               */
    odk_language language;          /* default ODK_LANG_UND (baseline
                                       generic behavior)      */
    int morph_light;                /* 0 = off (default; token
                                       identity stable), 1 = light
                                       morphology per pack in the
                                       MATCHING channel (see
                                       docs/LANGUAGES.md; brain memory keys
                                       keep canonical forms)       */
} odk_kernel_config;

/* Default construction: ASCII_COMPAT, EN pack, legacy limits. */
odk_kernel *odk_kernel_create(void);

/* Configured construction.  NULL cfg equals odk_kernel_create().
 * Returns NULL only on allocation failure. */
odk_kernel *odk_kernel_create_ex(const odk_kernel_config *cfg);

void odk_kernel_free(odk_kernel *kernel);

/* Config accessors (never NULL-safe defaults when kernel is NULL). */
odk_text_mode odk_kernel_text_mode(const odk_kernel *kernel);
odk_stopword_pack odk_kernel_stopwords(const odk_kernel *kernel);
odk_input_profile odk_kernel_profile(const odk_kernel *kernel);
odk_language odk_kernel_language(const odk_kernel *kernel);

/* Per-call evaluation configuration.  Unlike the
 * output caps (which never affect the numeric decision), the
 * call config CAN change the decision: an explicit language hint
 * overrides the kernel config for that call.  ODK_LANG_DEFAULT (or a
 * NULL call pointer) keeps the kernel setting; ODK_LANG_AUTO runs
 * the deterministic detector on the call's texts (falls back to
 * UND when unsure).  Only the *_call evaluator variants below read
 * it. */
typedef struct {
    odk_language language;   /* ODK_LANG_DEFAULT = kernel setting */
    int morph_light;         /* 0 = kernel setting (a zeroed
                              * struct keeps the default), 1 = light
                              * morphology ON for this call */
} odk_call_config;

/* Per-call language for detectors/diagnostics.  Conservative
 * and deterministic: resolves ODK_LANG_AUTO by detecting on `text`
 * (script ranges first, then Latin stopword-hit rates); returns UND
 * when unsure.  Pure function of the input. */
odk_language odk_lang_detect_text(const char *text, double *confidence,
                                   char *signal_buf, size_t signal_cap);

/* Conservative deterministic input-profile detector.
 * Returns the detected profile (never AUTO) and, when reason_buf is
 * non-NULL, writes a human-readable explanation (NUL-terminated,
 * truncated to fit).  Pure function: no state, no clock. */
odk_input_profile odk_detect_input_profile(const char *text,
                                           char *reason_buf, size_t reason_cap);

/* ------------------------------------------------------------------ */
/* Unicode text utilities                     */
/* ------------------------------------------------------------------ */

/* Strict UTF-8 decode of one sequence at s[0] (len bytes available).
 * Stores the code point in *out_cp and returns the consumed byte count
 * (1-4).  Returns 0 for an invalid sequence: treat s[0] as a separator
 * and advance one byte.  Never reads past s[len-1]. */
int odk_utf8_decode_next(const char *s, size_t len, uint32_t *out_cp);

/* fold(NFC(text)) for a NUL-terminated string: returns a malloc'd
 * UTF-8 copy (caller frees; NULL on allocation failure).  Pure-ASCII
 * input returns an unchanged copy.  This is exactly the normalization
 * the UNICODE-mode tokenizer applies. */
char *odk_text_normalize_alloc(const char *text);

/* Always "embedded-fallback" (the deterministic engine is the only backend). */
const char *odk_kernel_backend(const odk_kernel *kernel);

/* All evaluators fill *out on ODK_OK; the caller owns it and must call
 * odk_result_release().  On failure *out is left zeroed and *err (if
 * given) carries the diagnostic message. */

/* choice: select the option best supported by the evidence channel. */
odk_status odk_evaluate_choice(odk_kernel *kernel,
                               const char *instruction,
                               const char *const *options, size_t option_count,
                               const char *context, const odk_image *image,
                               odk_result *out, odk_error *err);

/* score: place the evidence on an ordered rubric scale, result in [0, 1]. */
odk_status odk_evaluate_score(odk_kernel *kernel,
                              const char *instruction,
                              const char *const *rubric, size_t rubric_count,
                              const char *context, const odk_image *image,
                              odk_result *out, odk_error *err);

/* noul: binary affirmation - is the statement supported by the evidence? */
odk_status odk_evaluate_noul(odk_kernel *kernel,
                             const char *statement,
                             const char *context, const odk_image *image,
                             odk_result *out, odk_error *err);

/* rank: order all options best-to-worst ("a > b > c"). */
odk_status odk_evaluate_rank(odk_kernel *kernel,
                             const char *instruction,
                             const char *const *options, size_t option_count,
                             const char *context, const odk_image *image,
                             odk_result *out, odk_error *err);

/* compare: head-to-head comparison of exactly two options with tie. */
odk_status odk_evaluate_compare(odk_kernel *kernel,
                                const char *instruction,
                                const char *const *options, size_t option_count,
                                const char *context, const odk_image *image,
                                odk_result *out, odk_error *err);

/* extract: top_k context snippets most relevant to the instruction. */
odk_status odk_evaluate_extract(odk_kernel *kernel,
                                const char *instruction,
                                const char *context, const odk_image *image,
                                int top_k,
                                odk_result *out, odk_error *err);

/* ------------------------------------------------------------------ */
/* Output caps, per-call variants and result metadata   */
/* ------------------------------------------------------------------ */

/* Per-call output caps.  0 selects the kernel-config
 * default for that field; the kernel fields themselves default per
 * the kernel config.  A NULL caps pointer equals an all-zero struct.  Caps
 * NEVER affect the numeric decision (a guaranteed property): result value,
 * confidence and distribution are bit-identical under every cap. */
typedef struct {
    size_t max_ranked;        /* 0 = kernel default (0 = all options)     */
    size_t max_snippet_chars; /* 0 = kernel default (= snippet_max)       */
    size_t max_total_chars;   /* 0 = kernel default (10,000 cps)          */
} odk_output_caps;

/* Per-call result metadata.  Filled by the _opts evaluator
 * variants when the meta pointer is non-NULL; the base evaluators
 * take no meta pointer and are unaffected.  Zero-initialize before the call. */
typedef struct {
    size_t total_options;   /* rank: the FULL option count (before caps)  */
    int truncated;          /* a cap limited the rendered output           */
    size_t emitted;         /* extract: snippets actually emitted          */
    size_t dropped;         /* extract: snippets dropped by the caps       */
    size_t chunks_kept;     /* select strategy: chunks kept                */
    size_t chunks_dropped;  /* select strategy: chunks dropped             */
    size_t work_used;       /* work accounting: cps + 100 * chunks + anchor */
    size_t work_budget;     /* effective budget (derived default when 0)   */
    int work_exceeded;      /* the budget stopped the scan                 */
    /* Language reporting.  language = the effective
     * language of the call (kernel config or per-call override; AUTO
     * stays AUTO here).  detected/detected_confidence/detected_signal
     * are filled only when the call ran with ODK_LANG_AUTO: the
     * conservative detector's verdict on the context text (UND when
     * unsure).  Zero-initialised = UND/0/"". */
    odk_language language;
    odk_language detected;
    double detected_confidence;
    char detected_signal[64];
    /* Context truncation reporting.  1 when a part of
     * the caller's context never entered the evidence view (over-limit
     * scan cut or work-budget cut).  context_units_dropped = full
     * context size minus the evidence scan coverage, measured in BYTES
     * in ASCII_COMPAT mode and CODE POINTS in UNICODE mode.  Pure
     * accounting: filling these fields never changes a decision, and
     * the base (no-meta) evaluators behave exactly as before. */
    int context_truncated;
    size_t context_units_dropped;
} odk_result_meta;

/* _opts evaluator variants: identical decisions to the base
 * evaluators; caps shape only the RENDERED output (rank string,
 * extract snippets) and fill *meta when non-NULL.  err/meta may be
 * NULL.  The base functions are thin wrappers of these. */
odk_status odk_evaluate_choice_opts(odk_kernel *kernel,
                                     const char *instruction,
                                     const char *const *options,
                                     size_t option_count, const char *context,
                                     const odk_image *image,
                                     const odk_output_caps *caps,
                                     odk_result *out, odk_result_meta *meta,
                                     odk_error *err);
odk_status odk_evaluate_score_opts(odk_kernel *kernel,
                                   const char *instruction,
                                   const char *const *rubric,
                                   size_t rubric_count, const char *context,
                                   const odk_image *image,
                                   const odk_output_caps *caps,
                                   odk_result *out, odk_result_meta *meta,
                                   odk_error *err);
odk_status odk_evaluate_noul_opts(odk_kernel *kernel,
                                  const char *statement, const char *context,
                                  const odk_image *image,
                                  const odk_output_caps *caps,
                                  odk_result *out, odk_result_meta *meta,
                                  odk_error *err);
odk_status odk_evaluate_rank_opts(odk_kernel *kernel,
                                  const char *instruction,
                                  const char *const *options,
                                  size_t option_count, const char *context,
                                  const odk_image *image,
                                  const odk_output_caps *caps,
                                  odk_result *out, odk_result_meta *meta,
                                  odk_error *err);
odk_status odk_evaluate_compare_opts(odk_kernel *kernel,
                                     const char *instruction,
                                     const char *const *options,
                                     size_t option_count,
                                     const char *context,
                                     const odk_image *image,
                                     const odk_output_caps *caps,
                                     odk_result *out, odk_result_meta *meta,
                                     odk_error *err);
odk_status odk_evaluate_extract_opts(odk_kernel *kernel,
                                      const char *instruction,
                                      const char *context,
                                      const odk_image *image, int top_k,
                                      const odk_output_caps *caps,
                                      odk_result *out, odk_result_meta *meta,
                                      odk_error *err);

/* Structured rank output: the FULL ranked array behind a
 * rank result, independent of max_ranked.  Items are ranked best to
 * worst; label borrows from the result (do not free); score is the
 * calibrated probability (rounded to 4 decimals, like the
 * distribution).  index resolves against the options array the CALLER
 * passes in (pointer identity first, then content match of
 * not-yet-matched entries); SIZE_MAX when options are not given or a
 * label cannot be resolved.  Returns the item count and stores a
 * malloc'd array in *out_items (release with
 * odk_result_rank_items_free); 0 with *out_items == NULL when the
 * result is not a rank result or on allocation failure. */
typedef struct {
    const char *label; /* borrowed from the result, ranked order */
    size_t index;      /* original 0-based option position, or SIZE_MAX */
    double score;      /* calibrated probability of that option */
} odk_ranked_item;

size_t odk_result_rank_items(const odk_result *result,
                             const char *const *options, size_t option_count,
                             odk_ranked_item **out_items);
void odk_result_rank_items_free(odk_ranked_item *items);

/* ------------------------------------------------------------------ */
/* Explain API: every intermediate, per option.  Read-only */
/* introspection: filling the struct never changes a decision.         */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t index;       /* 0-based slot (option or rubric level)        */
    char *label;        /* owned copy of the option / level text        */
    double tf_cosine;   /* channels of the pair (label, anchor), where  */
    double jaccard;     /* the anchor is the whole-context evidence     */
    double trigram;     /* view the kernel actually scored against      */
    double whole;       /* 0.35*cos + 0.35*jac + 0.30*trig, clamped     */
    double best_chunk;  /* blended similarity of the best chunk
                         * (0.0 when the context has no chunks)        */
    double contextual;  /* clamp01(0.4*whole + 0.6*best_chunk)          */
    double directional; /* pair similarity (label, instruction);
                         * 0.0 when the instruction is empty           */
    double fused;       /* clamp01(w_ctx*contextual + w_dir*directional
                         * [+ extra terms] under the active model)      */
    double probability; /* softmax(logits = s*(fused-0.5)); equals the
                         * calibrated distribution entry               */
    /* feature-set-1 values (zeroed when the model is the
     * built-in default; extra_count = 0 then).  Append-only. */
    double extra[8];
    int extra_count;
} odk_explain_option;

typedef struct {
    char primitive[16];  /* "choice", "score" or "noul"                 */
    odk_explain_option *options; /* option_count entries (malloc'd)     */
    size_t option_count; /* choice/rank/compare: options; score: rubric
                          * levels (defaults included); noul: 0         */
    size_t chunk_count;  /* chunks built from the context               */
    /* score only */
    double score_normalized; /* the result number: sum p_i * i/(n-1)    */
    double score_peak;       /* max level probability                   */
    /* noul only (options == NULL; use the noul_* fields)              */
    double noul_whole;       /* pair similarity (statement, anchor)     */
    double noul_best_chunk;
    double noul_evidence;    /* max(whole, best_chunk)                  */
    double noul_affirm;      /* sigmoid(8*(evidence-0.5))               */
} odk_explain;

/* Per-option intermediates for choice (rank and compare share the same
 * fused scores: call this with the same options to explain them).
 * Zero-initialise *out; release with odk_explain_release(). */
odk_status odk_explain_choice(odk_kernel *kernel, const char *instruction,
                              const char *const *options,
                              size_t option_count, const char *context,
                              const odk_image *image, odk_explain *out,
                              odk_error *err);

/* Per-level intermediates for score; with an empty rubric the five
 * default levels ("very low" .. "very high") are explained.
 * score_normalized equals the score result number. */
odk_status odk_explain_score(odk_kernel *kernel, const char *instruction,
                             const char *const *rubric, size_t rubric_count,
                             const char *context, const odk_image *image,
                             odk_explain *out, odk_error *err);

/* The noul affirmation path: whole, best chunk, evidence and the
 * calibrated affirm probability (sigmoid scale 8, threshold 0.5).
 * noul_affirm equals the noul result probability. */
odk_status odk_explain_noul(odk_kernel *kernel, const char *statement,
                            const char *context, const odk_image *image,
                            odk_explain *out, odk_error *err);

/* Serialise to the JSON shape documented in docs/EXPLAIN.md (doubles at
 * %.17g so values round-trip exactly).  Malloc'd, caller frees; NULL
 * only on allocation failure. */
char *odk_explain_to_json(const odk_explain *explain);

/* Release every allocation owned by *explain and zero the struct. */
void odk_explain_release(odk_explain *explain);

/* Dispatcher: routes an odk_query to its
 * primitive evaluation (noul falls back to instruction, extract to
 * statement, following the dispatcher rules). */
odk_status odk_evaluate_query(odk_kernel *kernel, const odk_query *query,
                              const char *context, const odk_image *image,
                              odk_result *out, odk_error *err);

/* Caps- and meta-aware dispatcher; NULL caps/meta
 * equals the base function. */
odk_status odk_evaluate_query_opts(odk_kernel *kernel,
                                  const odk_query *query,
                                  const char *context, const odk_image *image,
                                  const odk_output_caps *caps,
                                  odk_result *out, odk_result_meta *meta,
                                  odk_error *err);

/* Per-call language variants of the six
 * evaluators.  Behavior is identical to the _opts forms except that
 * `call` (may be NULL = kernel setting) can override the language for
 * this call, INCLUDING ODK_LANG_AUTO.  Decisions with the default
 * language are bit-identical to the _opts forms.  err/meta may be
 * NULL. */
odk_status odk_evaluate_choice_call(odk_kernel *kernel,
                                    const char *instruction,
                                    const char *const *options,
                                    size_t option_count, const char *context,
                                    const odk_image *image,
                                    const odk_call_config *call,
                                    const odk_output_caps *caps,
                                    odk_result *out, odk_result_meta *meta,
                                    odk_error *err);
odk_status odk_evaluate_score_call(odk_kernel *kernel,
                                   const char *instruction,
                                   const char *const *rubric,
                                   size_t rubric_count, const char *context,
                                   const odk_image *image,
                                   const odk_call_config *call,
                                   const odk_output_caps *caps,
                                   odk_result *out, odk_result_meta *meta,
                                   odk_error *err);
odk_status odk_evaluate_noul_call(odk_kernel *kernel,
                                  const char *statement,
                                  const char *context, const odk_image *image,
                                  const odk_call_config *call,
                                  const odk_output_caps *caps,
                                  odk_result *out, odk_result_meta *meta,
                                  odk_error *err);
odk_status odk_evaluate_rank_call(odk_kernel *kernel,
                                  const char *instruction,
                                  const char *const *options,
                                  size_t option_count,
                                  const char *context, const odk_image *image,
                                  const odk_call_config *call,
                                  const odk_output_caps *caps,
                                  odk_result *out, odk_result_meta *meta,
                                  odk_error *err);
odk_status odk_evaluate_compare_call(odk_kernel *kernel,
                                     const char *instruction,
                                     const char *left, const char *right,
                                     const char *context,
                                     const odk_image *image,
                                     const odk_call_config *call,
                                     const odk_output_caps *caps,
                                     odk_result *out, odk_result_meta *meta,
                                     odk_error *err);
odk_status odk_evaluate_extract_call(odk_kernel *kernel,
                                     const char *instruction,
                                     const char *statement,
                                     const char *context,
                                     const odk_image *image,
                                     int top_k, const odk_call_config *call,
                                     const odk_output_caps *caps,
                                     odk_result *out, odk_result_meta *meta,
                                     odk_error *err);
odk_status odk_evaluate_query_call(odk_kernel *kernel,
                                   const odk_query *query,
                                   const char *context,
                                   const odk_image *image,
                                   const odk_call_config *call,
                                   const odk_output_caps *caps,
                                   odk_result *out, odk_result_meta *meta,
                                   odk_error *err);

/* ------------------------------------------------------------------ */
/* Runtime lexicon / stopword loaders                */
/* ------------------------------------------------------------------ */

/* Load a runtime stopword pack from a strict JSON file:
 *   {"stopwords": ["w1", "w2", ...]}
 * The loaded list REPLACES the kernel's compiled-in pack for the
 * kernel's configured language (a per-kernel setting; no globals).
 * Hard caps: file <= 1 MB, <= 5,000 entries, entry <= 64 bytes;
 * entries must be non-empty UTF-8; duplicates are collapsed.
 * Hostile input (truncated JSON, nesting, NULs, huge entries) fails
 * with ODK_ERR_ARGUMENT and a diagnostic in err (may be NULL).
 * Returns ODK_OK on success.  Deterministic order of first load
 * wins; calling twice replaces the previous list. */
/* load a scoring model file (docs/MODEL.md, 128-byte v1 format).
 * Replaces the kernel's model on success; on ANY failure (bad magic,
 * version, size, NaN/inf weights, hash mismatch, unsupported feature
 * set) the kernel's model is left unchanged and ODK_ERR_ARGUMENT (or
 * ODK_ERR_INFERENCE for I/O problems) is returned.  Loading the default
 * constants as a file reproduces the no-model behaviour bit-for-bit. */
odk_status odk_kernel_load_model(odk_kernel *kernel, const char *path,
                                 odk_error *err);

odk_status odk_kernel_load_stopwords(odk_kernel *kernel, const char *path,
                                     odk_error *err);

/* Load a runtime lexicon from a strict JSON file (all keys optional):
 *   {"stopwords": [...],                      (as above)
 *    "concepts": {"name": ["w1", "w2", ...]}, (seed PINNED word links;
 *                                             link_weight 0 = inactive
 *                                             until rewarded)
 *    "words": ["词1", "词2", ...]}             (Chinese word list for
 *                                             zh lexicon mode)
 * Same caps per section as odk_kernel_load_stopwords, plus <= 200
 * concepts and <= 5,000 words.  Concept words may be in ANY language
 * (cross-script links are the documented mechanism).  Strict
 * parsing: hostile input fails with ODK_ERR_ARGUMENT. */
odk_status odk_kernel_load_lexicon(odk_kernel *kernel, const char *path,
                                   odk_error *err);

/* Pin every word pair of every loaded concept into the brain
 * (cross-script links are the documented mechanism).  Pinned
 * links stay inactive until rewarded outcomes activate them, so
 * link_weight = 0 keeps decisions bit-identical to no links.
 * Returns ODK_ERR_ARGUMENT when no concept lexicon is loaded or no
 * pair could be pinned; ODK_OK otherwise. */
odk_status odk_kernel_lexicon_seed_links(const odk_kernel *kernel,
                                         odk_brain *brain, odk_error *err);
size_t odk_kernel_lexicon_concept_count(const odk_kernel *kernel);
size_t odk_kernel_runtime_stopword_count(const odk_kernel *kernel);


/* ------------------------------------------------------------------ */
/* Markdown minifier                                                   */
/* ------------------------------------------------------------------ */

/* Return the compressed Markdown for text (NULL -> "").  Malloc'd result,
 * caller frees; NULL only on allocation failure. */
char *odk_minify(const char *text);

/* Mode-aware variant: UNICODE mode trims on the White_Space property
 * (ASCII text behaves identically to odk_minify). */
char *odk_minify_ex(const char *text, odk_text_mode mode);

/* Heuristic token estimate (""/NULL -> 0): max(chars/4, words*1.3),
 * rounded half-even, always >= 1 for non-empty input. */
int odk_estimate_tokens(const char *text);

/* Mode-aware variant (word boundaries on Unicode whitespace). */
int odk_estimate_tokens_ex(const char *text, odk_text_mode mode);

typedef struct {
    int raw_tokens;
    int compressed_tokens;
    double reduction_pct;
} odk_token_report_t;

void odk_token_report(const char *original, const char *minified,
                      odk_token_report_t *out);

/* ------------------------------------------------------------------ */
/* Client pipeline (five-key evaluate contract)                        */
/* ------------------------------------------------------------------ */

typedef struct odk_client odk_client;

/* device_preference: "auto" | "cpu" | "cuda" | "gpu" (NULL -> "auto").
 * This library has no GPU backends, so the resolved plan is always
 * CPU. */
odk_client *odk_client_create(const char *device_preference);
void odk_client_free(odk_client *client);

typedef struct {
    char *key;          /* owned query key                              */
    odk_result result;  /* zeroed (ODK_VALUE_NONE) when error != NULL   */
    char *error;        /* owned diagnostic or NULL                     */
} odk_decision;

typedef struct {
    char *file_name;          /* owned                                  */
    char *file_type;          /* owned ("text")                         */
    char *extracted_markdown; /* owned (minified unless kernel_only)    */
    char **meta_keys;         /* owned, insertion-ordered               */
    char **meta_values;       /* owned; each value is a JSON literal
                               * (number / "quoted string" / true / false /
                               * null / [array])                        */
    size_t meta_count;
    odk_decision *decisions;  /* owned array                            */
    size_t decision_count;
} odk_output;

void odk_output_release(odk_output *output);

/* Metadata lookup: returns the JSON literal for key, or NULL. */
const char *odk_output_meta_get(const odk_output *output, const char *key);

/* Serialise the exact five-root-key contract:
 * {"file_name","file_type","metadata","extracted_markdown","decisions"}.
 * Malloc'd, caller frees; NULL only on allocation failure. */
char *odk_output_to_json(const odk_output *output);

/* Full pipeline on a direct string input (the C client intentionally
 * supports inline text only; the host application converts files and
 * bytes to text).
 * queries may be NULL/empty.  analysis_mode: hybrid|parser_only|kernel_only
 * (NULL -> hybrid, unknown -> hybrid + recorded warning). */
odk_status odk_client_evaluate(odk_client *client,
                               const char *input_text,
                               const odk_query *const *queries,
                               size_t query_count,
                               const char *analysis_mode,
                               odk_output *out, odk_error *err);

/* Convenience wrapper: queries_json is a JSON object
 * {"key": {primitive...}, ...} preserving insertion order; on success
 * *out_json receives the five-key contract JSON (malloc'd, caller frees). */
odk_status odk_client_evaluate_json(odk_client *client,
                                    const char *input_text,
                                    const char *queries_json,
                                    const char *analysis_mode,
                                    char **out_json, odk_error *err);

/* ------------------------------------------------------------------ */
/* Output policy                               */
/* ------------------------------------------------------------------ */

/* Controls how much of the (minified) input is echoed back in
 * extracted_markdown.  Scoring is INDEPENDENT of the policy: the
 * result/confidence/distribution values are bit-identical under every
 * mode (a guaranteed property) - only echoed text changes. */
typedef enum {
    ODK_MARKDOWN_FULL = 0,     /* legacy behaviour (ASCII_COMPAT default) */
    ODK_MARKDOWN_TRUNCATE = 1, /* first N code points + "..." marker     */
    ODK_MARKDOWN_OMIT = 2,     /* "" (metadata still reports sizes)      */
    ODK_MARKDOWN_HASH = 3      /* length + stable 128-bit digest string  */
} odk_markdown_mode;

typedef struct {
    odk_markdown_mode markdown_mode;
    size_t markdown_truncate_chars; /* default 2,000, ceiling 100,000 */
} odk_output_policy;

/* Extended pipeline:
 *  - the client's kernel may be replaced with odk_client_attach_kernel
 *    (e.g. a UNICODE-mode kernel) - minification, token estimation and
 *    evaluation all follow it;
 *  - policy == NULL selects the mode default: FULL for ASCII_COMPAT
 *    (byte-identical legacy contract, no new metadata keys) and
 *    TRUNCATE@2,000 for UNICODE (with input_chars / kept_chars /
 *    output_chars / truncated metadata keys);
 *  - an explicit policy enables the new metadata keys in both modes. */
odk_status odk_client_evaluate_ex(odk_client *client,
                                  const char *input_text,
                                  const odk_query *const *queries,
                                  size_t query_count,
                                  const char *analysis_mode,
                                  const odk_output_policy *policy,
                                  odk_output *out, odk_error *err);

/* Convenience wrapper of odk_client_evaluate_ex with a queries JSON
 * object (same shape as odk_client_evaluate_json). */
odk_status odk_client_evaluate_json_ex(odk_client *client,
                                       const char *input_text,
                                       const char *queries_json,
                                       const char *analysis_mode,
                                       const odk_output_policy *policy,
                                       char **out_json, odk_error *err);

/* Per-call language variants (ODK_LANG_DEFAULT or
 * a concrete language / AUTO applies to every query; NULL call = the
 * kernel's configured language).  Behavior with the default language
 * is identical to the variants without a call config. */
odk_status odk_client_evaluate_json_lang(odk_client *client,
                                         const char *input_text,
                                         const char *queries_json,
                                         const char *analysis_mode,
                                         odk_language language,
                                         char **out_json, odk_error *err);
odk_status odk_client_evaluate_json_lang_ex(odk_client *client,
                                            const char *input_text,
                                            const char *queries_json,
                                            const char *analysis_mode,
                                            const odk_output_policy *policy,
                                            const odk_call_config *call,
                                            char **out_json,
                                            odk_error *err);
odk_status odk_client_evaluate_ex_lang(odk_client *client,
                                       const char *input_text,
                                       const odk_query *const *queries,
                                       size_t query_count,
                                       const char *analysis_mode,
                                       const odk_output_policy *policy,
                                       const odk_call_config *call,
                                       odk_output *out, odk_error *err);

/* Attach a kernel to the client (borrowed, never freed by the client):
 * subsequent evaluations use it instead of the internally created one.
 * Passing NULL detaches (back to the internal kernel). */
void odk_client_attach_kernel(odk_client *client, odk_kernel *kernel);

/* ------------------------------------------------------------------ */
/* Outcome learning - opt-in experience layer                  */
/* ------------------------------------------------------------------ */

/* Learned word links: the brain can learn from
 * rewarded outcomes that different context words behave as the same
 * evidence (e.g. "outage" and "downtime").  Deterministic, bounded,
 * auditable, reversible; never overrides strong direct evidence (the
 * influence bound is link_weight * link_cap, tested).  link_weight = 0
 * (the default) disables the whole mechanism bit-exactly. */
typedef struct odk_link_policy {
    double link_weight;      /* 0 = off (default); [0, 1]             */
    unsigned min_support;    /* min positive outcomes per token (4)    */
    unsigned min_contexts;   /* min distinct contexts per token (3)    */
    double active_threshold; /* strength needed to score (0.6)         */
    double link_cap;         /* max channel contribution (0.5)         */
    double link_gain;        /* per-pair gain (0.5)                    */
    double generic_fraction; /* contexts fraction => generic (0.5)     */
    size_t max_links;        /* per token (16)                         */
    size_t max_tokens;       /* tracked tokens (10,000)                */
    size_t max_total_links;  /* total links (50,000)                   */
    int decay_half_life;     /* epochs; 0 = off (default)              */
} odk_link_policy;

/* Fill a policy with the documented defaults. */
void odk_link_policy_defaults(odk_link_policy *policy);


/* The learning brain is a persistent, pure-data experience layer on top of
 * the deterministic kernel.  It never touches the stateless kernel
 * paths: with weight 0.0 (or without a brain at all) every result is
 * bit-identical to plain odk_evaluate_choice.
 *
 * Determinism contract: identical (history, brain snapshot) always yields
 * identical decisions - no RNG and no wall-clock input anywhere.
 *
 * Data model:
 *   - memory entry per (primitive, instruction, option) canonical key
 *     with wins / losses / draws / visits and an EMA of past rewards
 *   - advise(): per-option bonus in [-1, 1] = shrunk net win rate
 *     ((wins-losses)/visits) * visits/(visits+4); 0.0 for unseen or
 *     sparsely-visited (< 2) entries
 *   - evaluate_choice(): base evidence scores from the kernel, shifted
 *     with base + weight * bonus inside a wide [-1, 2] band - so repeated
 *     penalties can demote a zero-evidence option BELOW its peers, and
 *     repeated rewards can promote one above them - then re-calibrated
 *     with the standard softmax distribution (five-key shape preserved).
 *     Weight 0.0 reproduces odk_evaluate_choice bit for bit
 *   - hard evidence is never overridden by noise: bonuses are bounded and
 *     shrink to zero without repeated confirmation */

typedef enum odk_match_outcome {
    ODK_MATCH_WIN = 0,
    ODK_MATCH_LOSS = 1,
    ODK_MATCH_DRAW = 2
} odk_match_outcome;

typedef struct odk_brain odk_brain;

/* Install the link policy (NULL = defaults, link_weight 0 = off).
 * Record-with-context is required for learning; see
 * odk_brain_record_outcome_ctx. */
int odk_brain_set_link_policy(odk_brain *brain,
                              const odk_link_policy *policy, odk_error *err);

/* Read back the effective link policy (defaults when none was set). */
int odk_brain_get_link_policy(const odk_brain *brain, odk_link_policy *out);

/* Record one outcome WITH its decision context: besides the memory-key
 * update, the filtered context tokens update the
 * token-option association table that word links are learned from.
 * context NULL/"" = plain odk_brain_record_outcome (no link learning). */
int odk_brain_record_outcome_ctx(odk_brain *brain, const char *primitive,
                                 const char *instruction, const char *option,
                                 const char *context, double reward,
                                 odk_error *err);

/* Recompute candidate/active links from the associations (deterministic;
 * also run automatically at snapshot save). */
int odk_brain_links_maintain(odk_brain *brain, odk_error *err);

/* Human-readable JSON report of every link (status, strength, support,
 * contexts) for review.  Malloc'd, caller frees. */
int odk_brain_links_report(const odk_brain *brain, char **json_out,
                           odk_error *err);

/* Curation: pin (always active, survives pruning), ban (never forms),
 * unpin.  Order-insensitive token pair. */
int odk_brain_link_pin(odk_brain *brain, const char *a, const char *b,
                       odk_error *err);
int odk_brain_link_ban(odk_brain *brain, const char *a, const char *b,
                       odk_error *err);
int odk_brain_link_unpin(odk_brain *brain, const char *a, const char *b,
                         odk_error *err);

/* Text configuration for the brain's own tokenization (memory keys and
 * link learning).  Default ASCII_COMPAT + EN = legacy behaviour. */
int odk_brain_set_text_config(odk_brain *brain, odk_text_mode mode,
                              odk_stopword_pack pack, odk_error *err);

/* The brain's own language.  UND (default) keeps
 * the exact baseline memory keys.  Changing the language of a non-empty
 * brain is refused (keys are language-sensitive).  Snapshots record
 * the language (an optional field of the v3 snapshot format); loading
 * with a different expected language fails loudly, and v1-v3 snapshots
 * without the field load as UND. */
odk_status odk_brain_set_language(odk_brain *brain, odk_language lang,
                                  odk_error *err);
odk_language odk_brain_language(const odk_brain *brain);
odk_brain *odk_brain_load_file_lang(const char *path, odk_text_mode mode,
                                    odk_stopword_pack pack, odk_language lang,
                                    odk_error *err);

/* Memory-key limits.  key_part_max is the per-part
 * canonical-text bound (default 4096, clamped to [64, 65536]); parts of
 * at most 199 bytes stay byte-identical to the legacy keys.  Every key
 * also carries a 128-bit hash of the FULL canonical text, so longer
 * parts never collide.  Returns previous key_part_max, or 0 on bad
 * arguments (NULL brain). */
size_t odk_brain_set_key_limits(odk_brain *brain, size_t key_part_max);

/* Number of key builds whose canonical parts exceeded key_part_max
 * since the brain was created or loaded (never-silent truncation
 * reporting; also stored in snapshot v3 and the summary JSON). */
size_t odk_brain_key_truncations(const odk_brain *brain);

/* Learning evaluation with links + optional explain: identical to
 * odk_brain_evaluate_choice, plus (when explain_json is non-NULL) a
 * malloc'd JSON array of the links that fired and their soft
 * contributions.  weight 0 and/or no active links reproduces
 * odk_evaluate_choice bit for bit. */
int odk_brain_evaluate_choice_ex(odk_brain *brain, odk_kernel *kernel,
                                 const char *instruction,
                                 const char *const *options,
                                 size_t option_count, const char *context,
                                 const odk_image *image, double weight,
                                 char **explain_json, odk_result *out,
                                 odk_error *err);

odk_brain *odk_brain_create(void);
void odk_brain_free(odk_brain *brain);

/* Snapshot to / restore from a JSON file (snapshot format v3; v1 and v2
 * files still load).  Saving is
 * atomic: the payload is written to "<path>.tmp" and renamed.  Loading
 * ignores unknown fields and caps hostile inputs (entry count, key
 * length); a structurally broken file yields ODK_ERR_ARGUMENT. */
odk_status odk_brain_save_file(const odk_brain *brain, const char *path,
                               odk_error *err);
odk_brain *odk_brain_load_file(const char *path, odk_error *err);

/* Load with an EXPECTED text configuration.  A v2/v3 snapshot whose
 * text_mode / stopword pack / Unicode version disagrees with the
 * expectation fails LOUDLY (ODK_ERR_ARGUMENT with an explicit message)
 * instead of silently mis-keying memory; v1 snapshots migrate
 * transparently (they carry no text-layer provenance). */
odk_brain *odk_brain_load_file_ex(const char *path, odk_text_mode mode,
                                  odk_stopword_pack pack, odk_error *err);

/* Save/load variants with EXTENDED diagnostics.  The
 * odk_error_ex message carries the full untruncated caller data (e.g.
 * the whole path); the legacy odk_error functions keep their bounded
 * excerpt + "... [truncated]" marker.  Semantics otherwise identical
 * to odk_brain_save_file / odk_brain_load_file_ex.  err == NULL is
 * allowed and simply discards the diagnostic. */
odk_status odk_brain_save_file_ex(const odk_brain *brain, const char *path,
                                  odk_error_ex *err);
odk_brain *odk_brain_load_file_ex2(const char *path, odk_text_mode mode,
                                  odk_stopword_pack pack,
                                  odk_error_ex *err);

/* Record one decision outcome.  reward is clamped to [-1, 1]:
 * >= +0.05 counts a win, <= -0.05 a loss, otherwise a draw.  The memory
 * key is derived from (primitive, instruction, option); repeated keys
 * accumulate.  ODK_ERR_ARGUMENT for NULL primitive/option. */
odk_status odk_brain_record_outcome(odk_brain *brain, const char *primitive,
                                    const char *instruction,
                                    const char *option, double reward,
                                    odk_error *err);

/* Record one finished match for the aggregate epoch/record counters. */
odk_status odk_brain_record_match(odk_brain *brain, odk_match_outcome outcome,
                                  odk_error *err);

/* Experience bonus per option, in [-1, 1] (see data model above).
 * bonuses receives option_count doubles; options with no usable memory
 * get 0.0.  A NULL brain is "no experience yet" and yields zeros.
 * Deterministic; const, so it is safe beside concurrent evaluations. */
odk_status odk_brain_advise(const odk_brain *brain, const char *primitive,
                            const char *instruction,
                            const char *const *options, size_t option_count,
                            double *bonuses, odk_error *err);

/* Learning evaluation of choice: base evidence scores are computed by the
 * kernel's own option scorer, shifted with base + weight * bonus (clamped
 * to [-1, 2], which never binds at weight <= 1) and re-calibrated to the
 * standard softmax distribution.  The output has the exact five-key
 * choice shape (release with odk_result_release).
 * weight 0.0 reproduces odk_evaluate_choice bit for bit. */
odk_status odk_brain_evaluate_choice(odk_brain *brain, odk_kernel *kernel,
                                     const char *instruction,
                                     const char *const *options,
                                     size_t option_count, const char *context,
                                     const odk_image *image, double weight,
                                     odk_result *out, odk_error *err);

/* Additive stats payload (malloc'd, caller frees):
 * {"epoch":N,"matches":N,"record":{"wins":W,"losses":L,"draws":D},
 *  "memory_entries":M,"memory_visits":V,
 *  "top":[{"key":...,"wins":..,"losses":..,"draws":..,"visits":..,
 *          "ema":..,"bonus":..}, ... up to 5 by visits]} */
char *odk_brain_summary_json(const odk_brain *brain);

/* ------------------------------------------------------------------ */
/* Migration kit - review and re-key legacy entries.        */
/* Additive API: no existing signature or behaviour changes.          */
/* ------------------------------------------------------------------ */

/* Full memory listing with provenance flags (malloc'd, caller frees):
 * {"memory_entries":M,"legacy_entries":L,"key_truncations":T,
 *  "entries":[{"index":i,"key":"prim\x1finstr\x1fopt","legacy":bool,
 *              "truncated":bool,"wins":..,"losses":..,"draws":..,
 *              "visits":..,"ema":..,"bonus":..}, ...]}
 * The key text is JSON-escaped; \x1f appears escaped.  This is the
 * review step of the v1/v2 migration path (see docs/LEARNING.md): legacy entries
 * (key parts at or beyond the 199-byte legacy truncation boundary) are
 * listed with legacy:true so the owner can decide per entry. */
char *odk_brain_entries_report_alloc(const odk_brain *brain);

/* Re-key the entry at INDEX to new_key - the FULL canonical key text
 * "primitive\x1finstruction\x1foption" (the 128-bit full-text hash is
 * recomputed from it; parts must each fit the brain's key_part_max).
 * new_key is VALIDATED - exactly three \x1f-separated
 * parts, each already in canonical form for this brain (re-
 * canonicalising each part yields itself); a hand-built key that
 * disagrees with the canonical form is refused with a clear error
 * instead of silently stranding the experience.  Build keys with
 * odk_brain_canonical_key_alloc, or canonicalise in one step with
 * odk_brain_rekey_entry_raw.  Clears the legacy flag: the entry is a
 * first-class entry from here on.  Refuses a key that already
 * exists ANYWHERE in the brain - including the entry's own current
 * key (no silent merge, and a no-op re-key would hide caller bugs in
 * migration tooling).  INDEX refers to the
 * odk_brain_entries_report_alloc listing. */
odk_status odk_brain_rekey_entry(odk_brain *brain, size_t index,
                                 const char *new_key, odk_error *err);

/* The exact FULL canonical key text this brain builds for
 * raw (primitive, instruction, option) inputs - the untruncated
 * canonical parts joined by \x1f, honoring the brain's text mode and
 * stopword pack (a NULL brain uses the ASCII_COMPAT/EN defaults of
 * the no-learning advise path).  This is the text the brain HASHES;
 * re-keying an entry additionally requires every part to fit the
 * brain's key_part_max (odk_brain_rekey_entry enforces that).  Use it
 * to preview keys, to diff hand-built migration keys, or to log what
 * a raw triple maps to.  Returns a malloc'd string (caller frees);
 * NULL + err only on allocation failure. */
char *odk_brain_canonical_key_alloc(const odk_brain *brain,
                                    const char *primitive,
                                    const char *instruction,
                                    const char *option, odk_error *err);

/* Canonicalise (primitive, instruction, option) under
 * this brain's rules and re-key the entry at INDEX to the result.
 * Equivalent to odk_brain_canonical_key_alloc + odk_brain_rekey_entry
 * in one atomic step; the same refusals apply (target key exists,
 * parts exceed key_part_max).  INDEX refers to the
 * odk_brain_entries_report_alloc listing. */
odk_status odk_brain_rekey_entry_raw(odk_brain *brain, size_t index,
                                     const char *primitive,
                                     const char *instruction,
                                     const char *option, odk_error *err);

/* Remove the entry at INDEX (order-preserving, failure-atomic). */
odk_status odk_brain_discard_entry(odk_brain *brain, size_t index,
                                   odk_error *err);

/* Aggregate counters: epoch equals the number of learned matches. */
size_t odk_brain_epoch(const odk_brain *brain);
void odk_brain_record_get(const odk_brain *brain, size_t *wins,
                          size_t *losses, size_t *draws);

/* Forget everything (memory, record, epoch). */
void odk_brain_reset(odk_brain *brain);

/* ------------------------------------------------------------------ */
/* JSON utilities (also exposed for host applications)                       */
/* ------------------------------------------------------------------ */

typedef enum odk_json_type {
    ODK_JSON_NULL = 0,
    ODK_JSON_BOOL,
    ODK_JSON_NUMBER,
    ODK_JSON_STRING,
    ODK_JSON_ARRAY,
    ODK_JSON_OBJECT
} odk_json_type;

typedef struct odk_json_value {
    odk_json_type type;
    int boolean;                     /* ODK_JSON_BOOL      */
    double number;                   /* ODK_JSON_NUMBER    */
    int is_integer;                  /* literal had no . e E and fits int64 */
    long long integer;               /* valid when is_integer            */
    char *string;                    /* ODK_JSON_STRING (owned)          */
    struct odk_json_value **items;   /* ODK_JSON_ARRAY                   */
    size_t item_count;
    char **keys;                     /* ODK_JSON_OBJECT, insertion order */
    struct odk_json_value **members; /* ODK_JSON_OBJECT                  */
    size_t member_count;
} odk_json_value;

/* Strict JSON parser (RFC 8259): returns NULL and fills *err on any
 * syntax error, depth overflow (> 64) or trailing garbage. */
odk_json_value *odk_json_parse(const char *text, size_t len, odk_error *err);
void odk_json_free(odk_json_value *value);

/* Object member lookup (NULL when absent). */
const odk_json_value *odk_json_object_get(const odk_json_value *object,
                                          const char *key);

/* Serialisation helpers; all return malloc'd strings (NULL on OOM). */
char *odk_json_dump(const odk_json_value *value);
char *odk_json_string_literal(const char *utf8, size_t len);
char *odk_json_number_literal(double value);
char *odk_json_int_literal(long long value);

#ifdef __cplusplus
}
#endif

#endif /* OMNIDECIDE_H */
