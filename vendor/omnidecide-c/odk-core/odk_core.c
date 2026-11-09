/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_core.c - the OmniDecide evaluate() client pipeline for the
 * direct-string ingestion path:
 *
 *   direct string -> inline YAML/blockquote wrapping -> Markdown minifier
 *   -> kernel dispatch per query -> five-key contract assembly.
 *
 * Metadata keys, ordering, defaults and warning text are fixed by the
 * output contract; the hardware plan is always CPU (no CUDA backends
 * are shipped).
 *
 */

#include "odk_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define ODK_MAX_PATH_INPUT_CHARS 1024

struct odk_client {
    odk_kernel *kernel;   /* internal kernel (owned) */
    odk_kernel *attached; /* borrowed kernel, NULL when not attached */
    char *preference;     /* normalised device preference */
    char *descriptor;     /* "cpu" */
    char **notes;
    size_t note_count;
};

/* Effective kernel: the attached one when present, else the
 * internally created one. */
static odk_kernel *client_kernel(odk_client *client)
{
    return client != NULL && client->attached != NULL ? client->attached
                                                      : client->kernel;
}

void odk_client_attach_kernel(odk_client *client, odk_kernel *kernel)
{
    if (client != NULL) {
        client->attached = kernel;
    }
}

const char *odk_status_message(odk_status status); /* already in kernel.c */

/* ------------------------------------------------------------------ */
/* Client lifecycle                                                    */
/* ------------------------------------------------------------------ */

static char *client_normalise_preference(const char *raw)
{
    size_t len = raw != NULL ? strlen(raw) : 0;
    size_t lo;
    size_t hi;
    size_t i;
    char *out;

    odk_strip_bounds(raw != NULL ? raw : "", len, &lo, &hi);
    if (hi == lo) {
        out = strdup("auto");
        return out;
    }
    out = malloc(hi - lo + 1);
    if (out == NULL) {
        return NULL;
    }
    for (i = lo; i < hi; i++) {
        char c = raw[i];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        out[i - lo] = c;
    }
    out[hi - lo] = '\0';
    return out;
}

odk_client *odk_client_create(const char *device_preference)
{
    odk_client *client = calloc(1, sizeof(*client));
    char *pref = NULL;
    const char *raw = device_preference != NULL ? device_preference : "auto";

    if (client == NULL) {
        return NULL;
    }
    client->kernel = odk_kernel_create();
    if (client->kernel == NULL) {
        free(client);
        return NULL;
    }
    pref = client_normalise_preference(raw);
    if (pref == NULL) {
        odk_client_free(client);
        return NULL;
    }
    if (strcmp(pref, "gpu") == 0) {
        free(pref);
        pref = strdup("cuda");
        if (pref == NULL) {
            odk_client_free(client);
            return NULL;
        }
    }
    if (strcmp(pref, "auto") != 0 && strcmp(pref, "cpu") != 0 &&
        strcmp(pref, "cuda") != 0) {
        /* note: f"unknown device preference {raw!r}; falling back to 'auto'" */
        char **grown = realloc(client->notes,
                               (client->note_count + 1) * sizeof(char *));
        if (grown == NULL) {
            free(pref);
            odk_client_free(client);
            return NULL;
        }
        client->notes = grown;
        {
            char note[ODK_ERROR_MAX];
            snprintf(note, sizeof(note),
                     "unknown device preference '%s'; falling back to 'auto'",
                     raw);
            client->notes[client->note_count] = strdup(note);
            if (client->notes[client->note_count] == NULL) {
                free(pref);
                odk_client_free(client);
                return NULL;
            }
            client->note_count++;
        }
        free(pref);
        pref = strdup("auto");
        if (pref == NULL) {
            odk_client_free(client);
            return NULL;
        }
    }
    client->preference = pref;
    client->descriptor = strdup("cpu");
    if (client->descriptor == NULL) {
        odk_client_free(client);
        return NULL;
    }
    if (strcmp(pref, "cpu") != 0) {
        char note[ODK_ERROR_MAX];
        char **grown;
        snprintf(note, sizeof(note),
                 "device preference '%s': no usable CUDA device detected; "
                 "falling back to CPU",
                 pref);
        grown = realloc(client->notes,
                        (client->note_count + 1) * sizeof(char *));
        if (grown == NULL) {
            odk_client_free(client);
            return NULL;
        }
        client->notes = grown;
        client->notes[client->note_count] = strdup(note);
        if (client->notes[client->note_count] == NULL) {
            odk_client_free(client);
            return NULL;
        }
        client->note_count++;
    }
    return client;
}

void odk_client_free(odk_client *client)
{
    size_t i;
    if (client == NULL) {
        return;
    }
    odk_kernel_free(client->kernel); /* attached kernels are borrowed */
    free(client->preference);
    free(client->descriptor);
    for (i = 0; i < client->note_count; i++) {
        free(client->notes[i]);
    }
    free(client->notes);
    free(client);
}

/* ------------------------------------------------------------------ */
/* Mini YAML scalar dumper (PyYAML safe_dump approximation)            */
/* ------------------------------------------------------------------ */

