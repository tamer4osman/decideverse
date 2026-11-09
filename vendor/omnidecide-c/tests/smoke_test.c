/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * smoke_test.c - end-to-end checks of the public API.
 *
 *   make test
 *
 * Not an exhaustive suite: it proves the library builds, links and
 * behaves on every supported language, text mode and API family.  The
 * source file must be saved as UTF-8.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "omnidecide.h"

static int checks;
static int failures;

#define CHECK(cond)                                                       \
    do {                                                                  \
        checks++;                                                         \
        if (!(cond)) {                                                    \
            failures++;                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

typedef struct {
    const char *code;
    const char *question;
    const char *a;
    const char *b;
    const char *context;
} lang_case;

/* One easy decision per language: the context names option `a`. */
static const lang_case lang_cases[] = {
    {"en", "Which location should host the backup server?",
     "Berlin datacenter", "remote branch office",
     "The backup server should live in the Berlin datacenter because it has redundant power."},
    {"zh", "备份服务器应该放在哪里？", "柏林数据中心", "远程分公司",
     "备份服务器应该放在柏林数据中心，因为那里有冗余电源。"},
    {"hi", "बैकअप सर्वर कहाँ रखा जाना चाहिए?", "बर्लिन डेटासेंटर",
     "दूरस्थ शाखा कार्यालय",
     "बैकअप सर्वर बर्लिन डेटासेंटर में रखा जाना चाहिए क्योंकि वहाँ बिजली का बैकअप है।"},
    {"es", "¿Dónde debería alojarse el servidor de respaldo?",
     "centro de datos de Berlín", "oficina remota",
     "El servidor de respaldo debe estar en el centro de datos de Berlín porque tiene energía redundante."},
    {"ar", "أين يجب استضافة خادم النسخ الاحتياطي؟", "مركز بيانات برلين",
     "مكتب الفرع البعيد",
     "يجب أن يكون خادم النسخ الاحتياطي في مركز بيانات برلين لأنه يملك طاقة احتياطية."},
    {"fr", "Où faut-il héberger le serveur de sauvegarde ?",
     "centre de données de Berlin", "bureau distant",
     "Le serveur de sauvegarde doit être dans le centre de données de Berlin car il dispose d'une alimentation redondante."},
    {"bn", "ব্যাকআপ সার্ভার কোথায় রাখা উচিত?", "বার্লিন ডেটা সেন্টার",
     "দূরবর্তী শাখা অফিস",
     "ব্যাকআপ সার্ভার বার্লিন ডেটা সেন্টারে রাখা উচিত কারণ সেখানে অতিরিক্ত বিদ্যুৎ আছে।"},
    {"pt", "Onde o servidor de backup deve ficar hospedado?",
     "data center de Berlim", "escritório remoto",
     "O servidor de backup deve ficar no data center de Berlim porque tem energia redundante."},
    {"id", "Di mana server cadangan harus ditempatkan?", "pusat data Berlin",
     "kantor cabang jauh",
     "Server cadangan harus ditempatkan di pusat data Berlin karena memiliki daya redundan."},
    {"ur", "بیک اپ سرور کہاں رکھا جانا چاہیے؟", "برلن ڈیٹا سینٹر",
     "دور دراز شاخ دفتر",
     "بیک اپ سرور برلن ڈیٹا سینٹر میں رکھا جانا چاہیے کیونکہ وہاں اضافی بجلی ہے۔"},
};

/* Returns 1 when the kernel picks option `a`, 0 when it picks `b`, -1 on error. */
static int pick_a(odk_language lang, const lang_case *c)
{
    odk_kernel_config cfg;
    odk_kernel *kernel;
    const char *options[2];
    odk_result result;
    odk_error err;
    int answer = -1;

    memset(&cfg, 0, sizeof(cfg));
    cfg.text_mode = ODK_TEXT_UNICODE;
    cfg.language = lang;
    kernel = odk_kernel_create_ex(&cfg);
    if (kernel == NULL) {
        return -1;
    }
    options[0] = c->a;
    options[1] = c->b;
    if (odk_evaluate_choice(kernel, c->question, options, 2, c->context,
                            NULL, &result, &err) == ODK_OK) {
        answer = strcmp(result.string, c->a) == 0;
        odk_result_release(&result);
    }
    odk_kernel_free(kernel);
    return answer;
}

static void test_languages(void)
{
    size_t i;

    for (i = 0; i < sizeof(lang_cases) / sizeof(lang_cases[0]); i++) {
        const lang_case *c = &lang_cases[i];
        int ok = 0;
        odk_language lang = odk_lang_from_code(c->code, &ok);
        double confidence = 0.0;

        CHECK(ok == 1);
        CHECK(strcmp(odk_lang_to_code(lang), c->code) == 0);
        CHECK(pick_a(lang, c) == 1);          /* the language's own pack */
        CHECK(pick_a(ODK_LANG_AUTO, c) == 1); /* auto-detection          */
        CHECK(pick_a(ODK_LANG_UND, c) == 1);  /* generic Unicode mode    */
        CHECK(odk_lang_detect_text(c->context, &confidence, NULL, 0) == lang);
    }
}

static void test_primitives_and_determinism(void)
{
    const char *context =
        "The service stopped responding. "
        "Restarting the service restored normal operation within seconds.";
    const char *options[] = {"restart the service",
                             "reinstall the operating system",
                             "wait for the weekend"};
    const char *rubric[] = {"fails", "partly works", "works"};
    odk_kernel *kernel = odk_kernel_create();
    odk_result r1, r2;
    odk_error err;
    char *j1, *j2;

    CHECK(kernel != NULL);

    CHECK(odk_evaluate_choice(kernel, "How should we fix the outage?",
                              options, 3, context, NULL, &r1, &err) == ODK_OK);
    CHECK(strcmp(r1.string, "restart the service") == 0);
    CHECK(r1.confidence > 0.0 && r1.confidence <= 1.0);
    j1 = odk_result_to_json(&r1);
    CHECK(odk_evaluate_choice(kernel, "How should we fix the outage?",
                              options, 3, context, NULL, &r2, &err) == ODK_OK);
    j2 = odk_result_to_json(&r2);
    CHECK(j1 != NULL && j2 != NULL && strcmp(j1, j2) == 0); /* deterministic */
    free(j1);
    free(j2);
    odk_result_release(&r1);
    odk_result_release(&r2);

    CHECK(odk_evaluate_noul(kernel, "Restarting the service restored normal operation.",
                            context, NULL, &r1, &err) == ODK_OK);
    CHECK(r1.kind == ODK_VALUE_BOOL && r1.boolean == 1);
    odk_result_release(&r1);
    CHECK(odk_evaluate_noul(kernel, "The database was deleted by the intern.",
                            context, NULL, &r1, &err) == ODK_OK);
    CHECK(r1.boolean == 0);
    odk_result_release(&r1);

    CHECK(odk_evaluate_score(kernel, "How well did the fix work?", rubric, 3,
                             context, NULL, &r1, &err) == ODK_OK);
    CHECK(r1.kind == ODK_VALUE_NUMBER && r1.number >= 0.0 && r1.number <= 1.0);
    odk_result_release(&r1);

    CHECK(odk_evaluate_rank(kernel, "Rank the fixes", options, 3, context,
                            NULL, &r1, &err) == ODK_OK);
    CHECK(r1.kind == ODK_VALUE_STRING || r1.kind == ODK_VALUE_LIST);
    odk_result_release(&r1);

    /* compare needs exactly two options; extract returns snippets */
    CHECK(odk_evaluate_compare(kernel, "Which is better?", options, 2,
                               context, NULL, &r1, &err) == ODK_OK);
    odk_result_release(&r1);
    CHECK(odk_evaluate_extract(kernel, "service restored", context, NULL, 2,
                               &r1, &err) == ODK_OK);
    odk_result_release(&r1);

    /* bad arguments fail cleanly instead of crashing */
    CHECK(odk_evaluate_choice(kernel, "q", options, 0, context, NULL, &r1,
                              &err) != ODK_OK);
    CHECK(odk_evaluate_compare(kernel, "q", options, 3, context, NULL, &r1,
                               &err) != ODK_OK);
    odk_kernel_free(kernel);
}

/* A decisive sentence at the very end of a 90 KB context: found with
 * head+tail, with raised limits, and with the Unicode defaults. */
static void test_long_context(void)
{
    const char *filler = "The weather report mentions rain over the hills tomorrow. ";
    const char *options[] = {"Berlin", "Madrid"};
    size_t flen = strlen(filler);
    size_t cap = 90000 + 200;
    char *context = malloc(cap);
    size_t n = 0;
    int variant;

    CHECK(context != NULL);
    if (context == NULL) {
        return;
    }
    while (n + flen < 90000) {
        memcpy(context + n, filler, flen);
        n += flen;
    }
    strcpy(context + n, "Decisive fact: the backup server lives in Berlin.");

    for (variant = 0; variant < 3; variant++) {
        odk_kernel_config cfg;
        odk_kernel *kernel;
        odk_result result;
        odk_error err;

        memset(&cfg, 0, sizeof(cfg));
        if (variant == 0) {
            cfg.over_limit = ODK_OVERLIMIT_HEAD_TAIL;
        } else if (variant == 1) {
            cfg.context_max = 200000;
            cfg.chunk_source_max = 200000;
            cfg.max_chunks = 10000;
        } else {
            cfg.text_mode = ODK_TEXT_UNICODE;
        }
        kernel = odk_kernel_create_ex(&cfg);
        CHECK(kernel != NULL);
        if (kernel != NULL &&
            odk_evaluate_choice(kernel, "Where does the backup server live?",
                                options, 2, context, NULL, &result,
                                &err) == ODK_OK) {
            CHECK(strcmp(result.string, "Berlin") == 0);
            CHECK(result.distribution.entries[0].value > 0.6);
            odk_result_release(&result);
        }
        odk_kernel_free(kernel);
    }
    free(context);
}

/* Feature set 1, number match (F6): a number counts only when the anchor
 * holds an identical MAXIMAL digit run.  "5" must not match inside "15". */
static double number_dist0(const char *model, const char *const *options,
                           const char *context)
{
    odk_kernel *kernel = odk_kernel_create();
    odk_result result;
    odk_error err;
    double value = -1.0;

    CHECK(kernel != NULL);
    if (kernel == NULL) {
        return value;
    }
    CHECK(odk_kernel_load_model(kernel, model, &err) == ODK_OK);
    if (odk_evaluate_choice(kernel, "How many are there?", options, 2,
                            context, NULL, &result, &err) == ODK_OK) {
        value = result.distribution.entries[0].value;
        odk_result_release(&result);
    }
    odk_kernel_free(kernel);
    return value;
}

static void test_number_feature(void)
{
    const char *zero = "tests/fixtures/fs1_zero_extras.odkmodel";
    const char *num = "tests/fixtures/fs1_number_only.odkmodel";
    const char *options[] = {"5 boxes", "7 boxes"};
    const char *long_opts[] = {"id 1234567890123456789012345678901234567890",
                               "id 9"};
    double off;
    double on;

    /* "5" is inside "15" but is not the number 15: no credit */
    off = number_dist0(zero, options, "The warehouse holds 15 boxes today.");
    on = number_dist0(num, options, "The warehouse holds 15 boxes today.");
    CHECK(off > 0.0 && on == off);

    /* "5" inside "2025" and "250": still no credit */
    off = number_dist0(zero, options, "In 2025 the total was 250 units.");
    on = number_dist0(num, options, "In 2025 the total was 250 units.");
    CHECK(off > 0.0 && on == off);

    /* positive control: an identical run earns credit */
    off = number_dist0(zero, options, "The warehouse holds 5 boxes today.");
    on = number_dist0(num, options, "The warehouse holds 5 boxes today.");
    CHECK(on > off + 0.05);

    /* runs longer than 31 digits are compared whole, not truncated */
    off = number_dist0(zero, long_opts,
                       "Ticket 1234567890123456789012345678901234567890 closed.");
    on = number_dist0(num, long_opts,
                      "Ticket 1234567890123456789012345678901234567890 closed.");
    CHECK(on > off + 0.05);
    off = number_dist0(zero, long_opts,
                       "Ticket 1234567890123456789012345678901234567891 closed.");
    on = number_dist0(num, long_opts,
                      "Ticket 1234567890123456789012345678901234567891 closed.");
    CHECK(on == off);
}

/* Version: the macros agree, the client reports it, and snapshots written by
 * any earlier build (a different semantics_version) still load. */
static void test_version(void)
{
    char version[32];
    char *json = NULL;
    odk_client *client;
    odk_error err;
    char needle[64];
    const char *path = "smoke_test_version_brain.json";
    odk_brain *brain = odk_brain_create();
    odk_brain *loaded;
    char file[4096];
    char *at;
    FILE *fp;
    size_t n;

    snprintf(version, sizeof(version), "%d.%d.%d", ODK_VERSION_MAJOR,
             ODK_VERSION_MINOR, ODK_VERSION_PATCH);
    CHECK(strcmp(version, ODK_LIBRARY_VERSION) == 0);

    snprintf(needle, sizeof(needle), "\"library_version\":\"%s\"",
             ODK_LIBRARY_VERSION);
    client = odk_client_create("cpu");
    CHECK(client != NULL);
    CHECK(odk_client_evaluate_json(
              client, "The backup server runs in Berlin.",
              "{\"k\":{\"primitive\":\"choice\",\"question\":\"Where?\","
              "\"options\":[\"Berlin\",\"Madrid\"]}}",
              "hybrid", &json, &err) == ODK_OK);
    CHECK(json != NULL && strstr(json, needle) != NULL);
    free(json);
    odk_client_free(client);

    /* a snapshot stamped by another library version must still load */
    CHECK(brain != NULL);
    CHECK(odk_brain_record_outcome(brain, "choice", "q", "a", 1.0,
                                   &err) == ODK_OK);
    CHECK(odk_brain_save_file(brain, path, &err) == ODK_OK);
    fp = fopen(path, "rb");
    CHECK(fp != NULL);
    if (fp != NULL) {
        n = fread(file, 1, sizeof(file) - 1, fp);
        fclose(fp);
        file[n] = '\0';
        snprintf(needle, sizeof(needle), "\"semantics_version\":\"%s\"",
                 ODK_LIBRARY_VERSION);
        at = strstr(file, needle);
        CHECK(at != NULL);
        if (at != NULL) {
            /* same length keeps the file valid: rewrite the stamp */
            size_t len = strlen(ODK_LIBRARY_VERSION);
            char *digits = at + strlen("\"semantics_version\":\"");
            memset(digits, '9', len);
            fp = fopen(path, "wb");
            CHECK(fp != NULL);
            if (fp != NULL) {
                CHECK(fwrite(file, 1, n, fp) == n);
                fclose(fp);
            }
            loaded = odk_brain_load_file(path, &err);
            CHECK(loaded != NULL);
            odk_brain_free(loaded);
        }
    }
    remove(path);
    odk_brain_free(brain);
}

static void test_bad_input_is_safe(void)
{
    odk_kernel_config cfg;
    odk_kernel *kernel;
    const char *options[] = {"alpha", "beta"};
    const char bad[] = {'a', 'l', 'p', 'h', 'a', ' ', (char)0xFF, (char)0xFE,
                        (char)0xC0, (char)0x80, ' ', 'x', '\0'};
    odk_result r;
    odk_error err;

    memset(&cfg, 0, sizeof(cfg));
    cfg.text_mode = ODK_TEXT_UNICODE;
    kernel = odk_kernel_create_ex(&cfg);
    CHECK(kernel != NULL);
    /* invalid UTF-8 must not crash; invalid bytes act as separators */
    CHECK(odk_evaluate_choice(kernel, "which?", options, 2, bad, NULL, &r,
                              &err) == ODK_OK);
    odk_result_release(&r);
    CHECK(odk_evaluate_choice(kernel, "which?", options, 2, "", NULL, &r,
                              &err) == ODK_OK);
    odk_result_release(&r);
    odk_kernel_free(kernel);
}

static void test_profiles_and_minifier(void)
{
    char reason[128];
    char *minified;

    CHECK(odk_detect_input_profile("name,age,city\nAda,36,London\nLin,29,Paris\n",
                                   reason, sizeof(reason)) == ODK_PROFILE_CSV);
    CHECK(odk_detect_input_profile("A short plain sentence. Another one.",
                                   reason, sizeof(reason)) == ODK_PROFILE_PLAIN);
    minified = odk_minify("# Title\n\n\n\nBody   text\n");
    CHECK(minified != NULL);
    free(minified);
    CHECK(odk_estimate_tokens("one two three four") > 0);
}

static void test_query_and_client(void)
{
    odk_query q;
    odk_error err;
    odk_client *client;
    char *json = NULL;
    const char *roots[] = {"\"file_name\"", "\"file_type\"", "\"metadata\"",
                           "\"extracted_markdown\"", "\"decisions\""};
    size_t i;

    CHECK(odk_query_from_json("{\"primitive\":\"choice\",\"question\":\"q\","
                              "\"options\":[\"a\",\"b\"]}", &q, &err) == ODK_OK);
    CHECK(q.option_count == 2);
    odk_query_release(&q);
    CHECK(odk_query_from_json("{\"primitive\":\"nonsense\"}", &q, &err) != ODK_OK);
    CHECK(odk_query_from_json("not json", &q, &err) != ODK_OK);

    client = odk_client_create("cpu");
    CHECK(client != NULL);
    CHECK(odk_client_evaluate_json(
              client, "The backup server runs in Berlin.",
              "{\"k\":{\"primitive\":\"choice\",\"question\":\"Where?\","
              "\"options\":[\"Berlin\",\"Madrid\"]}}",
              "hybrid", &json, &err) == ODK_OK);
    CHECK(json != NULL);
    for (i = 0; json != NULL && i < sizeof(roots) / sizeof(roots[0]); i++) {
        CHECK(strstr(json, roots[i]) != NULL); /* five-key contract */
    }
    free(json);
    odk_client_free(client);
}

static void test_image(void)
{
    static const unsigned char white[2 * 2 * 3] = {
        255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    odk_image img;
    char buf[96];
    odk_error err;

    img.width = 2;
    img.height = 2;
    img.rgb = white;
    CHECK(odk_describe_image(&img, buf, sizeof(buf), &err) == ODK_OK);
    CHECK(strncmp(buf, "image", 5) == 0);
    CHECK(odk_describe_image(NULL, buf, sizeof(buf), &err) == ODK_OK);
    CHECK(strcmp(buf, "image") == 0); /* invalid input degrades, never fails */
}

static void test_brain(void)
{
    const char *question = "How do we clear the jam?";
    const char *options[] = {"open the paper tray", "power cycle the machine",
                             "replace the toner"};
    const char *context = "The machine reports a paper jam. Staff usually "
                          "open the paper tray first.";
    const char *path = "smoke_test_brain.json";
    odk_kernel *kernel = odk_kernel_create();
    odk_brain *brain = odk_brain_create();
    odk_brain *restored;
    odk_result base, zero, learned;
    odk_error err;
    double bonus[3];
    char *jb, *jz;
    int i;

    CHECK(kernel != NULL && brain != NULL);
    CHECK(odk_evaluate_choice(kernel, question, options, 3, context, NULL,
                              &base, &err) == ODK_OK);
    CHECK(strcmp(base.string, "open the paper tray") == 0);

    /* weight 0 reproduces the plain kernel bit for bit */
    CHECK(odk_brain_evaluate_choice(brain, kernel, question, options, 3,
                                    context, NULL, 0.0, &zero, &err) == ODK_OK);
    jb = odk_result_to_json(&base);
    jz = odk_result_to_json(&zero);
    CHECK(jb != NULL && jz != NULL && strcmp(jb, jz) == 0);
    free(jb);
    free(jz);
    odk_result_release(&zero);

    for (i = 0; i < 6; i++) {
        CHECK(odk_brain_record_outcome(brain, "choice", question,
                                       "power cycle the machine", 1.0,
                                       &err) == ODK_OK);
        CHECK(odk_brain_record_outcome(brain, "choice", question,
                                       "open the paper tray", -1.0,
                                       &err) == ODK_OK);
    }
    CHECK(odk_brain_evaluate_choice(brain, kernel, question, options, 3,
                                    context, NULL, 1.0, &learned,
                                    &err) == ODK_OK);
    CHECK(strcmp(learned.string, "power cycle the machine") == 0);
    odk_result_release(&learned);
    odk_result_release(&base);

    /* snapshot round trip preserves the experience */
    CHECK(odk_brain_save_file(brain, path, &err) == ODK_OK);
    restored = odk_brain_load_file(path, &err);
    CHECK(restored != NULL);
    if (restored != NULL) {
        CHECK(odk_brain_advise(restored, "choice", question, options, 3,
                               bonus, &err) == ODK_OK);
        CHECK(bonus[0] < 0.0 && bonus[1] > 0.0 && fabs(bonus[2]) < 1e-12);
        odk_brain_free(restored);
    }
    remove(path);
    CHECK(odk_brain_load_file("does_not_exist.json", &err) == NULL);
    odk_brain_free(brain);
    odk_kernel_free(kernel);
}

/* context truncation reporting (odk_result_meta).  A 90 KB
 * context under the default ASCII limits must report
 * context_truncated=1 with the dropped byte count; a short context must
 * report 0/0.  The flag is pure accounting: the five-key result is
 * byte-identical with and without a meta pointer. */
static void test_truncation_reporting(void)
{
    const char *filler =
        "Routine status entries continue with nothing decisive here. ";
    const char *decisive =
        "Final line: the board approves the annual plan.\n";
    const char *options[] = {"annual plan", "monthly plan"};
    const size_t filler_len = strlen(filler);
    size_t cap = 90000 + 128;
    char *context = malloc(cap);
    size_t n = 0;
    odk_kernel *kernel = odk_kernel_create();
    odk_result with_meta;
    odk_result without_meta;
    odk_result short_result;
    odk_result_meta meta;
    odk_error err;

    CHECK(context != NULL && kernel != NULL);
    if (context == NULL || kernel == NULL) {
        free(context);
        odk_kernel_free(kernel);
        return;
    }
    while (n + filler_len + strlen(decisive) < 90000) {
        memcpy(context + n, filler, filler_len);
        n += filler_len;
    }
    strcpy(context + n, decisive);
    n += strlen(decisive);

    memset(&meta, 0, sizeof(meta));
    memset(&with_meta, 0, sizeof(with_meta));
    memset(&without_meta, 0, sizeof(without_meta));
    memset(&err, 0, sizeof(err));
    CHECK(odk_evaluate_choice_opts(kernel, "Which plan did the board "
                                   "approve?", options, 2, context, NULL,
                                   NULL, &with_meta, &meta, &err) == ODK_OK);
    CHECK(odk_evaluate_choice(kernel, "Which plan did the board approve?",
                              options, 2, context, NULL, &without_meta,
                              &err) == ODK_OK);
    CHECK(meta.context_truncated == 1);
    CHECK(meta.context_units_dropped == n - 40000);
    CHECK(meta.context_units_dropped > 0);
    {
        char *j1 = odk_result_to_json(&with_meta);
        char *j2 = odk_result_to_json(&without_meta);
        CHECK(j1 != NULL && j2 != NULL && strcmp(j1, j2) == 0);
        free(j1);
        free(j2);
    }
    CHECK(with_meta.string != NULL && without_meta.string != NULL &&
          strcmp(with_meta.string, without_meta.string) == 0);

    /* short context: no truncation */
    memset(&meta, 0, sizeof(meta));
    memset(&short_result, 0, sizeof(short_result));
    memset(&err, 0, sizeof(err));
    CHECK(odk_evaluate_choice_opts(kernel, "Which plan did the board "
                                   "approve?", options, 2,
                                   "The board approves the annual plan.",
                                   NULL, NULL, &short_result, &meta,
                                   &err) == ODK_OK);
    CHECK(meta.context_truncated == 0);
    CHECK(meta.context_units_dropped == 0);

    /* UNICODE mode: units are code points */
    {
        odk_kernel_config cfg;
        odk_kernel *ukernel;
        odk_result uresult;
        memset(&cfg, 0, sizeof(cfg));
        cfg.text_mode = ODK_TEXT_UNICODE;
        ukernel = odk_kernel_create_ex(&cfg);
        CHECK(ukernel != NULL);
        if (ukernel != NULL) {
            memset(&meta, 0, sizeof(meta));
            memset(&uresult, 0, sizeof(uresult));
            memset(&err, 0, sizeof(err));
            CHECK(odk_evaluate_choice_opts(ukernel, "Which plan did the "
                                           "board approve?", options, 2,
                                           context, NULL, NULL, &uresult,
                                           &meta, &err) == ODK_OK);
            /* 90 KB of ASCII: default UNICODE limits (100,000 cp) do
             * not truncate it */
            CHECK(meta.context_truncated == 0);
            CHECK(meta.context_units_dropped == 0);
            odk_result_release(&uresult);
            odk_kernel_free(ukernel);
        }
    }

    /* work-budget cut also reports truncation */
    {
        odk_kernel_config cfg;
        odk_kernel *wkernel;
        odk_result wresult;
        memset(&cfg, 0, sizeof(cfg));
        cfg.max_work = 500;
        wkernel = odk_kernel_create_ex(&cfg);
        CHECK(wkernel != NULL);
        if (wkernel != NULL) {
            memset(&meta, 0, sizeof(meta));
            memset(&wresult, 0, sizeof(wresult));
            memset(&err, 0, sizeof(err));
            CHECK(odk_evaluate_choice_opts(wkernel, "Which plan did the "
                                           "board approve?", options, 2,
                                           context, NULL, NULL, &wresult,
                                           &meta, &err) == ODK_OK);
            CHECK(meta.work_exceeded == 1);
            CHECK(meta.context_truncated == 1);
            CHECK(meta.context_units_dropped > 0);
            odk_result_release(&wresult);
            odk_kernel_free(wkernel);
        }
    }

    odk_result_release(&with_meta);
    odk_result_release(&without_meta);
    odk_result_release(&short_result);
    odk_kernel_free(kernel);
    free(context);
}

/* explain API exactness tests.  The explained values must
 * recompute the probability from the intermediates and match the
 * primitive's own five-key output.  Rounding replicated from the
 * library (rint, half-even, 4 decimals). */
static double t_round4(double x)
{
    return rint(x * 10000.0) / 10000.0;
}

static double t_clamp01(double x)
{
    return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
}

static void t_expect_dist(const odk_result *result, size_t n,
                          const double *probs, const char *const *labels)
{
    size_t i;
    CHECK(result->distribution.count == n);
    for (i = 0; i < n && i < result->distribution.count; i++) {
        CHECK(strcmp(result->distribution.entries[i].key, labels[i]) == 0);
        CHECK(result->distribution.entries[i].value == t_round4(probs[i]));
    }
}

static void test_explain(void)
{
    odk_kernel *kernel = odk_kernel_create();
    struct {
        const char *instruction;
        const char *options[8];
        const char *context;
    } cases[] = {
        {"How should we fix the outage?",
         {"restart the service", "reboot the database", "ignore it"},
         "The on-call engineer fixed the outage after she decided to "
         "restart the service. Restarting restored normal operation."},
        {"Which city hosts the backup?",
         {"Berlin", "Madrid"},
         "The backup server lives in the Berlin datacenter because it "
         "has redundant power."},
        {"", {"alpha", "beta"}, "alpha alpha and beta evidence"}, /* empty
                                                                     instruction */
        {"Pick one", {"a", "b", "c", "d", "e"}, "no evidence at all"}};
    size_t ci;

    CHECK(kernel != NULL);
    if (kernel == NULL) {
        return;
    }
    for (ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
        const char *const *opts = cases[ci].options;
        size_t nopts = ci == 1 ? 2 : (ci == 2 ? 2 : (ci == 3 ? 5 : 3));
        odk_result result;
        odk_explain ex;
        odk_error err;
        double logits[8];
        double probs[8];
        double peak;
        double total;
        size_t i;
        size_t best = 0;

        memset(&result, 0, sizeof(result));
        memset(&ex, 0, sizeof(ex));
        memset(&err, 0, sizeof(err));
        CHECK(odk_evaluate_choice(kernel, cases[ci].instruction, opts, nopts,
                                  cases[ci].context, NULL, &result,
                                  &err) == ODK_OK);
        CHECK(odk_explain_choice(kernel, cases[ci].instruction, opts, nopts,
                                 cases[ci].context, NULL, &ex, &err) ==
              ODK_OK);
        CHECK(ex.option_count == nopts);
        for (i = 0; i < nopts; i++) {
            const odk_explain_option *o = &ex.options[i];
            double whole_re;
            double ctx_re;
            double fused_re;
            CHECK(o->index == i);
            CHECK(strcmp(o->label, opts[i]) == 0);
            /* channels recompute the blended pair similarity */
            whole_re = t_clamp01(0.35 * o->tf_cosine + 0.35 * o->jaccard +
                                 0.30 * o->trigram);
            CHECK(whole_re == o->whole);
            /* contextual recomputes from whole and best chunk */
            ctx_re = t_clamp01(0.4 * o->whole + 0.6 * o->best_chunk);
            CHECK(ctx_re == o->contextual);
            /* fused recomputes from contextual and directional */
            fused_re = t_clamp01(0.7 * o->contextual +
                                 0.3 * o->directional);
            CHECK(fused_re == o->fused);
            /* calibration recompute (scale 6, stable softmax) */
            logits[i] = 6.0 * (t_clamp01(o->fused) - 0.5);
        }
        peak = logits[0];
        for (i = 1; i < nopts; i++) {
            if (logits[i] > peak) {
                peak = logits[i];
            }
        }
        total = 0.0;
        for (i = 0; i < nopts; i++) {
            probs[i] = exp(logits[i] - peak);
            total += probs[i];
        }
        for (i = 0; i < nopts; i++) {
            probs[i] = probs[i] / total;
            CHECK(ex.options[i].probability == probs[i]);
        }
        /* the recomputed distribution equals the five-key result */
        t_expect_dist(&result, nopts, probs, opts);
        for (i = 1; i < nopts; i++) {
            if (probs[i] > probs[best] &&
                t_clamp01(ex.options[i].fused) >
                    t_clamp01(ex.options[best].fused)) {
                best = i;
            }
        }
        CHECK(strcmp(result.string, opts[best]) == 0);
        CHECK(result.probability == t_round4(probs[best]));
        {
            char *json = odk_explain_to_json(&ex);
            odk_json_value *v;
            CHECK(json != NULL);
            v = json != NULL
                    ? odk_json_parse(json, strlen(json), NULL)
                    : NULL;
            CHECK(v != NULL && v->type == ODK_JSON_OBJECT);
            CHECK(v != NULL &&
                  odk_json_object_get(v, "options") != NULL &&
                  odk_json_object_get(v, "chunk_count") != NULL);
            odk_json_free(v);
            free(json);
        }
        odk_explain_release(&ex);
        odk_result_release(&result);
    }

    /* identical strings: channels saturate */
    {
        const char *opts[1] = {"restart the service"};
        odk_explain ex;
        odk_error err;
        memset(&ex, 0, sizeof(ex));
        memset(&err, 0, sizeof(err));
        CHECK(odk_explain_choice(kernel, "restart the service", opts, 1,
                                 "restart the service now", NULL, &ex,
                                 &err) == ODK_OK);
        CHECK(ex.options[0].tf_cosine > 0.999);
        CHECK(ex.options[0].jaccard == 1.0);
        odk_explain_release(&ex);
    }

    /* score: normalized recomputes from level probabilities */
    {
        const char *rubric[3] = {"low", "medium", "high"};
        odk_result result;
        odk_explain ex;
        odk_error err;
        double normalized = 0.0;
        double peak = 0.0;
        size_t i;
        memset(&result, 0, sizeof(result));
        memset(&ex, 0, sizeof(ex));
        memset(&err, 0, sizeof(err));
        CHECK(odk_evaluate_score(kernel, "How well did the fix work?",
                                 rubric, 3,
                                 "the fix worked well and service recovered",
                                 NULL, &result, &err) == ODK_OK);
        CHECK(odk_explain_score(kernel, "How well did the fix work?", rubric,
                                3, "the fix worked well and service recovered",
                                NULL, &ex, &err) == ODK_OK);
        CHECK(ex.option_count == 3);
        for (i = 0; i < 3; i++) {
            normalized += ex.options[i].probability *
                          ((double)i / 2.0);
            if (ex.options[i].probability > peak) {
                peak = ex.options[i].probability;
            }
            CHECK(strcmp(ex.options[i].label, rubric[i]) == 0);
        }
        CHECK(result.number == t_round4(t_clamp01(normalized)));
        CHECK(ex.score_normalized == t_clamp01(normalized));
        CHECK(ex.score_peak == peak);
        CHECK(result.number == t_round4(ex.score_normalized));
        CHECK(result.distribution.count == 3);
        for (i = 0; i < 3 && i < result.distribution.count; i++) {
            CHECK(strcmp(result.distribution.entries[i].key, rubric[i]) ==
                  0);
            CHECK(result.distribution.entries[i].value ==
                  t_round4(ex.options[i].probability));
        }
        odk_explain_release(&ex);
        odk_result_release(&result);
    }

    /* noul: affirm recomputes from evidence */
    {
        odk_result result;
        odk_explain ex;
        odk_error err;
        double evidence;
        double affirm;
        memset(&result, 0, sizeof(result));
        memset(&ex, 0, sizeof(ex));
        memset(&err, 0, sizeof(err));
        CHECK(odk_evaluate_noul(kernel,
                                "Restarting the service restored normal "
                                "operation.",
                                "Restarting the service restored normal "
                                "operation after the outage.",
                                NULL, &result, &err) == ODK_OK);
        CHECK(odk_explain_noul(kernel,
                               "Restarting the service restored normal "
                               "operation.",
                               "Restarting the service restored normal "
                               "operation after the outage.",
                               NULL, &ex, &err) == ODK_OK);
        evidence = ex.noul_whole > ex.noul_best_chunk ? ex.noul_whole
                                                      : ex.noul_best_chunk;
        CHECK(evidence == ex.noul_evidence);
        affirm = evidence >= 0.0 ? 1.0 / (1.0 + exp(-8.0 * (evidence - 0.5)))
                                 : 0.0;
        affirm = t_clamp01(affirm);
        CHECK(affirm == ex.noul_affirm);
        CHECK(result.probability == t_round4(affirm));
        CHECK(result.boolean == (affirm >= 0.5 ? 1 : 0));
        CHECK(ex.option_count == 0);
        {
            char *json = odk_explain_to_json(&ex);
            CHECK(json != NULL && strstr(json, "\"noul\"") != NULL);
            free(json);
        }
        odk_explain_release(&ex);
        odk_result_release(&result);
    }

    /* error paths */
    {
        odk_explain ex;
        odk_error err;
        memset(&ex, 0, sizeof(ex));
        memset(&err, 0, sizeof(err));
        CHECK(odk_explain_choice(kernel, "x", NULL, 0, "ctx", NULL, &ex,
                                 &err) == ODK_ERR_INFERENCE);
        CHECK(odk_explain_noul(kernel, "  ", "ctx", NULL, &ex, &err) ==
              ODK_ERR_INFERENCE);
        odk_explain_release(&ex); /* release on zeroed struct is a no-op */
    }
    odk_kernel_free(kernel);
}

/* model file v1.  Loading the default constants as a file must
 * reproduce the no-model behaviour bit-for-bit; every corruption class
 * in docs/MODEL.md must be rejected with the model left unchanged. */
static void test_model(void)
{
    const char *fixture = "tests/fixtures/default_model.odkmodel";
    const char *options[] = {"restart the service", "reboot the database"};
    const char *instruction = "How should we fix the outage?";
    const char *context =
        "The engineer decided to restart the service and the outage was "
        "fixed immediately after the restart.";
    const int N = 12;
    char no_model[12][256];
    odk_kernel *k1 = odk_kernel_create();
    odk_kernel *k2 = odk_kernel_create();
    odk_error err;
    int i;
    int all_equal = 1;

    CHECK(k1 != NULL && k2 != NULL);
    if (k1 == NULL || k2 == NULL) {
        odk_kernel_free(k1);
        odk_kernel_free(k2);
        return;
    }
    /* battery of contexts (fixed; no RNG) */
    for (i = 0; i < N; i++) {
        odk_result r;
        char ctx[512];
        size_t j;
        for (j = 0; j < (size_t)i * 25; j++) {
            ctx[j] = (char)('a' + (j * 7 + (size_t)i) % 23);
        }
        ctx[(size_t)i * 25] = '\0';
        memcpy(ctx, "the board approves the annual plan for the team",
                48);
        memset(&r, 0, sizeof(r));
        memset(&err, 0, sizeof(err));
        if (odk_evaluate_choice(k1, instruction, options, 2, ctx, NULL, &r,
                                &err) != ODK_OK) {
            all_equal = 0;
            continue;
        }
        snprintf(no_model[i], sizeof(no_model[i]), "%s", r.string);
        {
            char *j1 = odk_result_to_json(&r);
            snprintf(no_model[i], sizeof(no_model[i]), "%s", j1);
            free(j1);
        }
        odk_result_release(&r);
    }
    CHECK(odk_kernel_load_model(k2, fixture, &err) == ODK_OK);
    for (i = 0; i < N && all_equal; i++) {
        odk_result r;
        char ctx[512];
        char *j2;
        size_t j;
        for (j = 0; j < (size_t)i * 25; j++) {
            ctx[j] = (char)('a' + (j * 7 + (size_t)i) % 23);
        }
        ctx[(size_t)i * 25] = '\0';
        memcpy(ctx, "the board approves the annual plan for the team",
                48);
        memset(&r, 0, sizeof(r));
        memset(&err, 0, sizeof(err));
        if (odk_evaluate_choice(k2, instruction, options, 2, ctx, NULL, &r,
                                &err) != ODK_OK) {
            all_equal = 0;
            break;
        }
        j2 = odk_result_to_json(&r);
        if (j2 == NULL || strcmp(j2, no_model[i]) != 0) {
            all_equal = 0;
        }
        free(j2);
        odk_result_release(&r);
    }
    CHECK(all_equal);

    /* rejection classes (docs/MODEL.md, loader rules) */
    {
        static const char *const NAMES[] = {
            "tests/fixtures/corrupt_bad_magic.odkmodel",
            "tests/fixtures/corrupt_version.odkmodel",
            "tests/fixtures/corrupt_nan.odkmodel",
            "tests/fixtures/corrupt_hash.odkmodel",
            "tests/fixtures/corrupt_truncated.odkmodel",
            "tests/fixtures/corrupt_oversized.odkmodel",
            "tests/fixtures/corrupt_random.odkmodel",
            "tests/fixtures/corrupt_negscale.odkmodel",
            "tests/fixtures/corrupt_v2_reserved.odkmodel"};
        size_t c;
        for (c = 0; c < sizeof(NAMES) / sizeof(NAMES[0]); c++) {
            memset(&err, 0, sizeof(err));
            CHECK(odk_kernel_load_model(k2, NAMES[c], &err) ==
                  ODK_ERR_ARGUMENT);
        }
        /* after all rejections the loaded default model still decides
         * identically to the never-loaded kernel */
        {
            odk_result r1;
            odk_result r2;
            char *j1;
            char *j2;
            memset(&r1, 0, sizeof(r1));
            memset(&r2, 0, sizeof(r2));
            memset(&err, 0, sizeof(err));
            CHECK(odk_evaluate_choice(k1, instruction, options, 2, context,
                                      NULL, &r1, &err) == ODK_OK);
            CHECK(odk_evaluate_choice(k2, instruction, options, 2, context,
                                      NULL, &r2, &err) == ODK_OK);
            j1 = odk_result_to_json(&r1);
            j2 = odk_result_to_json(&r2);
            CHECK(j1 != NULL && j2 != NULL && strcmp(j1, j2) == 0);
            free(j1);
            free(j2);
            odk_result_release(&r1);
            odk_result_release(&r2);
        }
    }
    CHECK(odk_kernel_load_model(k2, "tests/fixtures/no_such_file.odkmodel",
                                &err) == ODK_ERR_INFERENCE);
    odk_kernel_free(k1);
    odk_kernel_free(k2);
}

int main(void)
{
    CHECK(strlen(ODK_LIBRARY_VERSION) > 0);
    CHECK(strcmp(ODK_UNICODE_VERSION, "15.1.0") == 0);
    test_primitives_and_determinism();
    test_languages();
    test_long_context();
    test_truncation_reporting();
    test_explain();
    test_model();
    test_number_feature();
    test_version();
    test_bad_input_is_safe();
    test_profiles_and_minifier();
    test_query_and_client();
    test_image();
    test_brain();
    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
