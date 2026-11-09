/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * spine_test.c - tests for the v2.0.0 spine: generated schema types,
 * registries, extension API, classification, decomposition, calibration,
 * pinning and replay. Exit status 0 = all checks passed.
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "odk_spine.h"

static int g_checks;
static int g_failures;

#define CHECK(cond)                                                          \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
            fflush(stdout);                                                  \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------ */
/* Generated schema types (F5)                                         */
/* ------------------------------------------------------------------ */

static const char *g_scheme_json =
    "{\"id\":\"fixture-triage\",\"version\":2,\"valid_from\":1000,"
    "\"valid_to\":-1,\"default_label\":\"unsorted\","
    "\"facet_names\":[\"severity\",\"area\"],"
    "\"rules\":["
    "{\"label\":\"hot\",\"match_terms\":[\"frobnication\",\"overheat\"],"
    "\"any_term\":true,\"priority\":10,"
    "\"facets\":{\"severity\":\"high\",\"area\":\"thermal\"}},"
    "{\"label\":\"cold\",\"match_terms\":[\"slow\",\"stall\"],"
    "\"any_term\":false,\"priority\":5,"
    "\"facets\":{\"severity\":\"low\",\"area\":\"compute\"}}"
    "],\"notes\":\"synthetic fixture\"}";

static void test_schema_types(void)
{
    odk_spine_scheme scheme;
    odk_spine_scheme scheme2;
    odk_spine_error err;
    char *json1;
    char *json2;

    CHECK(odk_spine_scheme_from_json(g_scheme_json, &scheme, &err) ==
          ODK_SPINE_OK);
    CHECK(strcmp(scheme.id, "fixture-triage") == 0);
    CHECK(scheme.version == 2);
    CHECK(scheme.valid_to == -1);
    CHECK(scheme.rules.count == 2);
    CHECK(strcmp(scheme.rules.items[0].facets.items[0].key, "severity") == 0);

    /* writer output must be byte-identical to the canonical input */
    json1 = odk_spine_scheme_to_json(&scheme);
    CHECK(json1 != NULL);
    CHECK(strcmp(json1, g_scheme_json) == 0);

    /* round trip: parse -> write -> parse -> write is byte-stable */
    CHECK(odk_spine_scheme_from_json(json1, &scheme2, &err) == ODK_SPINE_OK);
    json2 = odk_spine_scheme_to_json(&scheme2);
    CHECK(json2 != NULL);
    CHECK(strcmp(json1, json2) == 0);

    /* exact int64 handling (no float drift on big timestamps) */
    {
        odk_spine_observation obs;
        char *obs_json;
        const char *text =
            "{\"observation_id\":\"obs-1\",\"kind\":\"classification\","
            "\"t\":1730611200000,\"scope\":\"\",\"subject_ref\":\"item-9\","
            "\"primitive\":\"\",\"scheme_id\":\"\",\"payload_json\":\"{}\","
            "\"confidence\":0.5,"
            "\"tier\":\"A\",\"brain_snapshot_id\":\"\",\"outcome\":\"\","
            "\"kernel_version\":\"2.0.0\",\"schema_fp\":\"ab12cd34ef56ab78\","
            "\"registrations\":[]}";
        CHECK(odk_spine_observation_from_json(text, &obs, &err) ==
              ODK_SPINE_OK);
        CHECK(obs.t == 1730611200000LL);
        obs_json = odk_spine_observation_to_json(&obs);
        CHECK(obs_json != NULL);
        CHECK(strcmp(obs_json, text) == 0);
        free(obs_json);
        odk_spine_observation_release(&obs);
    }

    /* strictness: unknown field, missing field, wrong type, bad enum */
    {
        odk_spine_scheme s;
        const char *bad1 =
            "{\"id\":\"x\",\"version\":1,\"valid_from\":0,\"valid_to\":-1,"
            "\"default_label\":\"\",\"facet_names\":[],\"rules\":[],"
            "\"notes\":\"\",\"surprise\":1}";
        const char *bad2 =
            "{\"id\":\"x\",\"version\":1,\"valid_from\":0,\"valid_to\":-1,"
            "\"default_label\":\"\",\"facet_names\":[],\"rules\":[]}";
        const char *bad3 =
            "{\"id\":\"x\",\"version\":\"one\",\"valid_from\":0,"
            "\"valid_to\":-1,\"default_label\":\"\",\"facet_names\":[],"
            "\"rules\":[],\"notes\":\"\"}";
        const char *bad4 = "not json at all";
        CHECK(odk_spine_scheme_from_json(bad1, &s, &err) ==
              ODK_SPINE_ERR_SCHEMA);
        CHECK(odk_spine_scheme_from_json(bad2, &s, &err) ==
              ODK_SPINE_ERR_SCHEMA);
        CHECK(odk_spine_scheme_from_json(bad3, &s, &err) ==
              ODK_SPINE_ERR_SCHEMA);
        CHECK(odk_spine_scheme_from_json(bad4, &s, &err) ==
              ODK_SPINE_ERR_SCHEMA);
    }

    free(json1);
    free(json2);
    odk_spine_scheme_release(&scheme);
    odk_spine_scheme_release(&scheme2);
}

