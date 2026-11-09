/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * explain.c - example: why did the kernel pick this option?
 *
 * Prints the five-key decision and then every intermediate that produced
 * it, per option (docs/EXPLAIN.md).  The values ARE the internal
 * intermediates: recomputing softmax(6*(fused-0.5)) from them
 * reproduces the published probabilities exactly.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "omnidecide.h"

int main(void)
{
    const char *options[] = {"restart the service", "reboot the database",
                             "ignore the alert"};
    const char *instruction = "How should we fix the outage?";
    const char *context =
        "Incident 4112: the on-call engineer fixed the outage after she "
        "decided to restart the service. Restarting restored normal "
        "operation within minutes.";
    odk_kernel *kernel = odk_kernel_create();
    odk_result result;
    odk_explain explain;
    odk_error err;
    char *json;
    size_t i;

    if (kernel == NULL) {
        return 1;
    }
    memset(&result, 0, sizeof(result));
    memset(&explain, 0, sizeof(explain));
    memset(&err, 0, sizeof(err));

    if (odk_evaluate_choice(kernel, instruction, options, 3, context, NULL,
                            &result, &err) != ODK_OK) {
        printf("decision failed: %s\n", err.text);
        odk_kernel_free(kernel);
        return 1;
    }
    json = odk_result_to_json(&result);
    printf("decision: %s\n\n", json ? json : "(null)");
    free(json);

    if (odk_explain_choice(kernel, instruction, options, 3, context, NULL,
                           &explain, &err) != ODK_OK) {
        printf("explain failed: %s\n", err.text);
        odk_result_release(&result);
        odk_kernel_free(kernel);
        return 1;
    }
    printf("evidence view: %zu chunks\n", explain.chunk_count);
    printf("%-22s %10s %10s %10s %10s %10s %10s %10s\n", "option", "tf-cos",
           "jaccard", "trigram", "whole", "best-chunk", "directional",
           "fused");
    for (i = 0; i < explain.option_count; i++) {
        const odk_explain_option *o = &explain.options[i];
        printf("%-22s %10.4f %10.4f %10.4f %10.4f %10.4f %10.4f %10.4f\n",
               o->label, o->tf_cosine, o->jaccard, o->trigram, o->whole,
               o->best_chunk, o->directional, o->fused);
    }
    printf("\nprobabilities (softmax over fused, scale 6):\n");
    for (i = 0; i < explain.option_count; i++) {
        printf("  %-22s %.4f\n", explain.options[i].label,
               explain.options[i].probability);
    }
    json = odk_explain_to_json(&explain);
    if (json != NULL) {
        printf("\nfull explain JSON:\n%s\n", json);
        free(json);
    }
    odk_explain_release(&explain);
    odk_result_release(&result);
    odk_kernel_free(kernel);
    return 0;
}
