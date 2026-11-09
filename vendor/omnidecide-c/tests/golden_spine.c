/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * golden_spine.c - golden-corpus runner for the v2.0.0 spine engines.
 *
 * The corpus is DATA (golden/spine/inputs.jsonl), loaded through the
 * documented extension API — the runner itself stays generic. Every
 * feature lands with its corpus additions in the same change; corpus
 * diffs are reviewed like code (see golden/spine/README.md).
 *
 *   golden_spine check   compare outputs against golden/spine/expected.jsonl
 *   golden_spine update  regenerate golden/spine/expected.jsonl
 *
 * Output bytes are deterministic: the same inputs and registered data
 * produce byte-identical documents, which is exactly what the
 * determinism gate (make determinism) re-checks across repeated runs.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "odk_spine.h"

static const char *g_data_dir = "examples/synthetic/data";

/* Local file reader (tests do not use spine internals). */
static char *gs_read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long sz;
    char *buf;

    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (sz = ftell(f)) < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = malloc((size_t)sz + 1);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf);
        fclose(f);
        return NULL;
    }
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

/* ------------------------------------------------------------------ */
/* Output buffer                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    char *data;
    size_t len, cap;
    int oom;
} obuf;

static void obuf_init(obuf *b)
{
    b->cap = 1024;
    b->len = 0;
    b->oom = 0;
    b->data = malloc(b->cap);
    if (b->data != NULL) {
        b->data[0] = '\0';
    } else {
        b->oom = 1;
    }
}

static void obuf_puts(obuf *b, const char *s)
{
    size_t n = strlen(s);

    if (b->oom) {
        return;
    }
    if (b->len + n + 1 > b->cap) {
        size_t ncap = b->cap;
        char *nd;

        while (ncap < b->len + n + 1) {
            ncap *= 2;
        }
        nd = realloc(b->data, ncap);
        if (nd == NULL) {
            b->oom = 1;
            return;
        }
        b->data = nd;
        b->cap = ncap;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void obuf_line(obuf *b, const char *s)
{
    obuf_puts(b, s);
    obuf_puts(b, "\n");
}

/* ------------------------------------------------------------------ */
/* Case execution                                                      */
/* ------------------------------------------------------------------ */

static void get_str(const odk_json_value *obj, const char *key,
                    const char **out)
{
    const odk_json_value *v = odk_json_object_get(obj, key);

    *out = (v != NULL && v->type == ODK_JSON_STRING) ? v->string : "";
}

static void get_int(const odk_json_value *obj, const char *key,
                    long long *out)
{
    const odk_json_value *v = odk_json_object_get(obj, key);

    *out = (v != NULL && v->type == ODK_JSON_NUMBER && v->is_integer)
               ? v->integer
               : 0;
}