/* ------------------------------------------------------------------ */
/* Spine handle basics                                                 */
/* ------------------------------------------------------------------ */

static void test_spine_handle(void)
{
    odk_spine *spine = odk_spine_create();

    CHECK(spine != NULL);
    CHECK(strcmp(odk_spine_schema_fingerprint(spine),
                 ODK_SPINE_SCHEMA_FINGERPRINT) == 0);
    CHECK(strlen(odk_spine_schema_fingerprint(spine)) == 16);
    odk_spine_free(spine);
    odk_spine_free(NULL); /* must not crash */
}

/* ------------------------------------------------------------------ */
/* F7 extension API                                                    */
/* ------------------------------------------------------------------ */

static const char *g_vocab_json =
    "{\"id\":\"fixture-synonyms\",\"version\":1,\"valid_from\":0,"
    "\"valid_to\":-1,\"groups\":["
    "{\"canonical\":\"frobnication\",\"terms\":[\"frob failure\","
    "\"frobnicate broke\",\"frobber malfunction\"]}"
    "]}";

static int g_provider_calls;

static int provider_vocabulary(void *user, char **out_text,
                               odk_spine_error *err)
{
    (void)user;
    (void)err;
    g_provider_calls++;
    *out_text = strdup(g_vocab_json);
    return *out_text == NULL ? -1 : 0;
}

static void test_extension_api(void)
{
    odk_spine *spine = odk_spine_create();
    odk_spine_error err;
    odk_spine_registration_ref_list regs;
    const odk_spine_scheme *scheme;
    const odk_spine_vocabulary *vocab;

    /* fresh build: zero registered customizations */
    CHECK(odk_spine_registrations(spine, &regs, &err) == ODK_SPINE_OK);
    CHECK(regs.count == 0);
    odk_spine_registration_ref_list_release(&regs);

    /* register a scheme set from JSON text */
    CHECK(odk_spine_register_json(spine, ODK_SPINE_KIND_SCHEME_SET,
                                  g_scheme_json, "fixture://schemes",
                                  &err) == ODK_SPINE_OK);
    CHECK(odk_spine_registrations(spine, &regs, &err) == ODK_SPINE_OK);
    CHECK(regs.count == 1);
    CHECK(strcmp(regs.items[0].kind, "scheme_set") == 0);
    CHECK(strcmp(regs.items[0].id, "fixture-triage") == 0);
    CHECK(regs.items[0].version == 2);
    CHECK(strlen(regs.items[0].content_hash) == 16);
    odk_spine_registration_ref_list_release(&regs);

    /* identical re-registration is idempotent (no duplicate) */
    CHECK(odk_spine_register_json(spine, ODK_SPINE_KIND_SCHEME_SET,
                                  g_scheme_json, "fixture://schemes",
                                  &err) == ODK_SPINE_OK);
    CHECK(odk_spine_registrations(spine, &regs, &err) == ODK_SPINE_OK);
    CHECK(regs.count == 1);
    odk_spine_registration_ref_list_release(&regs);

    /* as-of lookup */
    CHECK(odk_spine_scheme_at(spine, "fixture-triage", -1, 5000, &scheme,
                              &err) == ODK_SPINE_OK);
    CHECK(scheme->version == 2);
    CHECK(strcmp(scheme->default_label, "unsorted") == 0);
    CHECK(odk_spine_scheme_at(spine, "fixture-triage", 2, 5000, &scheme,
                              &err) == ODK_SPINE_OK);
    CHECK(scheme->version == 2);
    CHECK(odk_spine_scheme_at(spine, "fixture-triage", -1, 5000, &scheme,
                              &err) == ODK_SPINE_OK);
    CHECK(odk_spine_scheme_at(spine, "no-such-scheme", -1, 5000, &scheme,
                              &err) == ODK_SPINE_ERR_NOT_FOUND);

    /* provider path */
    CHECK(odk_spine_register_provider(spine, ODK_SPINE_KIND_VOCABULARY,
                                      provider_vocabulary, NULL,
                                      "fixture://vocab", &err) ==
          ODK_SPINE_OK);
    CHECK(g_provider_calls == 1);
    CHECK(odk_spine_vocabulary_at(spine, "fixture-synonyms", -1, 0, &vocab,
                                  &err) == ODK_SPINE_OK);
    CHECK(vocab->groups.count == 1);

    /* referential integrity: facet outside facet_names must fail */
    {
        const char *bad =
            "{\"id\":\"bad-scheme\",\"version\":1,\"valid_from\":0,"
            "\"valid_to\":-1,\"default_label\":\"x\","
            "\"facet_names\":[\"a\"],"
            "\"rules\":[{\"label\":\"r\",\"match_terms\":[\"t\"],"
            "\"any_term\":true,\"priority\":1,"
            "\"facets\":{\"not-a-facet\":\"v\"}}],\"notes\":\"\"}";
        CHECK(odk_spine_register_json(spine, ODK_SPINE_KIND_SCHEME_SET, bad,
                                      NULL, &err) ==
              ODK_SPINE_ERR_REFERENTIAL);
    }
    /* same version, different content: rejected */
    {
        const char *mutated =
            "{\"id\":\"fixture-triage\",\"version\":2,\"valid_from\":1000,"
            "\"valid_to\":-1,\"default_label\":\"changed\","
            "\"facet_names\":[\"severity\",\"area\"],\"rules\":[],"
            "\"notes\":\"synthetic fixture\"}";
        CHECK(odk_spine_register_json(spine, ODK_SPINE_KIND_SCHEME_SET,
                                      mutated, NULL, &err) ==
              ODK_SPINE_ERR_STATE);
    }
    /* malformed JSON: rejected */
    {
        CHECK(odk_spine_register_json(spine, ODK_SPINE_KIND_SCHEME_SET, "{",
                                      NULL, &err) == ODK_SPINE_ERR_SCHEMA);
        CHECK(odk_spine_register_json(spine, "unknown_kind", "{}", NULL,
                                      &err) == ODK_SPINE_ERR_ARGUMENT);
    }

    /* corpus registration: JSONL accepted, garbage rejected */
    {
        const char *corpus =
            "{\"case\":1}\n{\"case\":2}\n";
        CHECK(odk_spine_register_json(spine, ODK_SPINE_KIND_CORPUS, corpus,
                                      "fixture://corpus", &err) ==
              ODK_SPINE_OK);
        CHECK(odk_spine_register_json(spine, ODK_SPINE_KIND_CORPUS,
                                      "{\"a\":1}\nbogus\n", NULL,
                                      &err) == ODK_SPINE_ERR_SCHEMA);
    }

    /* brain snapshot registration */
    {
        odk_brain *brain = odk_brain_create();

        CHECK(brain != NULL);
        CHECK(odk_spine_register_brain(spine, brain, "fixture-brain-1",
                                       &err) == ODK_SPINE_OK);
        CHECK(odk_spine_brain_at(spine, "fixture-brain-1") == brain);
        CHECK(odk_spine_brain_at(spine, "missing") == NULL);
        CHECK(odk_spine_register_brain(spine, brain, "fixture-brain-1",
                                       &err) == ODK_SPINE_ERR_STATE);
        CHECK(odk_spine_registrations(spine, &regs, &err) == ODK_SPINE_OK);
        CHECK(regs.count == 4); /* scheme, vocab, corpus, brain */
        CHECK(strcmp(regs.items[3].kind, "brain_snapshot") == 0);
        odk_spine_registration_ref_list_release(&regs);
        odk_brain_free(brain);
    }

    odk_spine_free(spine);
}


