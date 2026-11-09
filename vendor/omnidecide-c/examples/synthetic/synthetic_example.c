/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * synthetic_example.c - the in-repo proof for OmniDecide v2.0.0 (F7).
 *
 * NON-NORMATIVE: this example and its fixtures are synthetic,
 * application-neutral content used by tests and docs only. It is NOT a
 * product component and can be replaced wholesale by an application's
 * own content — no kernel or spine code changes anywhere.
 *
 * The example exercises every extension point of the F7 extension API:
 *   1. scheme sets            (registered from a file)
 *   2. decomposition templates (registered from a file)
 *   3. vocabularies           (registered through a provider callback)
 *   4. golden corpora         (registered from a file, JSONL)
 *   5. brain snapshots        (registered from an application brain)
 * and then demonstrates, end to end:
 *   - one-call N-scheme classification (Tier A), with write-back
 *   - temporal validity: reclassification appends; as-of queries answer
 *     from stored intervals
 *   - Tier B: brain-assisted paraphrase resilience, labeled and
 *     attributed, disabled again for pure-replay contexts
 *   - parameterized decomposition with temporal template versions
 *   - decision pinning and replay (exact / semantic / foreign)
 *   - calibration over an ingested outcome log
 *   - the worldState context contract
 *
 * Usage: synthetic_example [data-dir]   (default: examples/synthetic/data)
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
        }                                                                    \
    } while (0)

/* Provider: the vocabulary supplied by the "consuming application". The
 * content is identical to examples/synthetic/data/vocabularies.json; the
 * provider callback path proves that no file access is required. */
static int provider_vocabulary(void *user, char **out_text,
                               odk_spine_error *err)
{
    static const char *vocab =
        "{\"id\":\"fixture-synonyms\",\"version\":1,\"valid_from\":0,"
        "\"valid_to\":-1,\"groups\":["
        "{\"canonical\":\"frobnication\",\"terms\":[\"frob failure\","
        "\"frobnicate broke\",\"frobber malfunction\",\"frobber fault\"]}"
        "]}";

    (void)user;
    (void)err;
    *out_text = strdup(vocab);
    return *out_text == NULL ? -1 : 0;
}

