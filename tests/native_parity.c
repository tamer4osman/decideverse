/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * native_parity.c - native envelope runner for parity verification.
 *
 * Reads tests/requests.jsonl, executes every request against the same
 * C interoperability wrapper that the WebAssembly build uses, and
 * writes one line per request to the output file:
 *
 *     id<TAB>status<TAB>envelope
 *
 * Envelopes are produced by odkw_evaluate() itself, so a WASM-side
 * runner that feeds the identical request corpus must produce
 * byte-identical output.  There is no skip path: malformed input is a
 * hard failure (exit 2), a missing allocation is a hard failure, and
 * every request line produces output.
 */

#include "minijson.h"
#include "odk_wasm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LINE_CAP (4 * 1024 * 1024)
#define FIELD_CAP (64 * 1024)
#define CONTEXT_CAP (4 * 1024 * 1024)
#define MAX_OPTS 64
#define MAX_KERNELS 16

static char g_context[CONTEXT_CAP];

typedef struct {
    char mode[32];
    char language[32];
    uint32_t handle;
    int used;
} kernel_slot;

static kernel_slot g_slots[MAX_KERNELS];

static uint32_t kernel_for(const char *mode, const char *language)
{
    size_t i;

    for (i = 0; i < MAX_KERNELS; i++) {
        if (g_slots[i].used && strcmp(g_slots[i].mode, mode) == 0 &&
            strcmp(g_slots[i].language, language) == 0) {
            return g_slots[i].handle;
        }
    }
    for (i = 0; i < MAX_KERNELS; i++) {
        if (!g_slots[i].used) {
            uint32_t h = odkw_kernel_create(mode, language);
            if (h == 0u) {
                return 0u;
            }
            g_slots[i].used = 1;
            snprintf(g_slots[i].mode, sizeof(g_slots[i].mode), "%s", mode);
            snprintf(g_slots[i].language, sizeof(g_slots[i].language), "%s",
                     language);
            g_slots[i].handle = h;
            return h;
        }
    }
    fprintf(stderr, "parity-runner: kernel cache exhausted\n");
    return 0u;
}

static void release_kernels(void)
{
    size_t i;

    for (i = 0; i < MAX_KERNELS; i++) {
        if (g_slots[i].used) {
            odkw_kernel_free(g_slots[i].handle);
            g_slots[i].used = 0;
        }
    }
}

typedef struct {
    long ok;
    long errors; /* requests that correctly produced error envelopes */
    long failed; /* runner-level failures (allocation, malformed)   */
} tally;