/* ------------------------------------------------------------------ */
/* F1 classify + F2 temporal validity                                  */
/* ------------------------------------------------------------------ */

static const char *g_scheme2_json =
    "{\"id\":\"fixture-impact\",\"version\":1,\"valid_from\":0,"
    "\"valid_to\":-1,\"default_label\":\"routine\","
    "\"facet_names\":[\"blast\"],"
    "\"rules\":["
    "{\"label\":\"wide\",\"match_terms\":[\"many users\",\"outage\"],"
    "\"any_term\":true,\"priority\":3,\"facets\":{\"blast\":\"wide\"}}"
    "],\"notes\":\"synthetic fixture\"}";

static void test_classify_tier_a(void)
{
    odk_spine *spine = odk_spine_create();
    odk_spine_error err;
    odk_spine_classification_result result;
    char *json1;
    char *json2;

    CHECK(odk_spine_register_json(spine, ODK_SPINE_KIND_SCHEME_SET,
                                  g_scheme_json, NULL, &err) == ODK_SPINE_OK);

    /* direct lexical hit: rule 0 (priority 10, any_term, both terms) */
    CHECK(odk_spine_classify(spine, "item-1",
                             "frobnication overheat on the thermal unit", "",
                             1000, &result, &err) == ODK_SPINE_OK);
    CHECK(result.classifications.count == 1);
    CHECK(strcmp(result.classifications.items[0].label, "hot") == 0);
    CHECK(strcmp(result.classifications.items[0].tier, "A") == 0);
    CHECK(strcmp(result.classifications.items[0].brain_snapshot_id, "") == 0);
    CHECK(result.classifications.items[0].rule_index == 0);
    CHECK(result.classifications.items[0].facets.count == 2);
    CHECK(strcmp(result.classifications.items[0].facets.items[0].key,
                 "severity") == 0);
    CHECK(strcmp(result.classifications.items[0].facets.items[0].value,
                 "high") == 0);
    CHECK(result.classifications.items[0].matched_terms.count == 2);

    /* write-back happened: one classification observation + one interval */
    CHECK(odk_spine_observation_count(spine) == 1);
    CHECK(strcmp(odk_spine_observation_at(spine, 0)->kind,
                 "classification") == 0);
    CHECK(strcmp(odk_spine_observation_at(spine, 0)->scheme_id,
                 "fixture-triage") == 0);
    CHECK(odk_spine_interval_count(spine) == 1);
    json1 = odk_spine_classification_result_to_json(&result);
    odk_spine_classification_result_release(&result);

    /* determinism: same input bytes -> same result bytes (the result
     * document embeds t, so the replay uses the identical input) */
    CHECK(odk_spine_classify(spine, "item-1",
                             "frobnication overheat on the thermal unit", "",
                             1000, &result, &err) == ODK_SPINE_OK);
    json2 = odk_spine_classification_result_to_json(&result);
    CHECK(strcmp(json1, json2) == 0);
    free(json1);
    free(json2);
    odk_spine_classification_result_release(&result);

    /* paraphrase miss under Tier A (the documented v1 weakness) */
    CHECK(odk_spine_classify(spine, "item-2",
                             "the frobber malfunction recurs nightly", "",
                             1002, &result, &err) == ODK_SPINE_OK);
    CHECK(strcmp(result.classifications.items[0].label, "unsorted") == 0);
    CHECK(result.classifications.items[0].rule_index == -1);
    CHECK(result.classifications.items[0].confidence == 0.5);
    odk_spine_classification_result_release(&result);

    /* as-of query: the miss is stored history now */
    {
        const odk_spine_classification_interval *iv;

        CHECK(odk_spine_classification_at(spine, "item-2", "fixture-triage",
                                          1002, &iv, &err) == ODK_SPINE_OK);
        CHECK(strcmp(iv->label, "unsorted") == 0);
        CHECK(odk_spine_classification_at(spine, "nobody", "fixture-triage",
                                          1002, &iv, &err) ==
              ODK_SPINE_ERR_NOT_FOUND);
    }
    odk_spine_free(spine);
}

