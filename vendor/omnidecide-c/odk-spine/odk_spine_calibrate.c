/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_spine_calibrate.c - F4 calibration reporting.
 *
 * A calibration report compares predicted confidence against observed
 * outcome rate, per primitive, per scheme, per tier, in ten confidence
 * bins. The report is fully deterministic given the same decision log:
 * two runs produce byte-identical documents (same doubles, same row
 * order, no wall clock).
 *
 * Log shape (Observation records):
 *   kind "decision": primitive, scheme_id, tier, confidence, id
 *   kind "outcome":  subject_ref = decision's observation_id,
 *                    outcome = "success" | "failure"
 */

#include "odk_spine_private.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ODK_CAL_BINS 10

typedef struct {
    char *observation_id;
    char *primitive;
    char *scheme_id;
    char *tier;
    double confidence;
    int has_outcome;
    int success; /* valid when has_outcome */
} cal_decision;

typedef struct {
    char *primitive;
    char *scheme_id;
    char *tier;
    size_t sample[ODK_CAL_BINS];
    double conf_sum[ODK_CAL_BINS];
    size_t success[ODK_CAL_BINS];
} cal_cell;

static void cal_decision_free(cal_decision *d)
{
    free(d->observation_id);
    free(d->primitive);
    free(d->scheme_id);
    free(d->tier);
}

/* Bin of a confidence value; comparisons use the exact doubles that the
 * generated JSON round-trips, so the binning is reproducible. */
static int cal_bin_of(double c)
{
    int b;

    if (c < 0.0) {
        c = 0.0;
    }
    for (b = 0; b < ODK_CAL_BINS - 1; b++) {
        if (c < (double)(b + 1) / 10.0) {
            return b;
        }
    }
    return ODK_CAL_BINS - 1;
}

static int cell_cmp(const void *pa, const void *pb)
{
    const cal_cell *a = pa;
    const cal_cell *b = pb;
    int c;

    c = strcmp(a->primitive, b->primitive);
    if (c != 0) {
        return c;
    }
    c = strcmp(a->scheme_id, b->scheme_id);
    if (c != 0) {
        return c;
    }
    return strcmp(a->tier, b->tier);
}

