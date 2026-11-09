/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * odk_wasm.c - implementation of the OmniDecide C interop wrapper.
 *
 * Compiled unchanged for native hosts and for Emscripten/WASM.  See
 * odk_wasm.h for the envelope contract.  Every allocation in this file
 * has a single owner and a release path on every exit, including the
 * partially-successful paths in odkw_kernel_create() (each string is
 * duplicated for normalization/validation; a failure of any allocation
 * releases everything already allocated before returning).
 */

#include "odk_wasm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Handle registry                                                     */
/* ------------------------------------------------------------------ */

/* Handles are registry slots, not raw pointers, so the same integer
 * protocol works on 32-bit WASM and 64-bit native hosts.  The module is
 * single-threaded (upstream WASM profile), so no locking is required. */
#define ODWK_MAX_KERNELS 64

static odk_kernel *g_kernels[ODWK_MAX_KERNELS];
static uint32_t g_kernel_live;

/* Diagnostics (single-threaded module). */
static int32_t g_last_status;
static char g_last_error[ODK_ERROR_MAX];

static void set_error(int32_t status, const char *msg)
{
    g_last_status = status;
    if (msg == NULL) {
        msg = "";
    }
    snprintf(g_last_error, sizeof(g_last_error), "%s", msg);
}

/* Append a bounded, JSON-escaped excerpt of user data to a message. */
static void append_excerpt(char *dst, size_t cap, const char *src,
                           size_t max_chars)
{
    size_t used = strlen(dst);
    size_t i, taken = 0;

    for (i = 0; src != NULL && src[i] != '\0' && taken < max_chars &&
                used + 7 < cap;
         i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            used += (size_t)snprintf(dst + used, cap - used, "\\%c", c);
            taken++;
        } else if (c < 0x20) {
            used += (size_t)snprintf(dst + used, cap - used, "\\u%04x", c);
            taken++;
        } else {
            dst[used++] = src[i];
            if ((c & 0xC0) != 0x80) { /* count code points, not bytes */
                taken++;
            }
        }
    }
    if (src != NULL && src[taken] != '\0' && used + 8 < cap) {
        used += (size_t)snprintf(dst + used, cap - used, "... [truncated]");
    }
    if (used < cap) {
        dst[used] = '\0'; /* terminate even when nothing was appended */
    } else if (cap > 0) {
        dst[cap - 1] = '\0';
    }
}

static void set_error_ex(int32_t status, const char *what, const char *excerpt)
{
    char msg[ODK_ERROR_MAX];

    snprintf(msg, sizeof(msg), "%s", what);
    if (excerpt != NULL) {
        size_t used = strlen(msg);
        snprintf(msg + used, sizeof(msg) - used, ": ");
        append_excerpt(msg, sizeof(msg), excerpt, 48);
    }
    set_error(status, msg);
}

/* ------------------------------------------------------------------ */
/* Kernel lifecycle                                                    */
/* ------------------------------------------------------------------ */

