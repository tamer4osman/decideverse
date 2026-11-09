/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_classify_b.c - Tier B classification machinery (F1): brain-assisted
 * synonym/paraphrase resilience, implemented in the odk-brain module.
 *
 * Two evidence channels, both deterministic:
 *   1. vocabulary canonicalization: registered synonym phrases in the
 *      item text are rewritten to their canonical term before Tier A
 *      rule matching (paraphrase resilience);
 *   2. brain link evidence: learned word links of the registered
 *      odk_brain snapshot confirm rule terms that do not appear
 *      lexically, with half the weight of a lexical match.
 *
 * Every Tier B result is labeled tier "B" and attributed to the brain
 * snapshot id supplied by the caller. Tier B is fully disableable via
 * odk_spine_set_tier_b(); pure-replay contexts run Tier A only.
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
/* Vocabulary canonicalization                                         */
/* ------------------------------------------------------------------ */

typedef struct {
    odk_spine_str_list phrase;   /* canonicalized term tokens          */
    char *canonical;             /* canonical replacement token(s)     */
    odk_spine_str_list replace;  /* canonical token list               */
} canon_entry;

typedef struct {
    canon_entry *items;
    size_t count, cap;
} canon_list;

static void canon_list_free(canon_list *cl)
{
    size_t i;

    for (i = 0; i < cl->count; i++) {
        odk_classify_tokens_free(&cl->items[i].phrase);
        free(cl->items[i].canonical);
        odk_classify_tokens_free(&cl->items[i].replace);
    }
    free(cl->items);
    cl->items = NULL;
    cl->count = cl->cap = 0;
}

static int canon_entry_cmp(const void *pa, const void *pb)
{
    const canon_entry *a = pa;
    const canon_entry *b = pb;

    /* longest phrase first; ties keep registration order via index */
    if (a->phrase.count != b->phrase.count) {
        return a->phrase.count > b->phrase.count ? -1 : 1;
    }
    return 0;
}

/* Build the deterministic phrase->canonical map from a vocabulary.
 * Insertion sort keeps equal-length entries in declaration order. */
static void canon_build(const odk_spine_vocabulary *vocab, canon_list *cl)
{
    size_t g, t, i;

    cl->items = NULL;
    cl->count = cl->cap = 0;
    if (vocab == NULL) {
        return;
    }
    for (g = 0; g < vocab->groups.count; g++) {
        const odk_spine_synonym_group *grp = &vocab->groups.items[g];

        for (t = 0; t < grp->terms.count; t++) {
            canon_entry e;

            memset(&e, 0, sizeof(e));
            odk_classify_tokenize(grp->terms.items[t], &e.phrase);
            if (e.phrase.count == 0) {
                odk_classify_tokens_free(&e.phrase);
                continue;
            }
            e.canonical = strdup(grp->canonical);
            odk_classify_tokenize(grp->canonical, &e.replace);
            if (e.canonical == NULL || e.replace.count == 0) {
                odk_classify_tokens_free(&e.phrase);
                free(e.canonical);
                odk_classify_tokens_free(&e.replace);
                continue;
            }
            if (cl->count == cl->cap) {
                size_t ncap = cl->cap == 0 ? 8 : cl->cap * 2;
                canon_entry *ni = realloc(cl->items, ncap * sizeof(*ni));

                if (ni == NULL) {
                    odk_classify_tokens_free(&e.phrase);
                    free(e.canonical);
                    odk_classify_tokens_free(&e.replace);
                    return;
                }
                cl->items = ni;
                cl->cap = ncap;
            }
            cl->items[cl->count++] = e;
        }
    }
    /* insertion sort by phrase length (stable) */
    for (i = 1; i < cl->count; i++) {
        canon_entry key = cl->items[i];
        size_t j = i;

        while (j > 0 && canon_entry_cmp(&cl->items[j - 1], &key) > 0) {
            cl->items[j] = cl->items[j - 1];
            j--;
        }
        cl->items[j] = key;
    }
}

/* Rewrite the token stream: at each position, the longest vocabulary
 * phrase (ties: earlier declaration) is replaced by its canonical
 * tokens. */
