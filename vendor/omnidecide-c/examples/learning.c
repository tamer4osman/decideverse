/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * learning.c - opt-in outcome learning with the experience brain.
 *
 *   make examples && ./build/learning
 *
 * The kernel alone is stateless: the same inputs always give the same
 * answer.  The brain is a small persistent memory of which options
 * worked in the past.  Feed it rewards in [-1, 1]; ask it to shift the
 * kernel's scores with odk_brain_evaluate_choice.  With weight 0.0 (or
 * no brain) you get exactly the plain kernel result.
 */
#include <stdio.h>
#include <stdlib.h>

#include "omnidecide.h"

static void show(const char *label, const odk_result *r)
{
    size_t i;

    printf("%-22s -> %s\n", label, r->string);
    for (i = 0; i < r->distribution.count; i++) {
        printf("    %-24s %.4f\n", r->distribution.entries[i].key,
               r->distribution.entries[i].value);
    }
}

int main(void)
{
    const char *question = "How do we clear the jam?";
    const char *options[] = {"open the paper tray", "power cycle the machine",
                             "replace the toner"};
    const char *context =
        "The machine reports a paper jam. Staff usually open the paper "
        "tray first.";
    const char *snapshot = "learning_brain.json";
    odk_kernel *kernel = odk_kernel_create();
    odk_brain *brain = odk_brain_create();
    odk_brain *restored;
    odk_result result;
    odk_error err;
    int i;

    if (kernel == NULL || brain == NULL) {
        return 1;
    }

    /* 1. Plain kernel decision (no experience). */
    if (odk_evaluate_choice(kernel, question, options, 3, context, NULL,
                            &result, &err) != ODK_OK) {
        fprintf(stderr, "error: %s\n", err.text);
        return 1;
    }
    show("kernel only", &result);
    odk_result_release(&result);

    /* 2. Teach the brain: power cycling worked repeatedly, the tray did not. */
    for (i = 0; i < 6; i++) {
        odk_brain_record_outcome(brain, "choice", question,
                                 "power cycle the machine", 1.0, &err);
        odk_brain_record_outcome(brain, "choice", question,
                                 "open the paper tray", -1.0, &err);
    }

    /* 3. Same decision, now shifted by experience (weight 1.0). */
    if (odk_brain_evaluate_choice(brain, kernel, question, options, 3,
                                  context, NULL, 1.0, &result,
                                  &err) != ODK_OK) {
        fprintf(stderr, "error: %s\n", err.text);
        return 1;
    }
    show("kernel + experience", &result);
    odk_result_release(&result);

    /* 4. Persist and restore: snapshots are plain JSON files. */
    if (odk_brain_save_file(brain, snapshot, &err) != ODK_OK) {
        fprintf(stderr, "save failed: %s\n", err.text);
        return 1;
    }
    restored = odk_brain_load_file(snapshot, &err);
    if (restored == NULL) {
        fprintf(stderr, "load failed: %s\n", err.text);
        return 1;
    }
    {
        double bonus[3];

        odk_brain_advise(restored, "choice", question, options, 3, bonus,
                         &err);
        printf("restored bonuses: %.3f %.3f %.3f\n", bonus[0], bonus[1],
               bonus[2]);
    }
    remove(snapshot);

    odk_brain_free(restored);
    odk_brain_free(brain);
    odk_kernel_free(kernel);
    return 0;
}
