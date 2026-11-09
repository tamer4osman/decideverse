/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_spine_classify.c - F1 multi-scheme classification orchestration and
 * F2 temporal validity.
 *
 *   - One call, N schemes, one result document: odk_spine_classify
 *     iterates every ACTIVE scheme whose validity covers t, in
 *     registration order. Callers never loop over schemes themselves.
 *   - Two clearly labeled tiers: Tier A (deterministic lexical/rule
 *     matching, bit-replayable) and Tier B (brain-assisted, labeled with
 *     the brain snapshot id, fully disableable).
 *   - A classification event is itself an Observation and is written
 *     back; every assignment appends a ClassificationInterval. History
 *     is never overwritten; as-of queries are answered from the stored
 *     intervals without recomputing.
 */

#include "odk_spine_private.h"

#include "odk_classify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

void odk_spine_set_tier_b(odk_spine *spine, int enabled)
{
    if (spine != NULL) {
        spine->tier_b_enabled = enabled ? 1 : 0;
    }
}

int odk_spine_tier_b_enabled(const odk_spine *spine)
{
    return spine == NULL ? 0 : spine->tier_b_enabled;
}

/* ------------------------------------------------------------------ */
/* Observation + interval stores                                       */
/* ------------------------------------------------------------------ */

size_t odk_spine_observation_count(const odk_spine *spine)
{
    return spine == NULL ? 0 : spine->observation_count;
}

const odk_spine_observation *odk_spine_observation_at(const odk_spine *spine,
                                                      size_t i)
{
    if (spine == NULL || i >= spine->observation_count) {
        return NULL;
    }
    return &spine->observations[i].obs;
}

size_t odk_spine_interval_count(const odk_spine *spine)
{
    return spine == NULL ? 0 : spine->interval_count;
}

const odk_spine_classification_interval *odk_spine_interval_at(
    const odk_spine *spine, size_t i)
{
    if (spine == NULL || i >= spine->interval_count) {
        return NULL;
    }
    return &spine->intervals[i].interval;
}

/* Append one observation (takes a deep copy; the record is immutable
 * once stored). Shared by classify and pinning. */