static char *dup_normalized(const char *src)
{
    /* Lowercase ASCII letters and trim surrounding whitespace; other
     * bytes are copied unchanged (language codes are ASCII).  The copy
     * is the ONLY allocation here; callers release it on every path. */
    size_t len, start, end, i;
    char *out;

    if (src == NULL) {
        src = "";
    }
    len = strlen(src);
    start = 0;
    while (start < len && (src[start] == ' ' || src[start] == '\t')) {
        start++;
    }
    end = len;
    while (end > start && (src[end - 1] == ' ' || src[end - 1] == '\t')) {
        end--;
    }
    out = (char *)malloc(end - start + 1);
    if (out == NULL) {
        return NULL;
    }
    for (i = start; i < end; i++) {
        char c = src[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        out[i - start] = c;
    }
    out[end - start] = '\0';
    return out;
}

static uint32_t slot_alloc(odk_kernel *kernel)
{
    uint32_t i;

    for (i = 0; i < ODWK_MAX_KERNELS; i++) {
        if (g_kernels[i] == NULL) {
            g_kernels[i] = kernel;
            g_kernel_live++;
            return i + 1u; /* 1-based; 0 stays invalid */
        }
    }
    return 0;
}

static odk_kernel *slot_resolve(uint32_t handle)
{
    if (handle == 0u || handle > ODWK_MAX_KERNELS) {
        return NULL;
    }
    return g_kernels[handle - 1u];
}

uint32_t odkw_kernel_create(const char *mode, const char *language)
{
    char *mode_copy = NULL;
    char *lang_copy = NULL;
    odk_kernel_config cfg;
    odk_kernel *kernel = NULL;
    uint32_t handle;
    int text_mode = ODK_TEXT_ASCII_COMPAT;
    odk_language lang = ODK_LANG_UND;
    int lang_ok = 0;

    odkw_clear_error();

    mode_copy = dup_normalized(mode != NULL ? mode : ODWK_MODE_ASCII);
    if (mode_copy == NULL) {
        set_error(ODK_ERR_MEMORY,
                  "odkw_kernel_create: out of memory copying mode");
        return 0u;
    }
    lang_copy = dup_normalized(language != NULL ? language : "und");
    if (lang_copy == NULL) {
        /* First allocation succeeded; release it before failing. */
        free(mode_copy);
        set_error(ODK_ERR_MEMORY,
                  "odkw_kernel_create: out of memory copying language");
        return 0u;
    }

    if (strcmp(mode_copy, ODWK_MODE_ASCII) == 0) {
        text_mode = ODK_TEXT_ASCII_COMPAT;
    } else if (strcmp(mode_copy, ODWK_MODE_UNICODE) == 0) {
        text_mode = ODK_TEXT_UNICODE;
    } else {
        set_error_ex(
            ODK_ERR_ARGUMENT,
            "odkw_kernel_create: unknown mode (expected \"ascii\" or \"unicode\")",
            mode_copy);
        free(mode_copy);
        free(lang_copy);
        return 0u;
    }

    if (strcmp(lang_copy, "default") == 0) {
        set_error(ODK_ERR_ARGUMENT,
                  "odkw_kernel_create: language \"default\" is per-call only; "
                  "use a concrete code or \"auto\"");
        free(mode_copy);
        free(lang_copy);
        return 0u;
    }
    lang = odk_lang_from_code(lang_copy, &lang_ok);
    if (lang_ok == 0) {
        set_error_ex(ODK_ERR_ARGUMENT,
                     "odkw_kernel_create: unknown language code", lang_copy);
        free(mode_copy);
        free(lang_copy);
        return 0u;
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.text_mode = text_mode;
    cfg.language = lang;
    kernel = odk_kernel_create_ex(&cfg);
    if (kernel == NULL) {
        set_error(ODK_ERR_MEMORY, "odkw_kernel_create: kernel allocation failed");
        free(mode_copy);
        free(lang_copy);
        return 0u;
    }

    free(mode_copy);
    free(lang_copy);

    handle = slot_alloc(kernel);
    if (handle == 0u) {
        odk_kernel_free(kernel);
        set_error(ODK_ERR_INTERNAL, "odkw_kernel_create: handle registry exhausted");
        return 0u;
    }
    return handle;
}

void odkw_kernel_free(uint32_t handle)
{
    odk_kernel *kernel = slot_resolve(handle);

    if (kernel != NULL) {
        g_kernels[handle - 1u] = NULL;
        g_kernel_live--;
        odk_kernel_free(kernel);
    }
}

uint32_t odkw_kernel_count(void)
{
    return g_kernel_live;
}

/* ------------------------------------------------------------------ */
/* Envelope composition                                                */
/* ------------------------------------------------------------------ */

#define META_OBJECT_MAX 512

/* {"totalOptions":n,...,"contextUnitsDropped":n} - integers only except
 * detectedConfidence, formatted %.4f (the kernel rounds it to 4
 * decimals; identical bit patterns serialize identically on both
 * hosts). */
static int append_meta_object(char *dst, size_t cap, const odk_result_meta *m)
{
    return snprintf(dst, cap,
                    "\"meta\":{\"totalOptions\":%zu,\"truncated\":%d,"
                    "\"emitted\":%zu,\"dropped\":%zu,\"chunksKept\":%zu,"
                    "\"chunksDropped\":%zu,\"workUsed\":%zu,\"workBudget\":%zu,"
                    "\"workExceeded\":%d,\"language\":\"%s\",\"detected\":\"%s\","
                    "\"detectedConfidence\":%.4f,\"contextTruncated\":%d,"
                    "\"contextUnitsDropped\":%zu}",
                    m->total_options, m->truncated, m->emitted, m->dropped,
                    m->chunks_kept, m->chunks_dropped, m->work_used,
                    m->work_budget, m->work_exceeded,
                    odk_lang_to_code(m->language), odk_lang_to_code(m->detected),
                    m->detected_confidence, m->context_truncated,
                    m->context_units_dropped) < 0
               ? -1
               : 0;
}

static char *compose_success_envelope(const odk_result *result,
                                      const odk_result_meta *meta)
{
    char *five_key = odk_result_to_json(result);
    char meta_buf[META_OBJECT_MAX];
    const char *meta_json = "";
    size_t five_len, total;
    char *envelope;

    if (five_key == NULL) {
        set_error(ODK_ERR_MEMORY, "odkw_evaluate: envelope allocation failed");
        return NULL;
    }
    if (meta != NULL) {
        if (append_meta_object(meta_buf, sizeof(meta_buf), meta) != 0) {
            free(five_key);
            set_error(ODK_ERR_INTERNAL, "odkw_evaluate: meta formatting failed");
            return NULL;
        }
        meta_json = meta_buf;
    }

    five_len = strlen(five_key);
    total = five_len + strlen(meta_json) + 16u; /* ",<meta>}" + slack */
    envelope = (char *)malloc(total);
    if (envelope == NULL) {
        free(five_key);
        set_error(ODK_ERR_MEMORY, "odkw_evaluate: envelope allocation failed");
        return NULL;
    }

    /* five_key is a complete object "...}"; splice the meta object in
     * before its closing brace so the decision payload stays byte-for-
     * byte the upstream five-key serialization. */
    memcpy(envelope, five_key, five_len - 1u);
    envelope[five_len - 1u] = '\0';
    if (meta != NULL) {
        snprintf(envelope + five_len - 1u, total - (five_len - 1u), ",%s}",
                 meta_json);
    } else {
        snprintf(envelope + five_len - 1u, total - (five_len - 1u), "}");
    }
    free(five_key);
    return envelope;
}

static char *compose_error_envelope(int32_t status, const char *message)
{
    char escaped[ODK_ERROR_MAX * 6u + 16u];
    size_t i, used = 0;
    char *envelope;

    for (i = 0; message != NULL && message[i] != '\0' && used + 7 < sizeof(escaped);
         i++) {
        unsigned char c = (unsigned char)message[i];
        if (c == '"' || c == '\\') {
            used += (size_t)snprintf(escaped + used, sizeof(escaped) - used,
                                     "\\%c", c);
        } else if (c < 0x20) {
            used += (size_t)snprintf(escaped + used, sizeof(escaped) - used,
                                     "\\u%04x", c);
        } else {
            escaped[used++] = (char)c;
        }
    }
    escaped[used] = '\0';

    envelope = (char *)malloc(used + 48u);
    if (envelope == NULL) {
        set_error(ODK_ERR_MEMORY, "odkw_evaluate: envelope allocation failed");
        return NULL;
    }
    snprintf(envelope, used + 48u, "{\"error\":{\"status\":%d,\"message\":\"%s\"}}",
             (int)status, escaped);
    return envelope;
}

/* ------------------------------------------------------------------ */
/* Evaluation                                                          */
/* ------------------------------------------------------------------ */

char *odkw_evaluate(uint32_t handle, const char *primitive,
                    const char *instruction, const char *const *options,
                    size_t option_count, const char *context, int32_t top_k,
                    int32_t max_ranked, int32_t max_snippet_chars,
                    int32_t max_total_chars, uint32_t flags)
{
    odk_kernel *kernel = slot_resolve(handle);
    odk_result result;
    odk_result_meta meta;
    odk_result_meta *meta_ptr = NULL;
    odk_output_caps caps;
    odk_error err;
    int status;
    char *envelope;
    const char *prim;

    odkw_clear_error();
    memset(&result, 0, sizeof(result));
    memset(&err, 0, sizeof(err));

    if (kernel == NULL) {
        set_error(ODK_ERR_ARGUMENT, "odkw_evaluate: invalid kernel handle");
        return compose_error_envelope(ODK_ERR_ARGUMENT, g_last_error);
    }
    prim = primitive != NULL ? primitive : "";
    if (strcmp(prim, "choice") != 0 && strcmp(prim, "score") != 0 &&
        strcmp(prim, "noul") != 0 && strcmp(prim, "rank") != 0 &&
        strcmp(prim, "compare") != 0 && strcmp(prim, "extract") != 0) {
        set_error_ex(ODK_ERR_ARGUMENT,
                     "odkw_evaluate: unknown primitive (expected choice, score, "
                     "noul, rank, compare or extract)",
                     prim);
        return compose_error_envelope(ODK_ERR_ARGUMENT, g_last_error);
    }

    if ((flags & ODWK_FLAG_META) != 0u) {
        memset(&meta, 0, sizeof(meta));
        meta_ptr = &meta;
    }
    memset(&caps, 0, sizeof(caps));
    caps.max_ranked = (size_t)(max_ranked > 0 ? max_ranked : 0);
    caps.max_snippet_chars =
        (size_t)(max_snippet_chars > 0 ? max_snippet_chars : 0);
    caps.max_total_chars = (size_t)(max_total_chars > 0 ? max_total_chars : 0);

    if (strcmp(prim, "choice") == 0) {
        if (options == NULL || option_count == 0u) {
            set_error(ODK_ERR_ARGUMENT,
                      "odkw_evaluate: choice requires at least one option");
            return compose_error_envelope(ODK_ERR_ARGUMENT, g_last_error);
        }
        status = (int)odk_evaluate_choice_opts(kernel, instruction, options,
                                               option_count, context, NULL,
                                               &caps, &result, meta_ptr, &err);
    } else if (strcmp(prim, "score") == 0) {
        if (options == NULL || option_count == 0u) {
            set_error(ODK_ERR_ARGUMENT,
                      "odkw_evaluate: score requires a non-empty rubric");
            return compose_error_envelope(ODK_ERR_ARGUMENT, g_last_error);
        }
        status = (int)odk_evaluate_score_opts(kernel, instruction, options,
                                              option_count, context, NULL,
                                              &caps, &result, meta_ptr, &err);
    } else if (strcmp(prim, "noul") == 0) {
        status = (int)odk_evaluate_noul_opts(kernel, instruction, context, NULL,
                                             &caps, &result, meta_ptr, &err);
    } else if (strcmp(prim, "rank") == 0) {
        if (options == NULL || option_count == 0u) {
            set_error(ODK_ERR_ARGUMENT,
                      "odkw_evaluate: rank requires at least one option");
            return compose_error_envelope(ODK_ERR_ARGUMENT, g_last_error);
        }
        status = (int)odk_evaluate_rank_opts(kernel, instruction, options,
                                             option_count, context, NULL,
                                             &caps, &result, meta_ptr, &err);
    } else if (strcmp(prim, "compare") == 0) {
        if (options == NULL || option_count != 2u) {
            set_error(ODK_ERR_ARGUMENT,
                      "odkw_evaluate: compare requires exactly 2 options");
            return compose_error_envelope(ODK_ERR_ARGUMENT, g_last_error);
        }
        status = (int)odk_evaluate_compare_opts(kernel, instruction, options,
                                                option_count, context, NULL,
                                                &caps, &result, meta_ptr, &err);
    } else { /* extract (validated above) */
        if (top_k < 1) {
            set_error(ODK_ERR_ARGUMENT,
                      "odkw_evaluate: extract requires top_k >= 1");
            return compose_error_envelope(ODK_ERR_ARGUMENT, g_last_error);
        }
        status = (int)odk_evaluate_extract_opts(kernel, instruction, context,
                                                NULL, top_k, &caps, &result,
                                                meta_ptr, &err);
    }

    if (status != (int)ODK_OK) {
        char msg[ODK_ERROR_MAX];
        snprintf(msg, sizeof(msg), "%s",
                 err.text[0] != '\0' ? err.text
                                     : odk_status_message((odk_status)status));
        set_error((int32_t)status, msg);
        odk_result_release(&result);
        return compose_error_envelope((int32_t)status, msg);
    }

    envelope = compose_success_envelope(&result, meta_ptr);
    odk_result_release(&result);
    return envelope;
}

/* ------------------------------------------------------------------ */
/* Utilities                                                           */
/* ------------------------------------------------------------------ */

const char *odkw_version(void)
{
    return ODK_LIBRARY_VERSION;
}

const char *odkw_backend(void)
{
    /* Backend identity is fixed upstream (omnidecide.h: always
     * "embedded-fallback"); report it without needing a kernel. */
    return "embedded-fallback";
}

int32_t odkw_last_status(void)
{
    return g_last_status;
}

const char *odkw_last_error(void)
{
    return g_last_error;
}

void odkw_clear_error(void)
{
    g_last_status = (int32_t)ODK_OK;
    g_last_error[0] = '\0';
}

void odkw_free(void *ptr)
{
    free(ptr);
}

char *odkw_last_error_envelope(void)
{
    return compose_error_envelope(g_last_status, g_last_error);
}
