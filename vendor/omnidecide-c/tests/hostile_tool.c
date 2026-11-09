/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * hostile_tool.c - hostile-input robustness runner.
 *
 * Feeds fixed-LCG byte blobs, invalid UTF-8, deep JSON, corrupt snapshots
 * and corrupt lexicon/stopword files through every public entry point.
 * Deterministic (fixed LCG seed, no clock, no env).  Must exit 0; run it
 * under ASan/UBSan/LSan.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "omnidecide.h"

static uint64_t g_lcg = 0x9E3779B97F4A7C15ULL;
static unsigned char lcg_byte(void)
{
    g_lcg = g_lcg * 6364136223846793005ULL + 1442695040888963407ULL;
    return (unsigned char)(g_lcg >> 33);
}

static char *blob(size_t n)
{
    char *s = malloc(n + 1);
    size_t i;
    if (s == NULL) {
        exit(1);
    }
    for (i = 0; i < n; i++) {
        unsigned char b = lcg_byte();
        s[i] = b ? (char)b : 'A';
    }
    s[n] = '\0';
    return s;
}

static const char *const INVALID[] = {
    "\xFF", "\xC3\x28", "\xE2\x82", "\xF0\x9F\x98", "\xC0\x80",
    "\xF4\x90\x80\x80", "\xED\xA0\x80", "text \x80\xFE more",
    "\xE0\x80\x80", "\xF5\x80\x80\x80 end"};
#define INVALID_N (sizeof(INVALID) / sizeof(INVALID[0]))

static int g_checks = 0;
static int g_failures_hint = 0; /* model-fuzz decision divergence */
static const char *const opts_null[1] = {"x"};

static void expect_status(odk_status st, const char *what)
{
    /* anything that is not a crash is fine: errors are expected here */
    (void)st;
    (void)what;
    g_checks++;
}

static void run_all_prims(odk_kernel *k, const char *instr, char **opts,
                          size_t nopts, const char *stmt, const char *ctx,
                          const char *rubric[], size_t nrub)
{
    odk_result out;
    odk_error err;
    odk_result_meta meta;
    odk_output_caps caps;

    memset(&out, 0, sizeof(out));
    memset(&err, 0, sizeof(err));
    expect_status(odk_evaluate_choice(k, instr, (const char *const *)opts,
                                      nopts, ctx, NULL, &out, &err),
                  "choice");
    odk_result_release(&out);
    memset(&out, 0, sizeof(out));
    memset(&err, 0, sizeof(err));
    expect_status(odk_evaluate_score(k, instr,
                                     (const char *const *)rubric, nrub, ctx,
                                     NULL, &out, &err),
                  "score");
    odk_result_release(&out);
    memset(&out, 0, sizeof(out));
    memset(&err, 0, sizeof(err));
    expect_status(odk_evaluate_noul(k, stmt, ctx, NULL, &out, &err), "noul");
    odk_result_release(&out);
    memset(&out, 0, sizeof(out));
    memset(&err, 0, sizeof(err));
    expect_status(odk_evaluate_rank(k, instr, (const char *const *)opts,
                                    nopts, ctx, NULL, &out, &err),
                  "rank");
    odk_result_release(&out);
    memset(&out, 0, sizeof(out));
    memset(&err, 0, sizeof(err));
    expect_status(odk_evaluate_compare(k, instr,
                                       (const char *const *)opts,
                                       nopts > 2 ? 2 : nopts, ctx, NULL,
                                       &out, &err),
                  "compare");
    odk_result_release(&out);
    memset(&out, 0, sizeof(out));
    memset(&err, 0, sizeof(err));
    expect_status(odk_evaluate_extract(k, instr, ctx, NULL, 5, &out, &err),
                  "extract");
    odk_result_release(&out);
    /* opts variant with caps + meta */
    memset(&out, 0, sizeof(out));
    memset(&err, 0, sizeof(err));
    memset(&meta, 0, sizeof(meta));
    memset(&caps, 0, sizeof(caps));
    caps.max_ranked = 1;
    caps.max_snippet_chars = 4;
    caps.max_total_chars = 8;
    expect_status(odk_evaluate_choice_opts(k, instr,
                                           (const char *const *)opts, nopts,
                                           ctx, NULL, &caps, &out, &meta,
                                           &err),
                  "choice_opts");
    odk_result_release(&out);
    {
        odk_ranked_item *items = NULL;
        size_t ni = odk_result_rank_items(&out, (const char *const *)opts,
                                          nopts, &items);
        odk_result_rank_items_free(items);
        (void)ni;
        g_checks++;
    }
}

static void write_file(const char *path, const char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    if (len > 0) {
        fwrite(data, 1, len, f);
    }
    fclose(f);
}

