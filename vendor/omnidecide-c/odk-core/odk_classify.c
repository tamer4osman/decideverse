/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_classify.c - Tier A classification engine (F1): deterministic
 * lexical/rule matching over declarative schemes. Bit-level replayable;
 * this file contains no application-domain vocabulary.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "odk_classify.h"

#include "odk_spine_base.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Tokenizer                                                           */
/* ------------------------------------------------------------------ */

static int tok_byte_alnum(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

static unsigned char tok_fold(unsigned char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (unsigned char)(c + ('a' - 'A'));
    }
    return c;
}

void odk_classify_tokenize(const char *text, odk_spine_str_list *tokens)
{
    size_t i = 0, n;
    size_t cap = 8;

    tokens->count = 0;
    tokens->items = NULL;
    if (text == NULL) {
        return;
    }
    n = strlen(text);
    if (n == 0) {
        return;
    }
    tokens->items = malloc(cap * sizeof(char *));
    if (tokens->items == NULL) {
        return; /* OOM: empty list; caller reports schema/memory errors */
    }
    while (i < n) {
        if (tok_byte_alnum((unsigned char)text[i])) {
            size_t start = i;
            size_t len;
            char *tok;

            while (i < n && tok_byte_alnum((unsigned char)text[i])) {
                i++;
            }
            len = i - start;
            tok = malloc(len + 1);
            if (tok == NULL) {
                return;
            }
            for (size_t k = 0; k < len; k++) {
                tok[k] = (char)tok_fold((unsigned char)text[start + k]);
            }
            tok[len] = '\0';
            if (tokens->count == cap) {
                size_t ncap = cap * 2;
                char **ni = realloc(tokens->items, ncap * sizeof(char *));
                if (ni == NULL) {
                    free(tok);
                    return;
                }
                tokens->items = ni;
                cap = ncap;
            }
            tokens->items[tokens->count++] = tok;
        } else {
            i++;
        }
    }
}

void odk_classify_tokens_free(odk_spine_str_list *tokens)
{
    size_t i;

    if (tokens == NULL) {
        return;
    }
    for (i = 0; i < tokens->count; i++) {
        free(tokens->items[i]);
    }
    free(tokens->items);
    tokens->items = NULL;
    tokens->count = 0;
}

/* ------------------------------------------------------------------ */
/* Term matching                                                       */
/* ------------------------------------------------------------------ */