static void run_case(odk_spine *spine, odk_kernel *kernel,
                     const odk_json_value *kase, obuf *out)
{
    const char *kind;
    const odk_json_value *input;
    const char *s;
    long long t, version, tier_b;

    get_str(kase, "kind", &kind);
    input = odk_json_object_get(kase, "input");
    if (input == NULL || input->type != ODK_JSON_OBJECT) {
        obuf_line(out, "{\"error\":\"case input missing\"}");
        return;
    }

    if (strcmp(kind, "classify") == 0) {
        odk_spine_classification_result result;
        char *json;

        get_str(input, "subject_ref", &s);
        {
            const char *item, *scope;

            get_str(input, "item_text", &item);
            get_str(input, "scope", &scope);
            get_int(input, "t", &t);
            get_int(input, "tier_b", &tier_b);
            odk_spine_set_tier_b(spine, tier_b ? 1 : 0);
            if (odk_spine_classify(spine, s, item, scope, (int64_t)t,
                                   &result, NULL) != ODK_SPINE_OK) {
                obuf_line(out, "{\"error\":\"classify failed\"}");
                return;
            }
            odk_spine_set_tier_b(spine, 0);
        }
        json = odk_spine_classification_result_to_json(&result);
        odk_spine_classification_result_release(&result);
        obuf_line(out, json != NULL ? json : "{\"error\":\"oom\"}");
        free(json);
    } else if (strcmp(kind, "asof") == 0) {
        const odk_spine_classification_interval *iv;
        const char *subject, *scheme;
        char *json;

        get_str(input, "subject_ref", &subject);
        get_str(input, "scheme_id", &scheme);
        get_int(input, "t", &t);
        if (odk_spine_classification_at(spine, subject, scheme, (int64_t)t,
                                        &iv, NULL) != ODK_SPINE_OK) {
            obuf_line(out, "null");
            return;
        }
        json = odk_spine_classification_interval_to_json(iv);
        obuf_line(out, json != NULL ? json : "null");
        free(json);
    } else if (strcmp(kind, "decompose") == 0) {
        odk_spine_components comps;
        const char *tmpl, *item, *params;
        char *json;

        get_str(input, "template_id", &tmpl);
        get_str(input, "item_text", &item);
        get_str(input, "params", &params);
        get_int(input, "version", &version);
        get_int(input, "t", &t);
        if (odk_spine_decompose(spine, tmpl, (int64_t)version, item,
                                params[0] != '\0' ? params : NULL,
                                (int64_t)t, &comps, NULL) != ODK_SPINE_OK) {
            obuf_line(out, "null");
            return;
        }
        json = odk_spine_components_to_json(&comps);
        odk_spine_components_release(&comps);
        obuf_line(out, json != NULL ? json : "null");
        free(json);
    } else if (strcmp(kind, "pin") == 0) {
        odk_spine_decision_pin pin;
        const char *request, *inputs_ref, *replay_class;
        char *json;

        get_str(input, "request", &request);
        get_str(input, "inputs_ref", &inputs_ref);
        get_str(input, "replay_class", &replay_class);
        get_int(input, "timestamp", &t);
        if (odk_spine_pin(spine, kernel, request, inputs_ref, replay_class,
                          (int64_t)t, &pin, NULL) != ODK_SPINE_OK) {
            obuf_line(out, "null");
            return;
        }
        json = odk_spine_decision_pin_to_json(&pin);
        odk_spine_decision_pin_release(&pin);
        obuf_line(out, json != NULL ? json : "null");
        free(json);
    } else if (strcmp(kind, "replay") == 0) {
        const char *pin_id;
        char *report = NULL;

        get_str(input, "pin_id", &pin_id);
        if (odk_spine_replay(spine, kernel, pin_id, &report, NULL) !=
            ODK_SPINE_OK) {
            obuf_line(out, "null");
            return;
        }
        obuf_line(out, report != NULL ? report : "null");
        free(report);
    } else if (strcmp(kind, "observation_ingest") == 0) {
        const char *json;

        get_str(input, "json", &json);
        if (odk_spine_observation_ingest(spine, json, NULL) != ODK_SPINE_OK) {
            obuf_line(out, "{\"ingested\":false}");
            return;
        }
        obuf_line(out, "{\"ingested\":true}");
    } else if (strcmp(kind, "calibrate") == 0) {
        odk_spine_calibration_report report;
        const char *from;
        char *json;

        get_str(input, "generated_from", &from);
        if (odk_spine_calibrate(spine, from, &report, NULL) != ODK_SPINE_OK) {
            obuf_line(out, "null");
            return;
        }
        json = odk_spine_calibration_report_to_json(&report);
        odk_spine_calibration_report_release(&report);
        obuf_line(out, json != NULL ? json : "null");
        free(json);
    } else if (strcmp(kind, "worldstate") == 0) {
        const char *json;

        get_str(input, "json", &json);
        obuf_line(out, odk_spine_worldstate_validate(json, NULL) ==
                               ODK_SPINE_OK
                           ? "{\"valid\":true}"
                           : "{\"valid\":false}");
    } else {
        obuf_line(out, "{\"error\":\"unknown case kind\"}");
    }
}

/* ------------------------------------------------------------------ */
/* Main                                                                */
/* ------------------------------------------------------------------ */

