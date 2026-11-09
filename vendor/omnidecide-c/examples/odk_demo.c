/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_demo.c - small CLI demo for the OmniDecide C library.
 *
 * Usage:
 *   odk_demo                      run a canned multi-primitive evaluation
 *   odk_demo minify  [file]       minify stdin or file and print the result
 *   odk_demo evaluate [file]      run the five-key contract on stdin/file
 *
 */

#include "omnidecide.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *read_all(FILE *fp)
{
    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (buf == NULL) {
        return NULL;
    }
    for (;;) {
        size_t n = fread(buf + len, 1, cap - len - 1, fp);
        len += n;
        buf[len] = '\0';
        if (len + 1 < cap) {
            break;
        }
        cap *= 2;
        {
            char *grown = realloc(buf, cap);
            if (grown == NULL) {
                free(buf);
                return NULL;
            }
            buf = grown;
        }
    }
    return buf;
}

static void print_result(const char *key, const odk_result *result)
{
    char *json = odk_result_to_json(result);
    if (json == NULL) {
        printf("  %s: <allocation failure>\n", key);
        return;
    }
    printf("  %s: %s\n", key, json);
    free(json);
}

static const char *KICKOFF_CONTEXT =
    "PROJECT KICKOFF NOTES\n"
    "The backup server should live in the Berlin datacenter because it has "
    "redundant power and cooling.\n"
    "The server room requires cooling maintenance before the racks are installed.\n"
    "The rollout is partially ready: the network wiring is finished, but the "
    "monitoring agents are still pending.\n"
    "Overall the rollout is partially ready.";

static int run_canned(void)
{
    odk_kernel *kernel = odk_kernel_create();
    odk_result result;
    odk_error err;
    odk_status status;
    const char *options[2] = {"Berlin datacenter", "remote branch office"};
    const char *rubric[3] = {"not ready", "partially ready", "fully ready"};

    if (kernel == NULL) {
        fprintf(stderr, "kernel allocation failed\n");
        return 1;
    }
    printf("backend: %s\n\n", odk_kernel_backend(kernel));

    status = odk_evaluate_choice(kernel, "Which location should host the backup server?",
                                 options, 2, KICKOFF_CONTEXT, NULL, &result, &err);
    if (status == ODK_OK) {
        print_result("choice", &result);
        odk_result_release(&result);
    } else {
        printf("choice failed: %s\n", err.text);
    }

    status = odk_evaluate_score(kernel, "Rate the operational readiness of the rollout.",
                                rubric, 3, KICKOFF_CONTEXT, NULL, &result, &err);
    if (status == ODK_OK) {
        print_result("score", &result);
        odk_result_release(&result);
    } else {
        printf("score failed: %s\n", err.text);
    }

    status = odk_evaluate_noul(kernel,
                               "The Berlin datacenter has redundant power and cooling.",
                               KICKOFF_CONTEXT, NULL, &result, &err);
    if (status == ODK_OK) {
        print_result("noul", &result);
        odk_result_release(&result);
    } else {
        printf("noul failed: %s\n", err.text);
    }

    status = odk_evaluate_rank(kernel, "Order the sites by suitability for the backup server.",
                               options, 2, KICKOFF_CONTEXT, NULL, &result, &err);
    if (status == ODK_OK) {
        print_result("rank", &result);
        odk_result_release(&result);
    } else {
        printf("rank failed: %s\n", err.text);
    }

    status = odk_evaluate_compare(kernel, "Compare the candidate sites.",
                                  options, 2, KICKOFF_CONTEXT, NULL, &result, &err);
    if (status == ODK_OK) {
        print_result("compare", &result);
        odk_result_release(&result);
    } else {
        printf("compare failed: %s\n", err.text);
    }

    status = odk_evaluate_extract(kernel, "What is the state of the rollout?",
                                  KICKOFF_CONTEXT, NULL, 3, &result, &err);
    if (status == ODK_OK) {
        print_result("extract", &result);
        odk_result_release(&result);
    } else {
        printf("extract failed: %s\n", err.text);
    }

    odk_kernel_free(kernel);
    return 0;
}

static int run_evaluate(const char *text)
{
    odk_client *client = odk_client_create(NULL);
    odk_error err;
    char *json = NULL;
    odk_status status;

    if (client == NULL) {
        fprintf(stderr, "client allocation failed\n");
        return 1;
    }
    status = odk_client_evaluate_json(
        client, text,
        "{\"pick\": {\"primitive\": \"choice\", \"instruction\": \"What should we "
        "evaluate?\", \"options\": [\"deploy now\", \"wait for QA\"]}}",
        "hybrid", &json, &err);
    if (status == ODK_OK) {
        printf("%s\n", json);
        free(json);
    } else {
        fprintf(stderr, "evaluate failed: %s\n", err.text);
    }
    odk_client_free(client);
    return status == ODK_OK ? 0 : 1;
}

static int run_minify(const char *text)
{
    char *minified = odk_minify(text);
    if (minified == NULL) {
        fprintf(stderr, "minify allocation failure\n");
        return 1;
    }
    fputs(minified, stdout);
    free(minified);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "minify") == 0) {
        char *text;
        int rc;
        if (argc >= 3) {
            FILE *fp = fopen(argv[2], "rb");
            if (fp == NULL) {
                fprintf(stderr, "cannot open %s\n", argv[2]);
                return 1;
            }
            text = read_all(fp);
            fclose(fp);
        } else {
            text = read_all(stdin);
        }
        if (text == NULL) {
            fprintf(stderr, "read failure\n");
            return 1;
        }
        rc = run_minify(text);
        free(text);
        return rc;
    }
    if (argc >= 2 && strcmp(argv[1], "evaluate") == 0) {
        char *text;
        int rc;
        if (argc >= 3) {
            FILE *fp = fopen(argv[2], "rb");
            if (fp == NULL) {
                fprintf(stderr, "cannot open %s\n", argv[2]);
                return 1;
            }
            text = read_all(fp);
            fclose(fp);
        } else {
            text = read_all(stdin);
        }
        if (text == NULL) {
            fprintf(stderr, "read failure\n");
            return 1;
        }
        rc = run_evaluate(text);
        free(text);
        return rc;
    }
    return run_canned();
}