int main(int argc, char **argv)
{
    const char *data_dir = argc > 1 ? argv[1] : "examples/synthetic/data";
    odk_spine *spine;
    odk_kernel *kernel;
    odk_brain *brain;
    odk_spine_error err;
    odk_spine_registration_ref_list regs;
    odk_spine_classification_result result;
    odk_spine_components comps;
    odk_spine_decision_pin pin;
    char path[512];
    char *report;
    const odk_spine_classification_interval *iv;
    char *json;

    spine = odk_spine_create();
    kernel = odk_kernel_create();
    brain = odk_brain_create();
    if (spine == NULL || kernel == NULL || brain == NULL) {
        printf("synthetic_example: init failed\n");
        return 2;
    }

    /* ---- the shipped build contains zero registered customizations */
    CHECK(odk_spine_registrations(spine, &regs, &err) == ODK_SPINE_OK);
    CHECK(regs.count == 0);
    odk_spine_registration_ref_list_release(&regs);

    /* ---- 1 + 2: scheme sets and templates from files ---- */
    snprintf(path, sizeof(path), "%s/schemes.json", data_dir);
    CHECK(odk_spine_register_file(spine, ODK_SPINE_KIND_SCHEME_SET, path,
                                  &err) == ODK_SPINE_OK);
    snprintf(path, sizeof(path), "%s/templates.json", data_dir);
    CHECK(odk_spine_register_file(spine, ODK_SPINE_KIND_TEMPLATE, path,
                                  &err) == ODK_SPINE_OK);

    /* ---- 3: vocabulary through a provider callback ---- */
    CHECK(odk_spine_register_provider(spine, ODK_SPINE_KIND_VOCABULARY,
                                      provider_vocabulary, NULL,
                                      "synthetic://vocab", &err) ==
          ODK_SPINE_OK);

    /* ---- 4: golden corpus from a JSONL file (name + hash only) ---- */
    snprintf(path, sizeof(path), "%s/corpus.jsonl", data_dir);
    CHECK(odk_spine_register_file(spine, ODK_SPINE_KIND_CORPUS, path,
                                  &err) == ODK_SPINE_OK);

    /* ---- 5: brain snapshot (application-owned brain) ---- */
    CHECK(odk_spine_register_brain(spine, brain, "synthetic-brain-1",
                                   &err) == ODK_SPINE_OK);

    /* the customization state, in registration order (two schemes) */
    CHECK(odk_spine_registrations(spine, &regs, &err) == ODK_SPINE_OK);
    CHECK(regs.count == 6);
    CHECK(strcmp(regs.items[0].kind, "scheme_set") == 0);
    CHECK(strcmp(regs.items[1].kind, "scheme_set") == 0);
    CHECK(strcmp(regs.items[2].kind, "decomposition_template") == 0);
    CHECK(strcmp(regs.items[3].kind, "vocabulary") == 0);
    CHECK(strcmp(regs.items[4].kind, "corpus") == 0);
    CHECK(strcmp(regs.items[5].kind, "brain_snapshot") == 0);
    odk_spine_registration_ref_list_release(&regs);

    /* ---- F1: one call, N schemes, one result document (Tier A) ---- */
    CHECK(odk_spine_classify(spine, "item-1",
                             "The frobnication loop stalled twice.", "",
                             1000, &result, &err) == ODK_SPINE_OK);
    CHECK(result.classifications.count == 2);
    CHECK(strcmp(result.classifications.items[0].label, "hot") == 0);
    CHECK(strcmp(result.classifications.items[0].tier, "A") == 0);
    CHECK(strcmp(result.classifications.items[1].label, "routine") == 0);
    odk_spine_classification_result_release(&result);

    /* ---- F2: reclassification appends; as-of answers from history ---- */
    CHECK(odk_spine_classify(spine, "item-1", "Completely quiet.", "",
                             2000, &result, &err) == ODK_SPINE_OK);
    odk_spine_classification_result_release(&result);
    CHECK(odk_spine_classification_at(spine, "item-1", "fixture-triage",
                                      1500, &iv, &err) == ODK_SPINE_OK);
    CHECK(strcmp(iv->label, "hot") == 0);
    CHECK(odk_spine_classification_at(spine, "item-1", "fixture-triage",
                                      2500, &iv, &err) == ODK_SPINE_OK);
    CHECK(strcmp(iv->label, "unsorted") == 0);

    /* ---- Tier B: paraphrase resilience, labeled + attributed ---- */
    odk_spine_set_tier_b(spine, 1);
    CHECK(odk_spine_classify(spine, "item-2",
                             "frobber malfunction reported again", "", 2100,
                             &result, &err) == ODK_SPINE_OK);
    CHECK(strcmp(result.classifications.items[0].tier, "B") == 0);
    CHECK(strcmp(result.classifications.items[0].brain_snapshot_id,
                 "synthetic-brain-1") == 0);
    CHECK(strcmp(result.classifications.items[0].label, "hot") == 0);
    odk_spine_classification_result_release(&result);
    /* disable again: pure-replay contexts run Tier A only */
    odk_spine_set_tier_b(spine, 0);
    CHECK(odk_spine_classify(spine, "item-2",
                             "frobber malfunction reported again", "", 2200,
                             &result, &err) == ODK_SPINE_OK);
    CHECK(strcmp(result.classifications.items[0].tier, "A") == 0);
    CHECK(strcmp(result.classifications.items[0].label, "unsorted") == 0);
    odk_spine_classification_result_release(&result);

    /* ---- F3: parameterized decomposition (as-of template version) ---- */
    CHECK(odk_spine_decompose(spine, "fixture-report", -1,
                              "The frobnication loop stalled twice.\n"
                              "Load: 81 percent\nPressure: normal\n"
                              "Everything settled.",
                              "{\"label\":\"nightly\"}", 1000, &comps,
                              &err) == ODK_SPINE_OK);
    CHECK(comps.template_version == 1);
    CHECK(comps.components.count == 4);
    CHECK(comps.components.items[0].item_count == 1);
    CHECK(strcmp(comps.components.items[1].content,
                 "Load: 81 percent\nPressure: normal") == 0);
    odk_spine_components_release(&comps);
    /* as-of 6000 the template is v2: one whole component */
    CHECK(odk_spine_decompose(spine, "fixture-report", -1,
                              "The frobnication loop stalled twice.",
                              "{\"label\":\"nightly\"}", 6000, &comps,
                              &err) == ODK_SPINE_OK);
    CHECK(comps.template_version == 2);
    CHECK(comps.components.count == 1);
    odk_spine_components_release(&comps);

    /* ---- F6: pin -> replay round-trips ---- */
    {
        const char *request =
            "{\"primitive\":\"choice\",\"instruction\":\"How should we fix "
            "the outage?\",\"options\":[\"restart the service\","
            "\"reinstall the operating system\",\"wait for the weekend\"],"
            "\"context\":\"The service stopped responding. Restarting the "
            "service restored normal operation within seconds.\"}";

        CHECK(odk_spine_pin(spine, kernel, request, "synthetic-inputs-1",
                            "exact", 3000, &pin, &err) == ODK_SPINE_OK);
        CHECK(strcmp(pin.decision, "restart the service") == 0);
        CHECK(pin.registrations.count == 6); /* customization state rides */
        CHECK(odk_spine_replay(spine, kernel, pin.pin_id, &report, &err) ==
              ODK_SPINE_OK);
        CHECK(strstr(report, "\"replay_class\":\"exact\"") != NULL);
        CHECK(strstr(report, "\"byte_equal\":true") != NULL);
        free(report);
        odk_spine_decision_pin_release(&pin);

        /* semantic: same decision, different bytes allowed */
        CHECK(odk_spine_pin(spine, kernel, request, "synthetic-inputs-1",
                            "semantic", 3001, &pin, &err) == ODK_SPINE_OK);
        CHECK(odk_spine_replay(spine, kernel, pin.pin_id, &report, &err) ==
              ODK_SPINE_OK);
        CHECK(strstr(report, "\"replay_class\":\"semantic\"") != NULL);
        free(report);
        odk_spine_decision_pin_release(&pin);

        /* foreign: recorded lineage, not re-derivable */
        CHECK(odk_spine_pin(spine, kernel, request, "synthetic-inputs-2",
                            "foreign", 3002, &pin, &err) == ODK_SPINE_OK);
        CHECK(odk_spine_replay(spine, kernel, pin.pin_id, &report, &err) ==
              ODK_SPINE_OK);
        CHECK(strstr(report, "\"replay_class\":\"foreign\"") != NULL);
        free(report);
        odk_spine_decision_pin_release(&pin);
    }

    /* ---- F4: calibration over an ingested log ---- */
    {
        const char *dec =
            "{\"observation_id\":\"obs-d1\",\"kind\":\"decision\","
            "\"t\":4000,\"scope\":\"\",\"subject_ref\":\"item-9\","
            "\"primitive\":\"choice\",\"scheme_id\":\"\",\"payload_json\":"
            "\"{}\",\"confidence\":0.85,\"tier\":\"A\","
            "\"brain_snapshot_id\":\"\",\"outcome\":\"\","
            "\"kernel_version\":\"2.0.0\",\"schema_fp\":\"x\",\""
            "registrations\":[]}";
        const char *out =
            "{\"observation_id\":\"obs-o1\",\"kind\":\"outcome\","
            "\"t\":4100,\"scope\":\"\",\"subject_ref\":\"obs-d1\","
            "\"primitive\":\"\",\"scheme_id\":\"\",\"payload_json\":\"{}\","
            "\"confidence\":0.0,\"tier\":\"\",\"brain_snapshot_id\":\"\","
            "\"outcome\":\"success\",\"kernel_version\":\"2.0.0\","
            "\"schema_fp\":\"x\",\"registrations\":[]}";
        odk_spine_calibration_report rep;

        CHECK(odk_spine_observation_ingest(spine, dec, &err) ==
              ODK_SPINE_OK);
        CHECK(odk_spine_observation_ingest(spine, out, &err) ==
              ODK_SPINE_OK);
        CHECK(odk_spine_calibrate(spine, "synthetic-log", &rep, &err) ==
              ODK_SPINE_OK);
        CHECK(rep.bins.count == 1);
        CHECK(rep.bins.items[0].bin_index == 8);
        CHECK(rep.bins.items[0].sample_count == 1);
        json = odk_spine_calibration_report_to_json(&rep);
        CHECK(strstr(json, "\"observed_rate\":1.0") != NULL);
        free(json);
        odk_spine_calibration_report_release(&rep);
    }

    /* ---- F6: worldState contract ---- */
    CHECK(odk_spine_worldstate_validate(
              "{\"t\":5000,\"scope_json\":\"{\\\"bbox\\\":[0,0,1,1]}\","
              "\"variables\":[{\"name\":\"load\",\"type\":\"number\","
              "\"value\":\"0.81\"}]}", &err) == ODK_SPINE_OK);

    printf("synthetic_example: %d checks, %d failures\n", g_checks,
           g_failures);
    printf("synthetic_example: OK — every F7 extension point exercised\n");

    odk_brain_free(brain);
    odk_kernel_free(kernel);
    odk_spine_free(spine);
    return g_failures == 0 ? 0 : 1;
}