static void canon_rewrite(const odk_spine_str_list *in, const canon_list *cl,
                          odk_spine_str_list *out)
{
    size_t i = 0, k;
    size_t cap = 0;
    /* Upper bound: every input token could be the start of a phrase
     * whose replacement is the largest canonical token count in the
     * vocabulary, and every unmatched token copies itself.  We take
     * the max replacement size and multiply by the input count. */
    {
        size_t c;
        for (c = 0; c < cl->count; c++) {
            if (cl->items[c].replace.count > cap) {
                cap = cl->items[c].replace.count;
            }
        }
    }
    if (cap == 0) {
        cap = 1;
    }
    cap = in->count * cap;
    if (cap < in->count + 8) {
        cap = in->count + 8;
    }

    out->count = 0;
    out->items = NULL;
    if (cap > 0) {
        out->items = calloc(cap, sizeof(char *));
        if (out->items == NULL) {
            return;
        }
    }
    while (i < in->count) {
        size_t c;
        int replaced = 0;

        for (c = 0; c < cl->count; c++) {
            const canon_entry *e = &cl->items[c];

            if (i + e->phrase.count > in->count) {
                continue;
            }
            {
                int ok = 1;
                size_t j;

                for (j = 0; j < e->phrase.count; j++) {
                    if (strcmp(in->items[i + j], e->phrase.items[j]) != 0) {
                        ok = 0;
                        break;
                    }
                }
                if (!ok) {
                    continue;
                }
            }
            for (k = 0; k < e->replace.count; k++) {
                char *cp = strdup(e->replace.items[k]);

                if (cp == NULL) {
                    return;
                }
                out->items[out->count++] = cp;
            }
            i += e->phrase.count;
            replaced = 1;
            break;
        }
        if (!replaced) {
            char *cp = strdup(in->items[i]);

            if (cp == NULL) {
                return;
            }
            out->items[out->count++] = cp;
            i++;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Brain link evidence                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    char *a;
    char *b;
    double strength;
} link_pair;

typedef struct {
    link_pair *items;
    size_t count, cap;
} link_list;

static void link_list_free(link_list *ll)
{
    size_t i;

    for (i = 0; i < ll->count; i++) {
        free(ll->items[i].a);
        free(ll->items[i].b);
    }
    free(ll->items);
    ll->items = NULL;
    ll->count = ll->cap = 0;
}

/* Load learned links (strength >= 0.5, not banned) from the brain
 * report. The report is deterministic, so the evidence is too. */
static void link_load(const odk_brain *brain, link_list *ll)
{
    char *json = NULL;
    odk_error jerr;
    odk_json_value *root;
    size_t i;

    ll->items = NULL;
    ll->count = ll->cap = 0;
    if (brain == NULL || odk_brain_links_report(brain, &json, &jerr) != ODK_OK ||
        json == NULL) {
        return;
    }
    root = odk_json_parse(json, strlen(json), &jerr);
    free(json);
    if (root == NULL || root->type != ODK_JSON_ARRAY) {
        odk_json_free(root);
        return;
    }
    for (i = 0; i < root->item_count; i++) {
        const odk_json_value *o = root->items[i];
        const odk_json_value *va, *vb, *vs, *vst;

        if (o->type != ODK_JSON_OBJECT) {
            continue;
        }
        va = odk_json_object_get(o, "a");
        vb = odk_json_object_get(o, "b");
        vs = odk_json_object_get(o, "strength");
        vst = odk_json_object_get(o, "status");
        if (va == NULL || vb == NULL || vs == NULL || vst == NULL ||
            va->type != ODK_JSON_STRING || vb->type != ODK_JSON_STRING ||
            vs->type != ODK_JSON_NUMBER || vst->type != ODK_JSON_STRING) {
            continue;
        }
        if (strcmp(vst->string, "banned") == 0 || vs->number < 0.5) {
            continue;
        }
        if (ll->count == ll->cap) {
            size_t ncap = ll->cap == 0 ? 16 : ll->cap * 2;
            link_pair *ni = realloc(ll->items, ncap * sizeof(*ni));

            if (ni == NULL) {
                break;
            }
            ll->items = ni;
            ll->cap = ncap;
        }
        ll->items[ll->count].a = strdup(va->string);
        ll->items[ll->count].b = strdup(vb->string);
        if (ll->items[ll->count].a == NULL ||
            ll->items[ll->count].b == NULL) {
            break;
        }
        ll->items[ll->count].strength = vs->number;
        ll->count++;
    }
    odk_json_free(root);
}

/* Is there a learned link between rule term `term` and item token `tok`? */
static int link_confirms(const link_list *ll, const char *term,
                         const char *tok)
{
    size_t i;

    for (i = 0; i < ll->count; i++) {
        if ((strcmp(ll->items[i].a, term) == 0 &&
             strcmp(ll->items[i].b, tok) == 0) ||
            (strcmp(ll->items[i].a, tok) == 0 &&
             strcmp(ll->items[i].b, term) == 0)) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Tier B scheme evaluation                                            */
/* ------------------------------------------------------------------ */

odk_spine_status odk_classify_scheme_tier_b(const odk_spine_scheme *scheme,
                                            const odk_spine_vocabulary *vocab,
                                            const odk_brain *brain,
                                            const char *item_text,
                                            const char *snapshot_id,
                                            odk_spine_classification *out,
                                            odk_spine_error *err)
{
    odk_spine_str_list raw;
    odk_spine_str_list canon;
    canon_list cl;
    link_list ll;
    char *canon_text = NULL;
    size_t cap, i, k;
    odk_spine_status st;

    if (scheme == NULL || out == NULL || snapshot_id == NULL) {
        if (err != NULL) {
            snprintf(err->text, sizeof(err->text), "null argument");
        }
        return ODK_SPINE_ERR_ARGUMENT;
    }
    odk_spine_classification_init(out);
    memset(&raw, 0, sizeof(raw));
    memset(&canon, 0, sizeof(canon));
    memset(&cl, 0, sizeof(cl));
    memset(&ll, 0, sizeof(ll));

    odk_classify_tokenize(item_text, &raw);
    canon_build(vocab, &cl);
    canon_rewrite(&raw, &cl, &canon);
    link_load(brain, &ll);

    /* re-join canonical tokens into the text Tier A will evaluate */
    cap = 1;
    for (i = 0; i < canon.count; i++) {
        cap += strlen(canon.items[i]) + 1;
    }
    canon_text = malloc(cap + 1);
    if (canon_text == NULL) {
        st = ODK_SPINE_ERR_MEMORY;
        goto done;
    }
    canon_text[0] = '\0';
    for (i = 0; i < canon.count; i++) {
        if (i > 0) {
            strcat(canon_text, " ");
        }
        strcat(canon_text, canon.items[i]);
    }

    /* Tier A evaluation over the canonical text */
    st = odk_classify_scheme_tier_a(scheme, canon_text, out, err);
    if (st != ODK_SPINE_OK) {
        goto done;
    }
    free(out->tier);
    free(out->brain_snapshot_id);
    out->tier = strdup("B");
    out->brain_snapshot_id = strdup(snapshot_id);
    if (out->tier == NULL || out->brain_snapshot_id == NULL) {
        odk_spine_classification_release(out);
        st = ODK_SPINE_ERR_MEMORY;
        goto done;
    }

    /* brain link evidence for unmatched terms of the winning rule */
    if (out->rule_index >= 0) {
        const odk_spine_scheme_rule *rule =
            &scheme->rules.items[out->rule_index];
        size_t link_hits = 0;
        double conf;

        for (i = 0; i < rule->match_terms.count; i++) {
            int lexical = 0;
            const char *term = rule->match_terms.items[i];

            for (k = 0; k < out->matched_terms.count; k++) {
                if (strcmp(out->matched_terms.items[k], term) == 0) {
                    lexical = 1;
                    break;
                }
            }
            if (lexical) {
                continue;
            }
            /* single-token rule terms can be confirmed by links against
             * any canonical item token */
            {
                odk_spine_str_list tt;

                odk_classify_tokenize(term, &tt);
                if (tt.count == 1) {
                    for (k = 0; k < canon.count; k++) {
                        if (link_confirms(&ll, tt.items[0], canon.items[k])) {
                            link_hits++;
                            odk_classify_push_term(&out->matched_terms,
                                                     canon.items[k]);
                            break;
                        }
                    }
                }
                odk_classify_tokens_free(&tt);
            }
        }
        if (link_hits > 0) {
            double evidence;

            /* lexical matches weigh 1, link evidence weighs 0.5 */
            evidence = 0.0;
            for (i = 0; i < out->matched_terms.count; i++) {
                int is_lexical = 0;

                for (k = 0; k < rule->match_terms.count; k++) {
                    if (strcmp(out->matched_terms.items[i],
                               rule->match_terms.items[k]) == 0) {
                        is_lexical = 1;
                        break;
                    }
                }
                evidence += is_lexical ? 1.0 : 0.5;
            }
            conf = 0.5 + 0.4 * (evidence / (double)rule->match_terms.count);
            if (conf > 0.9) {
                conf = 0.9;
            }
            out->confidence = conf;
        }
    }

    st = ODK_SPINE_OK;
done:
    free(canon_text);
    odk_classify_tokens_free(&raw);
    odk_classify_tokens_free(&canon);
    canon_list_free(&cl);
    link_list_free(&ll);
    return st;
}