static int run(int update)
{
    odk_spine *spine;
    odk_kernel *kernel;
    odk_brain *brain;
    odk_spine_error err;
    char path[512];
    char *inputs_text;
    FILE *expected = NULL;
    obuf got;
    int case_no = 0;
    char *p;

    spine = odk_spine_create();
    kernel = odk_kernel_create();
    brain = odk_brain_create();
    if (spine == NULL || kernel == NULL || brain == NULL) {
        printf("golden_spine: init failed\n");
        return 2;
    }

    /* registered customization state: the synthetic example fixtures,
     * loaded through the F7 extension API (the corpus harness stays
     * generic — corpora and schemes are data) */
    snprintf(path, sizeof(path), "%s/schemes.json", g_data_dir);
    if (odk_spine_register_file(spine, ODK_SPINE_KIND_SCHEME_SET, path,
                                &err) != ODK_SPINE_OK) {
        printf("golden_spine: cannot register %s: %s\n", path, err.text);
        return 2;
    }
    snprintf(path, sizeof(path), "%s/templates.json", g_data_dir);
    if (odk_spine_register_file(spine, ODK_SPINE_KIND_TEMPLATE, path,
                                &err) != ODK_SPINE_OK) {
        printf("golden_spine: cannot register %s: %s\n", path, err.text);
        return 2;
    }
    snprintf(path, sizeof(path), "%s/vocabularies.json", g_data_dir);
    if (odk_spine_register_file(spine, ODK_SPINE_KIND_VOCABULARY, path,
                                &err) != ODK_SPINE_OK) {
        printf("golden_spine: cannot register %s: %s\n", path, err.text);
        return 2;
    }
    snprintf(path, sizeof(path), "%s/corpus.jsonl", g_data_dir);
    if (odk_spine_register_file(spine, ODK_SPINE_KIND_CORPUS, path,
                                &err) != ODK_SPINE_OK) {
        printf("golden_spine: cannot register %s: %s\n", path, err.text);
        return 2;
    }
    if (odk_spine_register_brain(spine, brain, "golden-brain-1", &err) !=
        ODK_SPINE_OK) {
        printf("golden_spine: cannot register brain: %s\n", err.text);
        return 2;
    }

    inputs_text = gs_read_file("golden/spine/inputs.jsonl");
    if (inputs_text == NULL) {
        printf("golden_spine: cannot read golden/spine/inputs.jsonl\n");
        return 2;
    }
    obuf_init(&got);

    /* each non-empty line is one case; executed in order */
    p = inputs_text;
    while (*p != '\0') {
        char *eol = strchr(p, '\n');
        size_t len = eol != NULL ? (size_t)(eol - p) : strlen(p);
        odk_json_value *kase;
        odk_error jerr;

        if (len > 0) {
            kase = odk_json_parse(p, len, &jerr);
            if (kase == NULL) {
                printf("golden_spine: inputs line %d: invalid JSON: %s\n",
                       case_no + 1, jerr.text);
                return 2;
            }
            run_case(spine, kernel, kase, &got);
            odk_json_free(kase);
            case_no++;
        }
        if (eol == NULL) {
            break;
        }
        p = eol + 1;
    }

    if (update) {
        expected = fopen("golden/spine/expected.jsonl", "wb");
        if (expected == NULL) {
            printf("golden_spine: cannot write expected.jsonl\n");
            return 2;
        }
        fwrite(got.data, 1, got.len, expected);
        fclose(expected);
        printf("golden_spine: updated %d cases\n", case_no);
    } else {
        char *expected_text;

        expected_text = gs_read_file("golden/spine/expected.jsonl");
        if (expected_text == NULL) {
            printf("golden_spine: cannot read expected.jsonl "
                   "(run: make spine-golden-update)\n");
            return 2;
        }
        if (strcmp(expected_text, got.data) == 0) {
            printf("golden_spine: %d cases, all byte-identical\n", case_no);
        } else {
            size_t i, n = strlen(expected_text) < got.len
                              ? strlen(expected_text)
                              : got.len;
            size_t diff = 0;

            for (i = 0; i < n; i++) {
                if (expected_text[i] != got.data[i]) {
                    diff = i;
                    break;
                }
                diff = i + 1;
            }
            printf("golden_spine: CORPUS DIFF at byte %zu\n"
                   "  expected: %.90s\n"
                   "  got:      %.90s\n",
                   diff, expected_text + diff, got.data + diff);
            free(expected_text);
            free(got.data);
            return 1;
        }
        free(expected_text);
    }

    free(inputs_text);
    free(got.data);
    odk_brain_free(brain);
    odk_kernel_free(kernel);
    odk_spine_free(spine);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "update") == 0) {
        return run(1);
    }
    return run(0);
}
