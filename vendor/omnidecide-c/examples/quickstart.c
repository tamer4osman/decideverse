/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * quickstart.c - the smallest useful OmniDecide program.
 *
 *   make examples && ./build/quickstart
 *
 * Shows the three ways to call the kernel:
 *   1. a decision primitive directly (choice, noul),
 *   2. the five-key JSON output contract,
 *   3. the client pipeline that takes raw text plus JSON query specs.
 */
#include <stdio.h>
#include <stdlib.h>

#include "omnidecide.h"

static int fail(const char *what, const odk_error *err)
{
    fprintf(stderr, "%s failed: %s\n", what, err->text);
    return 1;
}

int main(void)
{
    /* The kernel scores how well each option is supported by the context
     * using lexical evidence (word overlap), so options should be phrased
     * with the words the context uses.  It does not understand meaning. */
    const char *context =
        "The service stopped responding. "
        "Restarting the service restored normal operation within seconds.";
    const char *options[] = {"restart the service",
                             "reinstall the operating system",
                             "wait for the weekend"};
    odk_kernel *kernel = odk_kernel_create(); /* default: ASCII text mode */
    odk_client *client;
    odk_result result;
    odk_error err;
    char *json = NULL;

    if (kernel == NULL) {
        return 1;
    }

    /* 1a. choice: pick the option best supported by the context. */
    if (odk_evaluate_choice(kernel, "How should we fix the outage?",
                            options, 3, context, NULL, &result,
                            &err) != ODK_OK) {
        return fail("choice", &err);
    }
    printf("choice : %s (confidence %.2f)\n", result.string,
           result.confidence);

    /* 2. every result serialises to the five-key contract. */
    json = odk_result_to_json(&result);
    printf("json   : %s\n", json);
    free(json);
    odk_result_release(&result);

    /* 1b. noul: is a statement supported by the context? */
    if (odk_evaluate_noul(kernel, "Restarting the service restored normal operation.",
                          context, NULL,
                          &result, &err) != ODK_OK) {
        return fail("noul", &err);
    }
    printf("noul   : %s (confidence %.2f)\n",
           result.boolean ? "supported" : "not supported",
           result.confidence);
    odk_result_release(&result);

    /* 3. client pipeline: raw text in, five-key JSON out. */
    client = odk_client_create("auto");
    if (client == NULL) {
        return 1;
    }
    if (odk_client_evaluate_json(
            client, context,
            "{\"fix\":{\"primitive\":\"choice\","
            "\"question\":\"How should we fix the outage?\","
            "\"options\":[\"restart the service\",\"wait for the weekend\"]}}",
            "hybrid", &json, &err) != ODK_OK) {
        return fail("client", &err);
    }
    printf("client : %s\n", json);
    free(json);

    odk_client_free(client);
    odk_kernel_free(kernel);
    return 0;
}
