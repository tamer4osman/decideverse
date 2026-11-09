/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * stress_test.c - high-pressure stress suite for the OmniDecide v2.0.0
 * kernel and spine.  Complements (does not duplicate) the other tools:
 *
 *   hostile_tool   one-shot hostile-input robustness (no-crash focus)
 *   thread_probe   TSan probe, choice-only, 8 threads x 500 calls
 *   stress_test    this file: endurance, invariants under load, and
 *                  cross-kernel byte determinism at scale
 *
 * Sections (all deterministic: fixed splitmix64 seeds, no wall clock,
 * no env, no randomness outside the seeds; output is byte-stable except
 * the S6 maxrss line, which reports host memory accounting):
 *
 *   S1  contract boundary abuse: extreme-but-legal inputs and the
 *       documented error paths, at scale (1024-option ranks, top_k
 *       extremes, duplicates, empty strings, NULL-kernel calls)
 *   S2  seeded fuzz marathon: valid-input cases through all six
 *       primitives on FOUR kernels (two default, two model-loaded);
 *       every call must succeed and pairwise kernel transcripts must
 *       be byte-identical; plus a garbage rotation with double-call
 *       byte-equality on whatever succeeds
 *   S3  truncation / caps / work-budget boundary sweep: context lengths
 *       0..context_max+200 with meta-accounting invariants, and the
 *       caps-never-touch-the-numbers guarantee (confidence, probability
 *       and distribution bit-identical under every cap)
 *   S4  concurrency storm: 8 threads x all six primitives against ONE
 *       shared kernel, then 8 threads each on its OWN kernel, both
 *       byte-identical to the single-threaded baseline
 *   S5  spine endurance: 1,600 classifications with append-only lineage
 *       and as-of integrity, decomposition (valid + invalid params),
 *       600 pins with exact-verdict replays, 1,200 decision/outcome
 *       observation pairs with byte-identical calibration reports, and
 *       versioned re-registration (append, never overwrite)
 *   S6  lifecycle churn + RSS soak: repeated create/load/evaluate/free
 *       of kernels, brains, clients and spines with a maxrss growth
 *       watch (LeakSanitizer under `make stress-asan` is the authority
 *       on leaks; this watch catches gross growth only)
 *
 * Must exit 0.  Run natively (`make stress`) and under the sanitizers
 * (`make stress-asan`).
 */
#define _POSIX_C_SOURCE 200809L

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#include "omnidecide.h"
#include "odk_spine.h"

/* ------------------------------------------------------------------ */
/* Deterministic PRNG (splitmix64), hashing, check accounting          */
/* ------------------------------------------------------------------ */

