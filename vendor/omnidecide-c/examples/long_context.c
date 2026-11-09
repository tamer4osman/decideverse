/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * long_context.c - example: see the default cut, then fix it.
 *
 * A decisive sentence at the END of a 90 KB context is not scored under
 * the default ASCII limits (20,000-byte context, 40,000-byte chunk
 * source): on this filler the kernel answers "monthly plan" at 0.5133 -
 * the decisive sentence never entered the evidence view.
 * odk_result_meta reports exactly what happened (context_truncated=1,
 * context_units_dropped=49,988 bytes) and three remedies keep the end of
 * the input (measured on gcc -O2).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "omnidecide.h"

static char *g_context = NULL;

static char *build_context(void)
{
    const char *filler =
        "Routine status entries continue with nothing decisive here. ";
    const char *decisive =
        "Final line: the board approves the annual plan.\n";
    size_t cap = 90000 + 128;
    char *context = malloc(cap);
    size_t n = 0;
    if (context == NULL) {
        return NULL;
    }
    while (n + strlen(filler) + strlen(decisive) < 90000) {
        memcpy(context + n, filler, strlen(filler));
        n += strlen(filler);
    }
    strcpy(context + n, decisive);
    return context;
}

static void run(const char *label, const odk_kernel_config *cfg)
{
    const char *options[] = {"annual plan", "monthly plan"};
    odk_kernel *kernel = odk_kernel_create_ex(cfg);
    odk_result result;
    odk_result_meta meta;
    odk_error err;
    char *json;

    if (kernel == NULL) {
        printf("%s: kernel alloc failed\n", label);
        return;
    }
    memset(&result, 0, sizeof(result));
    memset(&meta, 0, sizeof(meta));
    memset(&err, 0, sizeof(err));
    if (odk_evaluate_choice_opts(kernel, "Which plan did the board approve?",
                                 options, 2, g_context, NULL, NULL, &result,
                                 &meta, &err) != ODK_OK) {
        printf("%s: evaluation failed: %s\n", label, err.text);
    } else {
        json = odk_result_to_json(&result);
        printf("%s:\n  %s\n  context_truncated=%d units_dropped=%zu\n", label,
               json ? json : "(null)", meta.context_truncated,
               meta.context_units_dropped);
        free(json);
    }
    odk_result_release(&result);
    odk_kernel_free(kernel);
}

int main(void)
{
    odk_kernel_config cfg;

    g_context = build_context();
    if (g_context == NULL) {
        return 1;
    }
    printf("context size: %zu bytes, decisive sentence at the END\n\n",
           strlen(g_context));

    memset(&cfg, 0, sizeof(cfg)); /* defaults: the cut happens */
    run("1. default ASCII limits (HEAD)", &cfg);

    memset(&cfg, 0, sizeof(cfg));
    cfg.over_limit = ODK_OVERLIMIT_HEAD_TAIL; /* keep head AND tail */
    run("2. HEAD_TAIL strategy", &cfg);

    memset(&cfg, 0, sizeof(cfg));
    cfg.context_max = 200000; /* raise the limits */
    cfg.chunk_source_max = 200000;
    cfg.max_chunks = 10000;
    run("3. raised limits", &cfg);

    memset(&cfg, 0, sizeof(cfg));
    cfg.text_mode = ODK_TEXT_UNICODE; /* 100,000 cp default */
    run("4. Unicode mode", &cfg);

    free(g_context);
    return 0;
}