static void test_classify_tier_b_and_asof(void)
{
    odk_spine *spine = odk_spine_create();
    odk_spine_error err;
    odk_spine_classification_result result;
    odk_brain *brain = odk_brain_create();

    CHECK(odk_spine_register_json(spine, ODK_SPINE_KIND_SCHEME_SET,
                                  g_scheme_json, NULL, &err) == ODK_SPINE_OK);
    CHECK(odk_spine_register_json(spine, ODK_SPINE_KIND_SCHEME_SET,
                                  g_scheme2_json, NULL, &err) == ODK_SPINE_OK);
    CHECK(odk_spine_register_json(spine, ODK_SPINE_KIND_VOCABULARY,
                                  g_vocab_json, NULL, &err) == ODK_SPINE_OK);
    CHECK(odk_spine_register_brain(spine, brain, "fixture-brain-1", &err) ==
          ODK_SPINE_OK);

    /* Tier B disabled by default: two schemes, both Tier A, one call */
    CHECK(odk_spine_classify(spine, "item-9", "outage affects many users",
                             "", 2000, &result, &err) == ODK_SPINE_OK);
    CHECK(result.classifications.count == 2);
    CHECK(strcmp(result.classifications.items[0].scheme_id,
                 "fixture-triage") == 0);
    CHECK(strcmp(result.classifications.items[0].label, "unsorted") == 0);
    CHECK(strcmp(result.classifications.items[1].scheme_id,
                 "fixture-impact") == 0);
    CHECK(strcmp(result.classifications.items[1].label, "wide") == 0);
    odk_spine_classification_result_release(&result);

    /* enable Tier B: same item now canonicalizes to a hit, labeled B */
    odk_spine_set_tier_b(spine, 1);
    CHECK(odk_spine_classify(spine, "item-10",
                             "frobber malfunction reported again", "", 2100,
                             &result, &err) == ODK_SPINE_OK);
    CHECK(result.classifications.count == 2);
    CHECK(strcmp(result.classifications.items[0].tier, "B") == 0);
    CHECK(strcmp(result.classifications.items[0].brain_snapshot_id,
                 "fixture-brain-1") == 0);
    CHECK(strcmp(result.classifications.items[0].label, "hot") == 0);
    CHECK(result.classifications.items[0].confidence > 0.5);
    /* scheme 2 has no vocabulary help: default label, still Tier B call */
    CHECK(strcmp(result.classifications.items[1].label, "routine") == 0);
    odk_spine_classification_result_release(&result);

    /* disable Tier B again: pure-replay context runs Tier A only */
    odk_spine_set_tier_b(spine, 0);
    CHECK(odk_spine_classify(spine, "item-10",
                             "frobber malfunction reported again", "", 2200,
                             &result, &err) == ODK_SPINE_OK);
    CHECK(strcmp(result.classifications.items[0].tier, "A") == 0);
    CHECK(strcmp(result.classifications.items[0].label, "unsorted") == 0);
    odk_spine_classification_result_release(&result);

    /* reclassification appends; as-of answers from stored intervals */
    CHECK(odk_spine_classify(spine, "item-11", "frobnication overheat", "",
                             3000, &result, &err) == ODK_SPINE_OK);
    odk_spine_classification_result_release(&result);
    CHECK(odk_spine_classify(spine, "item-11", "completely quiet", "", 4000,
                             &result, &err) == ODK_SPINE_OK);
    odk_spine_classification_result_release(&result);
    {
        const odk_spine_classification_interval *iv;
        size_t n = odk_spine_interval_count(spine);

        /* two intervals for item-11 under fixture-triage (append-only) */
        CHECK(odk_spine_interval_count(spine) >= 2);
        CHECK(odk_spine_classification_at(spine, "item-11", "fixture-triage",
                                          3500, &iv, &err) == ODK_SPINE_OK);
        CHECK(strcmp(iv->label, "hot") == 0);
        CHECK(iv->valid_from == 3000);
        CHECK(odk_spine_classification_at(spine, "item-11", "fixture-triage",
                                          4500, &iv, &err) == ODK_SPINE_OK);
        CHECK(strcmp(iv->label, "unsorted") == 0);
        CHECK(iv->valid_from == 4000);
        (void)n;
    }
    odk_brain_free(brain);
    odk_spine_free(spine);
}