static int process_line(const char *line, size_t lineno, FILE *out, tally *t)
{
    mj_slice id_s, kernel_s, request_s, prim_s, instr_s, ctx_s, topk_s;
    mj_slice opts_s, meta_s, caps_s, cap_a, cap_b, cap_c;
    mj_slice mode_s, lang_s;
    char id[128], primitive[32], instruction[FIELD_CAP];
    char mode[32], language[32];
    static char opt_storage[MAX_OPTS][FIELD_CAP];
    const char *opt_ptrs[MAX_OPTS];
    size_t opt_count = 0;
    long top_k = 1, cap_a_v = 0, cap_b_v = 0, cap_c_v = 0;
    int want_meta = 0;
    size_t i;
    uint32_t handle;
    int32_t status;
    char *envelope;

    if (mj_member(line, "id", &id_s) != 1) {
        fprintf(stderr, "parity-runner: line %zu: missing \"id\"\n", lineno);
        return -1;
    }
    if (mj_string(&id_s, id, sizeof(id)) != 0) {
        fprintf(stderr, "parity-runner: line %zu: bad \"id\"\n", lineno);
        return -1;
    }

    if (mj_member(line, "kernel", &kernel_s) != 1) {
        fprintf(stderr, "parity-runner: line %zu (%s): missing \"kernel\"\n",
                lineno, id);
        return -1;
    }
    memset(mode, 0, sizeof(mode));
    memset(language, 0, sizeof(language));
    if (mj_member(kernel_s.start, "mode", &mode_s) == 1) {
        if (mj_string(&mode_s, mode, sizeof(mode)) != 0) {
            fprintf(stderr, "parity-runner: line %zu (%s): bad kernel.mode\n",
                    lineno, id);
            return -1;
        }
    }
    if (mj_member(kernel_s.start, "language", &lang_s) == 1) {
        if (mj_string(&lang_s, language, sizeof(language)) != 0) {
            fprintf(stderr,
                    "parity-runner: line %zu (%s): bad kernel.language\n",
                    lineno, id);
            return -1;
        }
    }
    if (mode[0] == '\0') {
        snprintf(mode, sizeof(mode), "%s", ODWK_MODE_ASCII);
    }
    if (language[0] == '\0') {
        snprintf(language, sizeof(language), "und");
    }

    if (mj_member(line, "request", &request_s) != 1) {
        fprintf(stderr, "parity-runner: line %zu (%s): missing \"request\"\n",
                lineno, id);
        return -1;
    }
    if (mj_member(request_s.start, "primitive", &prim_s) != 1 ||
        mj_string(&prim_s, primitive, sizeof(primitive)) != 0) {
        fprintf(stderr, "parity-runner: line %zu (%s): bad \"primitive\"\n",
                lineno, id);
        return -1;
    }
    if (mj_member(request_s.start, "instruction", &instr_s) == 1) {
        if (mj_string(&instr_s, instruction, sizeof(instruction)) != 0) {
            fprintf(stderr,
                    "parity-runner: line %zu (%s): bad \"instruction\"\n",
                    lineno, id);
            return -1;
        }
    } else {
        instruction[0] = '\0';
    }
    if (mj_member(request_s.start, "context", &ctx_s) == 1) {
        if (mj_string(&ctx_s, g_context, CONTEXT_CAP) != 0) {
            fprintf(stderr, "parity-runner: line %zu (%s): bad \"context\"\n",
                    lineno, id);
            return -1;
        }
    } else {
        g_context[0] = '\0';
    }
    top_k = 1;
    if (mj_member(request_s.start, "topK", &topk_s) == 1) {
        if (mj_int(&topk_s, &top_k) != 0) {
            fprintf(stderr, "parity-runner: line %zu (%s): bad \"topK\"\n",
                    lineno, id);
            return -1;
        }
    }
    if (mj_member(request_s.start, "meta", &meta_s) == 1) {
        if (mj_bool(&meta_s, &want_meta) != 0) {
            fprintf(stderr, "parity-runner: line %zu (%s): bad \"meta\"\n",
                    lineno, id);
            return -1;
        }
    }

    /* options: for score the rubric feeds the same wrapper parameter. */
    opts_s.start = NULL;
    opts_s.len = 0;
    if (strcmp(primitive, "score") == 0) {
        if (mj_member(request_s.start, "rubric", &opts_s) != 1) {
            fprintf(stderr,
                    "parity-runner: line %zu (%s): score requires \"rubric\"\n",
                    lineno, id);
            return -1;
        }
    } else if (mj_member(request_s.start, "options", &opts_s) != 1) {
        /* noul/extract legitimately omit options; choice-family
         * primitives fail downstream with a deterministic error
         * envelope, which is exactly what the corpus wants. */
        opts_s.start = NULL;
        opts_s.len = 0;
    }
    if (opts_s.start != NULL) {
        if (mj_array_len(&opts_s, &opt_count) != 0 || opt_count > MAX_OPTS) {
            fprintf(stderr,
                    "parity-runner: line %zu (%s): bad options array\n", lineno,
                    id);
            return -1;
        }
        for (i = 0; i < opt_count; i++) {
            if (mj_array_string(&opts_s, i, opt_storage[i], FIELD_CAP) != 0) {
                fprintf(stderr,
                        "parity-runner: line %zu (%s): bad option %zu\n", lineno,
                        id, i);
                return -1;
            }
            opt_ptrs[i] = opt_storage[i];
        }
    }

    if (mj_member(request_s.start, "caps", &caps_s) == 1) {
        if (mj_member(caps_s.start, "maxRanked", &cap_a) == 1) {
            if (mj_int(&cap_a, &cap_a_v) != 0) {
                fprintf(stderr,
                        "parity-runner: line %zu (%s): bad caps.maxRanked\n",
                        lineno, id);
                return -1;
            }
        }
        if (mj_member(caps_s.start, "maxSnippetChars", &cap_b) == 1) {
            if (mj_int(&cap_b, &cap_b_v) != 0) {
                fprintf(stderr,
                        "parity-runner: line %zu (%s): bad caps.maxSnippetChars\n",
                        lineno, id);
                return -1;
            }
        }
        if (mj_member(caps_s.start, "maxTotalChars", &cap_c) == 1) {
            if (mj_int(&cap_c, &cap_c_v) != 0) {
                fprintf(stderr,
                        "parity-runner: line %zu (%s): bad caps.maxTotalChars\n",
                        lineno, id);
                return -1;
            }
        }
    }

    handle = kernel_for(mode, language);
    if (handle == 0u) {
        /* Bad kernel config (e.g. unknown mode or language): the case
         * itself expects a deterministic error envelope.  Emit it via
         * the shared wrapper helper so the JS harness can reproduce the
         * exact same bytes - no silent skips either way. */
        envelope = odkw_last_error_envelope();
        if (envelope == NULL) {
            fprintf(stderr,
                    "parity-runner: line %zu (%s): error envelope allocation failed\n",
                    lineno, id);
            t->failed++;
            return -1;
        }
        status = odkw_last_status();
        fprintf(out, "%s\t%d\t%s\n", id, (int)status, envelope);
        t->errors++;
        odkw_free(envelope);
        return 0;
    }
    envelope = odkw_evaluate(handle, primitive, instruction,
                             opt_count > 0 ? opt_ptrs : NULL, opt_count,
                             g_context, (int32_t)top_k, (int32_t)cap_a_v,
                             (int32_t)cap_b_v, (int32_t)cap_c_v,
                             want_meta ? ODWK_FLAG_META : 0u);
    if (envelope == NULL) {
        fprintf(stderr,
                "parity-runner: line %zu (%s): envelope allocation failed\n",
                lineno, id);
        t->failed++;
        return -1;
    }
    status = odkw_last_status();
    fprintf(out, "%s\t%d\t%s\n", id, (int)status, envelope);
    if (status == (int32_t)ODK_OK) {
        t->ok++;
    } else {
        t->errors++;
    }
    odkw_free(envelope);
    return 0;
}