odk_spine_status odk_spine_observation_ingest(odk_spine *spine,
                                              const char *observation_json,
                                              odk_spine_error *err)
{
    odk_spine_observation obs;
    odk_spine_status st;

    if (spine == NULL || observation_json == NULL) {
        set_err(err, "null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    st = odk_spine_observation_from_json(observation_json, &obs, err);
    if (st != ODK_SPINE_OK) {
        return st;
    }
    {
        observation_entry entry;

        memset(&entry, 0, sizeof(entry));
        entry.obs = obs; /* the spine store takes ownership */
        if (spine->observation_count == spine->observation_cap) {
            size_t ncap = spine->observation_cap == 0
                              ? 16
                              : spine->observation_cap * 2;
            observation_entry *ng = realloc(spine->observations,
                                            ncap * sizeof(*ng));

            if (ng == NULL) {
                odk_spine_observation_release(&obs);
                set_err(err, "out of memory");
                return ODK_SPINE_ERR_MEMORY;
            }
            spine->observations = ng;
            spine->observation_cap = ncap;
        }
        spine->observations[spine->observation_count++] = entry;
    }
    return ODK_SPINE_OK;
}

odk_spine_status odk_spine_calibrate(odk_spine *spine,
                                     const char *generated_from,
                                     odk_spine_calibration_report *out,
                                     odk_spine_error *err)
{
    cal_decision *decisions = NULL;
    size_t decision_count = 0, decision_cap = 0;
    cal_cell *cells = NULL;
    size_t cell_count = 0, cell_cap = 0;
    size_t i, k, b;
    char *hash_input = NULL;
    size_t hash_len = 0, hash_cap = 0;
    char report_id[64];
    uint64_t h;

    if (spine == NULL || out == NULL) {
        set_err(err, "null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    odk_spine_calibration_report_init(out);

    /* 1. collect decision observations */
    for (i = 0; i < spine->observation_count; i++) {
        const odk_spine_observation *o = &spine->observations[i].obs;

        if (strcmp(o->kind, "decision") == 0) {
            if (decision_count == decision_cap) {
                size_t ncap = decision_cap == 0 ? 16 : decision_cap * 2;
                cal_decision *nd = realloc(decisions,
                                           ncap * sizeof(*nd));

                if (nd == NULL) {
                    goto oom;
                }
                decisions = nd;
                decision_cap = ncap;
            }
            memset(&decisions[decision_count], 0, sizeof(cal_decision));
            decisions[decision_count].observation_id =
                strdup(o->observation_id);
            decisions[decision_count].primitive = strdup(o->primitive);
            decisions[decision_count].scheme_id = strdup(o->scheme_id);
            decisions[decision_count].tier = strdup(o->tier);
            if (decisions[decision_count].observation_id == NULL ||
                decisions[decision_count].primitive == NULL ||
                decisions[decision_count].scheme_id == NULL ||
                decisions[decision_count].tier == NULL) {
                goto oom;
            }
            decisions[decision_count].confidence = o->confidence;
            decision_count++;
        }
    }

    /* 2. attach outcomes via subject_ref -> decision observation_id */
    for (i = 0; i < spine->observation_count; i++) {
        const odk_spine_observation *o = &spine->observations[i].obs;

        if (strcmp(o->kind, "outcome") == 0) {
            for (k = 0; k < decision_count; k++) {
                if (strcmp(decisions[k].observation_id, o->subject_ref) ==
                    0) {
                    decisions[k].has_outcome = 1;
                    decisions[k].success =
                        strcmp(o->outcome, "success") == 0;
                    break;
                }
            }
        }
    }

    /* 3. group into cells (insertion keeps discovery order; sorted below) */
    for (k = 0; k < decision_count; k++) {
        cal_decision *d = &decisions[k];
        cal_cell *cell = NULL;
        int bin;

        if (!d->has_outcome) {
            continue;
        }
        for (i = 0; i < cell_count; i++) {
            if (strcmp(cells[i].primitive, d->primitive) == 0 &&
                strcmp(cells[i].scheme_id, d->scheme_id) == 0 &&
                strcmp(cells[i].tier, d->tier) == 0) {
                cell = &cells[i];
                break;
            }
        }
        if (cell == NULL) {
            if (cell_count == cell_cap) {
                size_t ncap = cell_cap == 0 ? 8 : cell_cap * 2;
                cal_cell *nc = realloc(cells, ncap * sizeof(*nc));

                if (nc == NULL) {
                    goto oom;
                }
                cells = nc;
                cell_cap = ncap;
            }
            cell = &cells[cell_count++];
            memset(cell, 0, sizeof(*cell));
            cell->primitive = strdup(d->primitive);
            cell->scheme_id = strdup(d->scheme_id);
            cell->tier = strdup(d->tier);
            if (cell->primitive == NULL || cell->scheme_id == NULL ||
                cell->tier == NULL) {
                goto oom;
            }
        }
        bin = cal_bin_of(d->confidence);
        cell->sample[bin]++;
        cell->conf_sum[bin] += d->confidence;
        if (d->success) {
            cell->success[bin]++;
        }
    }

    /* 4. deterministic row order: primitive, scheme, tier */
    if (cell_count > 1) {
        qsort(cells, cell_count, sizeof(cal_cell), cell_cmp);
    }

    /* 5. build the report; rows only for non-empty bins */
    {
        size_t row = 0;

        for (i = 0; i < cell_count; i++) {
            cal_cell *cell = &cells[i];

            for (b = 0; b < ODK_CAL_BINS; b++) {
                if (cell->sample[b] == 0) {
                    continue;
                }
                if (row == 0) {
                    out->bins.items = calloc(cell_count * ODK_CAL_BINS,
                                             sizeof(odk_spine_calibration_bin));
                    if (out->bins.items == NULL) {
                        goto oom;
                    }
                }
                {
                    odk_spine_calibration_bin *bin_row =
                        &out->bins.items[row];
                    double predicted = cell->conf_sum[b] /
                                       (double)cell->sample[b];
                    double observed =
                        (double)cell->success[b] / (double)cell->sample[b];

                    odk_spine_calibration_bin_init(bin_row);
                    bin_row->primitive = strdup(cell->primitive);
                    bin_row->scheme_id = strdup(cell->scheme_id);
                    bin_row->tier = strdup(cell->tier);
                    if (bin_row->primitive == NULL ||
                        bin_row->scheme_id == NULL || bin_row->tier == NULL) {
                        goto oom;
                    }
                    bin_row->bin_index = (int64_t)b;
                    bin_row->lo = (double)b / 10.0;
                    bin_row->hi = (double)(b + 1) / 10.0;
                    bin_row->sample_count = (int64_t)cell->sample[b];
                    bin_row->predicted_mean = predicted;
                    bin_row->observed_rate = observed;
                    bin_row->calibration_error = fabs(predicted - observed);
                    row++;
                }
            }
        }
        out->bins.count = row;
    }
    out->generated_from = strdup(generated_from != NULL ? generated_from
                                                        : "");
    if (out->generated_from == NULL) {
        goto oom;
    }

    /* 6. deterministic report id: hash of the report body */
    for (i = 0; i < out->bins.count; i++) {
        char *row_json = odk_spine_calibration_bin_to_json(
            &out->bins.items[i]);
        size_t len;

        if (row_json == NULL) {
            goto oom;
        }
        len = strlen(row_json);
        if (hash_len + len + 1 > hash_cap) {
            size_t ncap = hash_cap == 0 ? 256 : hash_cap * 2;
            char *nh;

            while (ncap < hash_len + len + 1) {
                ncap *= 2;
            }
            nh = realloc(hash_input, ncap);
            if (nh == NULL) {
                free(row_json);
                goto oom;
            }
            hash_input = nh;
            hash_cap = ncap;
        }
        memcpy(hash_input + hash_len, row_json, len);
        hash_len += len;
        hash_input[hash_len] = '\0';
        free(row_json);
    }
    {
        char hash[17];

        odk_spine_fnv1a64_init(&h);
        odk_spine_fnv1a64_update(&h, out->generated_from,
                                 strlen(out->generated_from));
        odk_spine_fnv1a64_update(&h, "|", 1);
        odk_spine_fnv1a64_update(&h, hash_input != NULL ? hash_input : "",
                                 hash_len);
        odk_spine_fnv1a64_hex(h, hash);
        snprintf(report_id, sizeof(report_id), "cal-%s", hash);
    }
    out->report_id = strdup(report_id);
    if (out->report_id == NULL) {
        goto oom;
    }

    /* teardown */
    for (k = 0; k < decision_count; k++) {
        cal_decision_free(&decisions[k]);
    }
    free(decisions);
    for (k = 0; k < cell_count; k++) {
        free(cells[k].primitive);
        free(cells[k].scheme_id);
        free(cells[k].tier);
    }
    free(cells);
    free(hash_input);
    return ODK_SPINE_OK;

oom:
    if (decisions != NULL) {
        for (k = 0; k < decision_count; k++) {
            cal_decision_free(&decisions[k]);
        }
        free(decisions);
    }
    if (cells != NULL) {
        for (k = 0; k < cell_count; k++) {
            free(cells[k].primitive);
            free(cells[k].scheme_id);
            free(cells[k].tier);
        }
        free(cells);
    }
    free(hash_input);
    odk_spine_calibration_report_release(out);
    set_err(err, "out of memory");
    return ODK_SPINE_ERR_MEMORY;
}