/* ------------------------------------------------------------------ */
/* F3 parameterized decomposition                                      */
/* ------------------------------------------------------------------ */

static const char *g_template_json =
    "{\"id\":\"fixture-report\",\"version\":1,\"valid_from\":0,"
    "\"valid_to\":-1,"
    "\"parameters\":["
    "{\"name\":\"depth\",\"type\":\"int\",\"default_value\":\"2\","
    "\"required\":false},"
    "{\"name\":\"label\",\"type\":\"string\",\"default_value\":\"\","
    "\"required\":true}"
    "],"
    "\"component_specs\":["
    "{\"name\":\"symptoms\",\"kind\":\"sentences\",\"max_items\":3,"
    "\"required\":true},"
    "{\"name\":\"metrics\",\"kind\":\"keyvalue\",\"max_items\":5,"
    "\"required\":false},"
    "{\"name\":\"echo\",\"kind\":\"whole\",\"max_items\":1,"
    "\"required\":false}"
    "],"
    "\"extraction_rules\":["
    "{\"component\":\"symptoms\",\"terms\":[\"frobnication\","
    "\"stall\"],\"mode\":\"sentence\",\"window\":0},"
    "{\"component\":\"metrics\",\"terms\":[\"pressure\",\"load\"],"
    "\"mode\":\"keyvalue\",\"window\":0}"
    "]}";