static int yaml_scalar_needs_quotes(const char *s)
{
    size_t len = strlen(s);
    size_t i;
    static const char *const kBoolish[] = {
        "y", "Y", "yes", "Yes", "YES", "n", "N", "no", "No", "NO",
        "true", "True", "TRUE", "false", "False", "FALSE",
        "on", "On", "ON", "off", "Off", "OFF", "null", "Null", "NULL", "~"
    };

    if (len == 0) {
        return 1;
    }
    if (odk_ascii_isspace((unsigned char)s[0]) ||
        odk_ascii_isspace((unsigned char)s[len - 1])) {
        return 1;
    }
    if (strchr(s, ':') != NULL || strchr(s, '#') != NULL ||
        strchr(s, '\n') != NULL || strchr(s, '\t') != NULL) {
        return 1;
    }
    if (strpbrk(s, ",[]{}&*!|>'\"%@`") != NULL) {
        return 1;
    }
    if (s[0] == '-' || s[0] == '?' || s[0] == '~') {
        return 1;
    }
    for (i = 0; i < sizeof(kBoolish) / sizeof(kBoolish[0]); i++) {
        if (strcmp(s, kBoolish[i]) == 0) {
            return 1;
        }
    }
    {
        /* number-looking scalars must be quoted */
        int digits = 0;
        size_t k = (s[0] == '-') ? 1 : 0;
        while (k < len && s[k] >= '0' && s[k] <= '9') {
            k++;
            digits++;
        }
        if (digits > 0) {
            if (k == len) {
                return 1;
            }
            if (s[k] == '.') {
                k++;
                while (k < len && s[k] >= '0' && s[k] <= '9') {
                    k++;
                }
                if (k == len) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int yaml_dump_scalar(sb_t *out, const char *s)
{
    if (!yaml_scalar_needs_quotes(s)) {
        return sb_puts(out, s);
    }
    if (sb_putc(out, '\'') != 0) {
        return -1;
    }
    {
        size_t i;
        for (i = 0; s[i] != '\0'; i++) {
            if (s[i] == '\'') {
                if (sb_puts(out, "''") != 0) {
                    return -1;
                }
            } else if (sb_putc(out, s[i]) != 0) {
                return -1;
            }
        }
    }
    return sb_putc(out, '\'');
}

/* ------------------------------------------------------------------ */
/* Direct-string wrapping                                              */
/* ------------------------------------------------------------------ */

static char *wrap_direct_string(const char *text, const char *file_name)
{
    sb_t fm;
    sb_t quoted;
    sb_t result;
    const char *payload;
    char *payload_copy = NULL;
    size_t payload_len;
    size_t lo;
    size_t hi;
    size_t line_count = 1;
    size_t i;
    size_t line_start;
    const char *payload_text;

    payload_text = text != NULL ? text : "";
    odk_strip_bounds(payload_text, strlen(payload_text), &lo, &hi);
    if (hi == lo) {
        payload = "(empty input)";
        payload_len = strlen(payload);
    } else {
        payload_len = hi - lo;
        payload_copy = odk_strdup_range(payload_text + lo, payload_len);
        if (payload_copy == NULL) {
            return NULL;
        }
        payload = payload_copy;
    }
    for (i = 0; i < payload_len; i++) {
        if (payload[i] == '\n') {
            line_count++;
        }
    }

    if (sb_init(&fm) != 0) {
        free(payload_copy);
        return NULL;
    }
    if (sb_puts(&fm, "file_name: ") != 0 ||
        yaml_dump_scalar(&fm, file_name) != 0 ||
        sb_puts(&fm, "\nfile_type: ") != 0 ||
        yaml_dump_scalar(&fm, "text") != 0 ||
        sb_puts(&fm, "\nparser: ") != 0 ||
        yaml_dump_scalar(&fm, "omnidecide-inline-wrapper") != 0 ||
        sb_printf(&fm, "\nchar_count: %zu", odk_utf8_codepoints(payload, payload_len)) != 0 ||
        sb_printf(&fm, "\nline_count: %zu", line_count) != 0) {
        sb_free(&fm);
        free(payload_copy);
        return NULL;
    }

    if (sb_init(&quoted) != 0) {
        sb_free(&fm);
        free(payload_copy);
        return NULL;
    }
    line_start = 0;
    for (i = 0; i <= payload_len; i++) {
        if (i == payload_len || payload[i] == '\n') {
            const char *line = payload + line_start;
            size_t llen = i - line_start;
            size_t llo;
            size_t lhi;
            odk_strip_bounds(line, llen, &llo, &lhi);
            if (lhi > llo) {
                if (sb_puts(&quoted, "> ") != 0 ||
                    sb_append(&quoted, line, llen) != 0) {
                    goto quoted_oom;
                }
            } else {
                if (sb_putc(&quoted, '>') != 0) {
                    goto quoted_oom;
                }
            }
            if (i < payload_len && sb_putc(&quoted, '\n') != 0) {
                goto quoted_oom;
            }
            line_start = i + 1;
        }
    }
    free(payload_copy);
    payload_copy = NULL;
    {
        char *fm_body = NULL;
        char *quoted_body = sb_detach(&quoted);
        char *out;
        if (quoted_body == NULL) {
            sb_free(&fm);
            return NULL;
        }
        if (sb_init(&result) != 0) {
            sb_free(&fm);
            free(quoted_body);
            return NULL;
        }
        /* The frontmatter body is serialized with surrounding whitespace trimmed */
        {
            size_t flo;
            size_t fhi;
            odk_strip_bounds(fm.data, fm.len, &flo, &fhi);
            fm_body = odk_strdup_range(fm.data + flo, fhi - flo);
            sb_free(&fm);
            if (fm_body == NULL) {
                free(quoted_body);
                sb_free(&result);
                return NULL;
            }
        }
        if (sb_puts(&result, "---\n") != 0 ||
            sb_puts(&result, fm_body) != 0 ||
            sb_puts(&result, "\n---\n\n# Direct String Input\n\n"
                             "> **Telemetry**: inline text processed by "
                             "OmniDecide " ODK_STAMP_VERSION
                             " (fully local, air-gapped).\n\n") != 0 ||
            sb_puts(&result, quoted_body) != 0 ||
            sb_putc(&result, '\n') != 0) {
            free(fm_body);
            free(quoted_body);
            sb_free(&result);
            return NULL;
        }
        free(fm_body);
        free(quoted_body);
        out = sb_detach(&result);
        return out;
    }

quoted_oom:
    sb_free(&fm);
    sb_free(&quoted);
    free(payload_copy);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Output lifecycle and serialisation                                  */
/* ------------------------------------------------------------------ */

void odk_output_release(odk_output *output)
{
    size_t i;
    size_t k;
    if (output == NULL) {
        return;
    }
    free(output->file_name);
    free(output->file_type);
    free(output->extracted_markdown);
    for (i = 0; i < output->meta_count; i++) {
        free(output->meta_keys[i]);
        free(output->meta_values[i]);
    }
    free(output->meta_keys);
    free(output->meta_values);
    for (i = 0; i < output->decision_count; i++) {
        free(output->decisions[i].key);
        free(output->decisions[i].error);
        odk_result_release(&output->decisions[i].result);
    }
    free(output->decisions);
    memset(output, 0, sizeof(*output));
    (void)k;
}

const char *odk_output_meta_get(const odk_output *output, const char *key)
{
    size_t i;
    if (output == NULL || key == NULL) {
        return NULL;
    }
    for (i = 0; i < output->meta_count; i++) {
        if (strcmp(output->meta_keys[i], key) == 0) {
            return output->meta_values[i];
        }
    }
    return NULL;
}

static int meta_push(odk_output *out, const char *key,
                     const char *json_value)
{
    char **keys;
    char **values;
    char *key_copy;
    char *value_copy;

    if (out->meta_count == 0) {
        keys = malloc(24 * sizeof(char *));
        values = malloc(24 * sizeof(char *));
        if (keys == NULL || values == NULL) {
            free(keys);
            free(values);
            return -1;
        }
        out->meta_keys = keys;
        out->meta_values = values;
        out->meta_count = 0;
    } else if (out->meta_count % 24 == 0) {
        size_t new_cap = out->meta_count + 24;
        keys = realloc(out->meta_keys, new_cap * sizeof(char *));
        if (keys == NULL) {
            return -1;
        }
        out->meta_keys = keys;
        values = realloc(out->meta_values, new_cap * sizeof(char *));
        if (values == NULL) {
            return -1;
        }
        out->meta_values = values;
    }
    key_copy = strdup(key);
    value_copy = strdup(json_value);
    if (key_copy == NULL || value_copy == NULL) {
        free(key_copy);
        free(value_copy);
        return -1;
    }
    out->meta_keys[out->meta_count] = key_copy;
    out->meta_values[out->meta_count] = value_copy;
    out->meta_count++;
    return 0;
}

static int append_result_payload(sb_t *sb, const odk_result *result)
{
    char *literal;
    size_t i;

    if (result == NULL || result->primitive == NULL) {
        return -1;
    }
    if (sb_puts(sb, "{\"primitive\": ") != 0) {
        return -1;
    }
    literal = odk_json_string_literal(result->primitive,
                                      strlen(result->primitive));
    if (literal == NULL) {
        return -1;
    }
    if (sb_puts(sb, literal) != 0) {
        free(literal);
        return -1;
    }
    free(literal);
    if (sb_puts(sb, ", \"result\": ") != 0) {
        return -1;
    }
    switch (result->kind) {
    case ODK_VALUE_NONE:
        if (sb_puts(sb, "null") != 0) {
            return -1;
        }
        break;
    case ODK_VALUE_BOOL:
        if (sb_puts(sb, result->boolean ? "true" : "false") != 0) {
            return -1;
        }
        break;
    case ODK_VALUE_NUMBER:
        literal = odk_json_number_literal(result->number);
        if (literal == NULL) {
            return -1;
        }
        if (sb_puts(sb, literal) != 0) {
            free(literal);
            return -1;
        }
        free(literal);
        break;
    case ODK_VALUE_STRING:
        literal = odk_json_string_literal(result->string,
                                          strlen(result->string));
        if (literal == NULL) {
            return -1;
        }
        if (sb_puts(sb, literal) != 0) {
            free(literal);
            return -1;
        }
        free(literal);
        break;
    case ODK_VALUE_LIST:
        if (sb_putc(sb, '[') != 0) {
            return -1;
        }
        for (i = 0; i < result->item_count; i++) {
            if (i > 0 && sb_putc(sb, ',') != 0) {
                return -1;
            }
            literal = odk_json_string_literal(result->items[i],
                                              strlen(result->items[i]));
            if (literal == NULL) {
                return -1;
            }
            if (sb_puts(sb, literal) != 0) {
                free(literal);
                return -1;
            }
            free(literal);
        }
        if (sb_putc(sb, ']') != 0) {
            return -1;
        }
        break;
    default:
        return -1;
    }
    if (sb_puts(sb, ", \"confidence\": ") != 0 ||
        (literal = odk_json_number_literal(result->confidence)) == NULL) {
        return -1;
    }
    if (sb_puts(sb, literal) != 0) {
        free(literal);
        return -1;
    }
    free(literal);
    if (sb_puts(sb, ", \"probability\": ") != 0 ||
        (literal = odk_json_number_literal(result->probability)) == NULL) {
        return -1;
    }
    if (sb_puts(sb, literal) != 0) {
        free(literal);
        return -1;
    }
    free(literal);
    if (sb_puts(sb, ", \"distribution\": {") != 0) {
        return -1;
    }
    for (i = 0; i < result->distribution.count; i++) {
        if (i > 0 && sb_putc(sb, ',') != 0) {
            return -1;
        }
        literal = odk_json_string_literal(result->distribution.entries[i].key,
                                          strlen(result->distribution.entries[i].key));
        if (literal == NULL) {
            return -1;
        }
        if (sb_puts(sb, literal) != 0) {
            free(literal);
            return -1;
        }
        free(literal);
        if (sb_putc(sb, ':') != 0) {
            return -1;
        }
        literal = odk_json_number_literal(result->distribution.entries[i].value);
        if (literal == NULL) {
            return -1;
        }
        if (sb_puts(sb, literal) != 0) {
            free(literal);
            return -1;
        }
        free(literal);
    }
    return sb_puts(sb, "}}") == 0 ? 0 : -1;
}

char *odk_output_to_json(const odk_output *output)
{
    sb_t sb;
    char *literal;
    char *json;
    size_t i;

    if (output == NULL || sb_init(&sb) != 0) {
        return NULL;
    }
    if (sb_puts(&sb, "{\"file_name\": ") != 0 ||
        (literal = odk_json_string_literal(
             output->file_name != NULL ? output->file_name : "",
             output->file_name != NULL ? strlen(output->file_name) : 0)) ==
            NULL) {
        goto oom;
    }
    if (sb_puts(&sb, literal) != 0) {
        free(literal);
        goto oom;
    }
    free(literal);
    if (sb_puts(&sb, ", \"file_type\": ") != 0 ||
        (literal = odk_json_string_literal(
             output->file_type != NULL ? output->file_type : "",
             output->file_type != NULL ? strlen(output->file_type) : 0)) ==
            NULL) {
        goto oom;
    }
    if (sb_puts(&sb, literal) != 0) {
        free(literal);
        goto oom;
    }
    free(literal);
    if (sb_puts(&sb, ", \"metadata\": {") != 0) {
        goto oom;
    }
    for (i = 0; i < output->meta_count; i++) {
        if (i > 0 && sb_putc(&sb, ',') != 0) {
            goto oom;
        }
        literal = odk_json_string_literal(output->meta_keys[i],
                                          strlen(output->meta_keys[i]));
        if (literal == NULL) {
            goto oom;
        }
        if (sb_puts(&sb, literal) != 0) {
            free(literal);
            goto oom;
        }
        free(literal);
        if (sb_putc(&sb, ':') != 0 || sb_puts(&sb, output->meta_values[i]) != 0) {
            goto oom;
        }
    }
    if (sb_puts(&sb, "}, \"extracted_markdown\": ") != 0 ||
        (literal = odk_json_string_literal(
             output->extracted_markdown != NULL ? output->extracted_markdown
                                                : "",
             output->extracted_markdown != NULL
                 ? strlen(output->extracted_markdown)
                 : 0)) == NULL) {
        goto oom;
    }
    if (sb_puts(&sb, literal) != 0) {
        free(literal);
        goto oom;
    }
    free(literal);
    if (sb_puts(&sb, ", \"decisions\": {") != 0) {
        goto oom;
    }
    for (i = 0; i < output->decision_count; i++) {
        if (i > 0 && sb_putc(&sb, ',') != 0) {
            goto oom;
        }
        literal = odk_json_string_literal(output->decisions[i].key,
                                          strlen(output->decisions[i].key));
        if (literal == NULL) {
            goto oom;
        }
        if (sb_puts(&sb, literal) != 0) {
            free(literal);
            goto oom;
        }
        free(literal);
        if (sb_putc(&sb, ':') != 0) {
            goto oom;
        }
        if (append_result_payload(&sb, &output->decisions[i].result) != 0) {
            goto oom;
        }
    }
    if (sb_puts(&sb, "}}") != 0) {
        goto oom;
    }
    json = sb_detach(&sb);
    return json;

oom:
    sb_free(&sb);
    return NULL;
}

char *odk_result_to_json(const odk_result *result)
{
    sb_t sb;
    if (sb_init(&sb) != 0) {
        return NULL;
    }
    if (append_result_payload(&sb, result) != 0) {
        sb_free(&sb);
        return NULL;
    }
    return sb_detach(&sb);
}

/* ------------------------------------------------------------------ */
/* The evaluate pipeline                                               */
/* ------------------------------------------------------------------ */

static double mono_ms_since(const struct timespec *start,
                            const struct timespec *end)
{
    double seconds = (double)(end->tv_sec - start->tv_sec);
    double nanos = (double)(end->tv_nsec - start->tv_nsec);
    return seconds * 1000.0 + nanos / 1000000.0;
}

static int append_json_string_array(sb_t *sb, const char *const *items,
                                    size_t count)
{
    size_t i;
    if (sb_putc(sb, '[') != 0) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        char *literal;
        if (i > 0 && sb_putc(sb, ',') != 0) {
            return -1;
        }
        literal = odk_json_string_literal(items[i], strlen(items[i]));
        if (literal == NULL) {
            return -1;
        }
        if (sb_puts(sb, literal) != 0) {
            free(literal);
            return -1;
        }
        free(literal);
    }
    return sb_putc(sb, ']');
}

odk_status odk_client_evaluate(odk_client *client,
                               const char *input_text,
                               const odk_query *const *queries,
                               size_t query_count,
                               const char *analysis_mode,
                               odk_output *out, odk_error *err)
{
    return odk_client_evaluate_ex_lang(client, input_text, queries,
                                       query_count, analysis_mode, NULL,
                                       NULL, out, err);
}

/* Append one warning line to the client warning list (0/-1). */
static int client_push_warning(str_list_t *warnings, const char *text)
{
    char **grown;

    grown = realloc(warnings->items,
                    (warnings->count + 1) * sizeof(char *));
    if (grown == NULL) {
        return -1;
    }
    warnings->items = grown;
    warnings->items[warnings->count] = strdup(text);
    if (warnings->items[warnings->count] == NULL) {
        return -1;
    }
    warnings->count++;
    return 0;
}

/* The phases of odk_client_evaluate_ex_lang as static helpers;
 * each returns 0 on success and -1 on allocation failure. */

/* Phase 0: mode validation + the unknown-mode warning. */
static int client_validate_mode(const char *analysis_mode,
                                char mode[64],
                                str_list_t *warnings)
{
    char cleaned[64];
    size_t lo;
    size_t hi;
    size_t k;
    const char *raw_mode = analysis_mode != NULL ? analysis_mode : "hybrid";
    size_t raw_len = strlen(raw_mode);
    odk_strip_bounds(raw_mode, raw_len, &lo, &hi);
    if (hi - lo >= sizeof(cleaned)) {
        hi = lo + sizeof(cleaned) - 1;
    }
    for (k = lo; k < hi; k++) {
        char c = raw_mode[k];
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        cleaned[k - lo] = c;
    }
    cleaned[hi - lo] = '\0';
    if (strcmp(cleaned, "hybrid") == 0 || strcmp(cleaned, "parser_only") == 0 ||
        strcmp(cleaned, "kernel_only") == 0) {
        snprintf(mode, 64, "%s", cleaned);
    } else {
        char warning[ODK_ERROR_MAX];
        snprintf(mode, 64, "hybrid");
        snprintf(warning, sizeof(warning),
                 "unknown analysis_mode '%s'; falling back to 'hybrid'",
                 raw_mode);
        if (client_push_warning(warnings, warning) != 0) {
            return -1;
        }
    }
    return 0;
}

/* Phase 3: run the query loop, filling out->decisions and the
 * meta-driven warnings.  Returns 0 on success, -1 on allocation
 * failure (the caller goes to its oom path). */
static int client_run_queries(odk_client *client,
                              const odk_query *const *queries,
                              size_t query_count,
                              const char *final_markdown,
                              const odk_call_config *call,
                              odk_output *out,
                              str_list_t *warnings,
                              size_t *error_count)
{
    size_t i;
    odk_kernel *kern = client_kernel(client);

    out->decisions = calloc(query_count > 0 ? query_count : 1,
                            sizeof(odk_decision));
    if (out->decisions == NULL) {
        return -1;
    }
    out->decision_count = query_count;
    for (i = 0; i < query_count; i++) {
        odk_error query_err;
        odk_result result;
        odk_result_meta meta;
        memset(&result, 0, sizeof(result));
        memset(&meta, 0, sizeof(meta));
        out->decisions[i].key = strdup(queries[i]->query_key != NULL
                                           ? queries[i]->query_key
                                           : "");
        if (out->decisions[i].key == NULL) {
            return -1;
        }
        /* The caps come from the kernel config
         * (the client API has no per-call caps); the metadata is
         * surfaced as warnings ONLY when something was limited, so
         * default outputs stay byte-identical to the legacy output. */
        if (odk_evaluate_query_call(kern, queries[i],
                                    final_markdown, NULL, call, NULL,
                                    &result, &meta,
                                    &query_err) == ODK_OK) {
            out->decisions[i].result = result;
            if (meta.work_exceeded) {
                char warning[ODK_ERROR_MAX];
                snprintf(warning, sizeof(warning),
                         "query '%s': work budget exceeded "
                         "(work_used=%llu, budget=%llu); the context "
                         "scan stopped at a code-point boundary",
                         queries[i]->query_key != NULL
                             ? queries[i]->query_key
                             : "",
                         (unsigned long long)meta.work_used,
                         (unsigned long long)meta.work_budget);
                if (client_push_warning(warnings, warning) != 0) {
                    return -1;
                }
            }
            if (meta.chunks_dropped != 0) {
                char warning[ODK_ERROR_MAX];
                snprintf(warning, sizeof(warning),
                         "query '%s': select strategy kept %llu chunks, "
                         "dropped %llu",
                         queries[i]->query_key != NULL
                             ? queries[i]->query_key
                             : "",
                         (unsigned long long)meta.chunks_kept,
                         (unsigned long long)meta.chunks_dropped);
                if (client_push_warning(warnings, warning) != 0) {
                    return -1;
                }
            }
            if (meta.truncated) {
                char warning[ODK_ERROR_MAX];
                snprintf(warning, sizeof(warning),
                         "query '%s': output cap truncated the rendered "
                         "output (total_options=%llu, emitted=%llu, "
                         "dropped=%llu)",
                         queries[i]->query_key != NULL
                             ? queries[i]->query_key
                             : "",
                         (unsigned long long)meta.total_options,
                         (unsigned long long)meta.emitted,
                         (unsigned long long)meta.dropped);
                if (client_push_warning(warnings, warning) != 0) {
                    return -1;
                }
            }
        } else {
            (*error_count)++;
            out->decisions[i].result.primitive =
                strdup(queries[i]->primitive != NULL
                           ? queries[i]->primitive
                           : "");
            if (out->decisions[i].result.primitive == NULL) {
                return -1;
            }
            out->decisions[i].error = strdup(query_err.text);
            if (out->decisions[i].error == NULL) {
                return -1;
            }
        }
    }
    return 0;
}

/* Phase 4a: apply the output policy to the minified markdown.
 * Takes ownership of final_markdown on every path; stores the echo
 * in *out_echo (NULL only on allocation failure, which the caller
 * treats as oom) and reports the kept code-point count. */
static void client_apply_markdown_policy(odk_client *client,
                                         const odk_output_policy *policy,
                                         char *final_markdown,
                                         char **out_echo,
                                         size_t *kept_cps_out,
                                         int *mode_default_out)
{
    odk_markdown_mode md_mode = ODK_MARKDOWN_FULL;
    size_t truncate_chars = 2000;
    int mode_default = 1;
    size_t kept_cps;
    char *echo = NULL;

    *out_echo = NULL;
    if (policy != NULL) {
        md_mode = policy->markdown_mode;
        truncate_chars = policy->markdown_truncate_chars != 0
                             ? policy->markdown_truncate_chars
                             : 2000;
        if (truncate_chars > 100000) {
            truncate_chars = 100000;
        }
        mode_default = 0;
    } else if (odk_kernel_text_mode(client_kernel(client)) ==
               ODK_TEXT_UNICODE) {
        md_mode = ODK_MARKDOWN_TRUNCATE;
    }
    kept_cps = odk_utf8_codepoints(
        final_markdown != NULL ? final_markdown : "",
        final_markdown != NULL ? strlen(final_markdown) : 0);
    switch (md_mode) {
    case ODK_MARKDOWN_FULL:
        echo = final_markdown;
        final_markdown = NULL;
        break;
    case ODK_MARKDOWN_TRUNCATE: {
        size_t full_cps = odk_utf8_codepoints(final_markdown,
                                              strlen(final_markdown));
        if (full_cps > truncate_chars) {
            size_t kept = odk_u_truncate_cp(final_markdown,
                                            strlen(final_markdown),
                                            truncate_chars);
            echo = malloc(kept + 4);
            if (echo != NULL) {
                memcpy(echo, final_markdown, kept);
                memcpy(echo + kept, "...", 3);
                echo[kept + 3] = '\0';
            }
        } else {
            echo = final_markdown;
            final_markdown = NULL;
        }
        break;
    }
    case ODK_MARKDOWN_OMIT:
        echo = strdup("");
        break;
    case ODK_MARKDOWN_HASH: {
        /* stable 128-bit digest: two FNV-1a 64 runs with different
         * offset bases over the minified bytes */
        uint64_t h1 = 1469598103934665603ULL;
        uint64_t h2 = 1099511628211ULL;
        const char *p = final_markdown;
        char buf[96];
        for (; p != NULL && *p != '\0'; p++) {
            h1 ^= (uint64_t)(unsigned char)*p;
            h1 *= 1099511628211ULL;
            h2 ^= (uint64_t)(unsigned char)*p;
            h2 *= 14695981039346656037ULL;
        }
        snprintf(buf, sizeof(buf),
                 "len=%zu:fnv1a64a=%016llx:fnv1a64b=%016llx",
                 final_markdown != NULL ? strlen(final_markdown) : 0,
                 (unsigned long long)h1, (unsigned long long)h2);
        echo = strdup(buf);
        break;
    }
    default:
        echo = final_markdown;
        final_markdown = NULL;
        break;
    }
    /* ownership moved above or the echo was rebuilt: release the
     * full markdown when the policy did not take it */
    free(final_markdown);
    *out_echo = echo;
    *kept_cps_out = kept_cps;
    *mode_default_out = mode_default;
}

/* Phase 4b: the input/kept/output character metadata + truncation
 * warning for the new modes only (ASCII_COMPAT with the default
 * policy keeps the byte-exact legacy contract). */
static int client_push_echo_metadata(odk_client *client,
                                     odk_output *out,
                                     const char *input_text,
                                     const char *echo,
                                     size_t kept_cps,
                                     str_list_t *warnings)
{
    char num[32];
    size_t input_cps = odk_utf8_codepoints(
        input_text != NULL ? input_text : "",
        input_text != NULL ? strlen(input_text) : 0);
    size_t out_cps = odk_utf8_codepoints(
        echo != NULL ? echo : "",
        echo != NULL ? strlen(echo) : 0);
    /* input-side truncation: compare against the kernel limits
     * (ASCII: bytes vs the legacy limits; UNICODE: cps vs defaults) */
    int truncated = out_cps < kept_cps;
    if (input_text != NULL) {
        if (odk_kernel_text_mode(client_kernel(client)) ==
            ODK_TEXT_ASCII_COMPAT) {
            if (strlen(input_text) > (size_t)40000) {
                truncated = 1; /* chunk-source cap exceeded */
            }
        } else if (input_cps > 200000) {
            truncated = 1;
        }
    }
    snprintf(num, sizeof(num), "%zu", input_cps);
    if (meta_push(out, "input_chars", num) != 0) {
        return -1;
    }
    snprintf(num, sizeof(num), "%zu", kept_cps);
    if (meta_push(out, "kept_chars", num) != 0) {
        return -1;
    }
    snprintf(num, sizeof(num), "%zu", out_cps);
    if (meta_push(out, "output_chars", num) != 0) {
        return -1;
    }
    if (meta_push(out, "truncated", truncated ? "true" : "false") != 0) {
        return -1;
    }
    if (truncated) {
        char *warning =
            strdup("input or output truncated; see input_chars/"
                   "kept_chars/output_chars");
        if (warning == NULL ||
            str_list_push(warnings, warning, strlen(warning)) != 0) {
            free(warning);
            return -1;
        }
        free(warning); /* str_list_push copies */
    }
    return 0;
}

/* Phase 4c: the decision_errors JSON object over the failed
 * decisions.  Returns a malloc'd string or NULL on allocation
 * failure. */
static char *client_build_decision_errors(const odk_output *out)
{
    sb_t errors_sb;
    size_t i;
    size_t written = 0;
    char *json;

    if (sb_init(&errors_sb) != 0) {
        return NULL;
    }
    if (sb_putc(&errors_sb, '{') != 0) {
        sb_free(&errors_sb);
        return NULL;
    }
    for (i = 0; i < out->decision_count; i++) {
        if (out->decisions[i].error == NULL) {
            continue;
        }
        if (written > 0 && sb_putc(&errors_sb, ',') != 0) {
            sb_free(&errors_sb);
            return NULL;
        }
        {
            char *key_literal = odk_json_string_literal(
                out->decisions[i].key, strlen(out->decisions[i].key));
            char *msg_literal = odk_json_string_literal(
                out->decisions[i].error, strlen(out->decisions[i].error));
            int ok = key_literal != NULL && msg_literal != NULL;
            if (ok) {
                ok = sb_puts(&errors_sb, key_literal) == 0 &&
                     sb_putc(&errors_sb, ':') == 0 &&
                     sb_puts(&errors_sb, msg_literal) == 0;
            }
            free(key_literal);
            free(msg_literal);
            if (!ok) {
                sb_free(&errors_sb);
                return NULL;
            }
        }
        written++;
    }
    if (sb_putc(&errors_sb, '}') != 0) {
        sb_free(&errors_sb);
        return NULL;
    }
    json = sb_detach(&errors_sb);
    return json;
}


odk_status odk_client_evaluate_ex_lang(odk_client *client,
                                       const char *input_text,
                                       const odk_query *const *queries,
                                       size_t query_count,
                                       const char *analysis_mode,
                                       const odk_output_policy *policy,
                                       const odk_call_config *call,
                                       odk_output *out, odk_error *err)
{
    struct timespec ts_start;
    struct timespec ts_end;
    str_list_t warnings;
    char mode[64];
    char *extracted = NULL;
    char *final_markdown = NULL;
    int raw_tokens = 0;
    int compressed_tokens = 0;
    double elapsed_ms;
    double reduction_pct = 0.0;
    char *notes_json = NULL;
    sb_t notes_sb;
    int notes_sb_ready = 0;
    char *warnings_json = NULL;
    sb_t warnings_sb;
    int warnings_sb_ready = 0;
    char *decision_errors_json = NULL;
    size_t error_count = 0;

    memset(out, 0, sizeof(*out));
    str_list_init(&warnings);
    if (client == NULL) {
        str_list_free(&warnings);
        if (err != NULL) {
            snprintf(err->text, sizeof(err->text), "client handle is NULL");
        }
        return ODK_ERR_ARGUMENT;
    }
    if (clock_gettime(CLOCK_MONOTONIC, &ts_start) != 0) {
        ts_start.tv_sec = 0;
        ts_start.tv_nsec = 0;
    }

    /* Phase 0: mode validation (client_validate_mode; on failure the pre-alignment oom path
     * applies - nothing is owned yet beyond the warnings list). */
    if (client_validate_mode(analysis_mode, mode, &warnings) != 0) {
        str_list_free(&warnings);
        if (err != NULL) {
            snprintf(err->text, sizeof(err->text), "out of memory");
        }
        return ODK_ERR_MEMORY;
    }

    /* Phase 1: direct-string ingestion */
    extracted = wrap_direct_string(input_text, "direct_string_input");
    if (extracted == NULL) {
        str_list_free(&warnings);
        if (err != NULL) {
            snprintf(err->text, sizeof(err->text), "out of memory");
        }
        return ODK_ERR_MEMORY;
    }

    /* Phase 2: minification + token accounting (mode follows the
     * effective kernel; structural profiles skip minification) */
    {
        odk_text_mode text_mode = odk_kernel_text_mode(client_kernel(client));
        odk_input_profile profile = odk_kernel_profile(client_kernel(client));
        int structural = (profile == ODK_PROFILE_CODE || profile == ODK_PROFILE_LOG ||
                          profile == ODK_PROFILE_CSV || profile == ODK_PROFILE_TSV ||
                          profile == ODK_PROFILE_JSON || profile == ODK_PROFILE_JSONL ||
                          profile == ODK_PROFILE_AUTO);
        raw_tokens = odk_estimate_tokens_ex(extracted, text_mode);
        if (strcmp(mode, "kernel_only") == 0 || structural) {
            final_markdown = strdup(extracted);
        } else {
            final_markdown = odk_minify_ex(extracted, text_mode);
        }
    }
    if (final_markdown == NULL) {
        free(extracted);
        str_list_free(&warnings);
        if (err != NULL) {
            snprintf(err->text, sizeof(err->text), "out of memory");
        }
        return ODK_ERR_MEMORY;
    }
    compressed_tokens = odk_estimate_tokens_ex(
        final_markdown, odk_kernel_text_mode(client_kernel(client)));
    reduction_pct = raw_tokens != 0
                        ? odk_round2(100.0 *
                                     (1.0 - (double)compressed_tokens /
                                            (double)raw_tokens))
                        : 0.0;

    /* Phase 3: kernel dispatch (the query loop
     * lives in client_run_queries; the parser_only warning stays
     * inline). */
    if (strcmp(mode, "parser_only") == 0) {
        if (query_count > 0) {
            if (client_push_warning(
                    &warnings,
                    "analysis_mode='parser_only': decision kernel skipped") !=
                0) {
                goto oom;
            }
        }
    } else {
        if (client_run_queries(client, queries, query_count,
                               final_markdown, call, out, &warnings,
                               &error_count) != 0) {
            goto oom;
        }
    }

    if (clock_gettime(CLOCK_MONOTONIC, &ts_end) != 0) {
        ts_end = ts_start;
    }
    elapsed_ms = odk_round3(mono_ms_since(&ts_start, &ts_end));

    /* Phase 4: contract assembly */
    out->file_name = strdup("direct_string_input");
    out->file_type = strdup("text");
    {
        /* Output policy: the echo is bounded; scoring has
         * already happened on the full final_markdown, so decisions are
         * identical under every policy.  The
         * policy switch lives in client_apply_markdown_policy and the
         * echo metadata in client_push_echo_metadata (the helper takes
         * ownership of final_markdown). */
        size_t kept_cps;
        int mode_default;
        client_apply_markdown_policy(client, policy, final_markdown,
                                     &out->extracted_markdown, &kept_cps,
                                     &mode_default);
        final_markdown = NULL; /* ownership passed to the helper */
        /* metadata + warnings for the new modes only (ASCII_COMPAT with
         * the default policy keeps the byte-exact legacy contract) */
        if (!mode_default ||
            odk_kernel_text_mode(client_kernel(client)) == ODK_TEXT_UNICODE) {
            if (client_push_echo_metadata(client, out, input_text,
                                          out->extracted_markdown,
                                          kept_cps, &warnings) != 0) {
                goto oom;
            }
        }
    }
    if (out->file_name == NULL || out->file_type == NULL ||
        out->extracted_markdown == NULL) {
        goto oom;
    }
    {
        char byte_buf[32];
        snprintf(byte_buf, sizeof(byte_buf), "%zu",
                 input_text != NULL ? strlen(input_text) : 0);
        if (meta_push(out, "byte_size", byte_buf) != 0 ||
            meta_push(out, "mime_type", "\"text/plain\"") != 0 ||
            meta_push(out, "parser_backend", "\"inline-yaml-wrapper\"") != 0 ||
            meta_push(out, "error", "null") != 0 ||
            meta_push(out, "file_type", "\"text\"") != 0) {
            goto oom;
        }
    }
    {
        char *mode_literal = odk_json_string_literal(mode, strlen(mode));
        int push_ok;
        if (mode_literal == NULL) {
            goto oom;
        }
        push_ok = meta_push(out, "analysis_mode", mode_literal);
        free(mode_literal);
        if (push_ok != 0) {
            goto oom;
        }
    }
    {
        char *ms_literal = odk_json_number_literal(elapsed_ms);
        int push_ok;
        if (ms_literal == NULL) {
            goto oom;
        }
        push_ok = meta_push(out, "execution_time_ms", ms_literal);
        free(ms_literal);
        if (push_ok != 0) {
            goto oom;
        }
    }
    if (meta_push(out, "hardware_device", "\"cpu\"") != 0) {
        goto oom;
    }
    {
        char *pref_literal = odk_json_string_literal(
            client->preference != NULL ? client->preference : "auto",
            client->preference != NULL ? strlen(client->preference) : 4);
        int push_ok;
        if (pref_literal == NULL) {
            goto oom;
        }
        push_ok = meta_push(out, "device_preference", pref_literal);
        free(pref_literal);
        if (push_ok != 0) {
            goto oom;
        }
    }
    if (meta_push(out, "acceleration", "\"cpu\"") != 0) {
        goto oom;
    }
    if (sb_init(&notes_sb) != 0) {
        goto oom;
    }
    notes_sb_ready = 1;
    if (append_json_string_array(&notes_sb,
                                 (const char *const *)client->notes,
                                 client->note_count) != 0) {
        goto oom;
    }
    notes_json = sb_detach(&notes_sb);
    notes_sb_ready = 0;
    if (notes_json == NULL || meta_push(out, "device_notes", notes_json) != 0) {
        goto oom;
    }
    if (meta_push(out, "airgap_enforced", "true") != 0) {
        goto oom;
    }
    {
        char tokens_buf[32];
        snprintf(tokens_buf, sizeof(tokens_buf), "%d", raw_tokens);
        if (meta_push(out, "raw_tokens", tokens_buf) != 0) {
            goto oom;
        }
        snprintf(tokens_buf, sizeof(tokens_buf), "%d", compressed_tokens);
        if (meta_push(out, "compressed_tokens", tokens_buf) != 0) {
            goto oom;
        }
    }
    {
        char *pct_literal = odk_json_number_literal(reduction_pct);
        int push_ok;
        if (pct_literal == NULL) {
            goto oom;
        }
        push_ok = meta_push(out, "token_reduction_pct", pct_literal);
        free(pct_literal);
        if (push_ok != 0) {
            goto oom;
        }
    }
    if (meta_push(out, "kernel_backend", "\"embedded-fallback\"") != 0 ||
        meta_push(out, "library_version", "\"" ODK_LIBRARY_VERSION "\"") != 0) {
        goto oom;
    }
    if (warnings.count > 0) {
        if (sb_init(&warnings_sb) != 0) {
            goto oom;
        }
        warnings_sb_ready = 1;
        if (append_json_string_array(&warnings_sb,
                                     (const char *const *)warnings.items,
                                     warnings.count) != 0) {
            goto oom;
        }
        warnings_json = sb_detach(&warnings_sb);
        warnings_sb_ready = 0;
        if (warnings_json == NULL ||
            meta_push(out, "warnings", warnings_json) != 0) {
            goto oom;
        }
    }
    if (error_count > 0) {
        /* The errors JSON assembly lives in
         * client_build_decision_errors. */
        decision_errors_json = client_build_decision_errors(out);
        if (decision_errors_json == NULL ||
            meta_push(out, "decision_errors", decision_errors_json) != 0) {
            goto oom;
        }
    }

    free(notes_json);
    free(warnings_json);
    free(decision_errors_json);
    free(extracted);
    str_list_free(&warnings);
    return ODK_OK;

oom:
    free(notes_json);
    free(warnings_json);
    free(decision_errors_json);
    if (notes_sb_ready) {
        sb_free(&notes_sb);
    }
    if (warnings_sb_ready) {
        sb_free(&warnings_sb);
    }
    free(extracted);
    free(final_markdown);
    str_list_free(&warnings);
    odk_output_release(out);
    if (err != NULL) {
        snprintf(err->text, sizeof(err->text), "out of memory");
    }
    return ODK_ERR_MEMORY;
}

odk_status odk_client_evaluate_json(odk_client *client,
                                    const char *input_text,
                                    const char *queries_json,
                                    const char *analysis_mode,
                                    char **out_json, odk_error *err)
{
    return odk_client_evaluate_json_lang_ex(client, input_text,
                                            queries_json, analysis_mode,
                                            NULL, NULL, out_json, err);
}

/* Per-call language variant of the JSON batch API
 * (language applies to every query in the batch; ODK_LANG_DEFAULT =
 * kernel setting = the baseline behavior). */
odk_status odk_client_evaluate_json_lang(odk_client *client,
                                         const char *input_text,
                                         const char *queries_json,
                                         const char *analysis_mode,
                                         odk_language language,
                                         char **out_json, odk_error *err)
{
    odk_call_config call;

    memset(&call, 0, sizeof(call));
    call.language = language;
    return odk_client_evaluate_json_lang_ex(client, input_text,
                                            queries_json, analysis_mode,
                                            NULL, &call, out_json, err);
}

/* Public entry points without a call config: the kernel's configured
 * language applies, which is UND unless a language was set. */
odk_status odk_client_evaluate_json_ex(odk_client *client,
                                       const char *input_text,
                                       const char *queries_json,
                                       const char *analysis_mode,
                                       const odk_output_policy *policy,
                                       char **out_json, odk_error *err)
{
    return odk_client_evaluate_json_lang_ex(client, input_text,
                                            queries_json, analysis_mode,
                                            policy, NULL, out_json, err);
}

odk_status odk_client_evaluate_ex(odk_client *client,
                                   const char *input_text,
                                   const odk_query *const *queries,
                                   size_t query_count,
                                   const char *analysis_mode,
                                   const odk_output_policy *policy,
                                   odk_output *out, odk_error *err)
{
    return odk_client_evaluate_ex_lang(client, input_text, queries,
                                       query_count, analysis_mode, policy,
                                       NULL, out, err);
}

odk_status odk_client_evaluate_json_lang_ex(odk_client *client,
                                            const char *input_text,
                                            const char *queries_json,
                                            const char *analysis_mode,
                                            const odk_output_policy *policy,
                                            const odk_call_config *call,
                                            char **out_json,
                                            odk_error *err)
{
    odk_json_value *root = NULL;
    odk_query *queries = NULL;
    const odk_query **query_ptrs = NULL;
    odk_output output;
    size_t query_count = 0;
    size_t i;
    odk_status status;
    char *json = NULL;

    memset(&output, 0, sizeof(output));
    if (out_json == NULL) {
        if (err != NULL) {
            snprintf(err->text, sizeof(err->text), "out_json is NULL");
        }
        return ODK_ERR_ARGUMENT;
    }
    *out_json = NULL;
    if (queries_json != NULL && queries_json[0] != '\0') {
        root = odk_json_parse(queries_json, strlen(queries_json), err);
        if (root == NULL) {
            return ODK_ERR_ARGUMENT;
        }
        if (root->type != ODK_JSON_OBJECT) {
            if (err != NULL) {
                snprintf(err->text, sizeof(err->text),
                         "queries must be a JSON object of query specs");
            }
            odk_json_free(root);
            return ODK_ERR_ARGUMENT;
        }
        if (root->member_count > 0) {
            queries = calloc(root->member_count, sizeof(odk_query));
            query_ptrs = calloc(root->member_count, sizeof(odk_query *));
            if (queries == NULL || query_ptrs == NULL) {
                free(queries);
                free(query_ptrs);
                odk_json_free(root);
                if (err != NULL) {
                    snprintf(err->text, sizeof(err->text), "out of memory");
                }
                return ODK_ERR_MEMORY;
            }
            for (i = 0; i < root->member_count; i++) {
                char *spec = odk_json_dump(root->members[i]);
                odk_status build;
                if (spec == NULL) {
                    size_t k;
                    for (k = 0; k < query_count; k++) {
                        odk_query_release(&queries[k]);
                    }
                    free(queries);
                    free(query_ptrs);
                    odk_json_free(root);
                    if (err != NULL) {
                        snprintf(err->text, sizeof(err->text), "out of memory");
                    }
                    return ODK_ERR_MEMORY;
                }
                build = odk_query_from_json(spec, &queries[query_count], err);
                free(spec);
                if (build == ODK_OK) {
                    /* The object key is the query_key (it is passed into
                     * the query spec). */
                    char *key_copy = odk_strdup(root->keys[i]);
                    if (key_copy == NULL) {
                        odk_query_release(&queries[query_count]);
                        build = ODK_ERR_MEMORY;
                    } else {
                        free(queries[query_count].query_key);
                        queries[query_count].query_key = key_copy;
                    }
                }
                if (build != ODK_OK) {
                    size_t k;
                    for (k = 0; k < query_count; k++) {
                        odk_query_release(&queries[k]);
                    }
                    free(queries);
                    free(query_ptrs);
                    odk_json_free(root);
                    return ODK_ERR_ARGUMENT;
                }
                query_ptrs[query_count] = &queries[query_count];
                query_count++;
            }
        }
    }
    status = odk_client_evaluate_ex_lang(client, input_text,
                                    query_count > 0 ? query_ptrs : NULL,
                                    query_count, analysis_mode, policy,
                                    call, &output, err);
    if (status == ODK_OK) {
        json = odk_output_to_json(&output);
        if (json == NULL) {
            status = ODK_ERR_MEMORY;
            if (err != NULL) {
                snprintf(err->text, sizeof(err->text), "out of memory");
            }
        }
    }
    odk_output_release(&output);
    for (i = 0; i < query_count; i++) {
        odk_query_release(&queries[i]);
    }
    free(queries);
    free(query_ptrs);
    odk_json_free(root);
    if (status == ODK_OK) {
        *out_json = json;
    } else {
        free(json);
    }
    return status;
}

