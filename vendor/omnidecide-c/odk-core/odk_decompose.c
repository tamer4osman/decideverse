/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_decompose.c - Tier A parameterized decomposition engine (F3).
 *
 * Extraction is rule-driven and deterministic:
 *   - mode "sentence": every sentence containing any rule term (text
 *     order); an empty term list takes every sentence.
 *   - mode "before"/"after"/"around": a window of `window` characters
 *     around the first occurrence of any rule term (trimmed).
 *   - mode "keyvalue": lines of the form "key: value" whose key matches
 *     any rule term.
 *   - a component spec without any extraction rule: kind "whole" takes
 *     the entire item text; other kinds stay empty.
 *
 * This file contains no application-domain vocabulary.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "odk_decompose.h"

#include "odk_classify.h"
#include "odk_spine_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Parameter resolution                                                */
/* ------------------------------------------------------------------ */

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *out = malloc(n);

    if (out != NULL) {
        memcpy(out, s, n);
    }
    return out;
}

static char *fmt_int(long long v)
{
    char buf[32];

    snprintf(buf, sizeof(buf), "%lld", v);
    return dup_str(buf);
}

static char *fmt_double(double v)
{
    char *lit = odk_json_number_literal(v);

    return lit; /* already malloc'd */
}

odk_spine_status odk_decompose_resolve_params(
    const odk_spine_decomposition_template *tmpl, const char *params_json,
    odk_spine_map *out, odk_spine_error *err)
{
    odk_json_value *root = NULL;
    odk_error jerr;
    size_t i, k;

    if (tmpl == NULL || out == NULL) {
        set_err(err, "null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    if (params_json != NULL && params_json[0] != '\0') {
        root = odk_json_parse(params_json, strlen(params_json), &jerr);
        if (root == NULL) {
            set_errf(err, "params: invalid JSON: %s", jerr.text);
            return ODK_SPINE_ERR_SCHEMA;
        }
        if (root->type != ODK_JSON_OBJECT) {
            odk_json_free(root);
            set_err(err, "params: expected an object");
            return ODK_SPINE_ERR_SCHEMA;
        }
        /* every caller-supplied key must be declared by the template */
        for (k = 0; k < root->member_count; k++) {
            int declared = 0;

            for (i = 0; i < tmpl->parameters.count; i++) {
                if (strcmp(root->keys[k], tmpl->parameters.items[i].name) ==
                    0) {
                    declared = 1;
                    break;
                }
            }
            if (!declared) {
                char key_copy[128];

                snprintf(key_copy, sizeof(key_copy), "%s", root->keys[k]);
                odk_json_free(root);
                set_errf(err, "params: unknown parameter '%s'", key_copy);
                return ODK_SPINE_ERR_SCHEMA;
            }
        }
    }

    /* resolve declared parameters in declaration order (deterministic) */
    for (i = 0; i < tmpl->parameters.count; i++) {
        const odk_spine_template_parameter *p = &tmpl->parameters.items[i];
        const odk_json_value *v = NULL;
        char *value = NULL;
        odk_spine_kv *ni;
        char *kd;

        if (root != NULL) {
            v = odk_json_object_get(root, p->name);
        }
        if (v != NULL) {
            if (strcmp(p->type, "string") == 0) {
                if (v->type != ODK_JSON_STRING) {
                    odk_json_free(root);
                    set_errf(err, "params.%s: expected a string", p->name);
                    return ODK_SPINE_ERR_SCHEMA;
                }
                value = dup_str(v->string);
            } else if (strcmp(p->type, "int") == 0) {
                if (v->type != ODK_JSON_NUMBER || !v->is_integer) {
                    odk_json_free(root);
                    set_errf(err, "params.%s: expected an integer", p->name);
                    return ODK_SPINE_ERR_SCHEMA;
                }
                value = fmt_int(v->integer);
            } else { /* double */
                if (v->type != ODK_JSON_NUMBER) {
                    odk_json_free(root);
                    set_errf(err, "params.%s: expected a number", p->name);
                    return ODK_SPINE_ERR_SCHEMA;
                }
                value = fmt_double(v->number);
            }
        } else {
            /* not supplied: default or required error */
            if (p->required != 0 && p->default_value[0] == '\0') {
                odk_json_free(root);
                odk_spine_map_release_records(out);
                set_errf(err, "params: missing required parameter '%s'",
                         p->name);
                return ODK_SPINE_ERR_SCHEMA;
            }
            value = dup_str(p->default_value);
        }
        if (value == NULL) {
            odk_json_free(root);
            odk_spine_map_release_records(out);
            set_err(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
        ni = realloc(out->items, (out->count + 1) * sizeof(odk_spine_kv));
        if (ni == NULL) {
            free(value);
            odk_json_free(root);
            odk_spine_map_release_records(out);
            set_err(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
        out->items = ni;
        kd = dup_str(p->name);
        if (kd == NULL) {
            free(value);
            odk_json_free(root);
            odk_spine_map_release_records(out);
            set_err(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
        out->items[out->count].key = kd;
        out->items[out->count].value = value;
        out->count++;
    }
    odk_json_free(root);
    return ODK_SPINE_OK;
}

/* ------------------------------------------------------------------ */
/* Text helpers                                                        */
/* ------------------------------------------------------------------ */

static int is_term_byte_alnum(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

static int byte_eq_fold(unsigned char a, unsigned char b)
{
    if (a >= 'A' && a <= 'Z') {
        a = (unsigned char)(a + ('a' - 'A'));
    }
    if (b >= 'A' && b <= 'Z') {
        b = (unsigned char)(b + ('a' - 'A'));
    }
    return a == b;
}

/* Deterministic sentence split: a sentence ends at '.', '!' or '?'
 * followed by whitespace or end-of-text; a trailing non-empty fragment
 * is the final sentence. Returns the number of sentences; sentence i
 * spans out[i].start/out[i].len (trimmed, terminator included). */
typedef struct {
    size_t start;
    size_t len;
} span_t;

static size_t split_sentences(const char *text, span_t **out)
{
    size_t n = strlen(text);
    size_t cap = 8, count = 0;
    span_t *spans = malloc(cap * sizeof(span_t));
    size_t start = 0, i;

    if (spans == NULL) {
        *out = NULL;
        return 0;
    }
    for (i = 0; i < n; i++) {
        char c = text[i];

        if ((c == '.' || c == '!' || c == '?') &&
            (i + 1 >= n || text[i + 1] == ' ' || text[i + 1] == '\t' ||
             text[i + 1] == '\n' || text[i + 1] == '\r')) {
            size_t end = i + 1;

            while (start < end && (text[start] == ' ' || text[start] == '\t' ||
                                   text[start] == '\n')) {
                start++;
            }
            while (end > start && (text[end - 1] == ' ' ||
                                   text[end - 1] == '\t' ||
                                   text[end - 1] == '\n')) {
                end--;
            }
            if (end > start) {
                if (count == cap) {
                    size_t ncap = cap * 2;
                    span_t *ns = realloc(spans, ncap * sizeof(span_t));

                    if (ns == NULL) {
                        free(spans);
                        *out = NULL;
                        return 0;
                    }
                    spans = ns;
                    cap = ncap;
                }
                spans[count].start = start;
                spans[count].len = end - start;
                count++;
            }
            start = i + 1;
        }
    }
    /* trailing fragment */
    {
        size_t end = n;

        while (start < end && (text[start] == ' ' || text[start] == '\t' ||
                               text[start] == '\n')) {
            start++;
        }
        while (end > start && (text[end - 1] == ' ' ||
                               text[end - 1] == '\t' ||
                               text[end - 1] == '\n')) {
            end--;
        }
        if (end > start) {
            if (count == cap) {
                size_t ncap = cap * 2;
                span_t *ns = realloc(spans, ncap * sizeof(span_t));

                if (ns != NULL) {
                    spans = ns;
                    cap = ncap;
                }
            }
            if (count < cap) {
                spans[count].start = start;
                spans[count].len = end - start;
                count++;
            }
        }
    }
    *out = spans;
    return count;
}

/* Any rule term present in the text? (word-bounded, case-folded) */
static int text_has_term(const char *text, size_t len,
                         const odk_spine_str_list *terms)
{
    size_t t, i;

    for (t = 0; t < terms->count; t++) {
        const char *term = terms->items[t];
        size_t tlen = strlen(term);

        for (i = 0; i + tlen <= len; i++) {
            size_t before_ok = (i == 0) ||
                !is_term_byte_alnum((unsigned char)text[i - 1]);
            size_t after_ok = (i + tlen >= len) ||
                !is_term_byte_alnum((unsigned char)text[i + tlen]);
            size_t k;
            int ok = 1;

            if (!before_ok || !after_ok) {
                continue;
            }
            for (k = 0; k < tlen; k++) {
                if (!byte_eq_fold((unsigned char)text[i + k],
                                  (unsigned char)term[k])) {
                    ok = 0;
                    break;
                }
            }
            if (ok) {
                return 1;
            }
        }
    }
    return 0;
}

/* Append `add` to a growable content buffer. */
typedef struct {
    char *data;
    size_t len, cap;
} cbuf;

static int cbuf_init(cbuf *b)
{
    b->cap = 128;
    b->len = 0;
    b->data = malloc(b->cap);
    if (b->data == NULL) {
        return -1;
    }
    b->data[0] = '\0';
    return 0;
}

static int cbuf_puts(cbuf *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        size_t ncap = b->cap * 2;
        char *nd;

        while (ncap < b->len + n + 1) {
            ncap *= 2;
        }
        nd = realloc(b->data, ncap);
        if (nd == NULL) {
            return -1;
        }
        b->data = nd;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* Extraction                                                          */
/* ------------------------------------------------------------------ */

static int rule_first_match(const char *text, size_t len,
                            const odk_spine_str_list *terms, size_t *pos)
{
    size_t t, i;

    for (t = 0; t < terms->count; t++) {
        const char *term = terms->items[t];
        size_t tlen = strlen(term);

        for (i = 0; i + tlen <= len; i++) {
            size_t before_ok = (i == 0) ||
                !is_term_byte_alnum((unsigned char)text[i - 1]);
            size_t after_ok = (i + tlen >= len) ||
                !is_term_byte_alnum((unsigned char)text[i + tlen]);
            size_t k;
            int ok = 1;

            if (!before_ok || !after_ok) {
                continue;
            }
            for (k = 0; k < tlen; k++) {
                if (!byte_eq_fold((unsigned char)text[i + k],
                                  (unsigned char)term[k])) {
                    ok = 0;
                    break;
                }
            }
            if (ok) {
                *pos = i;
                return 1;
            }
        }
    }
    return 0;
}

odk_spine_status odk_decompose_run(const odk_spine_decomposition_template *tmpl,
                                   const char *item_text,
                                   const odk_spine_map *params_used,
                                   odk_spine_components *out,
                                   odk_spine_error *err)
{
    span_t *sentences = NULL;
    size_t sentence_count;
    size_t s, i;

    if (tmpl == NULL || out == NULL) {
        set_err(err, "null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    if (item_text == NULL) {
        item_text = "";
    }
    odk_spine_components_init(out);
    sentence_count = split_sentences(item_text, &sentences);

    out->template_id = strdup(tmpl->id);
    if (out->template_id == NULL) {
        free(sentences);
        set_err(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    out->template_version = tmpl->version;
    /* copy resolved params */
    if (params_used != NULL && params_used->count > 0) {
        out->params_used.items = calloc(params_used->count,
                                        sizeof(odk_spine_kv));
        if (out->params_used.items == NULL) {
            free(sentences);
            set_err(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
        for (i = 0; i < params_used->count; i++) {
            out->params_used.items[i].key =
                strdup(params_used->items[i].key);
            out->params_used.items[i].value =
                strdup(params_used->items[i].value);
            if (out->params_used.items[i].key == NULL ||
                out->params_used.items[i].value == NULL) {
                odk_spine_components_release(out);
                free(sentences);
                set_err(err, "out of memory");
                return ODK_SPINE_ERR_MEMORY;
            }
        }
        out->params_used.count = params_used->count;
    }

    /* one output Component per declared component spec, in spec order */
    if (tmpl->component_specs.count > 0) {
        out->components.items = calloc(tmpl->component_specs.count,
                                       sizeof(odk_spine_component));
        if (out->components.items == NULL) {
            free(sentences);
            set_err(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
        out->components.count = tmpl->component_specs.count;
    }
    for (s = 0; s < tmpl->component_specs.count; s++) {
        const odk_spine_component_spec *spec = &tmpl->component_specs.items[s];
        odk_spine_component *comp = &out->components.items[s];
        const odk_spine_extraction_rule *rule = NULL;
        cbuf content;
        long long items_taken = 0;
        int truncated = 0;
        size_t r;

        comp->name = strdup(spec->name);
        comp->kind = strdup(spec->kind);
        comp->content = NULL;
        if (comp->name == NULL || comp->kind == NULL) {
            free(sentences);
            set_err(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
        for (r = 0; r < tmpl->extraction_rules.count; r++) {
            if (strcmp(tmpl->extraction_rules.items[r].component,
                       spec->name) == 0) {
                rule = &tmpl->extraction_rules.items[r];
                break; /* first rule wins; deterministic */
            }
        }
        if (cbuf_init(&content) != 0) {
            free(sentences);
            set_err(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }

        if (rule == NULL && strcmp(spec->kind, "whole") == 0) {
            if (cbuf_puts(&content, item_text, strlen(item_text)) != 0) {
                goto oom;
            }
            items_taken = item_text[0] != '\0' ? 1 : 0;
        } else if (rule != NULL &&
                   (strcmp(rule->mode, "sentence") == 0)) {
            long long limit = spec->max_items > 0 ? spec->max_items : -1;

            for (i = 0; i < sentence_count; i++) {
                int hit = rule->terms.count == 0 ||
                          text_has_term(item_text + sentences[i].start,
                                        sentences[i].len, &rule->terms);

                if (!hit) {
                    continue;
                }
                if (limit > 0 && items_taken >= limit) {
                    truncated = 1;
                    break;
                }
                if (items_taken > 0 && cbuf_puts(&content, " ", 1) != 0) {
                    goto oom;
                }
                if (cbuf_puts(&content, item_text + sentences[i].start,
                              sentences[i].len) != 0) {
                    goto oom;
                }
                items_taken++;
            }
        } else if (rule != NULL &&
                   (strcmp(rule->mode, "before") == 0 ||
                    strcmp(rule->mode, "after") == 0 ||
                    strcmp(rule->mode, "around") == 0)) {
            size_t pos = 0;
            size_t len = strlen(item_text);

            if (rule_first_match(item_text, len, &rule->terms, &pos)) {
                size_t w = (size_t)(rule->window > 0 ? rule->window : 0);
                size_t lo, hi;

                if (strcmp(rule->mode, "before") == 0) {
                    lo = pos > w ? pos - w : 0;
                    hi = pos;
                } else if (strcmp(rule->mode, "after") == 0) {
                    lo = pos;
                    hi = pos + w < len ? pos + w : len;
                } else {
                    lo = pos > w ? pos - w : 0;
                    hi = pos + w < len ? pos + w : len;
                }
                while (lo < hi && (item_text[lo] == ' ' ||
                                   item_text[lo] == '\t' ||
                                   item_text[lo] == '\n')) {
                    lo++;
                }
                while (hi > lo && (item_text[hi - 1] == ' ' ||
                                   item_text[hi - 1] == '\t' ||
                                   item_text[hi - 1] == '\n')) {
                    hi--;
                }
                if (hi > lo) {
                    if (cbuf_puts(&content, item_text + lo, hi - lo) != 0) {
                        goto oom;
                    }
                    items_taken = 1;
                }
            }
        } else if (rule != NULL && strcmp(rule->mode, "keyvalue") == 0) {
            const char *p = item_text;
            long long limit = spec->max_items > 0 ? spec->max_items : -1;

            while (*p != '\0') {
                const char *eol = strchr(p, '\n');
                size_t line_len = eol != NULL ? (size_t)(eol - p)
                                              : strlen(p);
                const char *colon = memchr(p, ':', line_len);

                if (colon != NULL) {
                    size_t key_len = (size_t)(colon - p);
                    int hit;

                    while (key_len > 0 && (p[key_len - 1] == ' ' ||
                                           p[key_len - 1] == '\t')) {
                        key_len--;
                    }
                    hit = text_has_term(p, key_len, &rule->terms);
                    if (hit) {
                        size_t vlo = (size_t)(colon - p) + 1;
                        size_t vhi = line_len;
                        size_t lstart = 0;

                        while (vlo < vhi && (p[vlo] == ' ' ||
                                             p[vlo] == '\t')) {
                            vlo++;
                        }
                        while (vhi > vlo && (p[vhi - 1] == ' ' ||
                                             p[vhi - 1] == '\t')) {
                            vhi--;
                        }
                        if (limit > 0 && items_taken >= limit) {
                            truncated = 1;
                            break;
                        }
                        if (items_taken > 0 &&
                            cbuf_puts(&content, "\n", 1) != 0) {
                            goto oom;
                        }
                        if (cbuf_puts(&content, p + lstart, key_len) != 0) {
                            goto oom;
                        }
                        if (cbuf_puts(&content, ": ", 2) != 0) {
                            goto oom;
                        }
                        if (vhi > vlo &&
                            cbuf_puts(&content, p + vlo, vhi - vlo) != 0) {
                            goto oom;
                        }
                        items_taken++;
                    }
                }
                if (eol == NULL) {
                    break;
                }
                p = eol + 1;
            }
        }
        /* rule == NULL, kind != whole: component stays empty */

        comp->content = content.data; /* transfer */
        comp->item_count = items_taken;
        comp->complete = truncated ? 0 : 1;
        if (spec->required != 0 && items_taken == 0) {
            free(sentences);
            set_errf(err, "decompose: required component '%s' is empty",
                     spec->name);
            /* failure contract: *out is left zeroed */
            odk_spine_components_release(out);
            return ODK_SPINE_ERR_SCHEMA;
        }
        continue;

oom:
        free(sentences);
        set_err(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    free(sentences);
    return ODK_SPINE_OK;
}