static void test_decompose(void)
{
    odk_spine *spine = odk_spine_create();
    odk_spine_error err;
    odk_spine_components comps;
    const char *item =
        "The frobnication loop stalled twice.\n"
        "Operators restarted the frobnication.\n"
        "Load: 81 percent\nPressure: normal\n"
        "Everything settled after the restart.";
    char *json1;
    char *json2;

    CHECK(odk_spine_register_json(spine, ODK_SPINE_KIND_TEMPLATE,
                                  g_template_json, NULL, &err) ==
          ODK_SPINE_OK);

    CHECK(odk_spine_decompose(spine, "fixture-report", -1, item,
                              "{\"label\":\"nightly\"}", 1000, &comps,
                              &err) == ODK_SPINE_OK);
    CHECK(comps.template_version == 1);
    CHECK(comps.components.count == 3);
    /* symptoms: the two matching sentences, in text order */
    CHECK(strcmp(comps.components.items[0].name, "symptoms") == 0);
    CHECK(comps.components.items[0].item_count == 2);
    CHECK(comps.components.items[0].complete == 1);
    CHECK(strstr(comps.components.items[0].content,
                 "The frobnication loop stalled twice.") != NULL);
    CHECK(strstr(comps.components.items[0].content,
                 "Operators restarted the frobnication.") != NULL);
    /* metrics: the two keyvalue lines, key order preserved */
    CHECK(strcmp(comps.components.items[1].name, "metrics") == 0);
    CHECK(comps.components.items[1].item_count == 2);
    CHECK(strcmp(comps.components.items[1].content,
                 "Load: 81 percent\nPressure: normal") == 0);
    /* echo: whole item, no rule needed */
    CHECK(strcmp(comps.components.items[2].name, "echo") == 0);
    CHECK(strcmp(comps.components.items[2].content, item) == 0);
    /* parameters resolved: label supplied, depth defaulted */
    CHECK(comps.params_used.count == 2);
    CHECK(strcmp(comps.params_used.items[0].key, "depth") == 0);
    CHECK(strcmp(comps.params_used.items[0].value, "2") == 0);
    CHECK(strcmp(comps.params_used.items[1].value, "nightly") == 0);

    /* determinism: byte-identical documents for identical inputs */
    json1 = odk_spine_components_to_json(&comps);
    odk_spine_components_release(&comps);
    CHECK(odk_spine_decompose(spine, "fixture-report", -1, item,
                              "{\"label\":\"nightly\"}", 1000, &comps,
                              &err) == ODK_SPINE_OK);
    json2 = odk_spine_components_to_json(&comps);
    CHECK(strcmp(json1, json2) == 0);
    free(json1);
    free(json2);
    odk_spine_components_release(&comps);

    /* contract violations are errors */
    {
        CHECK(odk_spine_decompose(spine, "fixture-report", -1, item, NULL,
                                  1000, &comps, &err) ==
              ODK_SPINE_ERR_SCHEMA);                        /* missing label */
        CHECK(odk_spine_decompose(spine, "fixture-report", -1, item,
                                  "{\"label\":\"x\",\"oops\":1}", 1000,
                                  &comps, &err) ==
              ODK_SPINE_ERR_SCHEMA);                        /* unknown param */
        CHECK(odk_spine_decompose(spine, "no-such-template", -1, item,
                                  "{\"label\":\"x\"}", 1000, &comps,
                                  &err) == ODK_SPINE_ERR_NOT_FOUND);
        CHECK(odk_spine_decompose(spine, "fixture-report", 7, item,
                                  "{\"label\":\"x\"}", 1000, &comps,
                                  &err) == ODK_SPINE_ERR_NOT_FOUND);
    }

    /* temporal template versions: v2 supersedes v1 as-of its start */
    {
        const char *v2 =
            "{\"id\":\"fixture-report\",\"version\":2,"
            "\"valid_from\":5000,\"valid_to\":-1,"
            "\"parameters\":[{\"name\":\"label\",\"type\":\"string\","
            "\"default_value\":\"\",\"required\":true}],"
            "\"component_specs\":[{\"name\":\"all\",\"kind\":\"whole\","
            "\"max_items\":1,\"required\":false}],"
            "\"extraction_rules\":[]}";
        CHECK(odk_spine_register_json(spine, ODK_SPINE_KIND_TEMPLATE, v2,
                                      NULL, &err) == ODK_SPINE_OK);
        /* t=1000 still resolves v1 (three components) */
        CHECK(odk_spine_decompose(spine, "fixture-report", -1, item,
                                  "{\"label\":\"x\"}", 1000, &comps,
                                  &err) == ODK_SPINE_OK);
        CHECK(comps.template_version == 1);
        odk_spine_components_release(&comps);
        /* t=6000 resolves v2 (one component) */
        CHECK(odk_spine_decompose(spine, "fixture-report", -1, item,
                                  "{\"label\":\"x\"}", 6000, &comps,
                                  &err) == ODK_SPINE_OK);
        CHECK(comps.template_version == 2);
        CHECK(comps.components.count == 1);
        odk_spine_components_release(&comps);
        /* pinned version survives regardless of t */
        CHECK(odk_spine_decompose(spine, "fixture-report", 1, item,
                                  "{\"label\":\"x\"}", 6000, &comps,
                                  &err) == ODK_SPINE_OK);
        CHECK(comps.template_version == 1);
        odk_spine_components_release(&comps);
    }
    odk_spine_free(spine);
}


/* ------------------------------------------------------------------ */
/* F4 calibration reports                                              */
/* ------------------------------------------------------------------ */

static const char *g_decision_fmt =
    "{\"observation_id\":\"%s\",\"kind\":\"decision\",\"t\":%lld,"
    "\"scope\":\"\",\"subject_ref\":\"item-x\",\"primitive\":\"choice\","
    "\"scheme_id\":\"fixture-triage\",\"payload_json\":\"{}\","
    "\"confidence\":%s,\"tier\":\"%s\",\"brain_snapshot_id\":\"\","
    "\"outcome\":\"\",\"kernel_version\":\"2.0.0\","
    "\"schema_fp\":\"ab12cd34ef56ab78\",\"registrations\":[]}";

static const char *g_outcome_fmt =
    "{\"observation_id\":\"out-%lld\",\"kind\":\"outcome\",\"t\":%lld,"
    "\"scope\":\"\",\"subject_ref\":\"%s\",\"primitive\":\"\","
    "\"scheme_id\":\"\",\"payload_json\":\"{}\",\"confidence\":0.0,"
    "\"tier\":\"\",\"brain_snapshot_id\":\"\",\"outcome\":\"%s\","
    "\"kernel_version\":\"2.0.0\",\"schema_fp\":\"ab12cd34ef56ab78\","
    "\"registrations\":[]}";