int main(int argc, char **argv)
{
    const char *requests_path;
    const char *output_path;
    FILE *in, *out;
    char *line = NULL;
    size_t line_cap = 0;
    size_t lineno = 0;
    tally t = {0, 0, 0};

    if (argc != 3) {
        fprintf(stderr,
                "usage: odk_parity_runner <requests.jsonl> <output.tsv>\n");
        return 2;
    }
    requests_path = argv[1];
    output_path = argv[2];

    in = fopen(requests_path, "rb");
    if (in == NULL) {
        fprintf(stderr, "parity-runner: cannot open %s\n", requests_path);
        return 2;
    }
    out = fopen(output_path, "wb");
    if (out == NULL) {
        fprintf(stderr, "parity-runner: cannot create %s\n", output_path);
        fclose(in);
        return 2;
    }

    line_cap = LINE_CAP;
    line = (char *)malloc(line_cap);
    if (line == NULL) {
        fprintf(stderr, "parity-runner: out of memory\n");
        fclose(in);
        fclose(out);
        return 2;
    }

    while (fgets(line, (int)line_cap, in) != NULL) {
        size_t len = strlen(line);
        lineno++;
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0) {
            continue; /* blank separator lines are not test cases */
        }
        if (process_line(line, lineno, out, &t) != 0) {
            release_kernels();
            free(line);
            fclose(in);
            fclose(out);
            fprintf(stderr,
                    "parity-runner: FAILED at line %zu (no silent skips)\n",
                    lineno);
            return 2;
        }
    }

    release_kernels();
    free(line);
    fclose(in);
    fclose(out);

    fprintf(stderr,
            "parity-runner: %ld success envelopes, %ld error envelopes, "
            "%ld runner failures (native host)\n",
            t.ok, t.errors, t.failed);
    return t.failed > 0 ? 2 : 0;
}
