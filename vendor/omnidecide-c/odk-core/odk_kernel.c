/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_kernel.c - the embedded deterministic decision engine + all six
 * primitives.
 *
 * There is no neural backbone: no model or weight files are loaded and
 * the deterministic engine is the only backend.
 *
 * Floating-point fidelity: the arithmetic expression order is fixed
 * (insertion-ordered counter iteration, clamps, softmax peak
 * subtraction, stable sigmoid), so results are bit-for-bit
 * reproducible on the same libm.
 *
 */

#include "odk_internal.h"
#include "odk_links.h"
#include "odk_lang.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* -- Tuning constants ------------------------------------------------ */

#define ODK_MAX_CONTEXT_CHARS 20000
#define ODK_NOUL_LOGIT_SCALE 8.0
#define ODK_EARLY_EXIT_SIMILARITY 0.95
#define ODK_MAX_CHUNKS 2000
#define ODK_COMPARE_TIE_MARGIN 0.02
#define ODK_EXTRACT_SNIPPET_MAX_CHARS 240
#define ODK_MIN_RANK_OPTIONS 2
#define ODK_COMPARE_REQUIRED_OPTIONS 2
#define ODK_NOUL_DECISION_THRESHOLD 0.5
#define ODK_MIN_TRIGRAM_CHARS 3
#define ODK_MIN_CHUNK_CHARS 3

/* Text-layer configuration (public odk_kernel_config).
 * Hard ceilings clamp hostile configuration values. */
#define ODK_CTX_CEILING 10000000u
#define ODK_CHUNK_SRC_CEILING 20000000u
#define ODK_CHUNKS_CEILING 100000u
#define ODK_SNIPPET_CEILING 10000u
#define ODK_UNICODE_CTX_DEFAULT 100000u
#define ODK_UNICODE_CHUNK_SRC_DEFAULT 200000u
#define ODK_UNICODE_CHUNKS_DEFAULT 10000u
#define ODK_UNICODE_SNIPPET_DEFAULT 1000u
/* Ceilings and defaults. */
#define ODK_RANKED_CEILING 10000u
#define ODK_SNIPPET_CAP_CEILING 10000u
#define ODK_TOTAL_CHARS_CEILING 100000u
#define ODK_WORK_CEILING 2000000000u
#define ODK_TOTAL_CHARS_DEFAULT 10000u
#define ODK_WORK_CHUNK_UNIT 100u /* chunks_scored multiplier */

/* capture of the three lexical channels (explain API).  Written
 * behind optional out-parameters only; never read by the decision path. */
typedef struct {
    double cosine;
    double jaccard;
    double trigram;
} sim_parts_t;

/* Per-target capture for the explain API: the (target, anchor) channels
 * and the two context scores of one score_against_context_l call. */
typedef struct {
    sim_parts_t whole_parts;
    double whole;
    double best_chunk;
} explain_capture_t;

typedef struct {
    odk_text_mode mode;
    odk_stopword_pack pack;
    odk_input_profile profile;
    size_t context_max;      /* bytes (ASCII) / code points (UNICODE)  */
    size_t chunk_source_max;
    size_t max_chunks;
    size_t snippet_max;
    odk_over_limit_strategy over_limit; /* resolved enum         */
    size_t max_ranked;       /* 0 = all options                  */
    size_t max_snippet_chars;/* 0 = snippet_max                  */
    size_t max_total_chars;  /* 0 = 10,000 cps                   */
    size_t max_work;         /* 0 = derived                  */
    odk_language language;   /* UND = baseline generic   */
    const odk_rt_text *rt;   /* runtime-loaded pack */
    int morph_light;         /* light morphology in the matching
                                channel (brain keys stay canonical)*/
    int ctx_explicit;        /* user set context_max (profile defaults)*/
    int chunk_explicit;      /* user set chunk_source_max             */
    const odk_model *model;  /* scoring constants (never NULL)  */
} kernel_text_cfg;

static const kernel_text_cfg kCompatCfg = {
    ODK_TEXT_ASCII_COMPAT, ODK_STOPWORDS_EN, ODK_PROFILE_PLAIN,
    ODK_MAX_CONTEXT_CHARS, ODK_MAX_CONTEXT_CHARS * 2,
    ODK_MAX_CHUNKS, ODK_EXTRACT_SNIPPET_MAX_CHARS,
    ODK_OVERLIMIT_PROFILE_DEFAULT, 0, 0, 0, 0, ODK_LANG_UND, NULL, 0, 0, 0,
    &odk_default_model
};

static const kernel_text_cfg kUnicodeDefaults = {
    ODK_TEXT_UNICODE, ODK_STOPWORDS_EN, ODK_PROFILE_PLAIN,
    ODK_UNICODE_CTX_DEFAULT, ODK_UNICODE_CHUNK_SRC_DEFAULT,
    ODK_UNICODE_CHUNKS_DEFAULT, ODK_UNICODE_SNIPPET_DEFAULT,
    ODK_OVERLIMIT_PROFILE_DEFAULT, 0, 0, 0, 0, ODK_LANG_UND, NULL, 0, 0, 0,
    &odk_default_model
};

/* the built-in default model equals today's constants exactly
 * (docs/MODEL.md).  With no model file loaded, every expression below is
 * character-for-character the original arithmetic. */
const odk_model odk_default_model = {
    0u, /* feature_set_id: built-in six-channel scoring              */
    0.35, 0.35, 0.30, /* pair channels: tf-cosine, Jaccard, trigram   */
    0.40, 0.60,       /* context blend: whole, best chunk             */
    0.70, 0.30,       /* fusion: contextual, directional              */
    6.0, 8.0,         /* calibration scales (choice family, noul)     */
    0, {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0} /* no extra features */
};

struct odk_kernel {
    char backend[24];
    kernel_text_cfg cfg;
    odk_rt_text *rt; /* runtime-loaded stopwords/lexicon (owned) */
    odk_model model; /* per-kernel scoring model (owns state)   */
};

/* Profile-default limits : applied when the user left
 * the field at its default AND the effective profile is not
 * PLAIN/HTML_TEXT.  Same numbers in both modes (units follow the mode). */
static size_t profile_ctx_default(odk_input_profile profile)
{
    switch (profile) {
    case ODK_PROFILE_CHAT:
        return 20000u;
    case ODK_PROFILE_LOG:
        return 200000u;
    case ODK_PROFILE_MARKDOWN:
    case ODK_PROFILE_JSON:
    case ODK_PROFILE_JSONL:
    case ODK_PROFILE_CSV:
    case ODK_PROFILE_TSV:
    case ODK_PROFILE_CODE:
    case ODK_PROFILE_EXTRACTED_PDF:
        return 100000u;
    default:
        return 0; /* PLAIN / HTML_TEXT / AUTO: mode default */
    }
}

static size_t profile_chunk_default(odk_input_profile profile)
{
    switch (profile) {
    case ODK_PROFILE_CHAT:
        return 40000u;
    case ODK_PROFILE_LOG:
        return 400000u;
    case ODK_PROFILE_MARKDOWN:
    case ODK_PROFILE_JSON:
    case ODK_PROFILE_JSONL:
    case ODK_PROFILE_CSV:
    case ODK_PROFILE_TSV:
    case ODK_PROFILE_CODE:
    case ODK_PROFILE_EXTRACTED_PDF:
        return 200000u;
    default:
        return 0; /* PLAIN / HTML_TEXT / AUTO: mode default */
    }
}

static const kernel_text_cfg *kernel_cfg(const odk_kernel *kernel);

/* Resolve PROFILE_DEFAULT to the concrete strategy:
 * head+tail for LOG, head everywhere else. */
static odk_over_limit_strategy effective_strategy(
    const kernel_text_cfg *cfg)
{
    switch (cfg->over_limit) {
    case ODK_OVERLIMIT_HEAD:
    case ODK_OVERLIMIT_HEAD_TAIL:
    case ODK_OVERLIMIT_SELECT:
        return cfg->over_limit;
    default:
        return cfg->profile == ODK_PROFILE_LOG ? ODK_OVERLIMIT_HEAD_TAIL
                                               : ODK_OVERLIMIT_HEAD;
    }
}

/* Effective configuration for one evaluation: resolves the AUTO
 * profile against the actual context and applies the profile-default
 * limits when the user left them at default.  Returns a STACK copy - a
 * plain fixed-profile kernel with explicit limits resolves to itself,
 * so legacy configurations are byte-identical. */
static kernel_text_cfg kernel_effective_cfg(const odk_kernel *kernel,
                                            const char *context)
{
    kernel_text_cfg cfg = *kernel_cfg(kernel);
    odk_input_profile profile = cfg.profile;

    /* Runtime-loaded data follows the kernel (no globals) */
    cfg.rt = (kernel != NULL) ? kernel->rt : NULL;
    /* the scoring model follows the kernel (default or loaded) */
    cfg.model = (kernel != NULL) ? &kernel->model : &odk_default_model;

    if (profile == ODK_PROFILE_AUTO && context != NULL &&
        context[0] != '\0') {
        profile = odk_detect_input_profile(context, NULL, 0);
        cfg.profile = profile;
    }
    if (!cfg.ctx_explicit) {
        size_t def = profile_ctx_default(profile);
        if (def != 0) {
            cfg.context_max = def;
        }
    }
    if (!cfg.chunk_explicit) {
        size_t def = profile_chunk_default(profile);
        if (def != 0) {
            cfg.chunk_source_max = def;
        }
    }
    return cfg;
}

/* Call-config-aware effective configuration.  An
 * explicit per-call language (anything but ODK_LANG_DEFAULT) wins
 * over the kernel config; NULL call keeps the kernel setting.
 * ODK_LANG_AUTO stays AUTO here - the tokenizer resolves it per text
 * and the UNICODE chunker resolves it once for the ender set. */
static kernel_text_cfg kernel_effective_cfg_call(
    const odk_kernel *kernel, const char *context,
    const odk_call_config *call)
{
    kernel_text_cfg cfg = kernel_effective_cfg(kernel, context);
    if (call != NULL && call->language != ODK_LANG_DEFAULT) {
        cfg.language = call->language;
    }
    if (call != NULL && call->morph_light) {
        cfg.morph_light = 1;
    }
    return cfg;
}

/* Fill the language metadata fields (no-op when
 * meta is NULL; zeroed meta = UND).  AUTO additionally reports the
 * detector verdict on the context text. */
static void fill_language_meta(odk_result_meta *meta,
                               const kernel_text_cfg *eff,
                               const char *context)
{
    if (meta == NULL) {
        return;
    }
    meta->language = eff->language;
    meta->detected = ODK_LANG_UND;
    meta->detected_confidence = 0.0;
    meta->detected_signal[0] = '\0';
    if (eff->language == ODK_LANG_AUTO) {
        const char *text = (context != NULL) ? context : "";
        meta->detected = odk_lang_detect(
            text, &meta->detected_confidence, meta->detected_signal,
            sizeof(meta->detected_signal));
    }
}

/* Derived work budget: context_max + chunk_source_max +
 * 100 * max_chunks over the EFFECTIVE limits - equals the worst-case
 * head-mode usage, so the default never binds. */
static size_t derived_work_budget(const kernel_text_cfg *cfg)
{
    return cfg->context_max + cfg->chunk_source_max +
           ODK_WORK_CHUNK_UNIT * cfg->max_chunks;
}

static size_t effective_work_budget(const kernel_text_cfg *cfg)
{
    if (cfg->max_work != 0 && cfg->max_work <= ODK_WORK_CEILING) {
        return cfg->max_work;
    }
    if (cfg->max_work > ODK_WORK_CEILING) {
        return ODK_WORK_CEILING;
    }
    return derived_work_budget(cfg);
}

static const kernel_text_cfg *kernel_cfg(const odk_kernel *kernel)
{
    return kernel != NULL ? &kernel->cfg : &kCompatCfg;
}

odk_kernel *odk_kernel_create(void)
{
    odk_kernel *kernel = calloc(1, sizeof(*kernel));
    if (kernel == NULL) {
        return NULL;
    }
    snprintf(kernel->backend, sizeof(kernel->backend), "embedded-fallback");
    kernel->cfg = kCompatCfg;
    kernel->model = odk_default_model; /* built-in default */
    return kernel;
}

odk_kernel *odk_kernel_create_ex(const odk_kernel_config *cfg)
{
    odk_kernel *kernel = odk_kernel_create();
    if (kernel == NULL) {
        return NULL;
    }
    if (cfg != NULL) {
        kernel->cfg = kUnicodeDefaults;
        if (cfg->text_mode == ODK_TEXT_ASCII_COMPAT) {
            kernel->cfg = kCompatCfg;
        }
        kernel->cfg.pack = cfg->stopwords;
        kernel->cfg.profile = cfg->profile;
        if (cfg->context_max != 0) {
            kernel->cfg.context_max = cfg->context_max;
        }
        if (cfg->chunk_source_max != 0) {
            kernel->cfg.chunk_source_max = cfg->chunk_source_max;
        }
        if (cfg->max_chunks != 0) {
            kernel->cfg.max_chunks = cfg->max_chunks;
        }
        if (cfg->snippet_max != 0) {
            kernel->cfg.snippet_max = cfg->snippet_max;
        }
        kernel->cfg.ctx_explicit = cfg->context_max != 0;
        kernel->cfg.chunk_explicit = cfg->chunk_source_max != 0;
        /* Fields: appended to the public struct, so
         * zero-initializing a legacy-style config selects every default. */
        if (cfg->over_limit >= ODK_OVERLIMIT_PROFILE_DEFAULT &&
            cfg->over_limit <= ODK_OVERLIMIT_SELECT) {
            kernel->cfg.over_limit = cfg->over_limit;
        } else {
            kernel->cfg.over_limit = ODK_OVERLIMIT_PROFILE_DEFAULT;
        }
        if (cfg->max_ranked != 0) {
            kernel->cfg.max_ranked = cfg->max_ranked;
        }
        if (cfg->max_snippet_chars != 0) {
            kernel->cfg.max_snippet_chars = cfg->max_snippet_chars;
        }
        if (cfg->max_total_chars != 0) {
            kernel->cfg.max_total_chars = cfg->max_total_chars;
        }
        /* Language.  Invalid values fall back to
         * UND (defensive: the enum is ABI-stable, but hostile struct
         * content must not select an out-of-range row). */
        if (cfg->language >= ODK_LANG_UND && cfg->language <= ODK_LANG_UR) {
            kernel->cfg.language = cfg->language;
        } else if (cfg->language == ODK_LANG_AUTO) {
            kernel->cfg.language = ODK_LANG_AUTO;
        } else {
            kernel->cfg.language = ODK_LANG_UND;
        }
        if (cfg->max_work != 0) {
            kernel->cfg.max_work = cfg->max_work;
        }
        /* Light morphology knob (0 = off = baseline token identity;
         * nonzero clamps to 1) */
        kernel->cfg.morph_light = cfg->morph_light ? 1 : 0;
        /* hostile-config clamps */
        if (kernel->cfg.context_max > ODK_CTX_CEILING) {
            kernel->cfg.context_max = ODK_CTX_CEILING;
        }
        if (kernel->cfg.chunk_source_max > ODK_CHUNK_SRC_CEILING) {
            kernel->cfg.chunk_source_max = ODK_CHUNK_SRC_CEILING;
        }
        if (kernel->cfg.max_chunks > ODK_CHUNKS_CEILING) {
            kernel->cfg.max_chunks = ODK_CHUNKS_CEILING;
        }
        if (kernel->cfg.snippet_max > ODK_SNIPPET_CEILING) {
            kernel->cfg.snippet_max = ODK_SNIPPET_CEILING;
        }
        if (kernel->cfg.max_ranked > ODK_RANKED_CEILING) {
            kernel->cfg.max_ranked = ODK_RANKED_CEILING;
        }
        if (kernel->cfg.max_snippet_chars > ODK_SNIPPET_CAP_CEILING) {
            kernel->cfg.max_snippet_chars = ODK_SNIPPET_CAP_CEILING;
        }
        if (kernel->cfg.max_total_chars > ODK_TOTAL_CHARS_CEILING) {
            kernel->cfg.max_total_chars = ODK_TOTAL_CHARS_CEILING;
        }
        if (kernel->cfg.max_work > ODK_WORK_CEILING) {
            kernel->cfg.max_work = ODK_WORK_CEILING;
        }
        /* Profile-default limits: fixed non-PLAIN
         * profiles with default limits get the measured defaults here
         * (the table REPLACES the mode default in both directions);
         * AUTO resolves per call in kernel_effective_cfg(). */
        if (!kernel->cfg.ctx_explicit) {
            size_t def = profile_ctx_default(kernel->cfg.profile);
            if (def != 0) {
                kernel->cfg.context_max = def;
            }
        }
        if (!kernel->cfg.chunk_explicit) {
            size_t def = profile_chunk_default(kernel->cfg.profile);
            if (def != 0) {
                kernel->cfg.chunk_source_max = def;
            }
        }
        /* pack validation: unknown values fall back to EN */
        if (cfg->stopwords < ODK_STOPWORDS_EN ||
            cfg->stopwords > ODK_STOPWORDS_NONE) {
            kernel->cfg.pack = ODK_STOPWORDS_EN;
        }
        /* profile validation: unknown values fall back to PLAIN */
        if (cfg->profile < ODK_PROFILE_PLAIN ||
            cfg->profile > ODK_PROFILE_AUTO) {
            kernel->cfg.profile = ODK_PROFILE_PLAIN;
        }
    }
    return kernel;
}

odk_text_mode odk_kernel_text_mode(const odk_kernel *kernel)
{
    return kernel != NULL ? kernel->cfg.mode : ODK_TEXT_ASCII_COMPAT;
}

odk_stopword_pack odk_kernel_stopwords(const odk_kernel *kernel)
{
    return kernel != NULL ? kernel->cfg.pack : ODK_STOPWORDS_EN;
}

odk_input_profile odk_kernel_profile(const odk_kernel *kernel)
{
    return kernel != NULL ? kernel->cfg.profile : ODK_PROFILE_PLAIN;
}

void odk_kernel_free(odk_kernel *kernel)
{
    if (kernel != NULL) {
        odk_lang_rt_free(kernel->rt);
    }
    free(kernel);
}

const char *odk_kernel_backend(const odk_kernel *kernel)
{
    return kernel != NULL ? kernel->backend : "embedded-fallback";
}

