/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * bench_spine.c - v2.0.0 benchmarks (Section 6.3 of the build brief).
 *
 *   1. Tier A decision latency budget: <= 1 ms per decision at 2 KB
 *      context (the v1.0.0 envelope measured 0.54 ms on the reference
 *      VM; do not regress it materially).
 *   2. Classification throughput baseline, established at 2.0.0 and
 *      tracked from here on. Any regression > 10% requires written
 *      justification in the PR.
 *
 * Deterministic: fixed inputs, no RNG; timing uses CLOCK_MONOTONIC
 * (measurement only, never a decision input).
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "odk_spine.h"

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void fill_context(char *ctx, size_t target)
{
    const char *s =
        "The frobnication loop stalled twice while the load climbed. ";
    size_t off = 0;
    size_t len = strlen(s);

    while (off + len < target) {
        memcpy(ctx + off, s, len);
        off += len;
    }
    ctx[off] = '\0';
}

int main(void)
{
    odk_kernel *kernel = odk_kernel_create();
    odk_spine *spine = odk_spine_create();
    odk_spine_error err;
    char ctx[4096];
    const char *options[] = {"restart the service",
                             "reinstall the operating system",
                             "wait for the weekend"};
    const char *request;
    int i, runs = 100;
    double t0, t1, best, sum;
    int budget_ok = 1;
    char *request_buf;

    fill_context(ctx, 2000);

    /* ---- 1. Tier A decision latency @ 2 KB context ---- */
    best = 1e9;
    sum = 0.0;
    for (i = 0; i < 20; i++) { /* warmup */
        odk_result result;
        odk_error kerr;

        if (odk_evaluate_choice(kernel, "How should we fix the outage?",
                                options, 3, ctx, NULL, &result,
                                &kerr) == ODK_OK) {
            odk_result_release(&result);
        }
    }
    for (i = 0; i < runs; i++) {
        odk_result result;
        odk_error kerr;

        t0 = now_ms();
        if (odk_evaluate_choice(kernel, "How should we fix the outage?",
                                options, 3, ctx, NULL, &result,
                                &kerr) == ODK_OK) {
            odk_result_release(&result);
        }
        t1 = now_ms();
        sum += t1 - t0;
        if (t1 - t0 < best) {
            best = t1 - t0;
        }
    }
    printf("bench: choice @ 2KB: best %.3f ms, avg %.3f ms "
           "(budget <= 1.0 ms)\n",
           best, sum / runs);
    budget_ok = best <= 1.0;

    /* ---- 2. classification throughput baseline ---- */
    request = "{\"id\":\"fixture-triage\",\"version\":2,\"valid_from\":0,"
              "\"valid_to\":-1,\"default_label\":\"unsorted\","
              "\"facet_names\":[\"severity\",\"area\"],"
              "\"rules\":[{\"label\":\"hot\",\"match_terms\":["
              "\"frobnication\",\"overheat\"],\"any_term\":true,"
              "\"priority\":10,\"facets\":{\"severity\":\"high\","
              "\"area\":\"thermal\"}}],\"notes\":\"\"}";
    if (odk_spine_register_json(spine, ODK_SPINE_KIND_SCHEME_SET, request,
                                NULL, &err) != ODK_SPINE_OK) {
        printf("bench: registration failed: %s\n", err.text);
        return 2;
    }
    best = 1e9;
    sum = 0.0;
    runs = 1000;
    for (i = 0; i < 50; i++) { /* warmup */
        odk_spine_classification_result r;

        if (odk_spine_classify(spine, "bench", ctx, "", 1, &r, &err) ==
            ODK_SPINE_OK) {
            odk_spine_classification_result_release(&r);
        }
    }
    for (i = 0; i < runs; i++) {
        odk_spine_classification_result r;

        t0 = now_ms();
        if (odk_spine_classify(spine, "bench", ctx, "", 1, &r, &err) ==
            ODK_SPINE_OK) {
            odk_spine_classification_result_release(&r);
        }
        t1 = now_ms();
        sum += t1 - t0;
        if (t1 - t0 < best) {
            best = t1 - t0;
        }
    }
    printf("bench: classify @ 2KB: best %.3f ms/call, avg %.3f ms/call "
           "(%.0f classifications/s; baseline at 2.0.0)\n",
           best, sum / runs, sum > 0 ? 1000.0 / (sum / runs) : 0.0);

    /* ---- 3. pin + replay round-trip ---- */
    sum = 0.0;
    request_buf = malloc(8192); /* fixed-size: 2000-byte context + template */
    sprintf(request_buf,
            "{\"primitive\":\"choice\",\"instruction\":\"How should we fix "
            "the outage?\",\"options\":[\"restart the service\","
            "\"reinstall the operating system\",\"wait for the weekend\"],"
            "\"context\":\"%s\"}", ctx);
    best = 1e9;
    runs = 100;
    for (i = 0; i < runs; i++) {
        odk_spine_decision_pin pin;
        char *report = NULL;

        t0 = now_ms();
        if (odk_spine_pin(spine, kernel, request_buf, "bench", "exact", 1,
                          &pin, &err) == ODK_SPINE_OK) {
            odk_spine_replay(spine, kernel, pin.pin_id, &report, &err);
            free(report);
            odk_spine_decision_pin_release(&pin);
        }
        t1 = now_ms();
        sum += t1 - t0;
        if (t1 - t0 < best) {
            best = t1 - t0;
        }
    }
    printf("bench: pin+replay @ 2KB: best %.3f ms, avg %.3f ms\n", best,
           sum / runs);
    free(request_buf);

    odk_kernel_free(kernel);
    odk_spine_free(spine);
    if (!budget_ok) {
        printf("bench: FAIL - Tier A decision latency budget exceeded\n");
        return 1;
    }
    return 0;
}
