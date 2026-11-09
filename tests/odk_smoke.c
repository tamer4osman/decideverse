/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * odk_smoke.c - native smoke test of the C interoperability wrapper.
 *
 * Exercises lifecycle, the six primitives, error envelopes, handle
 * bookkeeping and the partially-failing creation paths.  Built with
 * -fsanitize=address,leak,undefined by build-native.sh --asan so that
 * every allocation path (including the error paths) is verified
 * leak-free.
 */

#include "odk_wasm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(cond)                                                        \
    do {                                                                   \
        checks++;                                                          \
        if (!(cond)) {                                                     \
            failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);\
        }                                                                  \
    } while (0)

int main(void)
{
    const char *context =
        "The service stopped responding. "
        "Restarting the service restored normal operation within seconds.";
    const char *options[] = {"restart the service",
                             "reinstall the operating system",
                             "wait for the weekend"};
    const char *rubric[] = {"fails", "partly works", "works"};
    const char *zh_options[2] = {"柏林数据中心", "远程分公司"};
    const char *two[2];
    char *env;
    uint32_t k1, k2, k3;

    /* --- lifecycle ------------------------------------------------- */
    CHECK(odkw_kernel_count() == 0u);
    k1 = odkw_kernel_create("ascii", "und");
    CHECK(k1 != 0u);
    CHECK(odkw_kernel_count() == 1u);
    CHECK(strcmp(odkw_version(), ODK_LIBRARY_VERSION) == 0);
    CHECK(strcmp(odkw_version(), "2.0.0") == 0);
    CHECK(strcmp(odkw_backend(), "embedded-fallback") == 0);

    /* normalization: case and whitespace are tolerated */
    k2 = odkw_kernel_create("  Unicode ", "ZH");
    CHECK(k2 != 0u);
    CHECK(odkw_kernel_count() == 2u);

    /* --- choice ----------------------------------------------------- */
    env = odkw_evaluate(k1, "choice", "How should we fix the outage?",
                        options, 3, context, 1, 0, 0, 0, 0u);
    CHECK(env != NULL);
    CHECK(odkw_last_status() == (int32_t)ODK_OK);
    CHECK(env != NULL && strncmp(env, "{\"primitive\": \"choice\"", 22) == 0);
    CHECK(env != NULL && strstr(env, "\"result\": \"restart the service\"") != NULL);
    CHECK(env != NULL && strstr(env, "\"distribution\": {") != NULL);
    CHECK(env != NULL && strstr(env, "\"meta\"") == NULL); /* flag not set */
    odkw_free(env);

    /* --- choice with meta ------------------------------------------- */
    env = odkw_evaluate(k1, "choice", "How should we fix the outage?",
                        options, 3, context, 1, 0, 0, 0, ODWK_FLAG_META);
    CHECK(env != NULL);
    CHECK(env != NULL && strstr(env, "\"meta\":{") != NULL);
    CHECK(env != NULL && strstr(env, "\"contextTruncated\":0") != NULL);
    odkw_free(env);

    /* --- score ------------------------------------------------------ */
    env = odkw_evaluate(k1, "score", "Rate the proposed fix.", rubric, 3,
                        context, 1, 0, 0, 0, 0u);
    CHECK(env != NULL);
    CHECK(env != NULL && strncmp(env, "{\"primitive\": \"score\"", 21) == 0);
    odkw_free(env);

    /* --- noul ------------------------------------------------------- */
    env = odkw_evaluate(k1, "noul",
                        "Restarting the service restored normal operation.",
                        NULL, 0, context, 1, 0, 0, 0, 0u);
    CHECK(env != NULL);
    CHECK(env != NULL && strncmp(env, "{\"primitive\": \"noul\"", 20) == 0);
    CHECK(env != NULL && strstr(env, "\"result\": true") != NULL);
    odkw_free(env);

    /* --- rank with caps + meta --------------------------------------- */
    env = odkw_evaluate(k1, "rank", "Order the fixes.", options, 3, context,
                        1, 2, 0, 0, ODWK_FLAG_META);
    CHECK(env != NULL);
    CHECK(env != NULL && strstr(env, "\"truncated\":1") != NULL);
    CHECK(env != NULL && strstr(env, "\"totalOptions\":3") != NULL);
    odkw_free(env);

    /* --- compare ----------------------------------------------------- */
    two[0] = options[0];
    two[1] = options[1];
    env = odkw_evaluate(k1, "compare", "Which fix first?", two, 2, context,
                        1, 0, 0, 0, 0u);
    CHECK(env != NULL);
    CHECK(env != NULL && strncmp(env, "{\"primitive\": \"compare\"", 23) == 0);
    odkw_free(env);

    /* --- extract ----------------------------------------------------- */
    env = odkw_evaluate(k1, "extract", "What restored operation?", NULL, 0,
                        context, 2, 0, 80, 0, ODWK_FLAG_META);
    CHECK(env != NULL);
    CHECK(env != NULL && strncmp(env, "{\"primitive\": \"extract\"", 23) == 0);
    CHECK(env != NULL && strstr(env, "\"emitted\":2") != NULL);
    odkw_free(env);

    /* --- unicode kernel decision (zh) --------------------------------- */
    env = odkw_evaluate(k2, "choice", "备份服务器应该放在哪里？", NULL, 0,
                        "备份服务器应该放在柏林数据中心，因为那里有冗余电源。", 1,
                        0, 0, 0, 0u);
    CHECK(env != NULL); /* options missing for choice -> error envelope */
    CHECK(odkw_last_status() == (int32_t)ODK_ERR_ARGUMENT);
    odkw_free(env);

    env = odkw_evaluate(k2, "choice", "备份服务器应该放在哪里？", zh_options, 2,
                        "备份服务器应该放在柏林数据中心，因为那里有冗余电源。", 1,
                        0, 0, 0, 0u);
    CHECK(env != NULL);
    CHECK(env != NULL && strstr(env, "柏林数据中心") != NULL);
    odkw_free(env);

    /* --- deterministic envelopes -------------------------------------- */
    {
        char *e1, *e2;
        e1 = odkw_evaluate(k2, "choice", "备份服务器应该放在哪里？", zh_options,
                           2, "备份服务器应该放在柏林数据中心，因为那里有冗余电源。",
                           1, 0, 0, 0, 0u);
        e2 = odkw_evaluate(k2, "choice", "备份服务器应该放在哪里？", zh_options,
                           2, "备份服务器应该放在柏林数据中心，因为那里有冗余电源。",
                           1, 0, 0, 0, 0u);
        CHECK(e1 != NULL && e2 != NULL && strcmp(e1, e2) == 0);
        odkw_free(e1);
        odkw_free(e2);
    }

    /* --- error envelopes ----------------------------------------------- */
    env = odkw_evaluate(k1, "frobnicate", "x", NULL, 0, NULL, 1, 0, 0, 0, 0u);
    CHECK(env != NULL);
    CHECK(strstr(env, "{\"error\":{\"status\":1") == env);
    CHECK(strstr(env, "unknown primitive") != NULL);
    odkw_free(env);

    env = odkw_evaluate(k1, "compare", "x", options, 3, NULL, 1, 0, 0, 0, 0u);
    CHECK(env != NULL && strstr(env, "exactly 2 options") != NULL);
    odkw_free(env);

    env = odkw_evaluate(k1, "extract", "x", NULL, 0, "ctx", 0, 0, 0, 0, 0u);
    CHECK(env != NULL && strstr(env, "top_k >= 1") != NULL);
    odkw_free(env);

    env = odkw_evaluate(0, "choice", "x", options, 3, NULL, 1, 0, 0, 0, 0u);
    CHECK(env != NULL && strstr(env, "invalid kernel handle") != NULL);
    odkw_free(env);

    env = odkw_evaluate(9999, "choice", "x", options, 3, NULL, 1, 0, 0, 0, 0u);
    CHECK(env != NULL && strstr(env, "invalid kernel handle") != NULL);
    odkw_free(env);

    /* --- creation failures ---------------------------------------------- */
    k3 = odkw_kernel_create("turbo", "und");
    CHECK(k3 == 0u);
    CHECK(odkw_last_status() == (int32_t)ODK_ERR_ARGUMENT);
    CHECK(strstr(odkw_last_error(), "unknown mode") != NULL);

    k3 = odkw_kernel_create("ascii", "klingon");
    CHECK(k3 == 0u);
    CHECK(strstr(odkw_last_error(), "unknown language code") != NULL);

    k3 = odkw_kernel_create("ascii", "default");
    CHECK(k3 == 0u);
    CHECK(strstr(odkw_last_error(), "per-call only") != NULL);

    /* NULL arguments select documented defaults */
    k3 = odkw_kernel_create(NULL, NULL);
    CHECK(k3 != 0u);
    env = odkw_evaluate(k3, "choice", "How should we fix the outage?", options,
                        3, context, 1, 0, 0, 0, 0u);
    CHECK(env != NULL && strstr(env, "restart the service") != NULL);
    odkw_free(env);
    odkw_kernel_free(k3);
    CHECK(odkw_kernel_count() == 2u);

    /* double free is a safe no-op */
    odkw_kernel_free(k3);
    CHECK(odkw_kernel_count() == 2u);

    odkw_kernel_free(k1);
    odkw_kernel_free(k2);
    CHECK(odkw_kernel_count() == 0u);

    if (failures == 0) {
        fprintf(stderr, "odk_smoke: %d checks, 0 failures\n", checks);
        return 0;
    }
    fprintf(stderr, "odk_smoke: %d checks, %d failures\n", checks, failures);
    return 1;
}