static void ingest_log(odk_spine *spine)
{
    odk_spine_error err;
    char buf[1024];
    int i;

    /* four decisions in bin 0.5-0.6 (0.5), two in 0.8-0.9 (0.85) */
    for (i = 0; i < 4; i++) {
        snprintf(buf, sizeof(buf), g_decision_fmt,
                 i % 2 ? (i == 1 ? "d2" : "d4") : (i == 0 ? "d1" : "d3"),
                 (long long)(100 + i), i % 2 ? "0.51" : "0.5", "A");
        CHECK(odk_spine_observation_ingest(spine, buf, &err) ==
              ODK_SPINE_OK);
    }
    for (i = 0; i < 2; i++) {
        snprintf(buf, sizeof(buf), g_decision_fmt, i % 2 ? "d6" : "d5",
                 (long long)(200 + i), "0.85", "A");
        CHECK(odk_spine_observation_ingest(spine, buf, &err) ==
              ODK_SPINE_OK);
    }
    /* outcomes: d1 success, d2 failure, d3 success, d4 failure */
    snprintf(buf, sizeof(buf), g_outcome_fmt, 1LL, 300LL, "d1", "success");
    CHECK(odk_spine_observation_ingest(spine, buf, &err) == ODK_SPINE_OK);
    snprintf(buf, sizeof(buf), g_outcome_fmt, 2LL, 301LL, "d2", "failure");
    CHECK(odk_spine_observation_ingest(spine, buf, &err) == ODK_SPINE_OK);
    snprintf(buf, sizeof(buf), g_outcome_fmt, 3LL, 302LL, "d3", "success");
    CHECK(odk_spine_observation_ingest(spine, buf, &err) == ODK_SPINE_OK);
    snprintf(buf, sizeof(buf), g_outcome_fmt, 4LL, 303LL, "d4", "failure");
    CHECK(odk_spine_observation_ingest(spine, buf, &err) == ODK_SPINE_OK);
    snprintf(buf, sizeof(buf), g_outcome_fmt, 5LL, 304LL, "d5", "success");
    CHECK(odk_spine_observation_ingest(spine, buf, &err) == ODK_SPINE_OK);
    snprintf(buf, sizeof(buf), g_outcome_fmt, 6LL, 305LL, "d6", "failure");
    CHECK(odk_spine_observation_ingest(spine, buf, &err) == ODK_SPINE_OK);
}

static void test_calibrate(void)
{
    odk_spine *spine = odk_spine_create();
    odk_spine_error err;
    odk_spine_calibration_report report;
    odk_spine_calibration_report report2;
    char *json1;
    char *json2;

    ingest_log(spine);

    CHECK(odk_spine_calibrate(spine, "fixture-log", &report, &err) ==
          ODK_SPINE_OK);
    CHECK(report.bins.count == 2);
    /* bin 5: [0.5,0.6), 4 samples, predicted ~0.505, observed 0.5 */
    CHECK(report.bins.items[0].bin_index == 5);
    CHECK(report.bins.items[0].sample_count == 4);
    CHECK(report.bins.items[0].lo == 0.5);
    CHECK(report.bins.items[0].hi == 0.6);
    CHECK(report.bins.items[0].observed_rate == 0.5);
    /* bin 8: [0.8,0.9), 2 samples, observed 0.5 */
    CHECK(report.bins.items[1].bin_index == 8);
    CHECK(report.bins.items[1].sample_count == 2);
    CHECK(report.bins.items[1].observed_rate == 0.5);
    CHECK(report.bins.items[1].calibration_error > 0.3);
    CHECK(strncmp(report.report_id, "cal-", 4) == 0);

    /* byte-identical reports for the same log */
    json1 = odk_spine_calibration_report_to_json(&report);
    odk_spine_calibration_report_release(&report);
    CHECK(odk_spine_calibrate(spine, "fixture-log", &report2, &err) ==
          ODK_SPINE_OK);
    json2 = odk_spine_calibration_report_to_json(&report2);
    CHECK(strcmp(json1, json2) == 0);
    free(json1);
    free(json2);
    odk_spine_calibration_report_release(&report2);

    /* empty log: empty report with a stable id */
    {
        odk_spine *empty = odk_spine_create();
        odk_spine_calibration_report r;

        CHECK(odk_spine_calibrate(empty, "nothing", &r, &err) ==
              ODK_SPINE_OK);
        CHECK(r.bins.count == 0);
        CHECK(strncmp(r.report_id, "cal-", 4) == 0);
        odk_spine_calibration_report_release(&r);
        odk_spine_free(empty);
    }
    odk_spine_free(spine);
}


/* ------------------------------------------------------------------ */
/* F6 pinning, replay, worldState                                      */
/* ------------------------------------------------------------------ */