static uint64_t sm_next(uint64_t *s)
{
    uint64_t z = (*s += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static uint32_t sm_below(uint64_t *s, uint32_t n)
{
    return (uint32_t)(sm_next(s) % n); /* bias irrelevant for stress */
}

static uint64_t fnv1a(const char *s, uint64_t h)
{
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        h ^= *p++;
        h *= 0x100000001B3ULL;
    }
    h ^= 0xFF;
    h *= 0x100000001B3ULL;
    return h;
}

static int g_checks = 0;
static int g_failures = 0;
static int g_reported = 0;

static void check(int ok, const char *what)
{
    g_checks++;
    if (!ok) {
        g_failures++;
        if (g_reported++ < 25) {
            printf("stress: FAIL: %s\n", what);
        }
    }
}

static void checkf(int ok, const char *what, long detail)
{
    g_checks++;
    if (!ok) {
        g_failures++;
        if (g_reported++ < 25) {
            printf("stress: FAIL: %s (%ld)\n", what, detail);
        }
    }
}

/* A call that must merely survive (any status is acceptable); the only
 * way to fail it is to crash the process. */
static void survived(void)
{
    g_checks++;
}

static long rss_kb(void)
{
    struct rusage ru;
    memset(&ru, 0, sizeof(ru));
    if (getrusage(RUSAGE_SELF, &ru) != 0) {
        return -1;
    }
    return (long)ru.ru_maxrss; /* Linux: kilobytes */
}

/* ------------------------------------------------------------------ */
/* Text generation (neutral, application-agnostic word bank)           */
/* ------------------------------------------------------------------ */

static const char *const WORDS[] = {
    "alpha", "bravo", "delta", "echo",  "kilo",  "lima",  "nova",
    "orbit", "pulse", "quota", "relay", "sigma", "tango", "unity",
    "vector", "wander", "xenon", "yield", "zonal", "argo",  "berm",
    "cedar", "dune",  "ember", "frost", "grove", "harbor", "ingot"};
#define WORDS_N (sizeof(WORDS) / sizeof(WORDS[0]))

static char *gen_text(uint64_t *s, size_t n_words)
{
    size_t cap = n_words * 8 + 16;
    char *out = malloc(cap);
    size_t i;
    if (out == NULL) {
        exit(1);
    }
    out[0] = '\0';
    for (i = 0; i < n_words; i++) {
        if (i > 0) {
            strcat(out, " ");
        }
        strcat(out, WORDS[sm_below(s, WORDS_N)]);
    }
    return out;
}

/* Append an invalid-UTF-8 fragment to a valid base (S2b only). */
static char *poison_utf8(const char *base, uint64_t *s)
{
    static const char *const BAD[] = {
        "\xFF", "\xC3\x28", "\xE2\x82", "\xF0\x9F\x98", "\xC0\x80",
        "\xED\xA0\x80", "\xF4\x90\x80\x80", " \x80\xFE "};
    size_t n = sizeof(BAD) / sizeof(BAD[0]);
    const char *frag = BAD[sm_below(s, n)];
    size_t len = strlen(base) + strlen(frag) + 1;
    char *out = malloc(len);
    if (out == NULL) {
        exit(1);
    }
    strcpy(out, base);
    strcat(out, frag);
    return out;
}

/* ------------------------------------------------------------------ */
/* Workload slots: one slot drives all six primitives                  */
/* ------------------------------------------------------------------ */

typedef struct {
    char *instruction;
    char *context;
    char *stmt;
    char *opts[6];
    size_t nopts;
    char *rubric[4];
    size_t nrub;
    int top_k;
} slot_t;

static void slot_build(slot_t *sl, uint64_t *s)
{
    uint32_t i;
    memset(sl, 0, sizeof(*sl));
    sl->instruction = gen_text(s, 2 + sm_below(s, 5));
    sl->context = gen_text(s, 8 + sm_below(s, 40));
    sl->stmt = gen_text(s, 3 + sm_below(s, 6));
    sl->nopts = 2 + sm_below(s, 5); /* >= 2 so compare is always legal */
    for (i = 0; i < sl->nopts; i++) {
        sl->opts[i] = gen_text(s, 1 + sm_below(s, 3));
    }
    sl->nrub = 2 + sm_below(s, 3);
    for (i = 0; i < sl->nrub; i++) {
        sl->rubric[i] = gen_text(s, 1 + sm_below(s, 2));
    }
    sl->top_k = 1 + (int)sm_below(s, 5);
}

static void slot_free(slot_t *sl)
{
    uint32_t i;
    for (i = 0; i < sl->nopts; i++) {
        free(sl->opts[i]);
    }
    for (i = 0; i < sl->nrub; i++) {
        free(sl->rubric[i]);
    }
    free(sl->instruction);
    free(sl->context);
    free(sl->stmt);
}

/* Run all six primitives for one slot; store the six JSON strings
 * (NULL when a call failed).  Caller frees. */
static void slot_eval(odk_kernel *k, const slot_t *sl, char *out[6])
{
    odk_result r;
    odk_error err;
    int i;
    for (i = 0; i < 6; i++) {
        out[i] = NULL;
    }
    memset(&r, 0, sizeof(r));
    memset(&err, 0, sizeof(err));
    if (odk_evaluate_choice(k, sl->instruction,
                            (const char *const *)sl->opts, sl->nopts,
                            sl->context, NULL, &r, &err) == ODK_OK) {
        out[0] = odk_result_to_json(&r);
    }
    odk_result_release(&r);
    memset(&r, 0, sizeof(r));
    memset(&err, 0, sizeof(err));
    if (odk_evaluate_score(k, sl->instruction,
                           (const char *const *)sl->rubric, sl->nrub,
                           sl->context, NULL, &r, &err) == ODK_OK) {
        out[1] = odk_result_to_json(&r);
    }
    odk_result_release(&r);
    memset(&r, 0, sizeof(r));
    memset(&err, 0, sizeof(err));
    if (odk_evaluate_noul(k, sl->stmt, sl->context, NULL, &r, &err) ==
        ODK_OK) {
        out[2] = odk_result_to_json(&r);
    }
    odk_result_release(&r);
    memset(&r, 0, sizeof(r));
    memset(&err, 0, sizeof(err));
    if (odk_evaluate_rank(k, sl->instruction,
                          (const char *const *)sl->opts, sl->nopts,
                          sl->context, NULL, &r, &err) == ODK_OK) {
        out[3] = odk_result_to_json(&r);
    }
    odk_result_release(&r);
    memset(&r, 0, sizeof(r));
    memset(&err, 0, sizeof(err));
    if (odk_evaluate_compare(k, sl->instruction,
                             (const char *const *)sl->opts,
                             sl->nopts >= 2 ? 2 : sl->nopts, sl->context,
                             NULL, &r, &err) == ODK_OK) {
        out[4] = odk_result_to_json(&r);
    }
    odk_result_release(&r);
    memset(&r, 0, sizeof(r));
    memset(&err, 0, sizeof(err));
    if (odk_evaluate_extract(k, sl->instruction, sl->context, NULL,
                             sl->top_k, &r, &err) == ODK_OK) {
        out[5] = odk_result_to_json(&r);
    }
    odk_result_release(&r);
}

static void six_free(char *out[6])
{
    int i;
    for (i = 0; i < 6; i++) {
        free(out[i]);
        out[i] = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* S1 - contract boundary abuse                                        */
/* ------------------------------------------------------------------ */

static void s1_boundary(void)
{
    odk_kernel *k = odk_kernel_create();
    odk_error err;
    odk_result r;
    int ok;
    const char *one[1] = {"go"};
    size_t i;

    /* 1024 distinct options: rank must yield a permutation of indices */
    {
        static const char *opts[1024];
        static char buf[1024][12];
        odk_ranked_item *items = NULL;
        static int seen[1024];
        odk_result_meta meta;
        size_t n_items;
        double sum = 0.0;
        for (i = 0; i < 1024; i++) {
            snprintf(buf[i], sizeof(buf[i]), "opt-%04lx", (unsigned long)i);
            opts[i] = buf[i];
        }
        memset(&r, 0, sizeof(r));
        memset(&err, 0, sizeof(err));
        ok = odk_evaluate_rank(k, "pick one of the many", opts, 1024,
                               "alpha bravo delta under load", NULL, &r,
                               &err) == ODK_OK;
        check(ok, "S1 rank with 1024 options succeeds");
        if (ok) {
            memset(&meta, 0, sizeof(meta));
            n_items = odk_result_rank_items(&r, opts, 1024, &items);
            checkf((long)n_items == 1024, "S1 rank_items count == 1024",
                   (long)n_items);
            memset(seen, 0, sizeof(seen));
            for (i = 0; i < n_items && i < 1024; i++) {
                if (items[i].index < 1024) {
                    seen[items[i].index]++;
                }
            }
            ok = 1;
            for (i = 0; i < 1024; i++) {
                if (seen[i] != 1) {
                    ok = 0;
                }
            }
            check(ok, "S1 rank indices form a permutation of 0..1023");
            odk_result_rank_items_free(items);
            for (i = 0; i < r.distribution.count; i++) {
                sum += r.distribution.entries[i].value;
            }
            check(sum > 0.94 && sum < 1.06,
                  "S1 1024-option distribution sums to ~1");
            for (i = 0; i < r.distribution.count; i++) {
                if (r.distribution.entries[i].value < 0.0 ||
                    r.distribution.entries[i].value > 1.0) {
                    g_failures++;
                    check(0, "S1 distribution entry out of [0,1]");
                    break;
                }
            }
        }
        odk_result_release(&r);
    }

    /* duplicates + empty + whitespace options */
    {
        const char *opts[5] = {"", "x", "", "x", "  "};
        memset(&r, 0, sizeof(r));
        ok = odk_evaluate_choice(k, "dupes", opts, 5, "alpha x alpha",
                                 NULL, &r, &err) == ODK_OK;
        check(ok, "S1 choice with duplicate/empty options");
        odk_result_release(&r);
    }

    /* very long single option and long instruction */
    {
        char *big = malloc(12000);
        const char *opts[2] = {"short", NULL};
        if (big != NULL) {
            memset(big, 'a', 12000 - 1);
            big[12000 - 1] = '\0';
            opts[1] = big;
            memset(&r, 0, sizeof(r));
            ok = odk_evaluate_choice(k, big, opts, 2, big, NULL, &r,
                                     &err) == ODK_OK;
            check(ok, "S1 choice with 12k-char option/instruction/context");
            odk_result_release(&r);
            free(big);
        }
    }

    /* extract top_k extremes */
    {
        static const int ks[6] = {0, 1, -1, -2147483647 - 1, 2147483647,
                                  1000000};
        for (i = 0; i < 6; i++) {
            memset(&r, 0, sizeof(r));
            memset(&err, 0, sizeof(err));
            ok = odk_evaluate_extract(k, "find the load numbers",
                                      "load 81 then load 82 then load 83",
                                      NULL, ks[i], &r, &err);
            survived(); /* any status is fine; crashing is not */
            if (ok == ODK_OK && ks[i] >= 1) {
                /* top_k == 0 selects the kernel default, like caps */
                checkf((long)r.item_count <= (long)ks[i],
                       "S1 extract respects top_k", ks[i]);
            }
            odk_result_release(&r);
        }
    }

    /* empty strings everywhere */
    {
        const char *opts[2] = {"a", "b"};
        memset(&r, 0, sizeof(r));
        ok = odk_evaluate_choice(k, "", opts, 2, "", NULL, &r, &err) ==
             ODK_OK;
        check(ok, "S1 choice with empty instruction and context");
        odk_result_release(&r);
    }

    /* NULL kernel: documented to fall back to defaults; must not crash
     * and must produce a clean releasable result */
    {
        memset(&r, 0, sizeof(r));
        ok = odk_evaluate_choice(NULL, "i", one, 1, "c", NULL, &r,
                                 &err) == ODK_OK;
        check(ok, "S1 NULL-kernel choice falls back cleanly");
        odk_result_release(&r);
    }

    /* count 0 / NULL options: documented error, not a crash */
    {
        memset(&r, 0, sizeof(r));
        memset(&err, 0, sizeof(err));
        ok = odk_evaluate_choice(k, "i", one, 0, "c", NULL, &r, &err) !=
             ODK_OK;
        check(ok, "S1 choice with 0 options is an error");
        odk_result_release(&r);
        memset(&r, 0, sizeof(r));
        ok = odk_evaluate_choice(k, "i", NULL, 3, "c", NULL, &r, &err) !=
             ODK_OK;
        check(ok, "S1 choice with NULL options is an error");
        odk_result_release(&r);
    }

    /* caps never change numbers: base vs _opts with aggressive caps */
    {
        const char *opts[3] = {"alpha plan", "bravo plan", "delta plan"};
        odk_result r1, r2;
        odk_output_caps caps;
        size_t j;
        int same;
        memset(&r1, 0, sizeof(r1));
        memset(&r2, 0, sizeof(r2));
        memset(&caps, 0, sizeof(caps));
        caps.max_ranked = 1;
        caps.max_snippet_chars = 2;
        caps.max_total_chars = 3;
        ok = odk_evaluate_rank(k, "which plan", opts, 3, "alpha plan run",
                               NULL, &r1, &err) == ODK_OK;
        ok = ok && odk_evaluate_rank_opts(k, "which plan", opts, 3,
                                          "alpha plan run", NULL, &caps,
                                          &r2, NULL, &err) == ODK_OK;
        check(ok, "S1 rank base and _opts both succeed");
        if (ok) {
            same = r1.confidence == r2.confidence &&
                   r1.probability == r2.probability &&
                   r1.distribution.count == r2.distribution.count;
            for (j = 0; same && j < r1.distribution.count; j++) {
                same = r1.distribution.entries[j].value ==
                           r2.distribution.entries[j].value &&
                       strcmp(r1.distribution.entries[j].key,
                              r2.distribution.entries[j].key) == 0;
            }
            check(same, "S1 caps leave rank numbers bit-identical");
        }
        odk_result_release(&r1);
        odk_result_release(&r2);
    }

    odk_kernel_free(k);
    printf("stress: S1 boundary: %d checks so far, %d failures so far\n",
           g_checks, g_failures);
}

/* ------------------------------------------------------------------ */
/* S2 - seeded fuzz marathon                                           */
/* ------------------------------------------------------------------ */

#define MARATHON_CASES 5000

/* One full pass over the marathon workload; returns the FNV of every
 * result JSON (and of the status byte).  require_ok: valid inputs must
 * succeed.  client_pairs: interleave client-pipeline calls. */
static uint64_t marathon_pass(odk_kernel *k, int require_ok,
                              int client_pairs, long *ok_calls)
{
    uint64_t s = 0xC0FFEE123456789ULL;
    uint64_t h = 0xCBF29CE484222325ULL;
    long oks = 0;
    odk_error err;
    int i;
    for (i = 0; i < MARATHON_CASES; i++) {
        slot_t sl;
        char *out[6];
        int j;
        slot_build(&sl, &s);
        if (i % 7 == 3) { /* occasionally a bigger context */
            free(sl.context);
            sl.context = gen_text(&s, 120 + sm_below(&s, 160));
        }
        slot_eval(k, &sl, out);
        for (j = 0; j < 6; j++) {
            if (out[j] != NULL) {
                oks++;
                h = fnv1a(out[j], h);
            } else if (require_ok) {
                char what[96];
                snprintf(what, sizeof(what),
                         "S2 marathon case %d prim %d returned error", i,
                         j);
                check(0, what);
            }
            free(out[j]);
        }
        if (client_pairs && i % 250 == 0) {
            odk_client *cl = odk_client_create("cpu");
            if (cl != NULL) {
                char *json = NULL;
                char q[512];
                memset(&err, 0, sizeof(err));
                snprintf(q, sizeof(q),
                         "{\"fix\":{\"primitive\":\"choice\",\"question\":"
                         "\"%s\",\"options\":[\"%s\",\"%s\"]}}",
                         sl.instruction, sl.opts[0],
                         sl.nopts > 1 ? sl.opts[1] : sl.opts[0]);
                /* coverage only: the CLIENT json embeds wall-clock
                 * timing metadata by design, so it must NOT enter the
                 * determinism transcript (the kernel primitives above
                 * carry that guarantee) */
                if (odk_client_evaluate_json_ex(cl, sl.context, q, "hybrid",
                                                NULL, &json, &err) ==
                    ODK_OK) {
                    oks++;
                } else if (require_ok) {
                    check(0, "S2 client pipeline call failed");
                }
                free(json);
                odk_client_free(cl);
            }
        }
        slot_free(&sl);
    }
    if (ok_calls != NULL) {
        *ok_calls = oks;
    }
    return h;
}

static void s2_marathon(void)
{
    odk_kernel *a = odk_kernel_create();
    odk_kernel *b = odk_kernel_create();
    odk_kernel *c = odk_kernel_create();
    odk_kernel *d = odk_kernel_create();
    odk_error err;
    uint64_t ha, hb, hc, hd;
    long oks = 0;

    check(a != NULL && b != NULL && c != NULL && d != NULL,
          "S2 four kernels allocated");
    if (a == NULL || b == NULL || c == NULL || d == NULL) {
        return;
    }
    memset(&err, 0, sizeof(err));
    check(odk_kernel_load_model(c, "tests/fixtures/default_model.odkmodel",
                                &err) == ODK_OK,
          "S2 model load for kernels c/d");
    memset(&err, 0, sizeof(err));
    check(odk_kernel_load_model(d, "tests/fixtures/default_model.odkmodel",
                                &err) == ODK_OK,
          "S2 model load for kernels c/d");

    ha = marathon_pass(a, 1, 1, &oks);
    hb = marathon_pass(b, 1, 1, NULL);
    check(ha == hb, "S2 default-kernel pair: byte-identical transcripts");
    hc = marathon_pass(c, 1, 0, NULL);
    hd = marathon_pass(d, 1, 0, NULL);
    check(hc == hd, "S2 model-kernel pair: byte-identical transcripts");
    checkf(oks > MARATHON_CASES * 6 / 2, "S2 marathon call volume", oks);

    /* garbage rotation: any status allowed, but successful calls must
     * be byte-stable across an immediate repeat */
    {
        uint64_t s = 0xDEADBEEFCAFEF00DULL;
        int i;
        int stable = 1;
        for (i = 0; i < 200; i++) {
            slot_t sl;
            char *o1[6];
            char *o2[6];
            int j;
            slot_build(&sl, &s);
            {
                char *p = poison_utf8(sl.context, &s);
                free(sl.context);
                sl.context = p;
                p = poison_utf8(sl.instruction, &s);
                free(sl.instruction);
                sl.instruction = p;
            }
            slot_eval(a, &sl, o1);
            slot_eval(a, &sl, o2);
            for (j = 0; j < 6; j++) {
                if ((o1[j] == NULL) != (o2[j] == NULL)) {
                    stable = 0;
                } else if (o1[j] != NULL &&
                           strcmp(o1[j], o2[j]) != 0) {
                    stable = 0;
                }
            }
            six_free(o1);
            six_free(o2);
            slot_free(&sl);
        }
        check(stable, "S2 garbage inputs: repeat calls byte-stable");
    }

    odk_kernel_free(a);
    odk_kernel_free(b);
    odk_kernel_free(c);
    odk_kernel_free(d);
    printf("stress: S2 marathon: %d checks so far, %d failures so far\n",
           g_checks, g_failures);
}

/* ------------------------------------------------------------------ */
/* S3 - truncation / caps / budget boundary sweep                      */
/* ------------------------------------------------------------------ */

static void s3_boundaries(void)
{
    odk_kernel_config cfg;
    odk_kernel *k;
    uint64_t s = 0x5A5A5A5A5A5A5A5AULL;
    size_t len;
    int stable = 1;
    long sweep_checks = 0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.context_max = 1000;
    cfg.snippet_max = 64;
    cfg.max_chunks = 16;
    cfg.max_work = 20000;
    k = odk_kernel_create_ex(&cfg);
    check(k != NULL, "S3 configured kernel allocated");
    if (k == NULL) {
        return;
    }

    /* every length 0..1200: meta accounting invariants + stability */
    for (len = 0; len <= 1200; len++) {
        char *ctx = gen_text(&s, len / 6 + 1);
        const char *opts[2] = {"alpha", "bravo"};
        odk_result r1, r2;
        odk_result_meta m1, m2;
        odk_error err;
        if (strlen(ctx) > len) {
            ctx[len] = '\0'; /* hard-truncate the generated text */
        }
        memset(&r1, 0, sizeof(r1));
        memset(&r2, 0, sizeof(r2));
        memset(&m1, 0, sizeof(m1));
        memset(&m2, 0, sizeof(m2));
        memset(&err, 0, sizeof(err));
        if (odk_evaluate_choice_opts(k, "which word", opts, 2, ctx, NULL,
                                     NULL, &r1, &m1, &err) == ODK_OK &&
            odk_evaluate_choice_opts(k, "which word", opts, 2, ctx, NULL,
                                     NULL, &r2, &m2, &err) == ODK_OK) {
            char *j1 = odk_result_to_json(&r1);
            char *j2 = odk_result_to_json(&r2);
            if (j1 == NULL || j2 == NULL || strcmp(j1, j2) != 0 ||
                m1.context_truncated != m2.context_truncated ||
                m1.context_units_dropped != m2.context_units_dropped) {
                stable = 0;
            }
            free(j1);
            free(j2);
            sweep_checks++;
            if (m1.context_truncated) {
                if (m1.context_units_dropped == 0 ||
                    m1.context_units_dropped > strlen(ctx)) {
                    stable = 0;
                }
            } else if (m1.context_units_dropped != 0) {
                stable = 0;
            }
        } else {
            stable = 0;
        }
        odk_result_release(&r1);
        odk_result_release(&r2);
        free(ctx);
    }
    check(stable, "S3 truncation sweep: 1201 lengths, invariants hold");

    /* work-budget pressure: long contexts, tiny budget; decisions stay
     * deterministic and work_exceeded is internally consistent */
    {
        int i;
        int consistent = 1;
        for (i = 0; i < 60; i++) {
            char *ctx = gen_text(&s, 600 + (size_t)i * 10);
            const char *opts[2] = {"alpha", "bravo"};
            odk_result r1, r2;
            odk_result_meta m1, m2;
            odk_error err;
            char *j1, *j2;
            memset(&r1, 0, sizeof(r1));
            memset(&r2, 0, sizeof(r2));
            memset(&m1, 0, sizeof(m1));
            memset(&m2, 0, sizeof(m2));
            memset(&err, 0, sizeof(err));
            if (odk_evaluate_choice_opts(k, "which word", opts, 2, ctx,
                                         NULL, NULL, &r1, &m1, &err) !=
                    ODK_OK ||
                odk_evaluate_choice_opts(k, "which word", opts, 2, ctx,
                                         NULL, NULL, &r2, &m2, &err) !=
                    ODK_OK) {
                consistent = 0;
            } else {
                j1 = odk_result_to_json(&r1);
                j2 = odk_result_to_json(&r2);
                if (strcmp(j1, j2) != 0 ||
                    m1.work_exceeded != m2.work_exceeded) {
                    consistent = 0;
                }
                free(j1);
                free(j2);
                sweep_checks++;
            }
            odk_result_release(&r1);
            odk_result_release(&r2);
            free(ctx);
        }
        check(consistent, "S3 work-budget pressure: deterministic");
    }

    /* extract emitted/dropped accounting under caps */
    {
        int i;
        int accounting = 1;
        for (i = 1; i <= 12; i++) {
            char *ctx = gen_text(&s, 400);
            odk_result r;
            odk_result_meta m;
            odk_output_caps caps;
            odk_error err;
            memset(&r, 0, sizeof(r));
            memset(&m, 0, sizeof(m));
            memset(&caps, 0, sizeof(caps));
            memset(&err, 0, sizeof(err));
            caps.max_ranked = (size_t)i;
            caps.max_snippet_chars = (size_t)i * 3;
            caps.max_total_chars = (size_t)i * 20;
            if (odk_evaluate_extract_opts(k, "find the words", ctx, NULL,
                                          i, &caps, &r, &m, &err) ==
                ODK_OK) {
                if (m.emitted > (size_t)i ||
                    m.emitted + m.dropped < m.emitted) {
                    accounting = 0;
                }
                sweep_checks++;
            } else {
                accounting = 0;
            }
            odk_result_release(&r);
            free(ctx);
        }
        check(accounting, "S3 extract caps: emitted <= top_k, no crash");
    }

    odk_kernel_free(k);
    check(sweep_checks > 1250, "S3 sweep volume");
    printf("stress: S3 boundaries: %d checks so far, %d failures so far\n",
           g_checks, g_failures);
}

/* ------------------------------------------------------------------ */
/* S4 - concurrency storm                                              */
/* ------------------------------------------------------------------ */

#define S4_SLOTS 240

static slot_t g_slots[S4_SLOTS];
static char *g_baseline[S4_SLOTS][6];
static int g_thread_divergence = 0;

static odk_kernel *g_kernel_handle = NULL;

static void *s4_worker(void *arg)
{
    char *out[6];
    int i, j;
    int tid = (int)(intptr_t)arg;
    for (i = 0; i < S4_SLOTS; i++) {
        int slot = (i * 7 + tid * 37) % S4_SLOTS; /* deterministic mix */
        slot_eval(g_kernel_handle, &g_slots[slot], out);
        for (j = 0; j < 6; j++) {
            if ((out[j] == NULL) != (g_baseline[slot][j] == NULL)) {
                __sync_fetch_and_add(&g_thread_divergence, 1);
            } else if (out[j] != NULL &&
                       strcmp(out[j], g_baseline[slot][j]) != 0) {
                __sync_fetch_and_add(&g_thread_divergence, 1);
            }
        }
        six_free(out);
    }
    return NULL;
}

static void *s4_worker_own(void *arg)
{
    odk_kernel *k = (odk_kernel *)arg;
    char *out[6];
    int i, j;
    for (i = 0; i < S4_SLOTS; i++) {
        int slot = (i * 11 + 5) % S4_SLOTS;
        slot_eval(k, &g_slots[slot], out);
        for (j = 0; j < 6; j++) {
            if ((out[j] == NULL) != (g_baseline[slot][j] == NULL)) {
                __sync_fetch_and_add(&g_thread_divergence, 1);
            } else if (out[j] != NULL &&
                       strcmp(out[j], g_baseline[slot][j]) != 0) {
                __sync_fetch_and_add(&g_thread_divergence, 1);
            }
        }
        six_free(out);
    }
    return NULL;
}

static void s4_concurrency(void)
{
    pthread_t th[8];
    odk_kernel *own[8] = {NULL};
    uint64_t s = 0x1234ABCD5678EF90ULL;
    int i, j;
    int baseline_ok = 1;

    g_kernel_handle = odk_kernel_create();
    check(g_kernel_handle != NULL, "S4 shared kernel allocated");
    if (g_kernel_handle == NULL) {
        return;
    }
    for (i = 0; i < S4_SLOTS; i++) {
        slot_build(&g_slots[i], &s);
        slot_eval(g_kernel_handle, &g_slots[i], g_baseline[i]);
        for (j = 0; j < 6; j++) {
            if (g_baseline[i][j] == NULL) {
                baseline_ok = 0;
            }
        }
    }
    check(baseline_ok, "S4 baseline: every primitive call succeeded");

    /* phase A: 8 threads, one shared kernel */
    g_thread_divergence = 0;
    for (i = 0; i < 8; i++) {
        if (pthread_create(&th[i], NULL, s4_worker,
                           (void *)(intptr_t)i) != 0) {
            check(0, "S4 pthread_create failed");
            for (j = 0; j < i; j++) {
                pthread_join(th[j], NULL);
            }
            goto cleanup;
        }
    }
    for (i = 0; i < 8; i++) {
        pthread_join(th[i], NULL);
    }
    check(g_thread_divergence == 0,
          "S4 phase A: 8 threads x 6 primitives on ONE shared kernel, "
          "byte-identical");

    /* phase B: 8 pre-created kernels, one per thread */
    g_thread_divergence = 0;
    for (i = 0; i < 8; i++) {
        own[i] = odk_kernel_create();
        if (own[i] == NULL) {
            check(0, "S4 own-kernel allocation failed");
            for (j = 0; j < i; j++) {
                pthread_join(th[j], NULL);
            }
            goto cleanup;
        }
    }
    for (i = 0; i < 8; i++) {
        if (pthread_create(&th[i], NULL, s4_worker_own, own[i]) != 0) {
            check(0, "S4 pthread_create failed (phase B)");
            for (j = 0; j < i; j++) {
                pthread_join(th[j], NULL);
            }
            goto cleanup;
        }
    }
    for (i = 0; i < 8; i++) {
        pthread_join(th[i], NULL);
    }
    check(g_thread_divergence == 0,
          "S4 phase B: 8 threads on 8 own kernels, byte-identical");

cleanup:
    for (i = 0; i < S4_SLOTS; i++) {
        six_free(g_baseline[i]);
        slot_free(&g_slots[i]);
    }
    for (i = 0; i < 8; i++) {
        if (own[i] != NULL) {
            odk_kernel_free(own[i]);
        }
    }
    odk_kernel_free(g_kernel_handle);
    g_kernel_handle = NULL;
    printf("stress: S4 concurrency: %d checks so far, %d failures so far\n",
           g_checks, g_failures);
}

/* ------------------------------------------------------------------ */
/* S5 - spine endurance: lineage, pins, replay, calibration            */
/* ------------------------------------------------------------------ */

#define S5_SUBJECTS 40
#define S5_ROUNDS 40
#define S5_T0 1000
#define S5_TSTEP 10

static const char *const S5_SCHEMES =
    "["
    "{\"id\":\"stress-triage\",\"version\":1,\"valid_from\":0,"
    "\"valid_to\":-1,\"default_label\":\"unsorted\","
    "\"facet_names\":[\"severity\"],"
    "\"rules\":["
    "{\"label\":\"spike\",\"match_terms\":[\"alpha\",\"delta\"],"
    "\"any_term\":true,\"priority\":10,\"facets\":{\"severity\":\"high\"}},"
    "{\"label\":\"drift\",\"match_terms\":[\"epsilon slow\"],"
    "\"any_term\":false,\"priority\":5,\"facets\":{\"severity\":\"low\"}}],"
    "\"notes\":\"stress fixture\"},"
    "{\"id\":\"stress-impact\",\"version\":1,\"valid_from\":0,"
    "\"valid_to\":-1,\"default_label\":\"routine\","
    "\"facet_names\":[\"blast\"],"
    "\"rules\":[{\"label\":\"wide\",\"match_terms\":[\"outage\",\"alpha\"],"
    "\"any_term\":true,\"priority\":8,\"facets\":{\"blast\":\"broad\"}}],"
    "\"notes\":\"stress fixture\"}"
    "]";

static const char *const S5_TEMPLATE =
    "[{\"id\":\"stress-report\",\"version\":1,\"valid_from\":0,"
    "\"valid_to\":-1,"
    "\"parameters\":["
    "{\"name\":\"depth\",\"type\":\"int\",\"default_value\":\"2\","
    "\"required\":false},"
    "{\"name\":\"tag\",\"type\":\"string\",\"default_value\":\"\","
    "\"required\":true}],"
    "\"component_specs\":["
    "{\"name\":\"symptoms\",\"kind\":\"sentences\",\"max_items\":3,"
    "\"required\":true},"
    "{\"name\":\"metrics\",\"kind\":\"keyvalue\",\"max_items\":5,"
    "\"required\":false}],"
    "\"extraction_rules\":["
    "{\"component\":\"symptoms\",\"terms\":[\"alpha\",\"delta\"],"
    "\"mode\":\"sentence\",\"window\":0},"
    "{\"component\":\"metrics\",\"terms\":[\"load\",\"queue\"],"
    "\"mode\":\"keyvalue\",\"window\":0}]}]";

static const char *const S5_VOCAB =
    "{\"id\":\"stress-syn\",\"version\":1,\"valid_from\":0,"
    "\"valid_to\":-1,\"groups\":[{\"canonical\":\"alpha\","
    "\"terms\":[\"alfa beginning\",\"alpha start\"]}]}";

static long s5_classifications = 0;

static void s5_spine(void)
{
    odk_spine *sp = odk_spine_create();
    odk_kernel *k = odk_kernel_create();
    odk_spine_error err;
    uint64_t s = 0x0F1E2D3C4B5A6978ULL;
    static char *seen[S5_SUBJECTS][S5_ROUNDS];
    int subj, round;
    long interval_expect;
    int asof_ok = 1;
    long i;

    check(sp != NULL && k != NULL, "S5 spine and kernel allocated");
    if (sp == NULL || k == NULL) {
        odk_spine_free(sp);
        odk_kernel_free(k);
        return;
    }
    memset(&err, 0, sizeof(err));
    check(odk_spine_register_json(sp, "scheme_set", S5_SCHEMES, "stress",
                                  &err) == ODK_SPINE_OK,
          "S5 register scheme set");
    memset(&err, 0, sizeof(err));
    check(odk_spine_register_json(sp, "decomposition_template",
                                  S5_TEMPLATE, "stress",
                                  &err) == ODK_SPINE_OK,
          "S5 register template");
    memset(&err, 0, sizeof(err));
    check(odk_spine_register_json(sp, "vocabulary", S5_VOCAB, "stress",
                                  &err) == ODK_SPINE_OK,
          "S5 register vocabulary");

    /* classification marathon with captured lineage */
    for (subj = 0; subj < S5_SUBJECTS; subj++) {
        for (round = 0; round < S5_ROUNDS; round++) {
            char subject[32];
            char *text;
            int64_t t = S5_T0 + (int64_t)(subj * S5_ROUNDS + round) *
                                     S5_TSTEP;
            odk_spine_classification_result res;
            snprintf(subject, sizeof(subject), "s5-item-%d", subj);
            text = gen_text(&s, 4 + sm_below(&s, 10));
            memset(&res, 0, sizeof(res));
            memset(&err, 0, sizeof(err));
            if (odk_spine_classify(sp, subject, text, "", t, &res,
                                   &err) == ODK_SPINE_OK) {
                s5_classifications++;
                checkf((long)res.classifications.count == 2,
                       "S5 classify: 2 schemes in one call",
                       (long)res.classifications.count);
                for (i = 0; i < (long)res.classifications.count; i++) {
                    if (strcmp(res.classifications.items[i].scheme_id,
                               "stress-triage") == 0) {
                        seen[subj][round] = strdup(
                            res.classifications.items[i].label);
                    }
                    check(res.classifications.items[i].tier != NULL &&
                              res.classifications.items[i].tier[0] == 'A',
                          "S5 tier labeled A (no brain registered)");
                }
                odk_spine_classification_result_release(&res);
            } else {
                check(0, "S5 classify call failed");
            }
            free(text);
        }
    }
    check(s5_classifications == (long)S5_SUBJECTS * S5_ROUNDS,
          "S5 classification volume");
    interval_expect = s5_classifications * 2; /* one interval per scheme */
    checkf((long)odk_spine_interval_count(sp) == interval_expect,
           "S5 append-only interval store: exact interval count",
           (long)odk_spine_interval_count(sp));

    /* as-of integrity: at each append time t, the store must answer
     * with exactly the label captured at that append */
    for (subj = 0; subj < S5_SUBJECTS; subj++) {
        for (round = 0; round < S5_ROUNDS; round += 3) {
            char subject[32];
            int64_t t = S5_T0 + (int64_t)(subj * S5_ROUNDS + round) *
                                     S5_TSTEP;
            const odk_spine_classification_interval *iv;
            memset(&err, 0, sizeof(err));
            snprintf(subject, sizeof(subject), "s5-item-%d", subj);
            if (odk_spine_classification_at(sp, subject, "stress-triage",
                                            t, &iv, &err) == ODK_SPINE_OK) {
                if (seen[subj][round] == NULL ||
                    iv->label == NULL ||
                    strcmp(iv->label, seen[subj][round]) != 0 ||
                    iv->valid_from > t) {
                    asof_ok = 0;
                }
            } else {
                asof_ok = 0;
            }
        }
    }
    check(asof_ok, "S5 as-of queries reproduce captured lineage exactly");

    /* decomposition: valid, missing-required and unknown-param errors */
    {
        int d;
        int d_ok = 1;
        for (d = 0; d < 120; d++) {
            char *text;
            char *full;
            char params[64];
            odk_spine_components comp;
            memset(&err, 0, sizeof(err));
            text = gen_text(&s, 8 + sm_below(&s, 20));
            full = malloc(strlen(text) + 64);
            if (full == NULL) {
                exit(1);
            }
            /* the trailing alpha sentence guarantees the required
             * 'symptoms' component extracts non-empty */
            sprintf(full, "%s\nLoad: 81 percent\nQueue: full\nalpha ran", text);
            free(text);
            text = full;
            if (d % 3 == 0) {
                snprintf(params, sizeof(params), "{\"tag\":\"s5-%d\"}", d);
            } else if (d % 3 == 1) {
                snprintf(params, sizeof(params),
                         "{\"tag\":\"s5-%d\",\"depth\":3}", d);
            } else {
                snprintf(params, sizeof(params),
                         "{\"depth\":2}"); /* missing required tag */
            }
            memset(&comp, 0, sizeof(comp));
            if (d % 3 == 2) {
                if (odk_spine_decompose(sp, "stress-report", -1, text,
                                        params, 900, &comp,
                                        &err) == ODK_SPINE_OK) {
                    d_ok = 0; /* missing required param must fail */
                    odk_spine_components_release(&comp);
                }
            } else if (odk_spine_decompose(sp, "stress-report", -1, text,
                                           params, 900, &comp,
                                           &err) == ODK_SPINE_OK) {
                /* a term-less item yields zero components by design */
                if (comp.template_version != 1) {
                    d_ok = 0;
                }
                odk_spine_components_release(&comp);
            } else {
                d_ok = 0;
            }
            free(text);
        }
        check(d_ok, "S5 decompose: params contract enforced, output sane");

        /* discovered contract, pinned here: a REQUIRED component that
         * extracts nothing is a decompose error, not an empty output */
        {
            odk_spine_components comp;
            memset(&err, 0, sizeof(err));
            memset(&comp, 0, sizeof(comp));
            check(odk_spine_decompose(sp, "stress-report", -1,
                                      "nothing matches here\nQueue: full",
                                      "{\"tag\":\"x\"}", 900, &comp,
                                      &err) != ODK_SPINE_OK,
                  "S5 empty required component is a decompose error");
        }
    }

    /* pin + replay: 600 pins, every 12th replayed with exact verdict */
    {
        long p;
        int pin_ok = 1;
        for (p = 0; p < 600; p++) {
            char req[512];
            char inputs[32];
            odk_spine_decision_pin pin;
            memset(&err, 0, sizeof(err));
            snprintf(req, sizeof(req),
                     "{\"primitive\":\"choice\",\"instruction\":\"pick "
                     "the word\",\"options\":[\"alpha\",\"bravo\","
                     "\"delta\"],\"context\":\"%s %s\"}",
                     WORDS[sm_below(&s, WORDS_N)],
                     WORDS[sm_below(&s, WORDS_N)]);
            snprintf(inputs, sizeof(inputs), "s5-inputs-%ld", p);
            memset(&pin, 0, sizeof(pin));
            if (odk_spine_pin(sp, k, req, inputs, "exact",
                              1700000000000LL + p, &pin,
                              &err) != ODK_SPINE_OK) {
                pin_ok = 0;
            } else {
                check(pin.decision != NULL && pin.schema_fp != NULL,
                      "S5 pin carries decision and schema fingerprint");
                if (p % 12 == 0) {
                    char *rep = NULL;
                    char *rep2 = NULL;
                    memset(&err, 0, sizeof(err));
                    if (odk_spine_replay(sp, k, pin.pin_id, &rep,
                                         &err) == ODK_SPINE_OK &&
                        rep != NULL) {
                        int be = strstr(rep, "\"byte_equal\":true") !=
                                     NULL ||
                                 strstr(rep, "\"byte_equal\": true") !=
                                     NULL;
                        int dm = strstr(rep, "\"decision_match\":true") !=
                                     NULL ||
                                 strstr(rep, "\"decision_match\": true") !=
                                     NULL;
                        if (!be || !dm ||
                            strstr(rep, "mismatch") != NULL) {
                            pin_ok = 0;
                        }
                        memset(&err, 0, sizeof(err));
                        if (odk_spine_replay(sp, k, pin.pin_id, &rep2,
                                             &err) == ODK_SPINE_OK &&
                            rep2 != NULL) {
                            if (strcmp(rep, rep2) != 0) {
                                pin_ok = 0;
                            }
                        } else {
                            pin_ok = 0;
                        }
                        free(rep2);
                    } else {
                        pin_ok = 0;
                    }
                    free(rep);
                }
                odk_spine_decision_pin_release(&pin);
            }
        }
        check(pin_ok, "S5 pin + replay: exact verdicts, byte-stable");
        check(odk_spine_pin_count(sp) == 600, "S5 pin store count");
    }

    /* calibration over 1200 matched decision/outcome pairs */
    {
        long o;
        char fp[64];
        const char *schema_fp = odk_spine_schema_fingerprint(sp);
        int cal_ok = 1;
        odk_spine_calibration_report c1, c2;
        char *j1, *j2;
        long total_samples = 0;
        snprintf(fp, sizeof(fp), "%s", schema_fp ? schema_fp : "stress");
        for (o = 0; o < 1200; o++) {
            char obs[512];
            double conf = o % 2 == 0 ? 0.85 : 0.55;
            memset(&err, 0, sizeof(err));
            snprintf(obs, sizeof(obs),
                     "{\"observation_id\":\"s5-d%ld\",\"kind\":\"decision\","
                     "\"t\":%ld,\"scope\":\"\",\"subject_ref\":\"s5-o%ld\","
                     "\"primitive\":\"choice\",\"scheme_id\":\"\","
                     "\"payload_json\":\"{}\",\"confidence\":%.4f,"
                     "\"tier\":\"A\",\"brain_snapshot_id\":\"\","
                     "\"outcome\":\"\",\"kernel_version\":\"%s\","
                     "\"schema_fp\":\"%s\",\"registrations\":[]}",
                     o, (long)(5000 + o), o, conf, ODK_LIBRARY_VERSION, fp);
            if (odk_spine_observation_ingest(sp, obs, &err) != ODK_SPINE_OK) {
                cal_ok = 0;
            }
            memset(&err, 0, sizeof(err));
            snprintf(obs, sizeof(obs),
                     "{\"observation_id\":\"s5-o%ld\",\"kind\":\"outcome\","
                     "\"t\":%ld,\"scope\":\"\",\"subject_ref\":\"s5-d%ld\","
                     "\"primitive\":\"\",\"scheme_id\":\"\","
                     "\"payload_json\":\"{}\",\"confidence\":0,"
                     "\"tier\":\"\",\"brain_snapshot_id\":\"\","
                     "\"outcome\":\"success\",\"kernel_version\":\"%s\","
                     "\"schema_fp\":\"%s\",\"registrations\":[]}",
                     o, (long)(5000 + o), o, ODK_LIBRARY_VERSION, fp);
            if (odk_spine_observation_ingest(sp, obs, &err) != ODK_SPINE_OK) {
                cal_ok = 0;
            }
        }
        memset(&c1, 0, sizeof(c1));
        memset(&c2, 0, sizeof(c2));
        memset(&err, 0, sizeof(err));
        if (odk_spine_calibrate(sp, "s5-log", &c1, &err) != ODK_SPINE_OK) {
            cal_ok = 0;
        }
        memset(&err, 0, sizeof(err));
        if (odk_spine_calibrate(sp, "s5-log", &c2, &err) != ODK_SPINE_OK) {
            cal_ok = 0;
        }
        j1 = odk_spine_calibration_report_to_json(&c1);
        j2 = odk_spine_calibration_report_to_json(&c2);
        if (j1 == NULL || j2 == NULL || strcmp(j1, j2) != 0) {
            cal_ok = 0;
        } else {
            long b;
            for (b = 0; b < (long)c1.bins.count; b++) {
                total_samples += (long)c1.bins.items[b].sample_count;
            }
            if (c1.bins.count == 0 || total_samples != 1200) {
                cal_ok = 0;
            }
        }
        free(j1);
        free(j2);
        odk_spine_calibration_report_release(&c1);
        odk_spine_calibration_report_release(&c2);
        check(cal_ok, "S5 calibration: byte-identical reports, 1200 samples");
    }

    /* versioned re-registration: append, never overwrite */
    {
        const char *v2 =
            "[{\"id\":\"stress-triage\",\"version\":2,\"valid_from\":"
            "1000000,\"valid_to\":-1,\"default_label\":\"unsorted\","
            "\"facet_names\":[\"severity\"],"
            "\"rules\":[{\"label\":\"nova\",\"match_terms\":[\"zeta\"],"
            "\"any_term\":true,\"priority\":10,\"facets\":"
            "{\"severity\":\"extreme\"}}],\"notes\":\"stress v2\"}]";
        odk_spine_classification_result res;
        const odk_spine_classification_interval *iv;
        memset(&err, 0, sizeof(err));
        check(odk_spine_register_json(sp, "scheme_set", v2, "stress-v2",
                                      &err) == ODK_SPINE_OK,
              "S5 re-registration appends version 2");
        memset(&err, 0, sizeof(err));
        memset(&res, 0, sizeof(res));
        if (odk_spine_classify(sp, "s5-late", "zeta event", "", 2000000,
                               &res, &err) == ODK_SPINE_OK) {
            int found = 0;
            for (i = 0; i < (long)res.classifications.count; i++) {
                if (strcmp(res.classifications.items[i].scheme_id,
                           "stress-triage") == 0) {
                    found = strcmp(res.classifications.items[i].label,
                                   "nova") == 0 &&
                            res.classifications.items[i].scheme_version ==
                                2;
                }
            }
            check(found, "S5 as-of registration: v2 rules apply when due");
            odk_spine_classification_result_release(&res);
        } else {
            check(0, "S5 classify after v2 registration failed");
        }
        /* old lineage untouched: as-of at an old t still answers with
         * the captured label */
        memset(&err, 0, sizeof(err));
        if (odk_spine_classification_at(sp, "s5-item-0", "stress-triage",
                                        S5_T0, &iv, &err) == ODK_SPINE_OK) {
            check(iv->label != NULL && seen[0][0] != NULL &&
                      strcmp(iv->label, seen[0][0]) == 0,
                  "S5 old lineage intact after re-registration");
        } else {
            check(0, "S5 as-of after re-registration failed");
        }
        /* same version + different content must be refused */
        memset(&err, 0, sizeof(err));
        check(odk_spine_register_json(sp, "scheme_set",
                                      "[{\"id\":\"stress-impact\","
                                      "\"version\":1,\"valid_from\":0,"
                                      "\"valid_to\":-1,\"default_label\":"
                                      "\"CHANGED\",\"facet_names\":[],"
                                      "\"rules\":[],\"notes\":\"x\"}]",
                                      "stress-bad", &err) != ODK_SPINE_OK,
              "S5 same-version/different-content re-registration refused");
    }

    /* registration listing is deterministic */
    {
        odk_spine_registration_ref_list r1, r2;
        char *a, *b;
        int same;
        memset(&err, 0, sizeof(err));
        if (odk_spine_registrations(sp, &r1, &err) == ODK_SPINE_OK &&
            odk_spine_registrations(sp, &r2, &err) == ODK_SPINE_OK) {
            same = r1.count == r2.count;
            if (same) {
                for (i = 0; i < (long)r1.count; i++) {
                    a = odk_spine_registration_ref_to_json(&r1.items[i]);
                    b = odk_spine_registration_ref_to_json(&r2.items[i]);
                    if (a == NULL || b == NULL || strcmp(a, b) != 0) {
                        same = 0;
                    }
                    free(a);
                    free(b);
                }
            }
            check(same, "S5 registration listing deterministic");
        } else {
            check(0, "S5 registration listing failed");
        }
        odk_spine_registration_ref_list_release(&r1);
        odk_spine_registration_ref_list_release(&r2);
    }

    for (subj = 0; subj < S5_SUBJECTS; subj++) {
        for (round = 0; round < S5_ROUNDS; round++) {
            free(seen[subj][round]);
        }
    }
    odk_spine_free(sp);
    odk_kernel_free(k);
    printf("stress: S5 spine: %d checks so far, %d failures so far\n",
           g_checks, g_failures);
}

/* ------------------------------------------------------------------ */
/* S6 - lifecycle churn + RSS soak                                     */
/* ------------------------------------------------------------------ */

static void s6_churn(void)
{
    long rss0 = rss_kb();
    long rss1;
    int i;
    odk_error err;

    /* kernel churn with model + stopword loads */
    for (i = 0; i < 300; i++) {
        odk_kernel *k = odk_kernel_create();
        if (k != NULL) {
            odk_result r;
            const char *opts[2] = {"alpha", "bravo"};
            memset(&err, 0, sizeof(err));
            (void)odk_kernel_load_model(
                k, "tests/fixtures/default_model.odkmodel", &err);
            memset(&r, 0, sizeof(r));
            (void)odk_evaluate_choice(k, "pick", opts, 2,
                                      "alpha under load", NULL, &r, &err);
            odk_result_release(&r);
            odk_kernel_free(k);
        }
        g_checks++;
    }

    /* brain churn: record, save, load, free */
    for (i = 0; i < 150; i++) {
        odk_brain *b = odk_brain_create();
        if (b != NULL) {
            char *loaded;
            memset(&err, 0, sizeof(err));
            (void)odk_brain_record_outcome(b, "choice", "pick", "alpha",
                                           1.0, &err);
            (void)odk_brain_save_file(b, "/tmp/odk_stress_brain.json",
                                      &err);
            loaded = NULL;
            (void)loaded;
            {
                odk_brain *b2 = odk_brain_load_file(
                    "/tmp/odk_stress_brain.json", NULL);
                if (b2 != NULL) {
                    odk_brain_free(b2);
                }
            }
            odk_brain_free(b);
        }
        g_checks++;
    }
    remove("/tmp/odk_stress_brain.json");

    /* client churn */
    for (i = 0; i < 150; i++) {
        odk_client *cl = odk_client_create("cpu");
        if (cl != NULL) {
            char *json = NULL;
            memset(&err, 0, sizeof(err));
            if (odk_client_evaluate_json_ex(
                    cl, "alpha bravo delta",
                    "{\"fix\":{\"primitive\":\"choice\",\"question\":"
                    "\"which\",\"options\":[\"alpha\",\"bravo\"]}}",
                    "hybrid", NULL, &json, &err) == ODK_OK) {
                free(json);
            }
            odk_client_free(cl);
        }
        g_checks++;
    }

    /* spine churn */
    for (i = 0; i < 80; i++) {
        odk_spine *sp = odk_spine_create();
        if (sp != NULL) {
            odk_spine_classification_result res;
            odk_spine_error serr;
            memset(&serr, 0, sizeof(serr));
            if (odk_spine_register_json(sp, "scheme_set", S5_SCHEMES,
                                        "stress", &serr) == ODK_SPINE_OK) {
                int c;
                for (c = 0; c < 3; c++) {
                    char t[32];
                    memset(&res, 0, sizeof(res));
                    memset(&serr, 0, sizeof(serr));
                    snprintf(t, sizeof(t), "t%d x y", c);
                    if (odk_spine_classify(sp, "churn", t, "",
                                           1000 + c, &res, &serr) ==
                        ODK_SPINE_OK) {
                        odk_spine_classification_result_release(&res);
                    }
                }
            }
            odk_spine_free(sp);
        }
        g_checks++;
    }

    rss1 = rss_kb();
    if (rss0 > 0 && rss1 > 0) {
        long growth = rss1 - rss0;
        printf("stress: S6 soak: maxrss %ld KB -> %ld KB (growth %ld KB)\n",
               rss0, rss1, growth);
        checkf(growth < 65536, "S6 soak: no gross RSS growth", growth);
    }
    printf("stress: S6 churn: %d checks so far, %d failures so far\n",
           g_checks, g_failures);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    int rc;
    printf("stress: start (fixed seeds, deterministic; run me under "
           "ASan/UBSan too)\n");
    s1_boundary();
    s2_marathon();
    s3_boundaries();
    s4_concurrency();
    s5_spine();
    s6_churn();
    printf("stress: TOTAL checks=%d failures=%d\n", g_checks, g_failures);
    rc = g_failures == 0 ? 0 : 1;
    printf("stress: %s\n", rc == 0 ? "OK" : "FAILED");
    return rc;
}