int main(void)
{
    odk_kernel *k = odk_kernel_create();
    int round;
    size_t v;
    char tmp_path[64];
    FILE *tf;

    if (k == NULL) {
        return 1;
    }
    tf = tmpfile();
    (void)tf; /* temp files are created below with distinct names */

    /* 1. LCG blobs of many sizes through every primitive */
    for (round = 0; round < 30; round++) {
        char *ctx = blob((size_t)(round + 1) * 137);
        char *instr = blob((size_t)(round % 7) + 1);
        char *stmt = blob((size_t)(round % 5) + 1);
        char *o0 = blob(6);
        char *o1 = blob(9);
        const char *opts[2] = {o0, o1};
        const char *rubric[3] = {"low", "high", blob(4)};
        run_all_prims(k, instr, (char **)opts, 2, stmt, ctx, rubric, 3);
        free(ctx);
        free(instr);
        free(stmt);
        free(o0);
        free(o1);
        free((void *)rubric[2]);
    }

    /* 2. invalid UTF-8 through every slot */
    for (v = 0; v < INVALID_N; v++) {
        const char *opts[2] = {INVALID[v], "ok option"};
        const char *rubric[2] = {INVALID[v], "high"};
        run_all_prims(k, INVALID[v], (char **)opts, 2, INVALID[v],
                      INVALID[v], rubric, 2);
    }

    /* 3. client pipeline with hostile text + hostile queries JSON */
    {
        odk_client *client = odk_client_create("cpu");
        if (client != NULL) {
            for (round = 0; round < 20; round++) {
                char *text = blob((size_t)(round + 1) * 53);
                char *qjson = blob((size_t)(round % 11) + 1);
                char *out_json = NULL;
                odk_error err;
                memset(&err, 0, sizeof(err));
                expect_status(odk_client_evaluate_json_ex(
                                  client, text, qjson, "hybrid", NULL,
                                  &out_json, &err),
                              "client hostile");
                free(out_json);
                free(text);
                free(qjson);
            }
            {
                /* deep nesting: 1000 levels (parser cap is 64) */
                size_t n = 1000;
                char *deep = malloc(2 * n + 2);
                char *out_json = NULL;
                odk_error err;
                size_t i;
                if (deep != NULL) {
                    for (i = 0; i < n; i++) {
                        deep[i] = '[';
                        deep[n + 1 + i] = ']';
                    }
                    deep[n] = 'x';
                    deep[2 * n + 1] = '\0';
                    memset(&err, 0, sizeof(err));
                    expect_status(odk_client_evaluate_json_ex(
                                      client, deep, "{}", "hybrid", NULL,
                                      &out_json, &err),
                                  "client deep text");
                    free(out_json);
                    memset(&err, 0, sizeof(err));
                    expect_status(odk_client_evaluate_json_ex(
                                      client, "text", deep, "hybrid", NULL,
                                      &out_json, &err),
                                  "client deep queries");
                    free(out_json);
                    free(deep);
                }
            }
            odk_client_free(client);
        }
    }

    /* 4. corrupt brain snapshots */
    snprintf(tmp_path, sizeof(tmp_path), "/tmp/odk_hostile_snap.json");
    {
        odk_brain *good = odk_brain_create();
        odk_error err;
        size_t t;
        if (good != NULL) {
            memset(&err, 0, sizeof(err));
            (void)odk_brain_record_outcome(good, "choice", "instr", "opt",
                                           1.0, &err);
            (void)odk_brain_save_file(good, tmp_path, &err);
            odk_brain_free(good);
        }
        {
            FILE *f = fopen(tmp_path, "rb");
            if (f != NULL) {
                static char snap[1 << 20];
                size_t len = fread(snap, 1, sizeof(snap), f);
                fclose(f);
                for (t = 0; t <= len; t += (len / 40 > 0 ? len / 40 : 1)) {
                    char *cut = malloc(t + 1);
                    odk_brain *b;
                    if (cut == NULL) {
                        break;
                    }
                    memcpy(cut, snap, t);
                    cut[t] = '\0';
                    write_file(tmp_path, cut, t);
                    b = odk_brain_load_file(tmp_path, NULL);
                    if (b != NULL) {
                        odk_brain_free(b);
                    }
                    free(cut);
                    g_checks++;
                }
                /* fully random snapshot */
                for (t = 0; t < 10; t++) {
                    char *rnd = blob((size_t)(t + 1) * 211);
                    odk_brain *b;
                    write_file(tmp_path, rnd, strlen(rnd));
                    b = odk_brain_load_file(tmp_path, NULL);
                    if (b != NULL) {
                        odk_brain_free(b);
                    }
                    free(rnd);
                    g_checks++;
                }
            }
        }
    }

    /* 5. corrupt stopword / lexicon JSON files */
    {
        static const char *const BAD_JSON[] = {
            "", "{", "{\"stopwords\":", "{\"stopwords\":[\"a\"",
            "{\"stopwords\":[1,2]}", "{\"stopwords\":[\"\\uD800\"]}",
            "{\"concepts\":{\"x\":\"noarray\"}}",
            "{\"stopwords\":[\"a\"],\"garbage\":}",
            "{\"words\":[\"\"]}", "null", "[]", "{\"stopwords\":\"x\"}",
            "{\"stopwords\":[\"a\"",
            ("{\"a\":[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[["
             "[[[[[[[[[[[[[[[[[[[[[]]}")};
        size_t t;
        for (t = 0; t < sizeof(BAD_JSON) / sizeof(BAD_JSON[0]); t++) {
            odk_error err;
            write_file(tmp_path, BAD_JSON[t], strlen(BAD_JSON[t]));
            expect_status(odk_kernel_load_stopwords(k, tmp_path, &err),
                          "load_stopwords hostile");
            memset(&err, 0, sizeof(err));
            expect_status(odk_kernel_load_lexicon(k, tmp_path, &err),
                          "load_lexicon hostile");
            g_checks++;
        }
        /* nonexistent path */
        expect_status(odk_kernel_load_stopwords(k, "/tmp/does_not_exist_zz",
                                                NULL),
                      "load_stopwords missing");
    }

    /* 6. extreme arguments through the public API */
    {
        odk_error err;
        odk_query q;
        const char *opts[1] = {"x"};
        memset(&err, 0, sizeof(err));
        expect_status(odk_query_from_spec("k", "weird", "i", "s", opts, 1,
                                          NULL, 0, 1, &q, &err),
                      "bad primitive spec");
        expect_status(odk_query_from_spec("k", "rank", "i", "s", opts, 1,
                                          NULL, 0, 1, &q, &err),
                      "rank arity");
        {
            char *big = blob(100000); /* huge instruction */
            odk_result out;
            memset(&err, 0, sizeof(err));
            memset(&out, 0, sizeof(out));
            expect_status(odk_evaluate_choice(k, big, (const char *const *)opts,
                                              1, big, NULL, &out, &err),
                          "huge strings");
            odk_result_release(&out);
            free(big);
        }
        {
            odk_result out;
            char *json;
            memset(&out, 0, sizeof(out)); /* zeroed result -> serializer */
            json = odk_result_to_json(&out);
            free(json);
            g_checks++;
        }
        {
            char buf[64];
            odk_image im;
            unsigned char rgb[3 * 2 * 2] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
                                            11, 12};
            im.width = 2;
            im.height = 2;
            im.rgb = rgb;
            expect_status(odk_describe_image(&im, buf, sizeof(buf), &err),
                          "describe_image tiny");
            im.width = 0; /* invalid */
            expect_status(odk_describe_image(&im, buf, sizeof(buf), &err),
                          "describe_image invalid");
            im.rgb = NULL;
            expect_status(odk_describe_image(&im, buf, 0, &err),
                          "describe_image no buf");
        }
    }

    /* 7. explain API with hostile inputs */
    {
        for (round = 0; round < 12; round++) {
            char *ctx = blob((size_t)(round + 1) * 97);
            char *instr = blob((size_t)(round % 5) + 1);
            char *stmt = blob((size_t)(round % 3) + 1);
            char *o0 = blob(5);
            char *o1 = blob(7);
            const char *opts[2] = {o0, o1};
            const char *rubric[2] = {"low", blob(6)};
            odk_explain ex;
            odk_error err;
            memset(&ex, 0, sizeof(ex));
            memset(&err, 0, sizeof(err));
            expect_status(odk_explain_choice(k, instr,
                                             (const char *const *)opts, 2,
                                             ctx, NULL, &ex, &err),
                          "explain choice");
            odk_explain_release(&ex);
            memset(&ex, 0, sizeof(ex));
            memset(&err, 0, sizeof(err));
            expect_status(odk_explain_score(k, instr,
                                            (const char *const *)rubric, 2,
                                            ctx, NULL, &ex, &err),
                          "explain score");
            odk_explain_release(&ex);
            memset(&ex, 0, sizeof(ex));
            memset(&err, 0, sizeof(err));
            expect_status(odk_explain_noul(k, stmt, ctx, NULL, &ex, &err),
                          "explain noul");
            {
                char *json = odk_explain_to_json(&ex);
                free(json);
            }
            odk_explain_release(&ex);
            free(ctx);
            free(instr);
            free(stmt);
            free(o0);
            free(o1);
            free((void *)rubric[1]);
            g_checks++;
        }
        {
            /* explain with zeroed kernel-side edge: NULL kernel */
            odk_explain ex;
            odk_error err;
            memset(&ex, 0, sizeof(ex));
            memset(&err, 0, sizeof(err));
            expect_status(odk_explain_choice(NULL, "i",
                                             (const char *const *)opts_null,
                                             1, "c", NULL, &ex, &err),
                          "explain NULL kernel");
        }
    }

    /* 8. model file fuzzing (docs/MODEL.md) */
    {
        const char *model_path = "/tmp/odk_hostile_model.odkmodel";
        odk_kernel *mk = odk_kernel_create();
        if (mk != NULL) {
            odk_error err;
            size_t t;
            /* seed with the valid fixture, then mutate */
            {
                FILE *src = fopen("tests/fixtures/default_model.odkmodel",
                                  "rb");
                static unsigned char base[128];
                size_t blen = 0;
                if (src != NULL) {
                    blen = fread(base, 1, sizeof(base), src);
                    fclose(src);
                }
                if (blen == 128) {
                    static unsigned char mut[140];
                    for (t = 0; t < 128; t++) {
                        size_t cut;
                        for (cut = 0; cut <= 128; cut += 8) {
                            memcpy(mut, base, cut);
                            write_file(model_path, (const char *)mut, cut);
                            memset(&err, 0, sizeof(err));
                            expect_status(odk_kernel_load_model(
                                              mk, model_path, &err),
                                          "model truncated");
                            g_checks++;
                        }
                        /* single-bit flips */
                        memcpy(mut, base, 128);
                        mut[t] ^= 0x01;
                        write_file(model_path, (const char *)mut, 128);
                        memset(&err, 0, sizeof(err));
                        expect_status(
                            odk_kernel_load_model(mk, model_path, &err),
                            "model bit flip");
                        g_checks++;
                        mut[t] ^= 0x80;
                        write_file(model_path, (const char *)mut, 128);
                        memset(&err, 0, sizeof(err));
                        expect_status(
                            odk_kernel_load_model(mk, model_path, &err),
                            "model bit flip high");
                        g_checks++;
                    }
                    /* trailing garbage (oversized) */
                    memcpy(mut, base, 128);
                    mut[128] = 'x';
                    write_file(model_path, (const char *)mut, 129);
                    memset(&err, 0, sizeof(err));
                    expect_status(odk_kernel_load_model(mk, model_path, &err),
                                  "model oversized");
                    g_checks++;
                    /* all-zero and all-FF files */
                    memset(mut, 0, 128);
                    write_file(model_path, (const char *)mut, 128);
                    expect_status(odk_kernel_load_model(mk, model_path, NULL),
                                  "model zeros");
                    g_checks++;
                    memset(mut, 0xFF, 128);
                    write_file(model_path, (const char *)mut, 128);
                    expect_status(odk_kernel_load_model(mk, model_path, NULL),
                                  "model 0xFF");
                    g_checks++;
                    /* LCG random files */
                    for (t = 0; t < 40; t++) {
                        size_t k2;
                        for (k2 = 0; k2 < 128; k2++) {
                            mut[k2] = lcg_byte();
                        }
                        write_file(model_path, (const char *)mut, 128);
                        expect_status(
                            odk_kernel_load_model(mk, model_path, NULL),
                            "model random");
                        g_checks++;
                    }
                }
            }
            remove(model_path);
            /* decisions after all the hostile loads must still be
             * identical to the pristine kernel (model unchanged on
             * failure) */
            {
                odk_result r1;
                odk_result r2;
                const char *opts[2] = {"annual plan", "monthly plan"};
                char *j1;
                char *j2;
                memset(&r1, 0, sizeof(r1));
                memset(&r2, 0, sizeof(r2));
                (void)odk_evaluate_choice(mk, "Which plan?", opts, 2,
                                          "The board approved the annual "
                                          "plan after review.",
                                          NULL, &r1, NULL);
                {
                    odk_kernel *pristine = odk_kernel_create();
                    (void)odk_evaluate_choice(pristine, "Which plan?", opts,
                                              2,
                                              "The board approved the "
                                              "annual plan after review.",
                                              NULL, &r2, NULL);
                    j1 = odk_result_to_json(&r1);
                    j2 = odk_result_to_json(&r2);
                    if (j1 == NULL || j2 == NULL || strcmp(j1, j2) != 0) {
                        __sync_fetch_and_add(&g_failures_hint, 1);
                    }
                    free(j1);
                    free(j2);
                    odk_result_release(&r1);
                    odk_result_release(&r2);
                    odk_kernel_free(pristine);
                }
            }
            odk_kernel_free(mk);
        }
    }

    remove(tmp_path);
    odk_kernel_free(k);
    printf("hostile: survived %d hostile checks (fixed-LCG, replayable)\n",
           g_checks);
    if (g_failures_hint != 0) {
        printf("hostile: FAILED - %d decision divergences after hostile "
               "model loads\n",
               g_failures_hint);
        return 1;
    }
    return 0;
}