static void test_pin_replay(void)
{
    odk_spine *spine = odk_spine_create();
    odk_kernel *kernel = odk_kernel_create();
    odk_spine_error err;
    odk_spine_decision_pin pin;
    const char *request =
        "{\"primitive\":\"choice\",\"instruction\":\"How should we "
        "fix the outage?\",\"options\":[\"restart the service\","
        "\"reinstall the operating system\",\"wait for the weekend\"],"
        "\"context\":\"The service stopped responding. Restarting the "
        "service restored normal operation within seconds.\"}";
    char *report1;
    char *report2;

    CHECK(kernel != NULL);

    /* pin an exact (Tier A) decision */
    CHECK(odk_spine_pin(spine, kernel, request, "fixture-inputs-1", "exact",
                        1000, &pin, &err) == ODK_SPINE_OK);
    CHECK(strncmp(pin.pin_id, "pin-", 4) == 0);
    CHECK(strcmp(pin.schema_fp, ODK_SPINE_SCHEMA_FINGERPRINT) == 0);
    CHECK(strcmp(pin.kernel_version, "2.0.0") == 0);
    CHECK(strlen(pin.context_hash) == 16);
    CHECK(strcmp(pin.replay_class, "exact") == 0);
    CHECK(pin.timestamp == 1000);
    CHECK(strcmp(pin.decision, "restart the service") == 0);
    CHECK(pin.registrations.count == 0); /* zero customizations by default */
    CHECK(odk_spine_pin_count(spine) == 1);
    /* decision observation written back */
    CHECK(odk_spine_observation_count(spine) == 1);
    CHECK(strcmp(odk_spine_observation_at(spine, 0)->kind, "decision") == 0);

    /* replay: byte-level verdict, deterministic report bytes */
    CHECK(odk_spine_replay(spine, kernel, pin.pin_id, &report1, &err) ==
          ODK_SPINE_OK);
    CHECK(strstr(report1, "\"replay_class\":\"exact\"") != NULL);
    CHECK(strstr(report1, "\"byte_equal\":true") != NULL);
    CHECK(strstr(report1, "\"registrations_match\":true") != NULL);
    CHECK(odk_spine_replay(spine, kernel, pin.pin_id, &report2, &err) ==
          ODK_SPINE_OK);
    CHECK(strcmp(report1, report2) == 0);
    free(report1);
    free(report2);
    odk_spine_decision_pin_release(&pin);

    /* semantic pin: same decision, bytes allowed to differ */
    CHECK(odk_spine_pin(spine, kernel, request, "fixture-inputs-1",
                        "semantic", 1001, &pin, &err) == ODK_SPINE_OK);
    CHECK(odk_spine_replay(spine, kernel, pin.pin_id, &report1, &err) ==
          ODK_SPINE_OK);
    CHECK(strstr(report1, "\"replay_class\":\"semantic\"") != NULL);
    CHECK(strstr(report1, "\"decision_match\":true") != NULL);
    free(report1);
    odk_spine_decision_pin_release(&pin);

    /* foreign pin: recorded lineage, not re-derivable */
    CHECK(odk_spine_pin(spine, kernel, request, "fixture-inputs-2",
                        "foreign", 1002, &pin, &err) == ODK_SPINE_OK);
    CHECK(strcmp(pin.brain_snapshot_id, "") == 0);
    CHECK(odk_spine_replay(spine, kernel, pin.pin_id, &report1, &err) ==
          ODK_SPINE_OK);
    CHECK(strstr(report1, "\"replay_class\":\"foreign\"") != NULL);
    free(report1);
    odk_spine_decision_pin_release(&pin);

    /* registrations recorded in pins */
    CHECK(odk_spine_register_json(spine, ODK_SPINE_KIND_SCHEME_SET,
                                  g_scheme_json, NULL, &err) == ODK_SPINE_OK);
    CHECK(odk_spine_pin(spine, kernel, request, "fixture-inputs-1", "exact",
                        1003, &pin, &err) == ODK_SPINE_OK);
    CHECK(pin.registrations.count == 1);
    CHECK(strcmp(pin.registrations.items[0].id, "fixture-triage") == 0);
    CHECK(strlen(pin.registrations.items[0].content_hash) == 16);
    odk_spine_decision_pin_release(&pin);

    /* unknown pin */
    CHECK(odk_spine_replay(spine, kernel, "pin-999", &report1, &err) ==
          ODK_SPINE_ERR_NOT_FOUND);

    odk_kernel_free(kernel);
    odk_spine_free(spine);
}

static void test_worldstate(void)
{
    odk_spine_error err;
    const char *good =
        "{\"t\":1000,\"scope_json\":\"{\\\"bbox\\\":[0,0,1,1]}\","
        "\"variables\":[{\"name\":\"load\",\"type\":\"number\","
        "\"value\":\"0.81\"},{\"name\":\"region\",\"type\":\"string\","
        "\"value\":\"west\"}]}";
    const char *bad_scope =
        "{\"t\":1000,\"scope_json\":\"not json{\","
        "\"variables\":[{\"name\":\"load\",\"type\":\"number\","
        "\"value\":\"0.81\"}]}";
    const char *bad_name =
        "{\"t\":1000,\"scope_json\":\"\","
        "\"variables\":[{\"name\":\"\",\"type\":\"number\","
        "\"value\":\"0.81\"}]}";

    CHECK(odk_spine_worldstate_validate(good, &err) == ODK_SPINE_OK);
    CHECK(odk_spine_worldstate_validate(bad_scope, &err) ==
          ODK_SPINE_ERR_SCHEMA);
    CHECK(odk_spine_worldstate_validate(bad_name, &err) ==
          ODK_SPINE_ERR_SCHEMA);
    CHECK(odk_spine_worldstate_validate("{\"bad\":1}", &err) ==
          ODK_SPINE_ERR_SCHEMA);
}

int main(void)
{
    odk_spine_error err;
    odk_spine_error_init(&err);

    test_schema_types();
    test_spine_handle();
    test_extension_api();
    test_classify_tier_a();
    test_classify_tier_b_and_asof();
    test_decompose();
    test_calibrate();
    test_pin_replay();
    test_worldstate();

    printf("spine_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
