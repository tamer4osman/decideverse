/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * model.c - example: load a scoring model file.
 *
 * The built-in default model IS today's behaviour; a loaded file swaps
 * the nine scoring constants (docs/MODEL.md).  Rejections never leave
 * the kernel in a half-loaded state.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "omnidecide.h"

int main(int argc, char **argv)
{
    const char *options[] = {"restart the service", "reboot the database"};
    const char *context =
        "The engineer decided to restart the service and the outage was "
        "fixed immediately.";
    odk_kernel *kernel = odk_kernel_create();
    odk_kernel *plain = odk_kernel_create();
    odk_result r1;
    odk_result r2;
    odk_error err;
    char *j1;
    char *j2;

    if (kernel == NULL || plain == NULL) {
        return 1;
    }
    if (argc > 1) {
        if (odk_kernel_load_model(kernel, argv[1], &err) != ODK_OK) {
            printf("model rejected (%s) - kernel keeps the default\n",
                   err.text);
        } else {
            printf("model '%s' loaded\n", argv[1]);
        }
    }
    memset(&r1, 0, sizeof(r1));
    memset(&r2, 0, sizeof(r2));
    if (odk_evaluate_choice(kernel, "How should we fix the outage?", options,
                            2, context, NULL, &r1, NULL) != ODK_OK ||
        odk_evaluate_choice(plain, "How should we fix the outage?", options,
                            2, context, NULL, &r2, NULL) != ODK_OK) {
        return 1;
    }
    j1 = odk_result_to_json(&r1);
    j2 = odk_result_to_json(&r2);
    printf("loaded model: %s\n", j1 ? j1 : "(null)");
    printf("built-in:     %s\n", j2 ? j2 : "(null)");
    printf("identical:    %s\n",
           j1 && j2 && strcmp(j1, j2) == 0 ? "yes" : "no");
    free(j1);
    free(j2);
    odk_result_release(&r1);
    odk_result_release(&r2);
    odk_kernel_free(kernel);
    odk_kernel_free(plain);
    return 0;
}
