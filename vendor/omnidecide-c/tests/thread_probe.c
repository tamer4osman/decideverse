/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * thread_probe.c - ThreadSanitizer probe: one shared kernel evaluated by
 * 8 threads concurrently.  The kernel is read-only after creation, so all
 * threads must observe byte-identical results to the single-threaded run.
 * Exit 0 = pass.
 */
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "omnidecide.h"

#define THREADS 8
#define CALLS_PER_THREAD 500

static odk_kernel *g_kernel = NULL;
static char **g_expected = NULL; /* serialized results, CALLS_PER_THREAD */
static int g_failures = 0;

static const char *const OPTS[3] = {"annual plan", "monthly plan",
                                    "trial plan"};

static char *run_one(odk_kernel *k, size_t i)
{
    const char *instruction =
        i % 2 == 0 ? "Which plan did the board approve?"
                   : "Pick the cheaper option";
    char *ctx = NULL;
    odk_result out;
    odk_error err;
    char *json;

    {
        size_t need = (size_t)(i % 17) * 137 + 64;
        size_t j;
        ctx = malloc(need + 1);
        if (ctx == NULL) {
            return NULL;
        }
        for (j = 0; j < need; j++) {
            ctx[j] = (char)('a' + (int)((i * 31 + j * 7) % 23));
        }
        ctx[need] = '\0';
        memcpy(ctx, "the board approves the annual plan", 34);
    }
    memset(&out, 0, sizeof(out));
    memset(&err, 0, sizeof(err));
    if (odk_evaluate_choice(k, instruction, OPTS, 3, ctx, NULL, &out,
                            &err) != ODK_OK) {
        json = NULL;
    } else {
        json = odk_result_to_json(&out);
        odk_result_release(&out);
    }
    free(ctx);
    return json;
}

static void *worker(void *arg)
{
    size_t tid = (size_t)(uintptr_t)arg;
    size_t i;
    for (i = 0; i < CALLS_PER_THREAD; i++) {
        char *json = run_one(g_kernel, i);
        if (json == NULL) {
            if (g_expected[i] != NULL) {
                __sync_fetch_and_add(&g_failures, 1);
            }
            continue;
        }
        if (g_expected[i] == NULL ||
            strcmp(json, g_expected[i]) != 0) {
            __sync_fetch_and_add(&g_failures, 1);
        }
        free(json);
        (void)tid;
    }
    return NULL;
}

static int run_phase(const char *label)
{
    pthread_t th[THREADS];
    size_t i;
    int phase_failures;

    g_failures = 0;

    g_expected = malloc(sizeof(char *) * CALLS_PER_THREAD);
    if (g_expected == NULL) {
        return 1;
    }
    for (i = 0; i < CALLS_PER_THREAD; i++) {
        g_expected[i] = run_one(g_kernel, i); /* single-threaded baseline */
    }
    for (i = 0; i < THREADS; i++) {
        if (pthread_create(&th[i], NULL, worker,
                           (void *)(uintptr_t)i) != 0) {
            fprintf(stderr, "pthread_create failed\n");
            return 1;
        }
    }
    for (i = 0; i < THREADS; i++) {
        pthread_join(th[i], NULL);
    }
    phase_failures = g_failures;
    if (phase_failures != 0) {
        printf("thread-probe: %s FAILED, %d divergent results\n", label,
               phase_failures);
    } else {
        printf("thread-probe: %s %d threads x %d calls on one shared "
               "kernel, all results identical\n",
               label, THREADS, CALLS_PER_THREAD);
    }
    for (i = 0; i < CALLS_PER_THREAD; i++) {
        free(g_expected[i]);
    }
    free(g_expected);
    g_expected = NULL;
    return phase_failures != 0;
}

int main(void)
{
    int rc = 0;
    odk_error err;

    g_kernel = odk_kernel_create();
    if (g_kernel == NULL) {
        fprintf(stderr, "kernel alloc failed\n");
        return 1;
    }
    /* phase 1: built-in default model (no file) */
    if (run_phase("default-model") != 0) {
        rc = 1;
    }
    /* phase 2: a LOADED model, same shared kernel pattern */
    memset(&err, 0, sizeof(err));
    if (odk_kernel_load_model(g_kernel,
                              "tests/fixtures/default_model.odkmodel",
                              &err) != ODK_OK) {
        fprintf(stderr, "model load failed: %s\n", err.text);
        rc = 1;
    } else if (run_phase("loaded-model") != 0) {
        rc = 1;
    }
    odk_kernel_free(g_kernel);
    return rc;
}
