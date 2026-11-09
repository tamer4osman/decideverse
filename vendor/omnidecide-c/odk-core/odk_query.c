/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_query.c - validated query construction from a raw spec and
 * from a JSON object.
 *
 */

#include "odk_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

static int query_valid_primitive(const char *primitive)
{
    static const char *const kValid[] = {"choice", "score", "noul",
                                         "rank", "compare", "extract"};
    size_t i;
    for (i = 0; i < 6; i++) {
        if (strcmp(primitive, kValid[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static void query_set_error(odk_error *err, const char *fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 2, 3)))
#endif
    ;

static void query_set_error(odk_error *err, const char *fmt, ...)
{
    va_list args;
    if (err == NULL) {
        return;
    }
    va_start(args, fmt);
    vsnprintf(err->text, sizeof(err->text), fmt, args);
    va_end(args);
}

void odk_query_release(odk_query *query)
{
    size_t i;
    if (query == NULL) {
        return;
    }
    free(query->query_key);
    free(query->primitive);
    free(query->instruction);
    free(query->statement);
    for (i = 0; i < query->option_count; i++) {
        free(query->options[i]);
    }
    free(query->options);
    for (i = 0; i < query->rubric_count; i++) {
        free(query->rubric[i]);
    }
    free(query->rubric);
    memset(query, 0, sizeof(*query));
}

/* Normalise a primitive name: strip ASCII whitespace + lowercase. */
static char *query_normalise_primitive(const char *raw)
{
    size_t len = raw != NULL ? strlen(raw) : 0;
    size_t lo;
    size_t hi;
    size_t i;
    char *out;

    odk_strip_bounds(raw != NULL ? raw : "", len, &lo, &hi);
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

static char **query_copy_list(const char *const *items, size_t count,
                              size_t *out_count)
{
    char **copies = NULL;
    size_t i;
    *out_count = 0;
    if (count == 0) {
        return NULL;
    }
    copies = calloc(count, sizeof(char *));
    if (copies == NULL) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        copies[i] = odk_strdup(items[i] != NULL ? items[i] : "");
        if (copies[i] == NULL) {
            size_t k;
            for (k = 0; k < i; k++) {
                free(copies[k]);
            }
            free(copies);
            return NULL;
        }
        *out_count = i + 1;
    }
    return copies;
}

odk_status odk_query_from_spec(const char *query_key,
                               const char *primitive,
                               const char *instruction_or_question,
                               const char *statement,
                               const char *const *options, size_t option_count,
                               const char *const *rubric, size_t rubric_count,
                               int top_k,
                               odk_query *out, odk_error *err)
{
    char *normalised = NULL;
    const char *instruction;
    const char *statement_value;
    int clamped_top_k;
    size_t copied = 0;

    memset(out, 0, sizeof(*out));
    normalised = query_normalise_primitive(primitive);
    if (normalised == NULL) {
        goto oom;
    }
    if (!query_valid_primitive(normalised)) {
        query_set_error(err,
                        "query '%s' has invalid primitive '%s'; expected one "
                        "of ['choice', 'compare', 'extract', 'noul', 'rank', "
                        "'score']",
                        query_key != NULL ? query_key : "",
                        normalised);
        free(normalised);
        return ODK_ERR_ARGUMENT;
    }
    /* From here on every allocation is owned by *out; the single oom
     * path releases it exactly once. */
    out->primitive = normalised;
    out->options = query_copy_list(options, option_count, &copied);
    if (out->options == NULL && option_count > 0) {
        goto oom;
    }
    out->rubric = query_copy_list(rubric, rubric_count, &copied);
    if (out->rubric == NULL && rubric_count > 0) {
        goto oom;
    }
    clamped_top_k = top_k < 1 ? 1 : (top_k > 10 ? 10 : top_k);
    instruction = instruction_or_question != NULL ? instruction_or_question : "";
    statement_value =
        (statement != NULL && statement[0] != '\0') ? statement : instruction;

    out->query_key = odk_strdup(query_key != NULL ? query_key : "");
    out->instruction = odk_strdup(instruction);
    out->statement = odk_strdup(statement_value);
    out->option_count = option_count;
    out->rubric_count = rubric_count;
    out->top_k = clamped_top_k;
    if (out->query_key == NULL || out->instruction == NULL ||
        out->statement == NULL) {
        goto oom;
    }
    if (strcmp(normalised, "rank") == 0 && option_count < 2) {
        query_set_error(
            err, "query '%s' uses primitive 'rank' and requires at least 2 "
                 "options",
            query_key != NULL ? query_key : "");
        odk_query_release(out);
        return ODK_ERR_ARGUMENT;
    }
    if (strcmp(normalised, "compare") == 0 && option_count != 2) {
        query_set_error(err,
                        "query '%s' uses primitive 'compare' and requires "
                        "exactly 2 options",
                        query_key != NULL ? query_key : "");
        odk_query_release(out);
        return ODK_ERR_ARGUMENT;
    }
    return ODK_OK;

oom:
    odk_query_release(out);
    query_set_error(err, "out of memory");
    return ODK_ERR_MEMORY;
}

/* JSON string list -> char** copy list. */
static int query_json_list(const odk_json_value *value, char ***out_items,
                           size_t *out_count)
{
    char **items = NULL;
    size_t count = 0;
    size_t i;

    *out_items = NULL;
    *out_count = 0;
    if (value == NULL || value->type == ODK_JSON_NULL) {
        return 0;
    }
    if (value->type != ODK_JSON_ARRAY) {
        return -1;
    }
    if (value->item_count == 0) {
        return 0;
    }
    items = calloc(value->item_count, sizeof(char *));
    if (items == NULL) {
        return -1;
    }
    for (i = 0; i < value->item_count; i++) {
        const odk_json_value *item = value->items[i];
        if (item->type != ODK_JSON_STRING) {
            size_t k;
            for (k = 0; k < count; k++) {
                free(items[k]);
            }
            free(items);
            return -1;
        }
        items[count] = odk_strdup(item->string);
        if (items[count] == NULL) {
            size_t k;
            for (k = 0; k < count; k++) {
                free(items[k]);
            }
            free(items);
            return -1;
        }
        count++;
    }
    *out_items = items;
    *out_count = count;
    return 0;
}

static const char *query_json_string(const odk_json_value *value)
{
    if (value == NULL || value->type != ODK_JSON_STRING) {
        return NULL;
    }
    return value->string;
}

odk_status odk_query_from_json(const char *json_spec, odk_query *out,
                               odk_error *err)
{
    odk_json_value *root = NULL;
    const odk_json_value *primitive_value;
    const odk_json_value *instruction_value;
    const odk_json_value *statement_value;
    const odk_json_value *options_value;
    const odk_json_value *rubric_value;
    const odk_json_value *top_k_value;
    const char *primitive_raw;
    const char *instruction_raw;
    const char *statement_raw;
    char **options = NULL;
    char **rubric = NULL;
    size_t option_count = 0;
    size_t rubric_count = 0;
    long long top_k = 3;
    odk_status status;

    memset(out, 0, sizeof(*out));
    root = odk_json_parse(json_spec, strlen(json_spec), err);
    if (root == NULL) {
        return ODK_ERR_ARGUMENT;
    }
    if (root->type != ODK_JSON_OBJECT) {
        if (err != NULL) {
            snprintf(err->text, sizeof(err->text),
                     "query spec must be a JSON object");
        }
        odk_json_free(root);
        return ODK_ERR_ARGUMENT;
    }
    primitive_value = odk_json_object_get(root, "primitive");
    primitive_raw = query_json_string(primitive_value);
    if (primitive_raw == NULL) {
        if (err != NULL) {
            snprintf(err->text, sizeof(err->text),
                     "query spec requires a string 'primitive'");
        }
        odk_json_free(root);
        return ODK_ERR_ARGUMENT;
    }
    /* "instruction" falls back to "question" when absent. */
    instruction_value = odk_json_object_get(root, "instruction");
    instruction_raw = query_json_string(instruction_value);
    if (instruction_value == NULL) {
        instruction_value = odk_json_object_get(root, "question");
        instruction_raw = query_json_string(instruction_value);
    }
    if (instruction_raw == NULL) {
        instruction_raw = "";
    }
    statement_value = odk_json_object_get(root, "statement");
    statement_raw = query_json_string(statement_value);
    if (statement_raw == NULL) {
        statement_raw = "";
    }
    options_value = odk_json_object_get(root, "options");
    if (options_value != NULL && options_value->type != ODK_JSON_NULL) {
        if (query_json_list(options_value, &options, &option_count) != 0) {
            if (err != NULL) {
                snprintf(err->text, sizeof(err->text),
                         "query 'options' must be an array of strings");
            }
            odk_json_free(root);
            return ODK_ERR_ARGUMENT;
        }
    }
    rubric_value = odk_json_object_get(root, "rubric");
    if (rubric_value != NULL && rubric_value->type != ODK_JSON_NULL) {
        if (query_json_list(rubric_value, &rubric, &rubric_count) != 0) {
            size_t i;
            for (i = 0; i < option_count; i++) {
                free(options[i]);
            }
            free(options);
            if (err != NULL) {
                snprintf(err->text, sizeof(err->text),
                         "query 'rubric' must be an array of strings");
            }
            odk_json_free(root);
            return ODK_ERR_ARGUMENT;
        }
    }
    top_k_value = odk_json_object_get(root, "top_k");
    if (top_k_value != NULL && top_k_value->type != ODK_JSON_NULL) {
        if (top_k_value->type == ODK_JSON_NUMBER) {
            top_k = top_k_value->is_integer ? top_k_value->integer
                                            : (long long)top_k_value->number;
        } else {
            size_t i;
            for (i = 0; i < option_count; i++) {
                free(options[i]);
            }
            free(options);
            for (i = 0; i < rubric_count; i++) {
                free(rubric[i]);
            }
            free(rubric);
            if (err != NULL) {
                snprintf(err->text, sizeof(err->text),
                         "query 'top_k' must be a number");
            }
            odk_json_free(root);
            return ODK_ERR_ARGUMENT;
        }
    }

    status = odk_query_from_spec("", primitive_raw, instruction_raw,
                                 statement_raw,
                                 (const char *const *)options, option_count,
                                 (const char *const *)rubric, rubric_count,
                                 (int)top_k, out, err);
    {
        size_t i;
        for (i = 0; i < option_count; i++) {
            free(options[i]);
        }
        free(options);
        for (i = 0; i < rubric_count; i++) {
            free(rubric[i]);
        }
        free(rubric);
    }
    odk_json_free(root);
    return status;
}