int odk_classify_term_present(const odk_spine_str_list *tokens,
                              const char *term)
{
    odk_spine_str_list tt;
    size_t i, k;

    odk_classify_tokenize(term, &tt);
    if (tt.count == 0) {
        odk_classify_tokens_free(&tt);
        return 0;
    }
    if (tt.count == 1) {
        int found = 0;

        for (i = 0; i < tokens->count; i++) {
            if (strcmp(tokens->items[i], tt.items[0]) == 0) {
                found = 1;
                break;
            }
        }
        odk_classify_tokens_free(&tt);
        return found;
    }
    /* phrase: consecutive run */
    for (i = 0; i + tt.count <= tokens->count; i++) {
        int ok = 1;

        for (k = 0; k < tt.count; k++) {
            if (strcmp(tokens->items[i + k], tt.items[k]) != 0) {
                ok = 0;
                break;
            }
        }
        if (ok) {
            odk_classify_tokens_free(&tt);
            return 1;
        }
    }
    odk_classify_tokens_free(&tt);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Tier A scheme evaluation                                            */
/* ------------------------------------------------------------------ */

void odk_classify_push_term(odk_spine_str_list *list, const char *term)
{
    char **ni;
    char *cp;

    ni = realloc(list->items, (list->count + 1) * sizeof(char *));
    if (ni == NULL) {
        return; /* evidence is best-effort; never changes the label */
    }
    list->items = ni;
    cp = strdup(term != NULL ? term : "");
    if (cp == NULL) {
        return;
    }
    list->items[list->count++] = cp;
}

odk_spine_status odk_classify_scheme_tier_a(const odk_spine_scheme *scheme,
                                            const char *item_text,
                                            odk_spine_classification *out,
                                            odk_spine_error *err)
{
    odk_spine_str_list tokens;
    long long best_priority = 0;
    size_t best_index = 0;
    size_t best_matched = 0;
    int found = 0;
    size_t r, i;

    if (scheme == NULL || out == NULL) {
        if (err != NULL) {
            snprintf(err->text, sizeof(err->text), "null argument");
        }
        return ODK_SPINE_ERR_ARGUMENT;
    }
    odk_spine_classification_init(out);
    odk_classify_tokenize(item_text, &tokens);

    for (r = 0; r < scheme->rules.count; r++) {
        const odk_spine_scheme_rule *rule = &scheme->rules.items[r];
        int hit;
        size_t matched_terms = 0;

        if (rule->any_term) {
            hit = 0;
            for (i = 0; i < rule->match_terms.count; i++) {
                if (odk_classify_term_present(&tokens,
                                              rule->match_terms.items[i])) {
                    matched_terms++;
                    hit = 1;
                }
            }
        } else {
            hit = 1;
            for (i = 0; i < rule->match_terms.count; i++) {
                if (odk_classify_term_present(&tokens,
                                              rule->match_terms.items[i])) {
                    matched_terms++;
                } else {
                    hit = 0;
                }
            }
        }
        if (hit && (!found || rule->priority > best_priority)) {
            /* highest priority wins; ties keep the earlier declaration */
            found = 1;
            best_priority = rule->priority;
            best_index = r;
            best_matched = matched_terms;
        }
    }
    odk_classify_tokens_free(&tokens);

    out->scheme_id = strdup(scheme->id);
    out->label = strdup(found ? scheme->rules.items[best_index].label
                              : scheme->default_label);
    out->tier = strdup("A");
    out->brain_snapshot_id = strdup("");
    if (out->scheme_id == NULL || out->label == NULL ||
        out->tier == NULL || out->brain_snapshot_id == NULL) {
        odk_spine_classification_release(out);
        if (err != NULL) {
            snprintf(err->text, sizeof(err->text), "out of memory");
        }
        return ODK_SPINE_ERR_MEMORY;
    }
    out->rule_index = found ? (int64_t)best_index : -1;
    out->scheme_version = scheme->version;

    /* facets of the winning rule (in rule facet order) */
    if (found && scheme->rules.items[best_index].facets.count > 0) {
        const odk_spine_scheme_rule *rule = &scheme->rules.items[best_index];

        out->facets.items = calloc(rule->facets.count,
                                   sizeof(odk_spine_kv));
        if (out->facets.items == NULL) {
            odk_spine_classification_release(out);
            if (err != NULL) {
                snprintf(err->text, sizeof(err->text), "out of memory");
            }
            return ODK_SPINE_ERR_MEMORY;
        }
        for (i = 0; i < rule->facets.count; i++) {
            out->facets.items[i].key = strdup(rule->facets.items[i].key);
            out->facets.items[i].value = strdup(rule->facets.items[i].value);
            if (out->facets.items[i].key == NULL ||
                out->facets.items[i].value == NULL) {
                odk_spine_classification_release(out);
                if (err != NULL) {
                    snprintf(err->text, sizeof(err->text), "out of memory");
                }
                return ODK_SPINE_ERR_MEMORY;
            }
        }
        out->facets.count = rule->facets.count;
    }

    /* confidence: deterministic mapping of evidence strength */
    if (found) {
        const odk_spine_scheme_rule *rule = &scheme->rules.items[best_index];
        double conf;

        if (rule->any_term) {
            conf = 0.5 + 0.4 * ((double)best_matched /
                                (double)rule->match_terms.count);
        } else {
            conf = 0.9; /* all terms confirmed */
        }
        if (conf > 0.9) {
            conf = 0.9;
        }
        out->confidence = conf;
    } else {
        out->confidence = 0.5;
    }

    /* evidence terms of the winning rule, in rule order */
    if (found) {
        const odk_spine_scheme_rule *rule = &scheme->rules.items[best_index];
        odk_spine_str_list tokens2;

        odk_classify_tokenize(item_text, &tokens2);
        for (i = 0; i < rule->match_terms.count; i++) {
            if (odk_classify_term_present(&tokens2,
                                          rule->match_terms.items[i])) {
                odk_classify_push_term(&out->matched_terms,
                                       rule->match_terms.items[i]);
            }
        }
        odk_classify_tokens_free(&tokens2);
    }

    return ODK_SPINE_OK;
}