odk_spine_status odk_spine_observation_append(odk_spine *spine,
                                              const odk_spine_observation *obs,
                                              odk_spine_error *err)
{
    observation_entry entry;

    memset(&entry, 0, sizeof(entry));
    if (odk_spine_observation_copy(&entry.obs, obs) != ODK_SPINE_OK) {
        set_err(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    if (spine->observation_count == spine->observation_cap) {
        size_t ncap = spine->observation_cap == 0 ? 16
                                                  : spine->observation_cap * 2;
        observation_entry *ng = realloc(spine->observations,
                                        ncap * sizeof(*ng));

        if (ng == NULL) {
            odk_spine_observation_release(&entry.obs);
            set_err(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
        spine->observations = ng;
        spine->observation_cap = ncap;
    }
    spine->observations[spine->observation_count++] = entry;
    return ODK_SPINE_OK;
}

static odk_spine_status interval_append(
    odk_spine *spine, const odk_spine_classification_interval *interval,
    odk_spine_error *err)
{
    interval_entry entry;

    memset(&entry, 0, sizeof(entry));
    if (odk_spine_classification_interval_copy(&entry.interval, interval) !=
        ODK_SPINE_OK) {
        set_err(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    if (spine->interval_count == spine->interval_cap) {
        size_t ncap = spine->interval_cap == 0 ? 16 : spine->interval_cap * 2;
        interval_entry *ng = realloc(spine->intervals, ncap * sizeof(*ng));

        if (ng == NULL) {
            odk_spine_classification_interval_release(&entry.interval);
            set_err(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
        spine->intervals = ng;
        spine->interval_cap = ncap;
    }
    spine->intervals[spine->interval_count++] = entry;
    return ODK_SPINE_OK;
}

/* ------------------------------------------------------------------ */
/* Deep-copy helpers for the generated records (appending stores own   */
/* their entries, so appends must copy).                               */
/* ------------------------------------------------------------------ */

odk_spine_status odk_spine_observation_copy(odk_spine_observation *dst,
                                            const odk_spine_observation *src)
{
    char *json = odk_spine_observation_to_json(src);

    if (json == NULL) {
        return ODK_SPINE_ERR_MEMORY;
    }
    {
        odk_spine_status st = odk_spine_observation_from_json(json, dst, NULL);
        free(json);
        return st;
    }
}

odk_spine_status odk_spine_decision_pin_copy(odk_spine_decision_pin *dst,
                                             const odk_spine_decision_pin *src)
{
    char *json = odk_spine_decision_pin_to_json(src);

    if (json == NULL) {
        return ODK_SPINE_ERR_MEMORY;
    }
    {
        odk_spine_status st =
            odk_spine_decision_pin_from_json(json, dst, NULL);
        free(json);
        return st;
    }
}

odk_spine_status odk_spine_classification_interval_copy(
    odk_spine_classification_interval *dst,
    const odk_spine_classification_interval *src)
{
    char *json = odk_spine_classification_interval_to_json(src);

    if (json == NULL) {
        return ODK_SPINE_ERR_MEMORY;
    }
    {
        odk_spine_status st =
            odk_spine_classification_interval_from_json(json, dst, NULL);
        free(json);
        return st;
    }
}

/* ------------------------------------------------------------------ */
/* F1: one call, N schemes, one result document                        */
/* ------------------------------------------------------------------ */

odk_spine_status odk_spine_classify(odk_spine *spine,
                                    const char *subject_ref,
                                    const char *item_text,
                                    const char *scope, int64_t t,
                                    odk_spine_classification_result *out,
                                    odk_spine_error *err)
{
    size_t i, k;
    size_t n_class = 0;
    const reg_brain_entry *brain_entry = NULL;

    if (spine == NULL || subject_ref == NULL || item_text == NULL || out == NULL) {
        set_err(err, "null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    odk_spine_classification_result_init(out);

    /* Tier B requires an explicitly registered brain snapshot */
    if (spine->tier_b_enabled) {
        for (i = 0; i < spine->brain_count; i++) {
            if (brain_entry == NULL ||
                spine->brains[i].seq < brain_entry->seq) {
                brain_entry = &spine->brains[i];
            }
        }
    }

    /* count matching schemes first (active + covering t) */
    for (i = 0; i < spine->scheme_count; i++) {
        const reg_scheme_entry *e = &spine->schemes[i];

        if (e->active && e->scheme.valid_from <= t &&
            (e->scheme.valid_to == -1 || t <= e->scheme.valid_to)) {
            n_class++;
        }
    }

    if (n_class > 0) {
        out->classifications.items = calloc(n_class,
                                            sizeof(odk_spine_classification));
        if (out->classifications.items == NULL) {
            set_err(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    out->subject_ref = strdup(subject_ref);
    if (out->subject_ref == NULL) {
        odk_spine_classification_result_release(out);
        set_err(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    out->t = t;

    /* classify per scheme, in registration order */
    for (i = 0; i < spine->scheme_count && n_class > 0; i++) {
        const reg_scheme_entry *e = &spine->schemes[i];
        odk_spine_classification *c;
        odk_spine_status st;
        int tier_b;
        const odk_spine_vocabulary *vocab = NULL;

        if (!e->active || e->scheme.valid_from > t ||
            (e->scheme.valid_to != -1 && t > e->scheme.valid_to)) {
            continue;
        }
        c = &out->classifications.items[out->classifications.count];
        tier_b = spine->tier_b_enabled && brain_entry != NULL;
        if (tier_b) {
            /* use every active vocabulary covering t, in registration
             * order: canonicalization is applied cumulatively */
            size_t v;

            for (v = 0; v < spine->vocab_count; v++) {
                const reg_vocab_entry *ve = &spine->vocabularies[v];

                if (ve->active && ve->vocab.valid_from <= t &&
                    (ve->vocab.valid_to == -1 || t <= ve->vocab.valid_to)) {
                    vocab = &ve->vocab;
                }
            }
        }
        if (tier_b) {
            st = odk_classify_scheme_tier_b(&e->scheme, vocab,
                                            brain_entry->brain, item_text,
                                            brain_entry->snapshot_id, c,
                                            err);
        } else {
            st = odk_classify_scheme_tier_a(&e->scheme, item_text, c, err);
        }
        if (st != ODK_SPINE_OK) {
            odk_spine_classification_result_release(out);
            return st;
        }
        out->classifications.count++;

        /* ---- write-back: observation + validity interval ---- */
        {
            odk_spine_observation obs;
            odk_spine_classification_interval interval;
            char obs_id[64];
            char *cls_json;
            char *interval_json;

            snprintf(obs_id, sizeof(obs_id), "obs-%llu",
                     (unsigned long long)(++spine->obs_seq));

            odk_spine_observation_init(&obs);
            obs.observation_id = strdup(obs_id);
            obs.kind = strdup("classification");
            obs.t = t;
            obs.scope = strdup(scope != NULL ? scope : "");
            obs.subject_ref = strdup(subject_ref);
            obs.primitive = strdup("");
            obs.scheme_id = strdup(e->scheme.id);
            obs.confidence = c->confidence;
            obs.tier = strdup(c->tier);
            obs.brain_snapshot_id = strdup(c->brain_snapshot_id);
            obs.outcome = strdup("");
            obs.kernel_version = strdup(ODK_SPINE_KERNEL_VERSION);
            obs.schema_fp = strdup(spine->schema_fp);
            cls_json = odk_spine_classification_to_json(c);
            obs.payload_json = cls_json != NULL
                                   ? cls_json
                                   : strdup("");
            if (obs.observation_id == NULL || obs.kind == NULL ||
                obs.scope == NULL || obs.subject_ref == NULL ||
                obs.primitive == NULL || obs.scheme_id == NULL ||
                obs.tier == NULL || obs.brain_snapshot_id == NULL ||
                obs.outcome == NULL || obs.kernel_version == NULL ||
                obs.schema_fp == NULL || obs.payload_json == NULL) {
                odk_spine_observation_release(&obs);
                odk_spine_classification_result_release(out);
                set_err(err, "out of memory");
                return ODK_SPINE_ERR_MEMORY;
            }
            st = odk_spine_observation_append(spine, &obs, err);
            odk_spine_observation_release(&obs);
            if (st != ODK_SPINE_OK) {
                odk_spine_classification_result_release(out);
                return st;
            }

            odk_spine_classification_interval_init(&interval);
            interval.subject_ref = strdup(subject_ref);
            interval.scheme_id = strdup(e->scheme.id);
            interval.scheme_version = e->scheme.version;
            interval.label = strdup(c->label);
            interval.tier = strdup(c->tier);
            interval.brain_snapshot_id = strdup(c->brain_snapshot_id);
            interval.valid_from = t > e->scheme.valid_from
                                      ? t
                                      : e->scheme.valid_from;
            interval.valid_to = e->scheme.valid_to;
            interval.observation_id = strdup(obs_id);
            interval_json = NULL;
            if (c->facets.count > 0) {
                interval.facets.items =
                    calloc(c->facets.count, sizeof(odk_spine_kv));
                if (interval.facets.items != NULL) {
                    for (k = 0; k < c->facets.count; k++) {
                        interval.facets.items[k].key =
                            strdup(c->facets.items[k].key);
                        interval.facets.items[k].value =
                            strdup(c->facets.items[k].value);
                    }
                    interval.facets.count = c->facets.count;
                }
            }
            if (interval.subject_ref == NULL || interval.scheme_id == NULL ||
                interval.label == NULL || interval.tier == NULL ||
                interval.brain_snapshot_id == NULL ||
                interval.observation_id == NULL) {
                odk_spine_classification_interval_release(&interval);
                odk_spine_classification_result_release(out);
                set_err(err, "out of memory");
                return ODK_SPINE_ERR_MEMORY;
            }
            (void)interval_json;
            st = interval_append(spine, &interval, err);
            odk_spine_classification_interval_release(&interval);
            if (st != ODK_SPINE_OK) {
                odk_spine_classification_result_release(out);
                return st;
            }
        }
    }
    return ODK_SPINE_OK;
}

/* ------------------------------------------------------------------ */
/* F2: as-of classification query (no recomputation)                   */
/* ------------------------------------------------------------------ */

odk_spine_status odk_spine_classification_at(
    const odk_spine *spine, const char *subject_ref, const char *scheme_id,
    int64_t t, const odk_spine_classification_interval **out,
    odk_spine_error *err)
{
    size_t i;
    size_t best = spine != NULL ? spine->interval_count : 0;
    int found = 0;

    if (spine == NULL || subject_ref == NULL || scheme_id == NULL ||
        out == NULL) {
        set_err(err, "null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    *out = NULL;
    /* newest valid_from wins; ties resolved by later append (history is
     * append-only, so the store order IS the lineage) */
    for (i = 0; i < spine->interval_count; i++) {
        const odk_spine_classification_interval *iv =
            &spine->intervals[i].interval;

        if (strcmp(iv->subject_ref, subject_ref) != 0 ||
            strcmp(iv->scheme_id, scheme_id) != 0) {
            continue;
        }
        if (iv->valid_from > t) {
            continue;
        }
        if (iv->valid_to != -1 && t > iv->valid_to) {
            continue;
        }
        if (!found || iv->valid_from >=
                          spine->intervals[best].interval.valid_from) {
            found = 1;
            best = i;
        }
    }
    if (!found) {
        set_errf(err, "no classification of '%s' under '%s' at t=%lld",
                 subject_ref, scheme_id, (long long)t);
        return ODK_SPINE_ERR_NOT_FOUND;
    }
    *out = &spine->intervals[best].interval;
    return ODK_SPINE_OK;
}