const char *odk_status_message(odk_status status)
{
    switch (status) {
    case ODK_OK:
        return "ok";
    case ODK_ERR_ARGUMENT:
        return "invalid argument";
    case ODK_ERR_INFERENCE:
        return "inference error";
    case ODK_ERR_MEMORY:
        return "out of memory";
    case ODK_ERR_INTERNAL:
        return "internal error";
    default:
        return "unknown status";
    }
}

/* Returns a malloc'd copy of the full
 * buffer text (caller frees), or NULL when err is NULL or empty.  Error
 * sites bound embedded caller data by construction, so the
 * buffer text is already the complete diagnostic. */
char *odk_error_message_alloc(const odk_error *err)
{
    size_t len;

    if (err == NULL) {
        return NULL;
    }
    len = strnlen(err->text, sizeof(err->text));
    if (len == 0) {
        return NULL;
    }
    return odk_strdup_range(err->text, len);
}

static void kernel_set_error(odk_error *err, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

static void kernel_set_error(odk_error *err, const char *fmt, ...)
{
    va_list args;
    if (err == NULL) {
        return;
    }
    va_start(args, fmt);
    vsnprintf(err->text, sizeof(err->text), fmt, args);
    va_end(args);
}

/* ------------------------------------------------------------------ */
/* Result construction helpers                                         */
/* ------------------------------------------------------------------ */

static void result_init(odk_result *out)
{
    memset(out, 0, sizeof(*out));
}

void odk_result_release(odk_result *result)
{
    size_t i;
    if (result == NULL) {
        return;
    }
    free(result->primitive);
    free(result->string);
    for (i = 0; i < result->item_count; i++) {
        free(result->items[i]);
    }
    free(result->items);
    for (i = 0; i < result->distribution.count; i++) {
        free(result->distribution.entries[i].key);
    }
    free(result->distribution.entries);
    result_init(result);
}

static int result_set_primitive(odk_result *out, const char *name)
{
    out->primitive = odk_strdup(name);
    return out->primitive != NULL ? 0 : -1;
}

static int result_set_distribution(odk_result *out, const char *const *labels,
                                   const double *values, size_t count)
{
    size_t i;
    size_t j;

    out->distribution.entries = NULL;
    out->distribution.count = 0;
    if (count == 0) {
        return 0;
    }
    out->distribution.entries = calloc(count, sizeof(odk_distribution_entry));
    if (out->distribution.entries == NULL) {
        return -1;
    }
    out->distribution.count = count;
    for (i = 0; i < count; i++) {
        const char *base = (labels[i] != NULL && labels[i][0] != '\0')
                               ? labels[i]
                               : "(blank)";
        size_t occurrence = 0;
        char suffixed[ODK_ERROR_MAX];
        for (j = 0; j < i; j++) {
            const char *prior = (labels[j] != NULL && labels[j][0] != '\0')
                                    ? labels[j]
                                    : "(blank)";
            if (strcmp(prior, base) == 0) {
                occurrence++;
            }
        }
        if (occurrence == 0) {
            out->distribution.entries[i].key = odk_strdup(base);
        } else {
            snprintf(suffixed, sizeof(suffixed), "%s#%zu", base,
                     occurrence + 1);
            out->distribution.entries[i].key = odk_strdup(suffixed);
        }
        if (out->distribution.entries[i].key == NULL) {
            return -1;
        }
        out->distribution.entries[i].value = odk_round4(values[i]);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Embedded deterministic semantic engine                              */
/* ------------------------------------------------------------------ */

static int tokens_to_counter(const str_list_t *tokens, cmap_t *map)
{
    size_t i;
    for (i = 0; i < tokens->count; i++) {
        if (cmap_increment(map, tokens->items[i],
                           strlen(tokens->items[i])) != 0) {
            return -1;
        }
    }
    return 0;
}


static double token_jaccard(const cmap_t *left, const cmap_t *right)
{
    size_t i;
    size_t intersection = 0;
    double union_size;

    for (i = 0; i < left->count; i++) {
        if (cmap_get(right, left->entries[i].key,
                     strlen(left->entries[i].key)) != 0) {
            intersection++;
        }
    }
    union_size = (double)(left->count + right->count) - (double)intersection;
    return intersection / (union_size > 1.0 ? union_size : 1.0);
}

/* Union size for the Jaccard channel (identical formula to
 * token_jaccard, exposed for the soft channel normalisation). */
static double token_jaccard_union(const cmap_t *left, const cmap_t *right)
{
    size_t i;
    size_t intersection = 0;
    for (i = 0; i < left->count; i++) {
        if (cmap_get(right, left->entries[i].key,
                     strlen(left->entries[i].key)) != 0) {
            intersection++;
        }
    }
    {
        double union_size =
            (double)(left->count + right->count) - (double)intersection;
        return union_size > 1.0 ? union_size : 1.0;
    }
}

/* ------------------------------------------------------------------ */
/* Per-evaluation text-analysis cache                     */
/*                                                                    */
/* The earlier scoring path rebuilt the SAME analyses over and over:    */
/* per option it re-tokenized and re-trigram-counted the anchor (a    */
/* context-sized text) and every chunk; per chunk similarity it       */
/* re-tokenized the option.  gprof on a 1 MB choice showed 6 full     */
/* anchor passes (3 options x {filtered, trigram}) and 3 passes per   */
/* chunk.  This cache analyses each distinct text (keyed by POINTER   */
/* identity - every text pointer is stable for the lifetime of one    */
/* evaluation) once and reuses the result.                            */
/*                                                                    */
/* Bit-identity: the tokens, token counter and trigram counter are    */
/* produced by exactly the same functions in exactly the same order   */
/* as the uncached path; every downstream consumer (cosine, jaccard,  */
/* trigram cosine, link soft channels, explain ordering) reads the    */
/* same cmap iteration order, so every double is computed identical-  */
/* ly.  Proven by the differential dump:                              */
/* Any change here must keep the token output byte-identical.        */
/*                                                                    */
/* Ownership: the cache owns all three parts of every slot; failures  */
/* leave slots half-built but freeable (str_list/cmap double-free is  */
/* safe: their free routines reset to the empty state).  No global    */
/* state; one cache per evaluation; thread-safety unchanged.         */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *text;    /* key: pointer identity within one evaluation */
    str_list_t raw_tokens;  /* unfiltered tokens (lazily built)         */
    str_list_t tokens;   /* filtered tokens (lazily built)              */
    cmap_t counter;      /* token counter (lazily built)                */
    cmap_t trigrams;     /* trigram counter (lazily built)              */
    int tokens_ready;
    int counter_ready;
    int trigrams_ready;
} text_analysis_t;

typedef struct {
    text_analysis_t *items;  /* slot storage (grows by realloc)         */
    size_t count;
    size_t cap;
    size_t *buckets;         /* open-addressing index: slot+1, 0=empty  */
    size_t bucket_count;     /* power of two, or 0 before first insert  */
} analysis_cache_t;

static void analysis_cache_init(analysis_cache_t *cache)
{
    cache->items = NULL;
    cache->count = 0;
    cache->cap = 0;
    cache->buckets = NULL;
    cache->bucket_count = 0;
}

static void analysis_cache_free(analysis_cache_t *cache)
{
    size_t i;
    for (i = 0; i < cache->count; i++) {
        str_list_free(&cache->items[i].raw_tokens);
        str_list_free(&cache->items[i].tokens);
        cmap_free(&cache->items[i].counter);
        cmap_free(&cache->items[i].trigrams);
    }
    free(cache->items);
    free(cache->buckets);
    cache->items = NULL;
    cache->count = 0;
    cache->cap = 0;
    cache->buckets = NULL;
    cache->bucket_count = 0;
}

/* Pointer hash (splitmix-style finalizer; deterministic). */
static uint64_t analysis_ptr_hash(const void *p)
{
    uint64_t x = (uint64_t)(uintptr_t)p;
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

/* Rebuild the bucket index after growth (or build it the first time).
 * Returns 0/-1. */
static int analysis_rehash(analysis_cache_t *cache, size_t new_count)
{
    size_t *grown = calloc(new_count, sizeof(size_t));
    size_t i;
    if (grown == NULL) {
        return -1;
    }
    free(cache->buckets);
    cache->buckets = grown;
    cache->bucket_count = new_count;
    for (i = 0; i < cache->count; i++) {
        size_t slot = (size_t)analysis_ptr_hash(cache->items[i].text) &
                      (new_count - 1);
        while (cache->buckets[slot] != 0) {
            slot = (slot + 1) & (new_count - 1);
        }
        cache->buckets[slot] = i + 1;
    }
    return 0;
}

/* Find (pointer identity) or create the analysis slot for text.
 * Returns the slot INDEX (the items array grows by realloc, so raw
 * pointers into it go stale on growth - callers must re-derive
 * pointers from the index AFTER all slot creation is done, and no
 * slot creation may happen while such pointers are live).  (size_t)-1
 * on allocation failure.  O(1) expected via the bucket index: the
 * first version of this cache used a linear pointer scan and the scan
 * itself became the dominant cost at 10k+ chunks (52k lookups over a
 * 17k-entry array). */
static size_t analysis_index(analysis_cache_t *cache, const char *text)
{
    size_t i;
    text_analysis_t *slot;
    size_t bucket;

    if (cache->bucket_count == 0) {
        if (analysis_rehash(cache, 16) != 0) {
            return (size_t)-1;
        }
    }
    bucket = (size_t)analysis_ptr_hash(text) & (cache->bucket_count - 1);
    for (;;) {
        size_t entry = cache->buckets[bucket];
        if (entry == 0) {
            break;
        }
        if (cache->items[entry - 1].text == text) {
            return entry - 1;
        }
        bucket = (bucket + 1) & (cache->bucket_count - 1);
    }

    /* not found: create a slot */
    if (cache->count == cache->cap) {
        size_t new_cap = cache->cap ? cache->cap * 2 : 8;
        text_analysis_t *grown =
            realloc(cache->items, new_cap * sizeof(*grown));
        if (grown == NULL) {
            return (size_t)-1;
        }
        cache->items = grown;
        cache->cap = new_cap;
    }
    slot = &cache->items[cache->count];
    slot->text = text;
    str_list_init(&slot->raw_tokens);
    str_list_init(&slot->tokens);
    cmap_init(&slot->counter);
    cmap_init(&slot->trigrams);
    slot->tokens_ready = 0;
    slot->counter_ready = 0;
    slot->trigrams_ready = 0;
    i = cache->count++;
    {
        size_t entry_slot = bucket;
        while (cache->buckets[entry_slot] != 0) {
            entry_slot = (entry_slot + 1) & (cache->bucket_count - 1);
        }
        cache->buckets[entry_slot] = i + 1;
    }
    if ((cache->count + 1) * 10 >= cache->bucket_count * 7) {
        if (analysis_rehash(cache, cache->bucket_count * 2) != 0) {
            /* index degrade only: slot is still usable via items[],
             * but future lookups linear-scan the buckets - bail out
             * cleanly instead (allocation failure path). */
            return (size_t)-1;
        }
    }
    return i;
}

/* Filtered tokens + unfiltered tokens, ONE tokenization pass
 * (odk_extract_tokens_both; byte-identical to the two
 * odk_extract_tokens_ex calls the earlier path made per text). */
static int analysis_tokens(text_analysis_t *slot,
                           const kernel_text_cfg *cfg)
{
    if (!slot->tokens_ready) {
        if (odk_extract_tokens_both_morph(slot->text, cfg->mode, cfg->pack,
                                          cfg->language, cfg->rt,
                                          cfg->morph_light,
                                          &slot->raw_tokens,
                                          &slot->tokens) != 0) {
            return -1;
        }
        slot->tokens_ready = 1;
    }
    return 0;
}

/* Token counter from the cached token list. */
static int analysis_counter(text_analysis_t *slot)
{
    if (!slot->counter_ready) {
        if (tokens_to_counter(&slot->tokens, &slot->counter) != 0) {
            return -1;
        }
        slot->counter_ready = 1;
    }
    return 0;
}

/* Trigram counter from the cached UNFILTERED token list.
 * cp_windows != 0 reproduces the odk_u_trigram_counter join +
 * per-code-point offsets + 3-cp windows over the same token list the
 * earlier builder re-extracted internally; cp_windows == 0 reproduces
 * the trigram_counter / trigram_ascii 3-byte windows.  Byte-identical
 * to both earlier builders on the same list.
 * strip_joiners drops U+200C/U+200D from the matching channel
 * (LANGUAGES.md "stripped for matching in hi/bn/ur/ar") - the raw
 * DISPLAY tokens keep them, so the strip happens here instead.  The
 * byte-window path never sees joiners: it only runs for pure-ASCII
 * text or ASCII_COMPAT, and neither emits tokens containing the
 * 3-byte U+200C/U+200D sequences. */
static int trigrams_from_raw(const str_list_t *raw, int cp_windows,
                             int strip_joiners, cmap_t *map)
{
    sb_t joined;
    size_t i;
    int failed = 0;

    if (sb_init(&joined) != 0) {
        return -1;
    }
    if (!cp_windows) {
        /* byte windows (ASCII_COMPAT, or UNICODE over pure-ASCII text) */
        for (i = 0; i < raw->count; i++) {
            if (i > 0 && sb_putc(&joined, ' ') != 0) {
                failed = 1;
                break;
            }
            if (sb_puts(&joined, raw->items[i]) != 0) {
                failed = 1;
                break;
            }
        }
        if (!failed && joined.len >= ODK_MIN_TRIGRAM_CHARS) {
            size_t k;
            for (k = 0; k + 3 <= joined.len; k++) {
                if (cmap_increment(map, joined.data + k, 3) != 0) {
                    failed = 1;
                    break;
                }
            }
        }
    } else {
        /* code-point windows (UNICODE mode over non-ASCII text) */
        size_t *offsets = NULL;
        size_t off_cap = 0;
        size_t cp_count = 0;

        off_cap = 64;
        offsets = malloc(off_cap * sizeof(size_t));
        if (offsets == NULL) {
            sb_free(&joined);
            return -1;
        }
        for (i = 0; i < raw->count && !failed; i++) {
            size_t k = 0;
            size_t tok_len = strlen(raw->items[i]);
            if (i > 0) {
                if (sb_putc(&joined, ' ') != 0) {
                    failed = 1;
                    break;
                }
                if (cp_count == off_cap) {
                    size_t *grown;
                    if (off_cap > SIZE_MAX / 2 / sizeof(size_t)) {
                        failed = 1;
                        break;
                    }
                    off_cap *= 2;
                    grown = realloc(offsets, off_cap * sizeof(size_t));
                    if (grown == NULL) {
                        failed = 1;
                        break;
                    }
                    offsets = grown;
                }
                offsets[cp_count++] = joined.len - 1;
            }
            while (k < tok_len) {
                uint32_t cp;
                int used = odk_u_decode_next(raw->items[i] + k,
                                             tok_len - k, &cp);
                if (used == 0) {
                    used = 1; /* tokens never contain invalid bytes */
                }
                if (strip_joiners && (cp == 0x200Cu || cp == 0x200Du)) {
                    k += (size_t)used;
                    continue;
                }
                if (sb_append(&joined, raw->items[i] + k, (size_t)used) != 0) {
                    failed = 1;
                    break;
                }
                if (cp_count == off_cap) {
                    size_t *grown;
                    if (off_cap > SIZE_MAX / 2 / sizeof(size_t)) {
                        failed = 1;
                        break;
                    }
                    off_cap *= 2;
                    grown = realloc(offsets, off_cap * sizeof(size_t));
                    if (grown == NULL) {
                        failed = 1;
                        break;
                    }
                    offsets = grown;
                }
                offsets[cp_count++] = joined.len - (size_t)used;
                k += (size_t)used;
            }
        }
        if (!failed && cp_count >= 3) {
            for (i = 0; i + 3 <= cp_count; i++) {
                size_t start = offsets[i];
                size_t end = (i + 3 < cp_count) ? offsets[i + 3]
                                                : joined.len;
                if (cmap_increment(map, joined.data + start,
                                   end - start) != 0) {
                    failed = 1;
                    break;
                }
            }
        }
        free(offsets);
    }
    sb_free(&joined);
    return failed ? -1 : 0;
}

/* Trigram counter for one analysis slot (window semantics follow the
 * earlier dispatch: UNICODE mode uses cp windows exactly when the text
 * is not pure ASCII, else byte windows; ASCII_COMPAT always byte). */
static int analysis_trigrams(text_analysis_t *slot,
                             const kernel_text_cfg *cfg)
{
    if (!slot->trigrams_ready) {
        int cp_windows = 0;
        int strip_joiners;
        if (cfg->mode == ODK_TEXT_UNICODE) {
            const char *p;
            for (p = slot->text; p != NULL && *p != '\0'; p++) {
                if ((unsigned char)*p >= 0x80) {
                    cp_windows = 1;
                    break;
                }
            }
        }
        /* the same deterministic resolution the tokenizer applied
         * (odk_lang_resolve(AUTO, text) == the language the tokens
         * were built with), so the trigram channel strips exactly
         * when the token channel stripped */
        strip_joiners = odk_lang_zwj_join(
            odk_lang_resolve(cfg->language, slot->text));
        if (trigrams_from_raw(&slot->raw_tokens, cp_windows,
                              strip_joiners, &slot->trigrams) != 0) {
            return -1;
        }
        slot->trigrams_ready = 1;
    }
    return 0;
}

/* Lexical similarity: 0.35*tf-cosine + 0.35*jaccard + 0.30*trigram.
 * the tokenizer and trigram channel follow the kernel text mode;
 * when a link view with link_weight > 0 is supplied, ACTIVE/PINNED word
 * links contribute bounded soft matches to the TF and Jaccard channels
 * (the trigram channel ignores links).  links == NULL or
 * weight 0 executes the unmodified arithmetic path (bit-identical).
 * all per-text analyses come from the evaluation-wide cache. */
static int lexical_similarity_l(analysis_cache_t *cache,
                                const kernel_text_cfg *cfg,
                                const odk_link_store *links,
                                const char *left, const char *right,
                                double *out, sb_t *explain,
                                sim_parts_t *parts)
{
    text_analysis_t *la;
    text_analysis_t *ra;
    size_t li;
    size_t ri;
    double cosine;
    double jaccard;
    double trigram;
    double blended;

    li = analysis_index(cache, left);
    ri = analysis_index(cache, right);
    if (li == (size_t)-1 || ri == (size_t)-1) {
        return -1;
    }
    /* both slots exist and no further slot creation happens below, so
     * these pointers stay valid for the whole function */
    la = &cache->items[li];
    ra = &cache->items[ri];
    if (analysis_tokens(la, cfg) != 0 || analysis_tokens(ra, cfg) != 0) {
        return -1;
    }
    if (la->tokens.count == 0 || ra->tokens.count == 0) {
        if (parts != NULL) {
            parts->cosine = 0.0;
            parts->jaccard = 0.0;
            parts->trigram = 0.0;
        }
        *out = 0.0;
        return 0;
    }
    if (analysis_counter(la) != 0 || analysis_counter(ra) != 0 ||
        analysis_trigrams(la, cfg) != 0 || analysis_trigrams(ra, cfg) != 0) {
        return -1;
    }
    cosine = odk_counter_cosine(&la->counter, &ra->counter);
    jaccard = token_jaccard(&la->counter, &ra->counter);
    trigram = odk_counter_cosine(&la->trigrams, &ra->trigrams);
    if (links != NULL) {
        double weight;
        double cap;
        double gain;
        odk_links_params(links, &weight, &cap, &gain, NULL);
        if (weight > 0.0 && odk_links_has_active(links)) {
            /* soft channel: bounded contributions.  Both
             * channels add at most weight * cap each, so the lexical
             * similarity lifts by <= 0.7 * weight * cap. */
            double soft_dot = 0.0;
            double soft_int = 0.0;
            double norm_left = 0.0;
            double norm_right = 0.0;
            size_t li;
            for (li = 0; li < la->counter.count; li++) {
                double v = (double)la->counter.entries[li].count;
                norm_left += v * v;
            }
            for (li = 0; li < ra->counter.count; li++) {
                double v = (double)ra->counter.entries[li].count;
                norm_right += v * v;
            }
            for (li = 0; li < la->counter.count; li++) {
                const char *x = la->counter.entries[li].key;
                const char *partners[16];
                double strengths[16];
                size_t np = odk_links_partners(links, x, partners,
                                               strengths, 16);
                size_t k;
                for (k = 0; k < np; k++) {
                    size_t rc = cmap_get(&ra->counter, partners[k],
                                         strlen(partners[k]));
                    if (rc != 0) {
                        double pair_w =
                            strengths[k] < cap ? strengths[k] : cap;
                        double contrib = pair_w * gain;
                        soft_dot += (double)la->counter.entries[li].count *
                                    (double)rc * contrib;
                        soft_int += contrib;
                        if (explain != NULL) {
                            char *ja = odk_json_string_literal(
                                x, strlen(x));
                            char *jb = odk_json_string_literal(
                                partners[k], strlen(partners[k]));
                            int rc2 = -1;
                            if (ja != NULL && jb != NULL) {
                                rc2 = sb_printf(
                                    explain,
                                    "%s{\"left\":%s,\"right\":%s,"
                                    "\"strength\":%.4f,\"soft\":%.4f}",
                                    explain->len > 1 ? "," : "", ja, jb,
                                    strengths[k], contrib);
                            }
                            free(ja);
                            free(jb);
                            if (rc2 != 0) {
                                return -1;
                            }
                        }
                    }
                }
            }
            {
                double soft_cosine =
                    soft_dot / (sqrt(norm_left) * sqrt(norm_right));
                double soft_jaccard =
                    soft_int /
                    (token_jaccard_union(&la->counter, &ra->counter));
                if (soft_cosine > cap) {
                    soft_cosine = cap;
                }
                if (soft_jaccard > cap) {
                    soft_jaccard = cap;
                }
                cosine = cosine + weight * soft_cosine;
                jaccard = jaccard + weight * soft_jaccard;
            }
        }
    }
    if (parts != NULL) {
        parts->cosine = cosine;
        parts->jaccard = jaccard;
        parts->trigram = trigram;
    }
    blended = cfg->model->w_cos * cosine + cfg->model->w_jaccard * jaccard +
              cfg->model->w_trigram * trigram;
    *out = odk_clamp01(blended);
    return 0;
}

/* Similarity: the embedded engine is the only backend, so this is the
 * lexical score. */
static int kernel_similarity_l(analysis_cache_t *cache,
                               const kernel_text_cfg *cfg,
                               const odk_link_store *links,
                               const char *left, const char *right,
                               double *out, sb_t *explain)
{
    return lexical_similarity_l(cache, cfg, links, left, right, out, explain,
                                NULL);
}

static int kernel_similarity(analysis_cache_t *cache,
                             const kernel_text_cfg *cfg,
                             const char *left, const char *right,
                             double *out)
{
    return lexical_similarity_l(cache, cfg, NULL, left, right, out, NULL,
                                NULL);
}

/* ------------------------------------------------------------------ */
/* Evidence assembly                                                   */
/* ------------------------------------------------------------------ */

static int emit_chunk(const kernel_text_cfg *cfg, const char *context,
                      size_t start, size_t end, str_list_t *out,
                      size_t *kept);

static int build_anchor(const kernel_text_cfg *cfg, const char *instruction,
                        const char *context, const odk_image *image,
                        char **out)
{
    sb_t anchor;
    char signature[64];

    if (sb_init(&anchor) != 0) {
        return -1;
    }
    if (instruction != NULL && instruction[0] != '\0') {
        if (sb_puts(&anchor, instruction) != 0) {
            goto oom;
        }
    }
    if (context != NULL && context[0] != '\0') {
        size_t len = strlen(context);
        if (cfg->mode == ODK_TEXT_ASCII_COMPAT) {
            if (len > cfg->context_max) {
                len = cfg->context_max; /* legacy byte cut */
            }
        } else {
            /* code-point boundary cut, never splits a sequence */
            len = odk_u_truncate_cp(context, len, cfg->context_max);
        }
        if (anchor.len > 0 && sb_putc(&anchor, '\n') != 0) {
            goto oom;
        }
        if (sb_append(&anchor, context, len) != 0) {
            goto oom;
        }
    }
    signature[0] = '\0';
    if (image != NULL) {
        if (odk_describe_image(image, signature, sizeof(signature), NULL) !=
            ODK_OK) {
            signature[0] = '\0';
        }
    }
    if (signature[0] != '\0') {
        if (anchor.len > 0 && sb_putc(&anchor, '\n') != 0) {
            goto oom;
        }
        if (sb_puts(&anchor, signature) != 0) {
            goto oom;
        }
    }
    {
        size_t start;
        size_t end;
        char *raw = sb_detach(&anchor);
        char *stripped;
        if (raw == NULL) {
            return -1;
        }
        if (cfg->mode == ODK_TEXT_ASCII_COMPAT) {
            odk_strip_bounds(raw, strlen(raw), &start, &end);
        } else {
            odk_u_strip_bounds(raw, strlen(raw), &start, &end);
        }
        stripped = odk_strdup_range(raw + start, end - start);
        free(raw);
        if (stripped == NULL) {
            return -1;
        }
        *out = stripped;
        return 0;
    }

oom:
    sb_free(&anchor);
    return -1;
}

/* Context chunks: split on newlines and on whitespace following
 * [.!?], then strip, min-length filter and MAX_CHUNKS cap. */
/* Sentence enders for the UNICODE chunker. */
static int is_sentence_ender_cp(uint32_t cp)
{
    switch (cp) {
    case 0x2E: /* . */
    case 0x21: /* ! */
    case 0x3F: /* ? */
    case 0x3002: /* 。 */
    case 0xFF01: /* ！ */
    case 0xFF1F: /* ？ */
    case 0x2026: /* … */
    case 0xFF1B: /* ； */
        return 1;
    default:
        return 0;
    }
}

/* Language-aware sentence enders.  UND keeps the
 * exact baseline set above (identity anchor); hi/bn add danda
 * U+0964/0965, ar adds U+061F/U+061b, ur adds U+06d4/U+061f/U+061b
 * (odk_lang_is_sentence_ender).  Spanish inverted marks are openers,
 * never enders. */
static int is_sentence_ender_lang(uint32_t cp, odk_language lang)
{
    if (lang == ODK_LANG_UND || lang == ODK_LANG_AUTO ||
        lang == ODK_LANG_DEFAULT) {
        return is_sentence_ender_cp(cp);
    }
    return odk_lang_is_sentence_ender(cp, lang);
}

/* A closing quote/bracket directly after a terminator must not block
 * the split ("sentence.")' style); openers are never enders. */
static int is_closing_punct_cp(uint32_t cp)
{
    switch (cp) {
    case 0x22u:   /* " */
    case 0x27u:   /* ' */
    case 0x29u:   /* ) */
    case 0x5Du:   /* ] */
    case 0x7Du:   /* } */
    case 0x2019u: /* right single quote */
    case 0x201Du: /* right double quote */
    case 0x00BBu: /* right guillemet (French) */
    case 0x300Bu: /* right CJK bracket */
    case 0xFF09u: /* fullwidth ) */
    case 0xFF3Du: /* fullwidth ] */
    case 0xFF5Du: /* fullwidth } */
        return 1;
    default:
        return 0;
    }
}

static int context_chunks(const kernel_text_cfg *cfg, const char *context,
                          str_list_t *out)
{
    size_t len;
    size_t i = 0;
    size_t start = 0;
    size_t kept = 0;

    str_list_init(out);
    if (context == NULL || context[0] == '\0') {
        return 0;
    }

    /* Input profiles: AUTO detects per call (pure
     * function - the kernel stays read-only); non-PLAIN profiles use
     * their own chunking unit. */
    {
        odk_input_profile profile = cfg->profile;
        if (profile == ODK_PROFILE_AUTO) {
            profile = odk_detect_input_profile(context, NULL, 0);
        }
        if (profile != ODK_PROFILE_PLAIN && profile != ODK_PROFILE_HTML_TEXT) {
            int rc = odk_profile_chunks(cfg->mode, profile,
                                        cfg->chunk_source_max,
                                        cfg->max_chunks, context, out);
            if (rc == 0 || rc == -1) {
                return rc;
            }
            /* -2: profile without a dedicated chunker -> fall through */
        }
    }

    if (cfg->mode == ODK_TEXT_ASCII_COMPAT) {
        len = strlen(context);
        if (len > cfg->chunk_source_max) {
            len = cfg->chunk_source_max;
        }
        while (i < len) {
            char c = context[i];
            char prev = (i > 0) ? context[i - 1] : '\0';
            if (c == '\n' || c == '\r') {
                size_t j = i;
                while (j < len && (context[j] == '\n' || context[j] == '\r')) {
                    j++;
                }
                if (emit_chunk(cfg, context, start, i, out, &kept) != 0) {
                    return -1;
                }
                i = j;
                start = j;
            } else if (odk_ascii_isspace((unsigned char)c) &&
                       (prev == '.' || prev == '!' || prev == '?')) {
                size_t j = i;
                while (j < len && odk_ascii_isspace((unsigned char)context[j])) {
                    j++;
                }
                if (emit_chunk(cfg, context, start, i, out, &kept) != 0) {
                    return -1;
                }
                i = j;
                start = j;
            } else {
                i++;
            }
        }
        if (emit_chunk(cfg, context, start, len, out, &kept) != 0) {
            return -1;
        }
        return 0;
    }

    /* UNICODE chunker: code-point walk over the raw UTF-8 text. */
    len = strlen(context);
    len = odk_u_truncate_cp(context, len, cfg->chunk_source_max);
    {
        uint32_t prev_cp = 0;
        /* AUTO resolves ONCE on the chunk source for the
         * ender set (cheap per-call handling of mixed text; the
         * tokenizer re-resolves per text). */
        odk_language ender_lang = cfg->language;
        if (ender_lang == ODK_LANG_AUTO) {
            ender_lang = odk_lang_resolve(ODK_LANG_AUTO, context);
        }
        while (i < len) {
            uint32_t cp;
            int used = odk_u_decode_next(context + i, len - i, &cp);
            if (used == 0) {
                used = 1;
                cp = 0;
            }
            if (cp == '\n' || cp == '\r') {
                size_t j = i;
                for (;;) {
                    uint32_t c2;
                    int u2 = odk_u_decode_next(context + j, len - j, &c2);
                    if (u2 > 0 && (c2 == '\n' || c2 == '\r')) {
                        j += (size_t)u2;
                    } else {
                        break;
                    }
                }
                if (emit_chunk(cfg, context, start, i, out, &kept) != 0) {
                    return -1;
                }
                i = j;
                start = j;
                prev_cp = 0;
            } else if (odk_u_is_space(cp) &&
                       is_sentence_ender_lang(prev_cp, ender_lang)) {
                size_t j = i;
                for (;;) {
                    uint32_t c2;
                    int u2 = odk_u_decode_next(context + j, len - j, &c2);
                    if (u2 > 0 && odk_u_is_space(c2)) {
                        j += (size_t)u2;
                    } else {
                        break;
                    }
                }
                if (emit_chunk(cfg, context, start, i, out, &kept) != 0) {
                    return -1;
                }
                i = j;
                start = j;
                prev_cp = 0;
            } else {
                i += (size_t)used;
                if (ender_lang != ODK_LANG_UND &&
                    ender_lang != ODK_LANG_DEFAULT &&
                    is_closing_punct_cp(cp) &&
                    is_sentence_ender_lang(prev_cp, ender_lang)) {
                    /* Language mode: a closing
                     * quote/bracket directly after a terminator must
                     * not block the split ("sentence.")' style).
                     * identity fix: UND/DEFAULT keep the exact
                     * baseline else-branch - the rule had leaked into
                     * the UND chunker and changed chunking for text
                     * like 'sentence." next' / 'sentence." next',
                     * violating the UND==baseline byte-identity anchor
                     * (found by the confirmation corpus nquote
                     * cases; pinned by
                     * test_und_closing_punct_identity). */
                } else {
                    prev_cp = cp;
                }
            }
        }
        if (emit_chunk(cfg, context, start, len, out, &kept) != 0) {
            return -1;
        }
    }
    return 0;
}

static int emit_chunk(const kernel_text_cfg *cfg, const char *context,
                      size_t start, size_t end, str_list_t *out,
                      size_t *kept)
{
    size_t lo;
    size_t hi;
    if (kept != NULL && *kept >= cfg->max_chunks) {
        return 0;
    }
    if (cfg->mode == ODK_TEXT_ASCII_COMPAT) {
        odk_strip_bounds(context + start, end - start, &lo, &hi);
        if (hi - lo >= ODK_MIN_CHUNK_CHARS) {
            if (str_list_push(out, context + start + lo, hi - lo) != 0) {
                str_list_free(out);
                return -1;
            }
            if (kept != NULL) {
                (*kept)++;
            }
        }
        return 0;
    }
    odk_u_strip_bounds(context + start, end - start, &lo, &hi);
    if (odk_utf8_codepoints(context + start + lo, hi - lo) >=
        (size_t)ODK_MIN_CHUNK_CHARS) {
        if (str_list_push(out, context + start + lo, hi - lo) != 0) {
            str_list_free(out);
            return -1;
        }
        if (kept != NULL) {
            (*kept)++;
        }
    }
    return 0;
}

/* Score against context: 0.4*whole + 0.6*best chunk, early exit. */
static int score_against_context_l(analysis_cache_t *cache,
                                   const kernel_text_cfg *cfg,
                                   const odk_link_store *links,
                                   const char *target, const char *anchor,
                                   const str_list_t *chunks, double *out,
                                   sb_t *explain, explain_capture_t *cap)
{
    double base;
    double best_chunk = 0.0;
    size_t i;

    if (cap != NULL) {
        memset(cap, 0, sizeof(*cap));
    }
    if (lexical_similarity_l(cache, cfg, links, target, anchor, &base,
                             explain,
                             cap != NULL ? &cap->whole_parts : NULL) != 0) {
        return -1;
    }
    if (cap != NULL) {
        cap->whole = base;
    }
    if (chunks->count == 0) {
        *out = odk_clamp01(base);
        return 0;
    }
    for (i = 0; i < chunks->count; i++) {
        double value;
        if (kernel_similarity_l(cache, cfg, links, target,
                                chunks->items[i], &value,
                                NULL) != 0) {
            return -1;
        }
        if (value > best_chunk) {
            best_chunk = value;
        }
        if (best_chunk >= ODK_EARLY_EXIT_SIMILARITY) {
            break;
        }
    }
    if (cap != NULL) {
        cap->best_chunk = best_chunk;
    }
    *out = odk_clamp01(cfg->model->w_whole * base +
                       cfg->model->w_best * best_chunk);
    return 0;
}

static int score_against_context(analysis_cache_t *cache,
                                 const kernel_text_cfg *cfg,
                                 const char *target, const char *anchor,
                                 const str_list_t *chunks, double *out)
{
    return score_against_context_l(cache, cfg, NULL, target, anchor, chunks,
                                   out, NULL, NULL);
}

/* ------------------------------------------------------------------ */
/* Calibration                                                         */
/* ------------------------------------------------------------------ */

static double kernel_sigmoid(double value)
{
    if (value >= 0.0) {
        return 1.0 / (1.0 + exp(-value));
    }
    {
        double exp_value = exp(value);
        return exp_value / (1.0 + exp_value);
    }
}

/* Calibrated distribution: logits = 6*(clamp01(s)-0.5), then a
 * numerically stable softmax. */
static int calibrated_distribution(const double *scores, size_t count,
                                   double *probs, double scale)
{
    double *logits;
    double peak;
    double total = 0.0;
    size_t i;

    if (count == 0) {
        return 0;
    }
    logits = malloc(count * sizeof(double));
    if (logits == NULL) {
        return -1;
    }
    peak = -HUGE_VAL;
    for (i = 0; i < count; i++) {
        logits[i] = scale * (odk_clamp01(scores[i]) - 0.5);
        if (logits[i] > peak) {
            peak = logits[i];
        }
    }
    for (i = 0; i < count; i++) {
        probs[i] = exp(logits[i] - peak);
        total += probs[i];
    }
    free(logits);
    if (total <= 0.0) {
        for (i = 0; i < count; i++) {
            probs[i] = 1.0 / (double)count;
        }
        return 0;
    }
    for (i = 0; i < count; i++) {
        probs[i] = probs[i] / total;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Shared option scoring (choice / rank / compare)                     */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Evidence view: over-limit strategies + the work budget          */
/*                                                    */
/* ------------------------------------------------------------------ */

/* Distinct-token query set for the select strategy: filtered tokens of
 * the instruction (or statement) UNION every option / rubric label,
 * tokenized with the kernel's mode and pack. */
static int select_query_set(const kernel_text_cfg *cfg,
                            const char *instruction,
                            const char *const *options, size_t option_count,
                            cmap_t *query_set)
{
    size_t i;

    if (cmap_init(query_set) != 0) {
        return -1;
    }
    if (instruction != NULL && instruction[0] != '\0') {
        str_list_t toks;
        size_t k;
        str_list_init(&toks);
        if (odk_extract_tokens_lang(instruction, cfg->mode, cfg->pack,
                                    cfg->language, cfg->rt, 1,
                                    &toks) != 0) {
            str_list_free(&toks);
            cmap_free(query_set);
            return -1;
        }
        for (k = 0; k < toks.count; k++) {
            if (cmap_increment(query_set, toks.items[k],
                               strlen(toks.items[k])) != 0) {
                str_list_free(&toks);
                cmap_free(query_set);
                return -1;
            }
        }
        str_list_free(&toks);
    }
    for (i = 0; i < option_count; i++) {
        if (options[i] != NULL && options[i][0] != '\0') {
            str_list_t toks;
            size_t k;
            str_list_init(&toks);
            if (odk_extract_tokens_lang(options[i], cfg->mode,
                                        cfg->pack, cfg->language,
                                        cfg->rt, 1,
                                        &toks) != 0) {
                str_list_free(&toks);
                cmap_free(query_set);
                return -1;
            }
            for (k = 0; k < toks.count; k++) {
                if (cmap_increment(query_set, toks.items[k],
                                   strlen(toks.items[k])) != 0) {
                    str_list_free(&toks);
                    cmap_free(query_set);
                    return -1;
                }
            }
            str_list_free(&toks);
        }
    }
    return 0;
}

/* select relevance of one chunk: |distinct chunk tokens in
 * the query set| / |distinct chunk tokens| (0 when no tokens). */
static double select_chunk_score(const cmap_t *query_set, const char *chunk,
                                 const kernel_text_cfg *cfg)
{
    str_list_t toks;
    cmap_t distinct;
    size_t k;
    size_t overlap = 0;
    size_t total = 0;
    double score = 0.0;

    str_list_init(&toks);
    if (odk_extract_tokens_lang(chunk, cfg->mode, cfg->pack,
                                cfg->language, cfg->rt, 1,
                                &toks) != 0) {
        str_list_free(&toks);
        return 0.0; /* OOM degrades to 0 - the caller also fails later */
    }
    if (cmap_init(&distinct) != 0) {
        str_list_free(&toks);
        return 0.0;
    }
    for (k = 0; k < toks.count; k++) {
        if (cmap_increment(&distinct, toks.items[k],
                           strlen(toks.items[k])) == 0) {
            if (cmap_get(&distinct, toks.items[k],
                         strlen(toks.items[k])) == 1) {
                total++;
                if (cmap_get(query_set, toks.items[k],
                             strlen(toks.items[k])) != 0) {
                    overlap++;
                }
            }
        }
    }
    if (total != 0) {
        score = (double)overlap / (double)total;
    }
    cmap_free(&distinct);
    str_list_free(&toks);
    return score;
}

/* Rank-comparison for select candidates: score DESC, position ASC. */
typedef struct {
    double score;
    size_t index;
} select_pair;

static int select_pair_cmp(const void *a, const void *b)
{
    const select_pair *pa = a;
    const select_pair *pb = b;
    if (pa->score != pb->score) {
        return (pa->score < pb->score) ? 1 : -1;
    }
    return (pa->index > pb->index) ? 1
                                   : (pa->index < pb->index ? -1 : 0);
}

/* head+tail view over the first scan_cps code points: first
 * ceil(min(scan,B)/2) cps, one '\n', last floor(min(scan,B)/2) cps.
 * Returns NULL (a no-op) when the scanned prefix fits the budget. */
static char *build_ht_view(const kernel_text_cfg *cfg, const char *context,
                           size_t scan_cps)
{
    size_t len = strlen(context);
    size_t context_cps = cfg->mode == ODK_TEXT_ASCII_COMPAT
                             ? len
                             : odk_utf8_codepoints(context, len);
    size_t eff = scan_cps < cfg->chunk_source_max ? scan_cps
                                                  : cfg->chunk_source_max;
    size_t head_cps = (eff + 1) / 2;
    size_t tail_cps = eff / 2;
    size_t head_bytes;
    size_t tail_start_bytes;
    char *view;

    if (context_cps <= eff) {
        return NULL; /* fits: the strategy is a no-op */
    }
    head_bytes = odk_u_truncate_cp(context, len, head_cps);
    tail_start_bytes =
        odk_u_truncate_cp(context, len, context_cps < tail_cps
                                            ? 0
                                            : context_cps - tail_cps);
    if (tail_start_bytes < head_bytes) {
        tail_start_bytes = head_bytes; /* degenerate tiny budgets */
    }
    view = malloc(head_bytes + 1 + (len - tail_start_bytes) + 1);
    if (view == NULL) {
        return NULL;
    }
    memcpy(view, context, head_bytes);
    view[head_bytes] = '\n';
    memcpy(view + head_bytes + 1, context + tail_start_bytes,
           len - tail_start_bytes);
    view[head_bytes + 1 + (len - tail_start_bytes)] = '\0';
    return view;
}

/* Count the chunks the scan of the first prefix_cps cps would emit
 * under the strategy (work accounting; results discarded).  For
 * head+tail the budget builds the halves view of the ORIGINAL context
 * (computing it over the pre-truncated prefix degenerated to
 * a no-op because the prefix exactly fits the budget). */
static int count_chunks_prefix(const kernel_text_cfg *cfg,
                               const char *context, size_t prefix_cps,
                               odk_over_limit_strategy strategy,
                               size_t *out_count)
{
    kernel_text_cfg scan_cfg = *cfg;
    str_list_t chunks;
    char *view = NULL;
    const char *eff_context = context;
    size_t len = strlen(context);
    size_t prefix_bytes =
        odk_u_truncate_cp(context, len, prefix_cps);
    char *prefix = NULL;

    if (strategy == ODK_OVERLIMIT_HEAD_TAIL) {
        /* the halves come from the ORIGINAL context */
        view = build_ht_view(cfg, context, prefix_cps);
        if (view != NULL) {
            eff_context = view;
        } else if (prefix_bytes < len) {
            /* context fits the budget: chunk it whole */
            prefix = odk_strdup_range(context, prefix_bytes);
            if (prefix == NULL) {
                return -1;
            }
            eff_context = prefix;
        }
    } else {
        if (prefix_bytes < len) {
            prefix = odk_strdup_range(context, prefix_bytes);
            if (prefix == NULL) {
                return -1;
            }
            eff_context = prefix;
        }
        if (strategy == ODK_OVERLIMIT_SELECT) {
            /* candidates come from the FULL prefix: no head pre-cut */
            scan_cfg.chunk_source_max = strlen(eff_context);
        }
    }
    str_list_init(&chunks);
    if (context_chunks(&scan_cfg, eff_context, &chunks) != 0) {
        str_list_free(&chunks);
        free(view);
        free(prefix);
        return -1;
    }
    *out_count = chunks.count;
    str_list_free(&chunks);
    free(view);
    free(prefix);
    return 0;
}

/* work(P) = P + 100 * chunks(P) + anchor_cps (the anchor
 * term is the conservative bound min(P, context_max); the head+tail
 * joining newline adds one).  Monotone non-decreasing in P. */
static int work_at_prefix(const kernel_text_cfg *cfg, const char *context,
                          size_t prefix_cps,
                          odk_over_limit_strategy strategy, size_t *out_work)
{
    size_t chunks = 0;
    size_t anchor_cps = prefix_cps < cfg->context_max ? prefix_cps
                                                      : cfg->context_max;

    if (prefix_cps == 0) {
        *out_work = 0;
        return 0;
    }
    if (count_chunks_prefix(cfg, context, prefix_cps, strategy, &chunks) !=
        0) {
        return -1;
    }
    if (strategy == ODK_OVERLIMIT_HEAD_TAIL &&
        prefix_cps > cfg->chunk_source_max) {
        anchor_cps++; /* the joining newline */
        if (anchor_cps > cfg->context_max) {
            anchor_cps = cfg->context_max;
        }
    }
    *out_work = prefix_cps + ODK_WORK_CHUNK_UNIT * chunks + anchor_cps;
    return 0;
}

/* Largest scan prefix whose work fits the budget (binary search over
 * the monotone work function; full_scan when the budget never binds). */
static int fit_scan_prefix(const kernel_text_cfg *cfg, const char *context,
                           size_t full_scan,
                           odk_over_limit_strategy strategy, size_t budget,
                           size_t *out_prefix, int *out_exceeded)
{
    size_t work = 0;
    size_t lo;
    size_t hi;

    if (work_at_prefix(cfg, context, full_scan, strategy, &work) != 0) {
        return -1;
    }
    if (work <= budget) {
        *out_prefix = full_scan;
        *out_exceeded = 0;
        return 0;
    }
    *out_exceeded = 1;
    lo = 0;
    hi = full_scan;
    while (lo < hi) {
        size_t mid = lo + (hi - lo + 1) / 2;
        if (work_at_prefix(cfg, context, mid, strategy, &work) != 0) {
            return -1;
        }
        if (work <= budget) {
            lo = mid;
        } else {
            hi = mid - 1;
        }
    }
    *out_prefix = lo;
    return 0;
}

/* select strategy: candidate chunks from the scanned prefix, ranked by
 * relevance, kept under chunk_source_max, emitted in ORIGINAL order
 *.  *out_view receives the kept chunks joined by '\n' (the
 * anchor view).  Returns 0/-1. */
static int build_select(const kernel_text_cfg *cfg, const char *context,
                        size_t scan_cps, const char *instruction,
                        const char *const *options, size_t option_count,
                        str_list_t *out_chunks, size_t *out_kept,
                        size_t *out_dropped, char **out_view)
{
    cmap_t query_set;
    str_list_t candidates;
    select_pair *ranking = NULL;
    size_t *kept_idx = NULL;
    size_t kept_count = 0;
    size_t running_cps = 0;
    size_t i;
    kernel_text_cfg scan_cfg = *cfg;
    char *prefix = NULL;
    const char *eff_context = context;
    size_t len = strlen(context);
    int rc = -1;
    sb_t joined;

    if (select_query_set(cfg, instruction, options, option_count,
                         &query_set) != 0) {
        return -1;
    }
    {
        size_t prefix_bytes = odk_u_truncate_cp(context, len, scan_cps);
        if (prefix_bytes < len) {
            prefix = odk_strdup_range(context, prefix_bytes);
            if (prefix == NULL) {
                cmap_free(&query_set);
                return -1;
            }
            eff_context = prefix;
        }
    }
    /* candidates: chunk the FULL scanned prefix (no head pre-cut) */
    scan_cfg.chunk_source_max = strlen(eff_context);
    str_list_init(&candidates);
    if (context_chunks(&scan_cfg, eff_context, &candidates) != 0) {
        goto done;
    }
    ranking = malloc((candidates.count ? candidates.count : 1) *
                     sizeof(*ranking));
    kept_idx = malloc((candidates.count ? candidates.count : 1) *
                      sizeof(*kept_idx));
    if (ranking == NULL || kept_idx == NULL) {
        goto done;
    }
    for (i = 0; i < candidates.count; i++) {
        ranking[i].score =
            select_chunk_score(&query_set, candidates.items[i], cfg);
        ranking[i].index = i;
    }
    qsort(ranking, candidates.count, sizeof(*ranking), select_pair_cmp);
    for (i = 0; i < candidates.count; i++) {
        const char *chunk = candidates.items[ranking[i].index];
        size_t chunk_cps =
            cfg->mode == ODK_TEXT_ASCII_COMPAT
                ? strlen(chunk)
                : odk_utf8_codepoints(chunk, strlen(chunk));
        if (running_cps + chunk_cps <= cfg->chunk_source_max) {
            kept_idx[kept_count++] = ranking[i].index;
            running_cps += chunk_cps;
        }
    }
    /* emit kept chunks in ORIGINAL order */
    for (i = 0; i < candidates.count; i++) {
        size_t k;
        int is_kept = 0;
        for (k = 0; k < kept_count; k++) {
            if (kept_idx[k] == i) {
                is_kept = 1;
                break;
            }
        }
        if (is_kept) {
            if (str_list_push(out_chunks, candidates.items[i],
                              strlen(candidates.items[i])) != 0) {
                goto done;
            }
        }
    }
    *out_kept = kept_count;
    *out_dropped = candidates.count - kept_count;
    /* anchor view: kept chunks joined by single newlines */
    if (sb_init(&joined) != 0) {
        goto done;
    }
    for (i = 0; i < out_chunks->count; i++) {
        if (i > 0 && sb_putc(&joined, '\n') != 0) {
            sb_free(&joined);
            goto done;
        }
        if (sb_puts(&joined, out_chunks->items[i]) != 0) {
            sb_free(&joined);
            goto done;
        }
    }
    *out_view = sb_detach(&joined);
    if (*out_view == NULL) {
        goto done;
    }
    rc = 0;

done:
    if (rc != 0 && *out_view != NULL) {
        free(*out_view);
        *out_view = NULL;
    }
    free(ranking);
    free(kept_idx);
    str_list_free(&candidates);
    free(prefix);
    cmap_free(&query_set);
    return rc;
}

/* Build the evidence view for one evaluation.  HEAD with the
 * derived budget takes the legacy fast path (byte-identical).  Fills the
 * accounting fields of *meta when non-NULL.  chunks/anchor are owned by
 * the caller exactly as before.  Returns 0/-1. */
static int evidence_build(const kernel_text_cfg *cfg,
                          const char *anchor_instruction,
                          const char *query_text, const char *context,
                          const odk_image *image,
                          const char *const *options, size_t option_count,
                          int need_anchor, str_list_t *chunks, char **anchor,
                          odk_result_meta *meta)
{
    odk_over_limit_strategy strategy = effective_strategy(cfg);
    size_t budget = effective_work_budget(cfg);
    size_t context_cps = 0;
    size_t full_scan;
    size_t scan_cps;
    int exceeded = 0;
    char *view = NULL;
    char *prefix = NULL;
    const char *eff_context = context;
    int rc = -1;

    str_list_init(chunks);
    if (anchor != NULL) {
        *anchor = NULL;
    }
    if (meta != NULL) {
        meta->work_budget = budget;
        meta->work_used = 0;
        meta->work_exceeded = 0;
    }
    if (context == NULL || context[0] == '\0') {
        return need_anchor ? build_anchor(cfg, anchor_instruction, context,
                                          image, anchor)
                           : 0;
    }
    {
        size_t len = strlen(context);
        context_cps = cfg->mode == ODK_TEXT_ASCII_COMPAT
                          ? len
                          : odk_utf8_codepoints(context, len);
    }
    full_scan = strategy == ODK_OVERLIMIT_SELECT
                    ? context_cps
                    : (context_cps < cfg->chunk_source_max
                           ? context_cps
                           : cfg->chunk_source_max);

    if (strategy == ODK_OVERLIMIT_HEAD && cfg->max_work == 0 &&
        meta == NULL) {
        /* Fast path: default budget never binds, no accounting. */
        if (context_chunks(cfg, context, chunks) != 0) {
            return -1;
        }
        if (need_anchor &&
            build_anchor(cfg, anchor_instruction, context, image,
                         anchor) != 0) {
            str_list_free(chunks);
            return -1;
        }
        return 0;
    }

    /* work budget (explicit or requested accounting): fit the scan */
    scan_cps = full_scan;
    if (fit_scan_prefix(cfg, context, full_scan, strategy, budget,
                        &scan_cps, &exceeded) != 0) {
        return -1;
    }
    {
        size_t len = strlen(context);
        size_t prefix_bytes = odk_u_truncate_cp(context, len, scan_cps);
        if (prefix_bytes < len) {
            prefix = odk_strdup_range(context, prefix_bytes);
            if (prefix == NULL) {
                return -1;
            }
            eff_context = prefix;
        }
    }

    if (strategy == ODK_OVERLIMIT_SELECT) {
        size_t kept = 0;
        size_t dropped = 0;
        char *select_view = NULL;
        if (build_select(cfg, eff_context, scan_cps, query_text, options,
                         option_count, chunks, &kept, &dropped,
                         &select_view) != 0) {
            goto done;
        }
        view = select_view;
        if (need_anchor &&
            build_anchor(cfg, anchor_instruction, view, image, anchor) !=
                0) {
            goto done;
        }
        if (meta != NULL) {
            meta->chunks_kept = kept;
            meta->chunks_dropped = dropped;
        }
    } else if (strategy == ODK_OVERLIMIT_HEAD_TAIL) {
        /* The halves view is built from the ORIGINAL context
         * (the pre-truncated prefix exactly fits the budget, so the
         * old call degenerated to a no-op and head+tail behaved like
         * head).  The view is bounded by the scan budget scan_cps. */
        char *ht_view = build_ht_view(cfg, context, scan_cps);
        if (ht_view != NULL) {
            view = ht_view;
            if (context_chunks(cfg, view, chunks) != 0) {
                goto done;
            }
            if (need_anchor &&
                build_anchor(cfg, anchor_instruction, view, image,
                             anchor) != 0) {
                goto done;
            }
        } else {
            /* Fits: no-op, the legacy path over the unmodified context */
            if (context_chunks(cfg, context, chunks) != 0) {
                goto done;
            }
            if (need_anchor &&
                build_anchor(cfg, anchor_instruction, context, image,
                             anchor) != 0) {
                goto done;
            }
        }
    } else { /* HEAD with accounting */
        if (context_chunks(cfg, eff_context, chunks) != 0) {
            goto done;
        }
        if (need_anchor &&
            build_anchor(cfg, anchor_instruction, eff_context, image,
                         anchor) != 0) {
            goto done;
        }
    }

    if (meta != NULL) {
        size_t anchor_cps = 0; /* extract has no anchor channel */
        if (need_anchor) {
            anchor_cps = scan_cps < cfg->context_max ? scan_cps
                                                     : cfg->context_max;
            if (strategy == ODK_OVERLIMIT_HEAD_TAIL && view != NULL) {
                anchor_cps++; /* the joining newline */
                if (anchor_cps > cfg->context_max) {
                    anchor_cps = cfg->context_max;
                }
            }
        }
        meta->work_used =
            scan_cps + ODK_WORK_CHUNK_UNIT * chunks->count + anchor_cps;
        meta->work_exceeded = exceeded;
        /* pure-accounting truncation report.  The evidence scan
         * covers scan_cps units of the caller's context; anything
         * beyond it never contributed evidence.  Never affects the
         * decision (chunks and anchor are already built above). */
        meta->context_truncated = context_cps > scan_cps ? 1 : 0;
        meta->context_units_dropped =
            context_cps > scan_cps ? context_cps - scan_cps : 0;
    }
    rc = 0;

done:
    if (rc != 0) {
        str_list_free(chunks);
        if (anchor != NULL) {
            free(*anchor);
            *anchor = NULL;
        }
    }
    free(view);
    free(prefix);
    return rc;
}

/* ------------------------------------------------------------------ */
/* feature set 1 — six extra per-option features                 */
/* (docs/FEATURES.md).  All are pure                                  */
/* functions of (option, anchor, instruction, competing options) via   */
/* the analysis cache; no state, no RNG.  With the default model       */
/* (feature_set_id 0) this code never runs.                            */
/* ------------------------------------------------------------------ */

static int tok_list_contains(const str_list_t *list, const char *tok)
{
    size_t i;
    for (i = 0; i < list->count; i++) {
        if (strcmp(list->items[i], tok) == 0) {
            return 1;
        }
    }
    return 0;
}

static size_t tok_seq_find(const str_list_t *hay, const str_list_t *needle)
{
    size_t i;
    if (needle->count == 0 || hay->count < needle->count) {
        return (size_t)-1;
    }
    for (i = 0; i + needle->count <= hay->count; i++) {
        size_t k;
        int ok = 1;
        for (k = 0; k < needle->count; k++) {
            if (strcmp(hay->items[i + k], needle->items[k]) != 0) {
                ok = 0;
                break;
            }
        }
        if (ok) {
            return i;
        }
    }
    return (size_t)-1;
}

/* Number match helper: 1 when the anchor contains a MAXIMAL run of ASCII
 * digits identical to run[0..len) (same length, same digits).  "5" is not
 * found inside "15" or "2025". */
static int anchor_has_digit_run(const char *anchor, const char *run,
                                size_t len)
{
    const char *q = anchor;
    while (*q != '\0') {
        if (*q >= '0' && *q <= '9') {
            size_t l2 = 0;
            while (q[l2] >= '0' && q[l2] <= '9') {
                l2++;
            }
            if (l2 == len && memcmp(q, run, len) == 0) {
                return 1;
            }
            q += l2;
        } else {
            q++;
        }
    }
    return 0;
}

static void extra_features_l(analysis_cache_t *cache,
                             const kernel_text_cfg *cfg,
                             const char *option, const char *anchor,
                             const char *instruction,
                             const char *const *options,
                             size_t option_count, size_t self_index,
                             double *out)
{
    static const char *const CUES[] = {
        "chose", "chosen", "selected", "approved", "picked", "decided",
        "rejected", "declined", "vetoed", "ratified", "endorsed", "opted",
        "批准", "选择", "决定", "拍板", "认可",
        "اعتمد", "اختار", "قرر",
        "aprobó", "seleccionó", "eligió", "ratificó", "avaló"};
    static const char *const NEG[] = {
        "not", "never", "none", "no", "cannot", "without",
        "不", "未", "别", "无", "لم", "لن", "لا", "nunca", "tampoco"};
    size_t li;
    size_t ai;
    size_t ii;
    size_t other_idx[64];
    size_t n_slots = 0;
    const size_t *other_slots = NULL;
    str_list_t *lt = NULL;
    str_list_t *at = NULL;
    str_list_t *it = NULL;
    size_t i;
    size_t inter_all = 0;
    size_t inter_excl = 0;
    size_t inter_q = 0;
    double f1 = 0.0;
    double f2 = 0.0;
    double f3 = 0.0;
    double f4 = 0.0;
    double f5 = 0.0;
    double f6 = 0.0;

    for (i = 0; i < ODK_MODEL_EXTRA_MAX; i++) {
        out[i] = 0.0;
    }
    /* Pass 1: create ALL cache slots first (analysis_index may grow the
     * item array by realloc, so NO token-list pointer may be held across
     * an analysis_index call).  Pass 2: tokenize and take pointers. */
    {
        size_t n_other = option_count > 64 ? 64 : option_count;
        li = analysis_index(cache, option);
        ai = (size_t)-1;
        ii = (size_t)-1;
        if (li == (size_t)-1) {
            return;
        }
        if (anchor != NULL && anchor[0] != '\0') {
            ai = analysis_index(cache, anchor);
        }
        if (instruction != NULL && instruction[0] != '\0') {
            ii = analysis_index(cache, instruction);
        }
        for (i = 0; i < n_other; i++) {
            other_idx[i] = i == self_index
                               ? (size_t)-1
                               : analysis_index(cache, options[i]);
        }
        if (analysis_tokens(&cache->items[li], cfg) != 0) {
            return;
        }
        lt = &cache->items[li].tokens;
        if (ai != (size_t)-1 &&
            analysis_tokens(&cache->items[ai], cfg) == 0) {
            at = &cache->items[ai].tokens;
        }
        if (ii != (size_t)-1 &&
            analysis_tokens(&cache->items[ii], cfg) == 0) {
            it = &cache->items[ii].tokens;
        }
        /* tokenize the other-option slots now: no further analysis_index
         * calls happen below, so the item array cannot move anymore */
        for (i = 0; i < n_other; i++) {
            if (other_idx[i] != (size_t)-1 &&
                analysis_tokens(&cache->items[other_idx[i]], cfg) != 0) {
                other_idx[i] = (size_t)-1;
            }
        }
        other_slots = other_idx;
        n_slots = n_other;
    }
    if (lt->count == 0) {
        return;
    }
    /* F1/F2 intersections over the option's distinct tokens */
    for (i = 0; i < lt->count; i++) {
        const char *tok = lt->items[i];
        int in_anchor = at != NULL && tok_list_contains(at, tok);
        int in_instr = it != NULL && tok_list_contains(it, tok);
        int in_other = 0;
        size_t o;
        for (o = 0; o < n_slots && !in_other; o++) {
            if (other_slots[o] == (size_t)-1) {
                continue;
            }
            if (tok_list_contains(&cache->items[other_slots[o]].tokens,
                                  tok)) {
                in_other = 1;
            }
        }
        if (in_anchor) {
            inter_all++;
            if (!in_other) {
                inter_excl++;
            }
        }
        if (in_instr) {
            inter_q++;
        }
    }
    f1 = inter_all > 0 ? (double)inter_excl / (double)inter_all : 0.0;
    f2 = (double)inter_q / (double)lt->count;
    /* F3/F4/F5 need the anchor token list */
    if (at != NULL) {
        size_t pos = tok_seq_find(at, lt);
        if (pos != (size_t)-1) {
            double best = -1.0;
            size_t k;
            for (k = 0; k < sizeof(CUES) / sizeof(CUES[0]); k++) {
                size_t c;
                for (c = 0; c < at->count; c++) {
                    if (strcmp(at->items[c], CUES[k]) == 0) {
                        double d = (double)(c > pos ? c - pos : pos - c);
                        if (best < 0.0 || d < best) {
                            best = d;
                        }
                    }
                }
            }
            if (best >= 0.0) {
                f3 = 1.0 / (1.0 + best);
            }
            for (k = 0; k < sizeof(NEG) / sizeof(NEG[0]); k++) {
                size_t c;
                for (c = 0; c < pos; c++) {
                    if (strcmp(at->items[c], NEG[k]) == 0 &&
                        pos - c <= 3) {
                        f4 = 1.0;
                        break;
                    }
                }
                if (f4 > 0.0) {
                    break;
                }
            }
            f5 = 1.0;
        }
    }
    /* F6: number match over the RAW option and anchor text.  Every maximal
     * run of ASCII digits in the option is one number; it counts as matched
     * when the anchor contains an identical maximal run.  f6 = matched
     * runs / runs in the option (0 when the option has no digits). */
    {
        size_t a_count = 0;
        size_t matched = 0;
        const char *p = option;
        while (*p != '\0') {
            if (*p >= '0' && *p <= '9') {
                size_t len = 0;
                while (p[len] >= '0' && p[len] <= '9') {
                    len++;
                }
                a_count++;
                if (anchor != NULL && anchor_has_digit_run(anchor, p, len)) {
                    matched++;
                }
                p += len;
            } else {
                p++;
            }
        }
        if (a_count > 0) {
            f6 = (double)matched / (double)a_count;
        }
    }
    out[0] = f1;
    out[1] = f2;
    out[2] = f3;
    out[3] = f4;
    out[4] = f5;
    out[5] = f6;
}

/* Option scoring: 70% chunk-aware context evidence + 30%
 * instruction directionality (model-weighted), clamped to [0, 1].  The
 * _l variant threads the word-link view through (NULL = unlinked,
 * bit-identical).  Feature-set-1 models add the six extra features. */
static int score_options_l(const kernel_text_cfg *cfg,
                           const odk_link_store *links,
                           const char *const *options, size_t option_count,
                           const char *instruction, const char *context,
                           const odk_image *image, double *scores,
                           sb_t *explain, odk_result_meta *meta)
{
    str_list_t chunks;
    char *anchor = NULL;
    analysis_cache_t cache;
    size_t i;
    int failed = 0;

    analysis_cache_init(&cache);
    if (evidence_build(cfg, instruction, instruction, context, image,
                       options, option_count, 1, &chunks, &anchor,
                       meta) != 0) {
        analysis_cache_free(&cache);
        return -1;
    }
    for (i = 0; i < option_count; i++) {
        double contextual;
        double directional = 0.0;
        double fused;
        if (score_against_context_l(&cache, cfg, links, options[i], anchor,
                                    &chunks, &contextual,
                                    i == 0 ? explain : NULL, NULL) != 0) {
            failed = 1;
            break;
        }
        if (instruction != NULL && instruction[0] != '\0') {
            if (kernel_similarity_l(&cache, cfg, links, options[i],
                                    instruction,
                                    &directional, NULL) != 0) {
                failed = 1;
                break;
            }
        }
        {
            double extra_sum = 0.0;
            if (cfg->model->feature_set_id == 1) {
                double feats[ODK_MODEL_EXTRA_MAX];
                size_t k;
                extra_features_l(&cache, cfg, options[i], anchor,
                                 instruction, options, option_count, i,
                                 feats);
                for (k = 0; k < (size_t)cfg->model->extra_count; k++) {
                    extra_sum += cfg->model->extra[k] * feats[k];
                }
            }
            fused = odk_clamp01(cfg->model->w_ctx * contextual +
                                cfg->model->w_dir * directional +
                                extra_sum);
        }
        scores[i] = fused;
    }
    free(anchor);
    str_list_free(&chunks);
    analysis_cache_free(&cache);
    return failed ? -1 : 0;
}

static int score_options(const kernel_text_cfg *cfg,
                         const char *const *options, size_t option_count,
                         const char *instruction, const char *context,
                         const odk_image *image, double *scores,
                         odk_result_meta *meta)
{
    return score_options_l(cfg, NULL, options, option_count, instruction,
                           context, image, scores, NULL, meta);
}

/* ------------------------------------------------------------------ */
/* Explain API (docs/EXPLAIN.md)                            */
/*                                                                     */
/* Read-only introspection: the runners below call the SAME internals  */
/* as the primitives with optional capture, so every explained value   */
/* equals the internal intermediate bit-for-bit and no decision path   */
/* is touched.                                                         */
/* ------------------------------------------------------------------ */

static char *explain_dup(const char *s)
{
    size_t n;
    char *d;
    if (s == NULL) {
        d = malloc(1);
        if (d != NULL) {
            d[0] = '\0';
        }
        return d;
    }
    n = strlen(s);
    d = malloc(n + 1);
    if (d != NULL) {
        memcpy(d, s, n + 1);
    }
    return d;
}

static void explain_zero(odk_explain *out)
{
    memset(out, 0, sizeof(*out));
}

void odk_explain_release(odk_explain *explain)
{
    size_t i;
    if (explain == NULL) {
        return;
    }
    for (i = 0; i < explain->option_count; i++) {
        free(explain->options[i].label);
    }
    free(explain->options);
    explain_zero(explain);
}

static int explain_alloc_options(odk_explain *out, size_t count)
{
    out->options = calloc(count, sizeof(odk_explain_option));
    if (out->options == NULL) {
        return -1;
    }
    out->option_count = count;
    return 0;
}

odk_status odk_explain_choice(odk_kernel *kernel, const char *instruction,
                              const char *const *options,
                              size_t option_count, const char *context,
                              const odk_image *image, odk_explain *out,
                              odk_error *err)
{
    kernel_text_cfg eff;
    str_list_t chunks;
    char *anchor = NULL;
    analysis_cache_t cache;
    double *probs = NULL;
    size_t i;
    int failed = 0;

    if (kernel == NULL || options == NULL || option_count == 0) {
        kernel_set_error(err, "explain choice requires a kernel and at "
                              "least one option");
        return ODK_ERR_INFERENCE;
    }
    explain_zero(out);
    eff = kernel_effective_cfg(kernel, context);
    if (evidence_build(&eff, instruction, instruction, context, image,
                       options, option_count, 1, &chunks, &anchor,
                       NULL) != 0) {
        kernel_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    analysis_cache_init(&cache);
    probs = malloc(option_count * sizeof(double));
    if (probs == NULL || explain_alloc_options(out, option_count) != 0) {
        failed = 1;
        goto done;
    }
    snprintf(out->primitive, sizeof(out->primitive), "choice");
    out->chunk_count = chunks.count;
    for (i = 0; i < option_count && !failed; i++) {
        double contextual = 0.0;
        double directional = 0.0;
        double fused;
        explain_capture_t cap;
        if (score_against_context_l(&cache, &eff, NULL, options[i], anchor,
                                    &chunks, &contextual, NULL,
                                    &cap) != 0) {
            failed = 1;
            break;
        }
        if (instruction != NULL && instruction[0] != '\0') {
            if (kernel_similarity_l(&cache, &eff, NULL, options[i],
                                    instruction, &directional,
                                    NULL) != 0) {
                failed = 1;
                break;
            }
        }
        {
            double extra_sum = 0.0;
            size_t k;
            if (eff.model->feature_set_id == 1) {
                extra_features_l(&cache, &eff, options[i], anchor,
                                 instruction, options, option_count, i,
                                 out->options[i].extra);
                out->options[i].extra_count = eff.model->extra_count;
                for (k = 0; k < (size_t)eff.model->extra_count; k++) {
                    extra_sum += eff.model->extra[k] *
                                 out->options[i].extra[k];
                }
            }
            fused = odk_clamp01(eff.model->w_ctx * contextual +
                                eff.model->w_dir * directional +
                                extra_sum);
        }
        out->options[i].index = i;
        out->options[i].label = explain_dup(options[i]);
        if (out->options[i].label == NULL) {
            failed = 1;
            break;
        }
        out->options[i].tf_cosine = cap.whole_parts.cosine;
        out->options[i].jaccard = cap.whole_parts.jaccard;
        out->options[i].trigram = cap.whole_parts.trigram;
        out->options[i].whole = cap.whole;
        out->options[i].best_chunk = cap.best_chunk;
        out->options[i].contextual = contextual;
        out->options[i].directional = directional;
        out->options[i].fused = fused;
        out->options[i].probability = 0.0; /* filled after calibration */
        probs[i] = fused;
    }
    if (!failed &&
        calibrated_distribution(probs, option_count, probs,
                                eff.model->logit_scale) != 0) {
        failed = 1;
    }
    if (!failed) {
        for (i = 0; i < option_count; i++) {
            out->options[i].probability = probs[i];
        }
    }
done:
    free(probs);
    free(anchor);
    str_list_free(&chunks);
    analysis_cache_free(&cache);
    if (failed) {
        odk_explain_release(out);
        kernel_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    return ODK_OK;
}

odk_status odk_explain_score(odk_kernel *kernel, const char *instruction,
                             const char *const *rubric, size_t rubric_count,
                             const char *context, const odk_image *image,
                             odk_explain *out, odk_error *err)
{
    static const char *const kDefaultLevels[] = {"very low", "low", "medium",
                                                 "high", "very high"};
    kernel_text_cfg eff;
    str_list_t levels;
    str_list_t chunks;
    char *anchor = NULL;
    analysis_cache_t cache;
    const char *const *level_labels;
    const char **owned_labels = NULL;
    double *probs = NULL;
    double *fused_scores = NULL;
    double normalized = 0.5;
    double peak_probability = 0.0;
    size_t level_count;
    size_t i;
    int failed = 0;

    if (kernel == NULL) {
        kernel_set_error(err, "explain score requires a kernel");
        return ODK_ERR_INFERENCE;
    }
    explain_zero(out);
    str_list_init(&levels);
    str_list_init(&chunks);
    analysis_cache_init(&cache);
    for (i = 0; rubric != NULL && i < rubric_count; i++) {
        const char *level = rubric[i] != NULL ? rubric[i] : "";
        size_t len = strlen(level);
        size_t lo;
        size_t hi;
        odk_strip_bounds(level, len, &lo, &hi);
        if (hi > lo) {
            if (str_list_push(&levels, level + lo, hi - lo) != 0) {
                failed = 1;
                goto done;
            }
        }
    }
    if (levels.count == 0) {
        size_t k;
        for (k = 0; k < 5; k++) {
            if (str_list_push(&levels, kDefaultLevels[k],
                              strlen(kDefaultLevels[k])) != 0) {
                failed = 1;
                goto done;
            }
        }
    }
    level_labels = (const char *const *)levels.items;
    level_count = levels.count;
    eff = kernel_effective_cfg(kernel, context);
    if (evidence_build(&eff, instruction, instruction, context, image,
                       level_labels, level_count, 1, &chunks, &anchor,
                       NULL) != 0) {
        failed = 1;
        goto done;
    }
    probs = calloc(level_count, sizeof(double));
    fused_scores = calloc(level_count, sizeof(double));
    owned_labels = calloc(level_count, sizeof(char *));
    if (probs == NULL || fused_scores == NULL || owned_labels == NULL ||
        explain_alloc_options(out, level_count) != 0) {
        failed = 1;
        goto done;
    }
    snprintf(out->primitive, sizeof(out->primitive), "score");
    out->chunk_count = chunks.count;
    for (i = 0; i < level_count && !failed; i++) {
        double contextual = 0.0;
        double directional = 0.0;
        double fused;
        explain_capture_t cap;
        if (score_against_context_l(&cache, &eff, NULL, level_labels[i],
                                    anchor, &chunks, &contextual, NULL,
                                    &cap) != 0) {
            failed = 1;
            break;
        }
        if (instruction != NULL && instruction[0] != '\0') {
            if (kernel_similarity_l(&cache, &eff, NULL, level_labels[i],
                                    instruction, &directional,
                                    NULL) != 0) {
                failed = 1;
                break;
            }
        }
        {
            double extra_sum = 0.0;
            size_t k;
            if (eff.model->feature_set_id == 1) {
                extra_features_l(&cache, &eff, level_labels[i], anchor,
                                 instruction, level_labels, level_count,
                                 i, out->options[i].extra);
                out->options[i].extra_count = eff.model->extra_count;
                for (k = 0; k < (size_t)eff.model->extra_count; k++) {
                    extra_sum += eff.model->extra[k] *
                                 out->options[i].extra[k];
                }
            }
            fused = odk_clamp01(eff.model->w_ctx * contextual +
                                eff.model->w_dir * directional +
                                extra_sum);
        }
        out->options[i].index = i;
        out->options[i].label = explain_dup(level_labels[i]);
        if (out->options[i].label == NULL) {
            failed = 1;
            break;
        }
        owned_labels[i] = out->options[i].label; /* for the done path */
        out->options[i].tf_cosine = cap.whole_parts.cosine;
        out->options[i].jaccard = cap.whole_parts.jaccard;
        out->options[i].trigram = cap.whole_parts.trigram;
        out->options[i].whole = cap.whole;
        out->options[i].best_chunk = cap.best_chunk;
        out->options[i].contextual = contextual;
        out->options[i].directional = directional;
        out->options[i].fused = fused;
        fused_scores[i] = fused;
    }
    if (!failed &&
        calibrated_distribution(fused_scores, level_count, probs,
                                eff.model->logit_scale) != 0) {
        failed = 1;
    }
    if (!failed) {
        if (level_count == 1) {
            normalized = 0.5;
            out->options[0].probability = probs[0]; /* 1.0 */
        } else {
            normalized = 0.0;
            for (i = 0; i < level_count; i++) {
                out->options[i].probability = probs[i];
                normalized += probs[i] *
                              ((double)i / (double)(level_count - 1));
            }
        }
        peak_probability = probs[0];
        for (i = 1; i < level_count; i++) {
            if (probs[i] > peak_probability) {
                peak_probability = probs[i];
            }
        }
        out->score_normalized = odk_clamp01(normalized);
        out->score_peak = peak_probability;
    }
done:
    free(probs);
    free(fused_scores);
    free(owned_labels);
    free(anchor);
    str_list_free(&chunks);
    str_list_free(&levels);
    analysis_cache_free(&cache);
    if (failed) {
        odk_explain_release(out);
        kernel_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    return ODK_OK;
}

odk_status odk_explain_noul(odk_kernel *kernel, const char *statement,
                            const char *context, const odk_image *image,
                            odk_explain *out, odk_error *err)
{
    kernel_text_cfg eff;
    str_list_t chunks;
    char *anchor = NULL;
    analysis_cache_t cache;
    double base;
    double best_chunk = 0.0;
    double evidence;
    double affirm;
    size_t i;

    if (kernel == NULL || statement == NULL || statement[0] == '\0') {
        kernel_set_error(err, "explain noul requires a non-empty statement");
        return ODK_ERR_INFERENCE;
    }
    {
        size_t len = strlen(statement);
        size_t lo;
        size_t hi;
        odk_strip_bounds(statement, len, &lo, &hi);
        if (hi == lo) {
            kernel_set_error(err,
                             "explain noul requires a non-empty statement");
            return ODK_ERR_INFERENCE;
        }
    }
    explain_zero(out);
    str_list_init(&chunks);
    eff = kernel_effective_cfg(kernel, context);
    if (evidence_build(&eff, "", statement, context, image, NULL, 0, 1,
                       &chunks, &anchor, NULL) != 0) {
        str_list_free(&chunks);
        free(anchor);
        kernel_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    analysis_cache_init(&cache);
    {
        int failed = 0;
        if (kernel_similarity(&cache, &eff, statement, anchor, &base) != 0) {
            failed = 1;
        }
        for (i = 0; i < chunks.count && !failed; i++) {
            double value;
            if (kernel_similarity(&cache, &eff, statement, chunks.items[i],
                                  &value) != 0) {
                failed = 1;
                break;
            }
            if (value > best_chunk) {
                best_chunk = value;
            }
            if (best_chunk >= ODK_EARLY_EXIT_SIMILARITY) {
                break;
            }
        }
        /* capture the count BEFORE freeing (the free
         * re-zeroes the struct; the old code read it afterwards and
         * always reported 0). */
        out->chunk_count = chunks.count;
        analysis_cache_free(&cache);
        free(anchor);
        str_list_free(&chunks);
        if (failed) {
            kernel_set_error(err, "out of memory");
            return ODK_ERR_MEMORY;
        }
    }
    evidence = base > best_chunk ? base : best_chunk;
    affirm = kernel_sigmoid(eff.model->noul_logit_scale *
                            (evidence - 0.5));
    affirm = odk_clamp01(affirm);
    snprintf(out->primitive, sizeof(out->primitive), "noul");
    out->noul_whole = base;
    out->noul_best_chunk = best_chunk;
    out->noul_evidence = evidence;
    out->noul_affirm = affirm;
    return ODK_OK;
}

char *odk_explain_to_json(const odk_explain *explain)
{
    sb_t b;
    size_t i;
    char *out;

    if (explain == NULL) {
        return NULL;
    }
    sb_init(&b);
    if (sb_printf(&b, "{\"primitive\":") != 0) {
        goto oom;
    }
    {
        char *lit = odk_json_string_literal(explain->primitive,
                                            strlen(explain->primitive));
        if (lit == NULL || sb_puts(&b, lit) != 0) {
            free(lit);
            goto oom;
        }
        free(lit);
    }
    if (sb_printf(&b, ",\"chunk_count\":%zu", explain->chunk_count) != 0) {
        goto oom;
    }
    if (strcmp(explain->primitive, "noul") == 0) {
        if (sb_printf(&b,
                      ",\"noul\":{\"whole\":%.17g,\"best_chunk\":%.17g,"
                      "\"evidence\":%.17g,\"affirm\":%.17g}",
                      explain->noul_whole, explain->noul_best_chunk,
                      explain->noul_evidence, explain->noul_affirm) != 0) {
            goto oom;
        }
    }
    if (explain->options != NULL) {
        if (sb_printf(&b, ",\"options\":[") != 0) {
            goto oom;
        }
        for (i = 0; i < explain->option_count; i++) {
            const odk_explain_option *o = &explain->options[i];
            char *lit = odk_json_string_literal(
                o->label != NULL ? o->label : "", o->label != NULL
                                                     ? strlen(o->label)
                                                     : 0);
            if (lit == NULL) {
                goto oom;
            }
            if (sb_printf(&b,
                          "%s{\"index\":%zu,\"label\":%s,"
                          "\"tf_cosine\":%.17g,\"jaccard\":%.17g,"
                          "\"trigram\":%.17g,\"whole\":%.17g,"
                          "\"best_chunk\":%.17g,\"contextual\":%.17g,"
                          "\"directional\":%.17g,\"fused\":%.17g,"
                          "\"probability\":%.17g,\"extra\":[",
                          i > 0 ? "," : "", o->index, lit, o->tf_cosine,
                          o->jaccard, o->trigram, o->whole, o->best_chunk,
                          o->contextual, o->directional, o->fused,
                          o->probability) != 0) {
                free(lit);
                goto oom;
            }
            free(lit);
            {
                size_t k;
                for (k = 0; k < (size_t)o->extra_count; k++) {
                    if (sb_printf(&b, "%s%.17g", k > 0 ? "," : "",
                                  o->extra[k]) != 0) {
                        goto oom;
                    }
                }
            }
            if (sb_printf(&b, "],\"extra_count\":%d}",
                          o->extra_count) != 0) {
                goto oom;
            }
        }
        if (sb_puts(&b, "]") != 0) {
            goto oom;
        }
    }
    if (strcmp(explain->primitive, "score") == 0) {
        if (sb_printf(&b, ",\"score\":{\"normalized\":%.17g,\"peak\":%.17g}",
                      explain->score_normalized,
                      explain->score_peak) != 0) {
            goto oom;
        }
    }
    if (sb_puts(&b, "}") != 0) {
        goto oom;
    }
    out = b.data;
    return out;
oom:
    sb_free(&b);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Primitive: choice                                                   */
/* ------------------------------------------------------------------ */

odk_status odk_evaluate_choice(odk_kernel *kernel,
                               const char *instruction,
                               const char *const *options, size_t option_count,
                               const char *context, const odk_image *image,
                               odk_result *out, odk_error *err)
{
    return odk_evaluate_choice_opts(kernel, instruction, options,
                                    option_count, context, image, NULL, out,
                                    NULL, err);
}

static odk_status choice_call_impl(odk_kernel *kernel,
                                   const char *instruction,
                                   const char *const *options,
                                   size_t option_count,
                                   const char *context,
                                   const odk_image *image,
                                   const odk_output_caps *caps,
                                   odk_result *out, odk_result_meta *meta,
                                   const odk_call_config *call,
                                        odk_error *err)
{
    double *scores = NULL;
    double *probs = NULL;
    size_t best_index = 0;
    size_t i;
    int rc;
    kernel_text_cfg eff;

    (void)caps; /* choice has no capped rendering (caps are
                   rank/extract-only) */
    result_init(out);
    if (options == NULL || option_count == 0) {
        kernel_set_error(err, "choice evaluation requires at least one option");
        return ODK_ERR_INFERENCE;
    }
    scores = malloc(option_count * sizeof(double));
    probs = malloc(option_count * sizeof(double));
    if (scores == NULL || probs == NULL) {
        goto oom;
    }
    eff = kernel_effective_cfg_call(kernel, context, call);
    fill_language_meta(meta, &eff, context);
    if (score_options(&eff, options, option_count, instruction, context,
                      image, scores, meta) != 0) {
        goto oom;
    }
    if (calibrated_distribution(scores, option_count, probs,
                                eff.model->logit_scale) != 0) {
        goto oom;
    }
    for (i = 1; i < option_count; i++) {
        if (scores[i] > scores[best_index]) {
            best_index = i;
        }
    }
    rc = result_set_primitive(out, "choice");
    if (rc == 0) {
        out->kind = ODK_VALUE_STRING;
        out->string = odk_strdup(options[best_index]);
        if (out->string == NULL) {
            rc = -1;
        }
    }
    if (rc == 0) {
        out->confidence = odk_round4(probs[best_index]);
        out->probability = odk_round4(probs[best_index]);
        rc = result_set_distribution(out, options, probs, option_count);
    }
    free(scores);
    free(probs);
    if (rc != 0) {
        odk_result_release(out);
        kernel_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    return ODK_OK;

oom:
    free(scores);
    free(probs);
    odk_result_release(out);
    kernel_set_error(err, "out of memory");
    return ODK_ERR_MEMORY;
}

/* ------------------------------------------------------------------ */
/* Primitive: score                                                    */
/* ------------------------------------------------------------------ */

odk_status odk_evaluate_score(odk_kernel *kernel,
                              const char *instruction,
                              const char *const *rubric, size_t rubric_count,
                              const char *context, const odk_image *image,
                              odk_result *out, odk_error *err)
{
    return odk_evaluate_score_opts(kernel, instruction, rubric, rubric_count,
                                   context, image, NULL, out, NULL, err);
}

static odk_status score_call_impl(odk_kernel *kernel,
                                  const char *instruction,
                                  const char *const *rubric,
                                  size_t rubric_count, const char *context,
                                  const odk_image *image,
                                  const odk_output_caps *caps,
                                  odk_result *out, odk_result_meta *meta,
                                  const odk_call_config *call,
                                        odk_error *err)
{
    (void)caps; /* score has no capped rendering */
    static const char *const kDefaultLevels[] = {"very low", "low", "medium",
                                                 "high", "very high"};
    str_list_t levels;
    const char *const *level_labels;
    str_list_t chunks;
    char *anchor = NULL;
    kernel_text_cfg eff;
    analysis_cache_t acache;
    double *scores = NULL;
    double *probs = NULL;
    double normalized = 0.5;
    double peak_probability = 0.0;
    size_t level_count;
    size_t i;
    int rc = 0;

    result_init(out);
    str_list_init(&levels);
    str_list_init(&chunks);
    analysis_cache_init(&acache);
    for (i = 0; rubric != NULL && i < rubric_count; i++) {
        const char *level = rubric[i] != NULL ? rubric[i] : "";
        size_t len = strlen(level);
        size_t lo;
        size_t hi;
        odk_strip_bounds(level, len, &lo, &hi);
        if (hi > lo) {
            if (str_list_push(&levels, level + lo, hi - lo) != 0) {
                rc = -1;
                goto done;
            }
        }
    }
    if (levels.count == 0) {
        size_t k;
        for (k = 0; k < 5; k++) {
            if (str_list_push(&levels, kDefaultLevels[k],
                              strlen(kDefaultLevels[k])) != 0) {
                rc = -1;
                goto done;
            }
        }
    }
    level_labels = (const char *const *)levels.items;
    level_count = levels.count;

    /* calloc: zeroed storage keeps -Wmaybe-uninitialized satisfied on
     * paths the compiler cannot prove fully initialised. */
    scores = calloc(level_count, sizeof(double));
    probs = calloc(level_count, sizeof(double));
    if (scores == NULL || probs == NULL) {
        rc = -1;
        goto done;
    }
    eff = kernel_effective_cfg_call(kernel, context, call);
    fill_language_meta(meta, &eff, context);
    if (evidence_build(&eff, instruction, instruction, context, image,
                       level_labels, level_count, 1, &chunks, &anchor,
                       meta) != 0) {
        rc = -1;
        goto done;
    }
    for (i = 0; i < level_count; i++) {
        double contextual;
        double directional = 0.0;
        if (score_against_context(&acache, &eff, level_labels[i], anchor,
                                  &chunks, &contextual) != 0) {
            rc = -1;
            goto done;
        }
        if (instruction != NULL && instruction[0] != '\0') {
            if (kernel_similarity(&acache, &eff, level_labels[i],
                                  instruction, &directional) != 0) {
                rc = -1;
                goto done;
            }
        }
        {
            double extra_sum = 0.0;
            if (eff.model->feature_set_id == 1) {
                double feats[ODK_MODEL_EXTRA_MAX];
                size_t k;
                extra_features_l(&acache, &eff, level_labels[i], anchor,
                                 instruction, level_labels, level_count,
                                 i, feats);
                for (k = 0; k < (size_t)eff.model->extra_count; k++) {
                    extra_sum += eff.model->extra[k] * feats[k];
                }
            }
            scores[i] = odk_clamp01(eff.model->w_ctx * contextual +
                                    eff.model->w_dir * directional +
                                    extra_sum);
        }
    }
    if (calibrated_distribution(scores, level_count, probs,
                                eff.model->logit_scale) != 0) {
        rc = -1;
        goto done;
    }
    if (level_count == 1) {
        normalized = 0.5;
    } else {
        normalized = 0.0;
        for (i = 0; i < level_count; i++) {
            normalized += probs[i] * ((double)i / (double)(level_count - 1));
        }
    }
    peak_probability = probs[0];
    for (i = 1; i < level_count; i++) {
        if (probs[i] > peak_probability) {
            peak_probability = probs[i];
        }
    }

    rc = result_set_primitive(out, "score");
    if (rc == 0) {
        out->kind = ODK_VALUE_NUMBER;
        out->number = odk_round4(odk_clamp01(normalized));
        out->confidence = odk_round4(peak_probability);
        out->probability = odk_round4(peak_probability);
        rc = result_set_distribution(out, level_labels, probs, level_count);
    }

done:
    free(anchor);
    analysis_cache_free(&acache);
    free(scores);
    free(probs);
    str_list_free(&chunks);
    str_list_free(&levels);
    if (rc != 0) {
        odk_result_release(out);
        kernel_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    return ODK_OK;
}

/* ------------------------------------------------------------------ */
/* Primitive: noul                                                     */
/* ------------------------------------------------------------------ */

odk_status odk_evaluate_noul(odk_kernel *kernel, const char *statement,
                             const char *context, const odk_image *image,
                             odk_result *out, odk_error *err)
{
    return odk_evaluate_noul_opts(kernel, statement, context, image, NULL,
                                  out, NULL, err);
}

static odk_status noul_call_impl(odk_kernel *kernel, const char *statement,
                                 const char *context,
                                 const odk_image *image,
                                 const odk_output_caps *caps,
                                 odk_result *out, odk_result_meta *meta,
                                 const odk_call_config *call,
                                        odk_error *err)
{
    str_list_t chunks;
    char *anchor = NULL;
    kernel_text_cfg eff;
    analysis_cache_t acache;
    double base;
    double best_chunk = 0.0;
    double evidence;
    double affirm;
    size_t i;
    int rc;

    (void)caps; /* noul has no capped rendering */
    result_init(out);
    if (statement == NULL || statement[0] == '\0') {
        kernel_set_error(err, "noul evaluation requires a non-empty statement");
        return ODK_ERR_INFERENCE;
    }
    {
        size_t len = strlen(statement);
        size_t lo;
        size_t hi;
        odk_strip_bounds(statement, len, &lo, &hi);
        if (hi == lo) {
            kernel_set_error(err,
                             "noul evaluation requires a non-empty statement");
            return ODK_ERR_INFERENCE;
        }
    }
    str_list_init(&chunks);
    eff = kernel_effective_cfg_call(kernel, context, call);
    fill_language_meta(meta, &eff, context);
    if (evidence_build(&eff, "", statement, context, image, NULL, 0, 1,
                       &chunks, &anchor, meta) != 0) {
        str_list_free(&chunks);
        free(anchor);
        kernel_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    analysis_cache_init(&acache);
    if (kernel_similarity(&acache, &eff, statement, anchor, &base) != 0) {
        analysis_cache_free(&acache);
        str_list_free(&chunks);
        free(anchor);
        kernel_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    for (i = 0; i < chunks.count; i++) {
        double value;
        if (kernel_similarity(&acache, &eff, statement, chunks.items[i],
                              &value) != 0) {
            analysis_cache_free(&acache);
            str_list_free(&chunks);
            free(anchor);
            kernel_set_error(err, "out of memory");
            return ODK_ERR_MEMORY;
        }
        if (value > best_chunk) {
            best_chunk = value;
        }
        if (best_chunk >= ODK_EARLY_EXIT_SIMILARITY) {
            break;
        }
    }
    analysis_cache_free(&acache);
    evidence = base > best_chunk ? base : best_chunk;
    affirm = kernel_sigmoid(eff.model->noul_logit_scale *
                            (evidence - 0.5));
    affirm = odk_clamp01(affirm);

    {
        const char *labels[2] = {"affirm", "deny"};
        double values[2];
        values[0] = affirm;
        values[1] = 1.0 - affirm;
        rc = result_set_primitive(out, "noul");
        if (rc == 0) {
            out->kind = ODK_VALUE_BOOL;
            out->boolean = affirm >= ODK_NOUL_DECISION_THRESHOLD ? 1 : 0;
            out->confidence =
                odk_round4(affirm > 1.0 - affirm ? affirm : 1.0 - affirm);
            out->probability = odk_round4(affirm);
            rc = result_set_distribution(out, labels, values, 2);
        }
    }
    str_list_free(&chunks);
    free(anchor);
    if (rc != 0) {
        odk_result_release(out);
        kernel_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    return ODK_OK;
}

/* ------------------------------------------------------------------ */
/* Primitive: rank                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    double score;
    size_t index;
} score_index_pair;

static int rank_order_cmp(const void *a, const void *b)
{
    const score_index_pair *pa = a;
    const score_index_pair *pb = b;
    /* Stable ordering: score
     * descending, original index ascending on ties.  Scores are clamped,
     * so NaN handling is unnecessary. */
    if (pa->score != pb->score) {
        return (pa->score < pb->score) ? 1 : -1;
    }
    return (pa->index > pb->index) ? 1 : (pa->index < pb->index ? -1 : 0);
}

odk_status odk_evaluate_rank(odk_kernel *kernel,
                             const char *instruction,
                             const char *const *options, size_t option_count,
                             const char *context, const odk_image *image,
                             odk_result *out, odk_error *err)
{
    return odk_evaluate_rank_opts(kernel, instruction, options,
                                  option_count, context, image, NULL, out,
                                  NULL, err);
}

static odk_status rank_call_impl(odk_kernel *kernel,
                                 const char *instruction,
                                 const char *const *options,
                                 size_t option_count,
                                 const char *context, const odk_image *image,
                                 const odk_output_caps *caps,
                                 odk_result *out, odk_result_meta *meta,
                                 const odk_call_config *call,
                                        odk_error *err)
{
    double *scores = NULL;
    double *probs = NULL;
    score_index_pair *order = NULL;
    const char **ranked_labels = NULL;
    double *ranked_probs = NULL;
    kernel_text_cfg eff;
    size_t max_ranked;
    size_t shown;
    size_t i;
    int rc;

    result_init(out);
    if (options == NULL || option_count < ODK_MIN_RANK_OPTIONS) {
        kernel_set_error(err, "rank evaluation requires at least two options");
        return ODK_ERR_INFERENCE;
    }
    scores = malloc(option_count * sizeof(double));
    probs = malloc(option_count * sizeof(double));
    order = malloc(option_count * sizeof(*order));
    ranked_labels = malloc(option_count * sizeof(char *));
    ranked_probs = malloc(option_count * sizeof(double));
    if (scores == NULL || probs == NULL || order == NULL ||
        ranked_labels == NULL || ranked_probs == NULL) {
        goto oom;
    }
    eff = kernel_effective_cfg_call(kernel, context, call);
    fill_language_meta(meta, &eff, context);
    if (score_options(&eff, options, option_count, instruction, context,
                      image, scores, meta) != 0 ||
        calibrated_distribution(scores, option_count, probs,
                                eff.model->logit_scale) != 0) {
        goto oom;
    }
    for (i = 0; i < option_count; i++) {
        order[i].score = scores[i];
        order[i].index = i;
    }
    qsort(order, option_count, sizeof(*order), rank_order_cmp);
    for (i = 0; i < option_count; i++) {
        ranked_labels[i] = options[order[i].index];
        ranked_probs[i] = probs[order[i].index];
    }
    /* max_ranked caps the RENDERED string only - the ranked
     * array (LIST storage), distribution and confidence stay complete
     *.  No textual ellipsis: the cap is metadata. */
    max_ranked = eff.max_ranked;
    if (caps != NULL && caps->max_ranked != 0) {
        max_ranked = caps->max_ranked;
    }
    if (max_ranked > ODK_RANKED_CEILING) {
        max_ranked = ODK_RANKED_CEILING;
    }
    shown = option_count;
    if (max_ranked != 0 && max_ranked < shown) {
        shown = max_ranked;
    }
    {
        double margin_sum = 0.0;
        double confidence;
        char *joined;
        for (i = 0; i + 1 < option_count; i++) {
            margin_sum += ranked_probs[i] - ranked_probs[i + 1];
        }
        confidence = margin_sum / (double)(option_count - 1);
        joined = odk_str_join(ranked_labels, shown, " > ");
        if (joined == NULL) {
            goto oom;
        }
        rc = result_set_primitive(out, "rank");
        if (rc == 0) {
            out->kind = ODK_VALUE_STRING;
            out->string = joined;
            out->confidence = odk_round4(odk_clamp01(confidence));
            out->probability = odk_round4(ranked_probs[0]);
            rc = result_set_distribution(out, ranked_labels, ranked_probs,
                                         option_count);
        } else {
            free(joined);
        }
        /* LIST storage: the FULL ranked order, released by
         * odk_result_release; the kind stays STRING (frozen contract) */
        if (rc == 0) {
            out->items = malloc(option_count * sizeof(char *));
            if (out->items == NULL) {
                rc = -1;
            } else {
                for (i = 0; i < option_count; i++) {
                    out->items[i] = odk_strdup(ranked_labels[i]);
                    if (out->items[i] == NULL) {
                        rc = -1;
                        break;
                    }
                }
                out->item_count = option_count;
            }
        }
    }
    if (meta != NULL) {
        meta->total_options = option_count;
        meta->truncated = shown < option_count;
    }
    free(scores);
    free(probs);
    free(order);
    free(ranked_labels);
    free(ranked_probs);
    if (rc != 0) {
        odk_result_release(out);
        kernel_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    return ODK_OK;

oom:
    free(scores);
    free(probs);
    free(order);
    free(ranked_labels);
    free(ranked_probs);
    odk_result_release(out);
    kernel_set_error(err, "out of memory");
    return ODK_ERR_MEMORY;
}

/* The structured ranked array behind a rank result.  Items
 * borrow the result's LIST labels and read the distribution entries in
 * ranked order (insertion order == ranked order for rank results). */
size_t odk_result_rank_items(const odk_result *result,
                             const char *const *options,
                             size_t option_count, odk_ranked_item **out_items)
{
    odk_ranked_item *items;
    size_t i;
    size_t *resolved;
    size_t count;

    if (out_items == NULL) {
        return 0;
    }
    *out_items = NULL;
    if (result == NULL || result->primitive == NULL ||
        strcmp(result->primitive, "rank") != 0 ||
        result->items == NULL || result->item_count == 0 ||
        result->distribution.count != result->item_count) {
        return 0;
    }
    count = result->item_count;
    items = malloc(count * sizeof(*items));
    resolved = calloc(option_count, sizeof(*resolved));
    if (items == NULL || (option_count != 0 && resolved == NULL)) {
        free(items);
        free(resolved);
        return 0;
    }
    for (i = 0; i < count; i++) {
        size_t j;
        size_t index = (size_t)-1;
        /* pointer identity first */
        for (j = 0; j < option_count; j++) {
            if (options[j] == result->items[i]) {
                index = j;
                break;
            }
        }
        if (index == (size_t)-1) {
            /* content match of not-yet-resolved entries */
            for (j = 0; j < option_count; j++) {
                if (resolved[j] == 0 && options[j] != NULL &&
                    strcmp(options[j], result->items[i]) == 0) {
                    index = j;
                    break;
                }
            }
        }
        if (index != (size_t)-1 && index < option_count) {
            resolved[index] = 1;
        } else {
            index = (size_t)-1; /* SIZE_MAX: unresolved */
        }
        items[i].label = result->items[i];
        items[i].index = index;
        items[i].score = result->distribution.entries[i].value;
    }
    free(resolved);
    *out_items = items;
    return count;
}

void odk_result_rank_items_free(odk_ranked_item *items)
{
    free(items);
}

/* ------------------------------------------------------------------ */
/* Primitive: compare                                                  */
/* ------------------------------------------------------------------ */

odk_status odk_evaluate_compare(odk_kernel *kernel,
                                const char *instruction,
                                const char *const *options,
                                size_t option_count,
                                const char *context, const odk_image *image,
                                odk_result *out, odk_error *err)
{
    return odk_evaluate_compare_opts(kernel, instruction, options,
                                     option_count, context, image, NULL,
                                     out, NULL, err);
}

static odk_status compare_call_impl(odk_kernel *kernel,
                                    const char *instruction,
                                    const char *const *options,
                                    size_t option_count,
                                    const char *context,
                                    const odk_image *image,
                                    const odk_output_caps *caps,
                                    odk_result *out, odk_result_meta *meta,
                                    const odk_call_config *call,
                                        odk_error *err)
{
    double scores[2];
    double probs[2];
    double margin;
    double confidence;
    const char *result_label;
    const char *labels[2];
    kernel_text_cfg eff;
    int rc;

    (void)caps; /* compare has no capped rendering */
    result_init(out);
    if (options == NULL || option_count != ODK_COMPARE_REQUIRED_OPTIONS) {
        kernel_set_error(err,
                         "compare evaluation requires exactly two options");
        return ODK_ERR_INFERENCE;
    }
    eff = kernel_effective_cfg_call(kernel, context, call);
    fill_language_meta(meta, &eff, context);
    if (score_options(&eff, options, 2, instruction, context, image, scores,
                      meta) != 0 ||
        calibrated_distribution(scores, 2, probs,
                                eff.model->logit_scale) != 0) {
        kernel_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    labels[0] = options[0];
    labels[1] = options[1];
    margin = scores[0] - scores[1];
    if (fabs(margin) < ODK_COMPARE_TIE_MARGIN) {
        result_label = "tie";
        confidence = 1.0 - fabs(probs[0] - probs[1]);
    } else if (margin > 0.0) {
        result_label = options[0];
        confidence = probs[0];
    } else {
        result_label = options[1];
        confidence = probs[1];
    }
    rc = result_set_primitive(out, "compare");
    if (rc == 0) {
        out->kind = ODK_VALUE_STRING;
        out->string = odk_strdup(result_label);
        if (out->string == NULL) {
            rc = -1;
        }
    }
    if (rc == 0) {
        double peak = probs[0] > probs[1] ? probs[0] : probs[1];
        out->confidence = odk_round4(odk_clamp01(confidence));
        out->probability = odk_round4(peak);
        rc = result_set_distribution(out, labels, probs, 2);
    }
    if (rc != 0) {
        odk_result_release(out);
        kernel_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    return ODK_OK;
}

/* ------------------------------------------------------------------ */
/* Primitive: extract                                                  */
/* ------------------------------------------------------------------ */

static int extract_order_cmp(const void *a, const void *b)
{
    const score_index_pair *pa = a;
    const score_index_pair *pb = b;
    /* Order by score descending, then original index ascending. */
    if (pa->score != pb->score) {
        return (pa->score < pb->score) ? 1 : -1;
    }
    return (pa->index > pb->index) ? 1 : (pa->index < pb->index ? -1 : 0);
}

/* Clip one snippet to at most cap_cps (bytes in
 * ASCII_COMPAT, code points in UNICODE), reproducing the legacy marker
 * semantics ("..." appended after a cut; the strip-bounds pass runs on
 * the clipped window exactly as the legacy path does).  The marker is presentation
 * and is NOT counted toward caps.  Returns a malloc'd string or NULL. */
static char *clip_snippet(const kernel_text_cfg *cfg, const char *snippet,
                          size_t cap_cps)
{
    size_t len = strlen(snippet);

    if (cfg->mode == ODK_TEXT_ASCII_COMPAT) {
        if (len > cap_cps) {
            size_t lo;
            size_t hi;
            odk_strip_bounds(snippet, cap_cps, &lo, &hi);
            {
                char *stored = malloc(hi - lo + 4);
                if (stored != NULL) {
                    memcpy(stored, snippet + lo, hi - lo);
                    memcpy(stored + (hi - lo), "...", 3);
                    stored[hi - lo + 3] = '\0';
                }
                return stored;
            }
        }
        return odk_strdup(snippet);
    }
    {
        size_t snippet_cps = odk_utf8_codepoints(snippet, len);
        if (snippet_cps > cap_cps) {
            size_t kept = odk_u_truncate_cp(snippet, len, cap_cps);
            size_t lo;
            size_t hi;
            odk_u_strip_bounds(snippet, kept, &lo, &hi);
            {
                char *stored = malloc(hi - lo + 4);
                if (stored != NULL) {
                    memcpy(stored, snippet + lo, hi - lo);
                    memcpy(stored + (hi - lo), "...", 3);
                    stored[hi - lo + 3] = '\0';
                }
                return stored;
            }
        }
        return odk_strdup(snippet);
    }
}

odk_status odk_evaluate_extract(odk_kernel *kernel, const char *instruction,
                                const char *context, const odk_image *image,
                                int top_k, odk_result *out, odk_error *err)
{
    return odk_evaluate_extract_opts(kernel, instruction, context, image,
                                     top_k, NULL, out, NULL, err);
}

static odk_status extract_call_impl(odk_kernel *kernel,
                                    const char *instruction,
                                    const char *statement,
                                    const char *context,
                                    const odk_image *image, int top_k,
                                    const odk_output_caps *caps,
                                    odk_result *out, odk_result_meta *meta,
                                    const odk_call_config *call,
                                    odk_error *err)
{
    str_list_t chunks;
    score_index_pair *scored = NULL;
    char **owned_snippets = NULL;
    double *selected_scores = NULL;
    kernel_text_cfg eff;
    analysis_cache_t acache;
    size_t limit;
    size_t emitted = 0;
    size_t max_snippet;
    size_t max_total;
    size_t running = 0;
    int done = 0;
    int truncated = 0;
    size_t i;
    int rc;

    (void)image; /* the image channel only feeds the anchor of score-based
                    primitives; extract similarity is text-only */
    result_init(out);
    if (instruction == NULL || instruction[0] == '\0') {
        kernel_set_error(err,
                         "extract evaluation requires a non-empty instruction");
        return ODK_ERR_INFERENCE;
    }
    /* Optional statement follows the query
     * extract semantics - instruction wins, statement is the
     * fallback text source.  The _opts form passes NULL. */
    if ((instruction == NULL || instruction[0] == '\0') && statement != NULL
        && statement[0] != '\0') {
        instruction = statement;
    }
    eff = kernel_effective_cfg_call(kernel, context, call);
    fill_language_meta(meta, &eff, context);
    {
        size_t len = strlen(instruction);
        size_t lo;
        size_t hi;
        if (eff.mode == ODK_TEXT_ASCII_COMPAT) {
            odk_strip_bounds(instruction, len, &lo, &hi);
        } else {
            odk_u_strip_bounds(instruction, len, &lo, &hi);
        }
        if (hi == lo) {
            kernel_set_error(
                err, "extract evaluation requires a non-empty instruction");
            return ODK_ERR_INFERENCE;
        }
    }
    /* extract has no anchor channel: the evidence view is chunks-only */
    str_list_init(&chunks);
    if (evidence_build(&eff, NULL, instruction, context, image, NULL, 0, 0,
                       &chunks, NULL, meta) != 0) {
        kernel_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    analysis_cache_init(&acache);
    if (chunks.count == 0) {
        analysis_cache_free(&acache);
        str_list_free(&chunks);
        rc = result_set_primitive(out, "extract");
        if (rc == 0) {
            out->kind = ODK_VALUE_LIST;
            out->items = NULL;
            out->item_count = 0;
            out->confidence = 0.0;
            out->probability = 0.0;
        }
        if (rc != 0) {
            odk_result_release(out);
            kernel_set_error(err, "out of memory");
            return ODK_ERR_MEMORY;
        }
        return ODK_OK;
    }

    if (top_k < 1) {
        limit = 1;
    } else if (top_k > 10) {
        limit = 10;
    } else {
        limit = (size_t)top_k;
    }
    if (limit > chunks.count) {
        limit = chunks.count;
    }
    /* Effective per-snippet and total caps.  Defaults never
     * bind (10 snippets x snippet_max stay below 10,000), so default
     * output is byte-identical to the legacy output. */
    max_snippet = eff.max_snippet_chars != 0 ? eff.max_snippet_chars
                                             : eff.snippet_max;
    max_total = eff.max_total_chars != 0 ? eff.max_total_chars
                                         : ODK_TOTAL_CHARS_DEFAULT;
    if (caps != NULL && caps->max_snippet_chars != 0) {
        max_snippet = caps->max_snippet_chars;
    }
    if (caps != NULL && caps->max_total_chars != 0) {
        max_total = caps->max_total_chars;
    }
    if (max_snippet > ODK_SNIPPET_CAP_CEILING) {
        max_snippet = ODK_SNIPPET_CAP_CEILING;
    }
    if (max_total > ODK_TOTAL_CHARS_CEILING) {
        max_total = ODK_TOTAL_CHARS_CEILING;
    }
    scored = malloc(chunks.count * sizeof(*scored));
    owned_snippets = calloc(limit, sizeof(char *));
    selected_scores = malloc(limit * sizeof(double));
    if (scored == NULL || owned_snippets == NULL || selected_scores == NULL) {
        goto oom;
    }
    for (i = 0; i < chunks.count; i++) {
        double value;
        if (kernel_similarity(&acache, &eff, instruction, chunks.items[i],
                              &value) != 0) {
            goto oom;
        }
        scored[i].score = value;
        scored[i].index = i;
    }
    qsort(scored, chunks.count, sizeof(*scored), extract_order_cmp);

    rc = result_set_primitive(out, "extract");
    for (i = 0; rc == 0 && i < limit; i++) {
        const char *snippet = chunks.items[scored[i].index];
        size_t len = strlen(snippet);
        size_t snippet_cps =
            eff.mode == ODK_TEXT_ASCII_COMPAT
                ? len
                : odk_utf8_codepoints(snippet, len);
        size_t capped_cps =
            snippet_cps < max_snippet ? snippet_cps : max_snippet;
        char *stored = NULL;

        if (done) {
            continue;
        }
        if (running >= max_total) {
            done = 1;
            continue;
        }
        if (running + capped_cps <= max_total) {
            /* Fits fully: the legacy emission path with max_snippet */
            stored = clip_snippet(&eff, snippet, max_snippet);
            if (stored == NULL) {
                rc = -1;
                break;
            }
            if (capped_cps < snippet_cps) {
                truncated = 1; /* per-snippet cap cut */
            }
            running += capped_cps;
        } else {
            /* the first snippet that would cross the cap is CUT on a
             * code-point boundary to exactly the remaining budget; a
             * snippet reduced to 0 cps is dropped */
            size_t remaining = max_total - running;
            if (remaining == 0) {
                    done = 1;
                continue;
            }
            stored = clip_snippet(&eff, snippet, remaining);
            if (stored == NULL) {
                rc = -1;
                break;
            }
            truncated = 1;
            running += remaining;
            done = 1; /* every following snippet is dropped */
        }
        owned_snippets[emitted] = stored;
        selected_scores[emitted] = scored[i].score;
        emitted++;
    }
    if (rc == 0) {
        out->kind = ODK_VALUE_LIST;
        out->items = owned_snippets;
        out->item_count = emitted;
        owned_snippets = NULL; /* ownership moved to the result */
        if (meta != NULL) {
            meta->emitted = emitted;
            meta->dropped = limit - emitted;
            meta->truncated = truncated;
        }
        out->confidence =
            emitted != 0
                ? odk_round4(odk_clamp01(selected_scores[0]))
                : 0.0;
        out->probability =
            emitted != 0
                ? odk_round4(odk_clamp01(selected_scores[0]))
                : 0.0;
        {
            odk_distribution_entry *entries =
                calloc(emitted != 0 ? emitted : 1,
                       sizeof(odk_distribution_entry));
            if (entries == NULL) {
                rc = -1;
            } else {
                for (i = 0; i < emitted; i++) {
                    char key[32];
                    snprintf(key, sizeof(key), "snippet_%zu", i + 1);
                    entries[i].key = odk_strdup(key);
                    if (entries[i].key == NULL) {
                        rc = -1;
                        /* free the keys allocated so far */
                        {
                            size_t k;
                            for (k = 0; k < i; k++) {
                                free(entries[k].key);
                            }
                        }
                        break;
                    }
                    entries[i].value = odk_round4(selected_scores[i]);
                }
                if (rc == 0) {
                    out->distribution.entries = entries;
                    out->distribution.count = emitted;
                } else {
                    free(entries);
                }
            }
        }
    }
    {
        size_t k;
        if (owned_snippets != NULL) {
            for (k = 0; k < limit; k++) {
                free(owned_snippets[k]);
            }
        }
    }
    free(scored);
    free(owned_snippets);
    free(selected_scores);
    analysis_cache_free(&acache);
    str_list_free(&chunks);
    if (rc != 0) {
        odk_result_release(out);
        kernel_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    return ODK_OK;

oom:
    {
        size_t k;
        if (owned_snippets != NULL) {
            for (k = 0; k < limit; k++) {
                free(owned_snippets[k]);
            }
        }
    }
    free(scored);
    free(owned_snippets);
    free(selected_scores);
    analysis_cache_free(&acache);
    str_list_free(&chunks);
    odk_result_release(out);
    kernel_set_error(err, "out of memory");
    return ODK_ERR_MEMORY;
}

/* ------------------------------------------------------------------ */
/* Dispatcher (evaluate_query)                                         */
/* ------------------------------------------------------------------ */

odk_status odk_evaluate_query(odk_kernel *kernel, const odk_query *query,
                              const char *context, const odk_image *image,
                              odk_result *out, odk_error *err)
{
    return odk_evaluate_query_opts(kernel, query, context, image, NULL, out,
                                   NULL, err);
}

/* Caps- and meta-aware dispatcher (the base function is a thin
 * wrapper with NULL caps/meta). */
static odk_status query_call_impl(odk_kernel *kernel, const odk_query *query,
                                  const char *context, const odk_image *image,
                                  const odk_output_caps *caps,
                                  odk_result *out, odk_result_meta *meta,
                                  const odk_call_config *call,
                                        odk_error *err)
{
    const char *primitive;

    if (query == NULL || query->primitive == NULL) {
        kernel_set_error(err, "unsupported decision primitive: (none)");
        return ODK_ERR_INFERENCE;
    }
    primitive = query->primitive;
    /* The per-call language threads through the whole query
     * dispatch. */
    if (strcmp(primitive, "choice") == 0) {
        return choice_call_impl(
            kernel, query->instruction, (const char *const *)query->options,
            query->option_count, context, image, caps, out, meta, call, err);
    }
    if (strcmp(primitive, "score") == 0) {
        return score_call_impl(kernel, query->instruction,
                                (const char *const *)query->rubric,
                                query->rubric_count, context, image, caps,
                                out, meta, call, err);
    }
    if (strcmp(primitive, "noul") == 0) {
        const char *statement =
            (query->statement != NULL && query->statement[0] != '\0')
                ? query->statement
                : query->instruction;
        return noul_call_impl(kernel, statement, context, image, caps, out,
                              meta, call, err);
    }
    if (strcmp(primitive, "rank") == 0) {
        return rank_call_impl(
            kernel, query->instruction, (const char *const *)query->options,
            query->option_count, context, image, caps, out, meta, call, err);
    }
    if (strcmp(primitive, "compare") == 0) {
        return compare_call_impl(
            kernel, query->instruction, (const char *const *)query->options,
            query->option_count, context, image, caps, out, meta, call, err);
    }
    if (strcmp(primitive, "extract") == 0) {
        const char *text =
            (query->instruction != NULL && query->instruction[0] != '\0')
                ? query->instruction
                : query->statement;
        return extract_call_impl(kernel, text, NULL, context, image,
                                 query->top_k, caps, out, meta, call, err);
    }
    kernel_set_error(err, "unsupported decision primitive: '%s'", primitive);
    return ODK_ERR_INFERENCE;
}

/* ------------------------------------------------------------------ */
/* Internal hooks for the outcome-learning module (see odk_learn.c).   */
/* Pure plumbing: forward to the static scoring path, no new behaviour. */
/* ------------------------------------------------------------------ */

int odk_internal_score_options(const char *const *options, size_t option_count,
                               const char *instruction, const char *context,
                               const odk_image *image, double *scores)
{
    return score_options(&kCompatCfg, options, option_count, instruction,
                         context, image, scores, NULL);
}

/* Mode-aware variant used by the learning module when the kernel
 * is configured for UNICODE text. */
int odk_internal_score_options_k(const odk_kernel *kernel,
                                 const char *const *options,
                                 size_t option_count,
                                 const char *instruction, const char *context,
                                 const odk_image *image, double *scores)
{
    kernel_text_cfg eff = kernel_effective_cfg(kernel, context);
    return score_options(&eff, options, option_count, instruction, context,
                         image, scores, NULL);
}

/* Link-aware scoring entry for the learning module (odk_learn.c).
 * explain (optional) collects the JSON array of fired links. */
int odk_internal_score_options_linked(const odk_kernel *kernel,
                                      const odk_link_store *links,
                                      const char *const *options,
                                      size_t option_count,
                                      const char *instruction,
                                      const char *context,
                                      const odk_image *image, double *scores,
                                      sb_t *explain)
{
    kernel_text_cfg eff = kernel_effective_cfg(kernel, context);
    return score_options_l(&eff, links, options, option_count, instruction,
                           context, image, scores, explain, NULL);
}

int odk_internal_calibrated_distribution(const double *scores, size_t count,
                                         double *probs)
{
    /* internal consumers (brain learning) keep the built-in scale in
     * the brain path is outside the model v1 scope */
    return calibrated_distribution(scores, count, probs,
                                   odk_default_model.logit_scale);
}



/* ------------------------------------------------------------------ */
/* Public wrappers: _opts (no call config) and _call (per-call  */
/* language override).  Decisions with the default language */
/* are bit-identical to the _opts forms (call == NULL path).          */
/* ------------------------------------------------------------------ */

odk_status odk_evaluate_choice_opts(odk_kernel *kernel,
                                    const char *instruction,
                                    const char *const *options,
                                    size_t option_count,
                                    const char *context,
                                    const odk_image *image,
                                    const odk_output_caps *caps,
                                    odk_result *out, odk_result_meta *meta,
                                    odk_error *err)
{
    return choice_call_impl(kernel, instruction, options, option_count,
                            context, image, caps, out, meta, NULL, err);
}

odk_status odk_evaluate_choice_call(odk_kernel *kernel,
                                    const char *instruction,
                                    const char *const *options,
                                    size_t option_count, const char *context,
                                    const odk_image *image,
                                    const odk_call_config *call,
                                    const odk_output_caps *caps,
                                    odk_result *out, odk_result_meta *meta,
                                    odk_error *err)
{
    return choice_call_impl(kernel, instruction, options, option_count,
                            context, image, caps, out, meta, call, err);
}

odk_status odk_evaluate_score_opts(odk_kernel *kernel,
                                   const char *instruction,
                                   const char *const *rubric,
                                   size_t rubric_count, const char *context,
                                   const odk_image *image,
                                   const odk_output_caps *caps,
                                   odk_result *out, odk_result_meta *meta,
                                   odk_error *err)
{
    return score_call_impl(kernel, instruction, rubric, rubric_count,
                           context, image, caps, out, meta, NULL, err);
}

odk_status odk_evaluate_score_call(odk_kernel *kernel,
                                   const char *instruction,
                                   const char *const *rubric,
                                   size_t rubric_count, const char *context,
                                   const odk_image *image,
                                   const odk_call_config *call,
                                   const odk_output_caps *caps,
                                   odk_result *out, odk_result_meta *meta,
                                   odk_error *err)
{
    return score_call_impl(kernel, instruction, rubric, rubric_count,
                           context, image, caps, out, meta, call, err);
}

odk_status odk_evaluate_noul_opts(odk_kernel *kernel, const char *statement,
                                 const char *context,
                                 const odk_image *image,
                                 const odk_output_caps *caps,
                                 odk_result *out, odk_result_meta *meta,
                                 odk_error *err)
{
    return noul_call_impl(kernel, statement, context, image, caps, out,
                          meta, NULL, err);
}

odk_status odk_evaluate_noul_call(odk_kernel *kernel,
                                  const char *statement,
                                  const char *context, const odk_image *image,
                                  const odk_call_config *call,
                                  const odk_output_caps *caps,
                                  odk_result *out, odk_result_meta *meta,
                                  odk_error *err)
{
    return noul_call_impl(kernel, statement, context, image, caps, out,
                          meta, call, err);
}

odk_status odk_evaluate_rank_opts(odk_kernel *kernel,
                                 const char *instruction,
                                 const char *const *options,
                                 size_t option_count,
                                 const char *context, const odk_image *image,
                                 const odk_output_caps *caps,
                                 odk_result *out, odk_result_meta *meta,
                                 odk_error *err)
{
    return rank_call_impl(kernel, instruction, options, option_count,
                          context, image, caps, out, meta, NULL, err);
}

odk_status odk_evaluate_rank_call(odk_kernel *kernel,
                                  const char *instruction,
                                  const char *const *options,
                                  size_t option_count,
                                  const char *context, const odk_image *image,
                                  const odk_call_config *call,
                                  const odk_output_caps *caps,
                                  odk_result *out, odk_result_meta *meta,
                                  odk_error *err)
{
    return rank_call_impl(kernel, instruction, options, option_count,
                          context, image, caps, out, meta, call, err);
}

odk_status odk_evaluate_compare_opts(odk_kernel *kernel,
                                    const char *instruction,
                                    const char *const *options,
                                    size_t option_count,
                                    const char *context,
                                    const odk_image *image,
                                    const odk_output_caps *caps,
                                    odk_result *out, odk_result_meta *meta,
                                    odk_error *err)
{
    return compare_call_impl(kernel, instruction, options, option_count,
                             context, image, caps, out, meta, NULL, err);
}

odk_status odk_evaluate_compare_call(odk_kernel *kernel,
                                     const char *instruction,
                                     const char *left, const char *right,
                                     const char *context,
                                     const odk_image *image,
                                     const odk_call_config *call,
                                     const odk_output_caps *caps,
                                     odk_result *out, odk_result_meta *meta,
                                     odk_error *err)
{
    const char *pair[2];
    pair[0] = (left != NULL) ? left : "";
    pair[1] = (right != NULL) ? right : "";
    return compare_call_impl(kernel, instruction, pair, 2, context, image,
                             caps, out, meta, call, err);
}

odk_status odk_evaluate_extract_opts(odk_kernel *kernel,
                                    const char *instruction,
                                    const char *context,
                                    const odk_image *image, int top_k,
                                    const odk_output_caps *caps,
                                    odk_result *out, odk_result_meta *meta,
                                    odk_error *err)
{
    return extract_call_impl(kernel, instruction, NULL, context, image,
                             top_k, caps, out, meta, NULL, err);
}

odk_status odk_evaluate_extract_call(odk_kernel *kernel,
                                     const char *instruction,
                                     const char *statement,
                                     const char *context,
                                     const odk_image *image,
                                     int top_k, const odk_call_config *call,
                                     const odk_output_caps *caps,
                                     odk_result *out, odk_result_meta *meta,
                                     odk_error *err)
{
    return extract_call_impl(kernel, instruction, statement, context, image,
                             top_k, caps, out, meta, call, err);
}

odk_status odk_evaluate_query_opts(odk_kernel *kernel, const odk_query *query,
                                  const char *context, const odk_image *image,
                                  const odk_output_caps *caps,
                                  odk_result *out, odk_result_meta *meta,
                                  odk_error *err)
{
    return query_call_impl(kernel, query, context, image, caps, out, meta,
                           NULL, err);
}

odk_status odk_evaluate_query_call(odk_kernel *kernel,
                                   const odk_query *query,
                                   const char *context,
                                   const odk_image *image,
                                   const odk_call_config *call,
                                   const odk_output_caps *caps,
                                   odk_result *out, odk_result_meta *meta,
                                   odk_error *err)
{
    return query_call_impl(kernel, query, context, image, caps, out, meta,
                           call, err);
}

odk_language odk_kernel_language(const odk_kernel *kernel)
{
    if (kernel == NULL) {
        return ODK_LANG_UND;
    }
    return kernel_cfg(kernel)->language;
}

/* ------------------------------------------------------------------ */
/* model file loader (docs/MODEL.md)                             */
/* ------------------------------------------------------------------ */

odk_status odk_kernel_load_model(odk_kernel *kernel, const char *path,
                                 odk_error *err)
{
    odk_model loaded;

    if (kernel == NULL || path == NULL || path[0] == '\0') {
        kernel_set_error(err, "kernel and path are required");
        return ODK_ERR_ARGUMENT;
    }
    {
        odk_status st = odk_model_parse_file(path, &loaded, err);
        if (st != ODK_OK) {
            /* the kernel's model is left UNCHANGED on any failure */
            return st;
        }
    }
    kernel->model = loaded;
    return ODK_OK;
}

/* ------------------------------------------------------------------ */
/* Runtime loaders                                   */
/* ------------------------------------------------------------------ */

odk_status odk_kernel_load_stopwords(odk_kernel *kernel, const char *path,
                                     odk_error *err)
{
    odk_rt_text *rt;

    if (kernel == NULL || path == NULL || path[0] == '\0') {
        kernel_set_error(err, "kernel and path are required");
        return ODK_ERR_ARGUMENT;
    }
    rt = odk_lang_rt_load_stopwords(path, err);
    if (rt == NULL) {
        return ODK_ERR_ARGUMENT;
    }
    /* keep the zh words / concepts of a previously loaded lexicon
     * when the new file is stopwords-only: merge the old lexicon
     * sections into the fresh stopword list (load order is the
     * documented deterministic merge). */
    if (kernel->rt != NULL) {
        rt->zh_words = kernel->rt->zh_words;
        rt->zh_word_count = kernel->rt->zh_word_count;
        rt->zh_word_max_cps = kernel->rt->zh_word_max_cps;
        rt->concept_names = kernel->rt->concept_names;
        rt->concept_count = kernel->rt->concept_count;
        rt->concept_words = kernel->rt->concept_words;
        rt->concept_starts = kernel->rt->concept_starts;
        rt->concept_word_total = kernel->rt->concept_word_total;
        free(kernel->rt);
    }
    kernel->rt = rt;
    return ODK_OK;
}

odk_status odk_kernel_load_lexicon(odk_kernel *kernel, const char *path,
                                   odk_error *err)
{
    odk_rt_text *rt;

    if (kernel == NULL || path == NULL || path[0] == '\0') {
        kernel_set_error(err, "kernel and path are required");
        return ODK_ERR_ARGUMENT;
    }
    rt = odk_lang_rt_load_lexicon(path, err);
    if (rt == NULL) {
        return ODK_ERR_ARGUMENT;
    }
    odk_lang_rt_free(kernel->rt);
    kernel->rt = rt;
    return ODK_OK;
}

odk_status odk_kernel_lexicon_seed_links(const odk_kernel *kernel,
                                         odk_brain *brain, odk_error *err)
{
    size_t c;
    size_t pinned = 0;

    if (kernel == NULL || brain == NULL) {
        kernel_set_error(err, "kernel and brain are required");
        return ODK_ERR_ARGUMENT;
    }
    if (kernel->rt == NULL || kernel->rt->concept_count == 0) {
        kernel_set_error(err, "no concept lexicon loaded");
        return ODK_ERR_ARGUMENT;
    }
    for (c = 0; c < kernel->rt->concept_count; c++) {
        size_t start = kernel->rt->concept_starts[c];
        size_t end = (c + 1 < kernel->rt->concept_count)
                         ? kernel->rt->concept_starts[c + 1]
                         : kernel->rt->concept_word_total;
        size_t i;
        size_t j;
        for (i = start; i + 1 < end; i++) {
            for (j = i + 1; j < end; j++) {
                if (odk_brain_link_pin(brain, kernel->rt->concept_words[i],
                                       kernel->rt->concept_words[j],
                                       NULL) == ODK_OK) {
                    pinned++;
                }
            }
        }
    }
    if (pinned == 0) {
        kernel_set_error(err, "no concept pairs could be pinned");
        return ODK_ERR_ARGUMENT;
    }
    return ODK_OK;
}

size_t odk_kernel_lexicon_concept_count(const odk_kernel *kernel)
{
    return (kernel != NULL && kernel->rt != NULL)
               ? kernel->rt->concept_count
               : 0;
}

size_t odk_kernel_runtime_stopword_count(const odk_kernel *kernel)
{
    return (kernel != NULL && kernel->rt != NULL)
               ? kernel->rt->stopword_count
               : 0;
}
