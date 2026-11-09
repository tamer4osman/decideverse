/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_spine_pin.c - F6 decision pinning and replay.
 *
 * A pin is an append-only record of one decision, sufficient for total
 * replay: the exact request bytes ride along (request_json), the
 * registered customization state is recorded by name + content hash,
 * and the schema fingerprint keeps historical pins interpretable.
 *
 * Replay: load pin + pinned inputs -> re-derive with the kernel ->
 * compare -> report. Classes: exact (bit-level, Tier A), semantic (same
 * decision, different bytes, e.g. brain-assisted), foreign (recorded
 * lineage, not re-derivable locally).
 *
 * The pin's timestamp is supplied by the caller; nothing in this file
 * reads the wall clock.
 */

#include "odk_spine_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Pin store                                                           */
/* ------------------------------------------------------------------ */

size_t odk_spine_pin_count(const odk_spine *spine)
{
    return spine == NULL ? 0 : spine->pin_count;
}

const odk_spine_decision_pin *odk_spine_pin_at(const odk_spine *spine,
                                               size_t i)
{
    if (spine == NULL || i >= spine->pin_count) {
        return NULL;
    }
    return &spine->pins[i].pin;
}

const odk_spine_decision_pin *odk_spine_pin_get(const odk_spine *spine,
                                                const char *pin_id)
{
    size_t i;

    if (spine == NULL || pin_id == NULL) {
        return NULL;
    }
    for (i = 0; i < spine->pin_count; i++) {
        if (strcmp(spine->pins[i].pin.pin_id, pin_id) == 0) {
            return &spine->pins[i].pin;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Request evaluation (shared by pin and replay)                       */
/* ------------------------------------------------------------------ */

odk_spine_status odk_spine_evaluate_request(odk_kernel *kernel,
                                            const char *request_json,
                                            char **decision,
                                            double *confidence,
                                            char **result_json,
                                            odk_spine_error *err)
{
    odk_json_value *root;
    odk_error jerr;
    const odk_json_value *v;
    const char *primitive;
    const char *instruction = "";
    const char *context = "";
    long long top_k = 3;
    const char **argv = NULL;
    size_t argc = 0;
    odk_result result;
    odk_status kst;
    odk_spine_status st = ODK_SPINE_OK;

    root = odk_json_parse(request_json, strlen(request_json), &jerr);
    if (root == NULL) {
        set_errf(err, "request: invalid JSON: %s", jerr.text);
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (root->type != ODK_JSON_OBJECT) {
        odk_json_free(root);
        set_err(err, "request: expected an object");
        return ODK_SPINE_ERR_SCHEMA;
    }
    v = odk_json_object_get(root, "primitive");
    if (v == NULL || v->type != ODK_JSON_STRING) {
        odk_json_free(root);
        set_err(err, "request.primitive: expected a string");
        return ODK_SPINE_ERR_SCHEMA;
    }
    primitive = v->string;
    v = odk_json_object_get(root, "instruction");
    if (v != NULL && v->type == ODK_JSON_STRING) {
        instruction = v->string;
    }
    v = odk_json_object_get(root, "context");
    if (v != NULL && v->type == ODK_JSON_STRING) {
        context = v->string;
    }
    v = odk_json_object_get(root, "top_k");
    if (v != NULL && v->type == ODK_JSON_NUMBER && v->is_integer) {
        top_k = v->integer;
    }

    /* options for choice/rank/compare */
    {
        const odk_json_value *opts = odk_json_object_get(root, "options");

        if (opts != NULL && opts->type == ODK_JSON_ARRAY) {
            size_t i;

            argc = opts->item_count;
            argv = calloc(argc > 0 ? argc : 1, sizeof(char *));
            if (argv == NULL) {
                odk_json_free(root);
                set_err(err, "out of memory");
                return ODK_SPINE_ERR_MEMORY;
            }
            for (i = 0; i < argc; i++) {
                if (opts->items[i]->type != ODK_JSON_STRING) {
                    free(argv);
                    odk_json_free(root);
                    set_errf(err, "request.options[%zu]: expected a string",
                             i);
                    return ODK_SPINE_ERR_SCHEMA;
                }
                argv[i] = opts->items[i]->string;
            }
        }
    }

    if (strcmp(primitive, "choice") == 0 && argc > 0) {
        kst = odk_evaluate_choice(kernel, instruction, argv, argc, context,
                                  NULL, &result, &jerr);
    } else if (strcmp(primitive, "rank") == 0 && argc > 0) {
        kst = odk_evaluate_rank(kernel, instruction, argv, argc, context,
                                NULL, &result, &jerr);
    } else if (strcmp(primitive, "compare") == 0 && argc == 2) {
        kst = odk_evaluate_compare(kernel, instruction, argv, argc, context,
                                   NULL, &result, &jerr);
    } else if (strcmp(primitive, "noul") == 0) {
        kst = odk_evaluate_noul(kernel,
                                instruction[0] != '\0' ? instruction : context,
                                context, NULL, &result, &jerr);
    } else if (strcmp(primitive, "extract") == 0) {
        kst = odk_evaluate_extract(kernel, instruction, context, NULL,
                                   (int)top_k, &result, &jerr);
    } else if (strcmp(primitive, "score") == 0) {
        /* rubric rides in "options" */
        kst = odk_evaluate_score(kernel, instruction, argv, argc, context,
                                 NULL, &result, &jerr);
    } else {
        free(argv);
        odk_json_free(root);
        set_errf(err, "request: unsupported primitive '%s' or arity",
                 primitive);
        return ODK_SPINE_ERR_ARGUMENT;
    }
    free(argv);
    odk_json_free(root);
    if (kst != ODK_OK) {
        set_errf(err, "evaluate failed: %s", jerr.text);
        return ODK_SPINE_ERR_STATE;
    }
    /* the decision text follows the result's value kind: bool -> true |
     * false, number -> shortest round-trip literal, string -> the text,
     * list -> the JSON array (extract) */
    switch (result.kind) {
    case ODK_VALUE_STRING:
        *decision = strdup(result.string != NULL ? result.string : "");
        break;
    case ODK_VALUE_BOOL:
        *decision = strdup(result.boolean ? "true" : "false");
        break;
    case ODK_VALUE_NUMBER: {
        char *lit = odk_json_number_literal(result.number);

        *decision = lit != NULL ? lit : strdup("0.0");
        break;
    }
    case ODK_VALUE_LIST: {
        odk_json_value *arr;
        size_t vi;

        arr = calloc(1, sizeof(*arr));
        if (arr != NULL) {
            arr->type = ODK_JSON_ARRAY;
            arr->items = calloc(result.item_count > 0 ? result.item_count : 1,
                                sizeof(odk_json_value *));
            arr->item_count = result.item_count;
            for (vi = 0; vi < result.item_count && arr->items != NULL; vi++) {
                odk_json_value *item = calloc(1, sizeof(*item));

                if (item == NULL) {
                    break;
                }
                item->type = ODK_JSON_STRING;
                item->string = strdup(result.items[vi] != NULL
                                          ? result.items[vi]
                                          : "");
                arr->items[vi] = item;
            }
            *decision = odk_json_dump(arr);
            odk_json_free(arr);
        }
        if (*decision == NULL) {
            *decision = strdup("[]");
        }
        break;
    }
    default:
        *decision = strdup("null");
        break;
    }
    *confidence = result.confidence;
    if (result_json != NULL) {
        *result_json = odk_result_to_json(&result);
        if (*result_json == NULL) {
            st = ODK_SPINE_ERR_MEMORY;
        }
    }
    if (*decision == NULL) {
        st = ODK_SPINE_ERR_MEMORY;
    }
    odk_result_release(&result);
    if (st != ODK_SPINE_OK) {
        free(*decision);
        if (result_json != NULL) {
            free(*result_json);
            *result_json = NULL;
        }
        *decision = NULL;
        set_err(err, "out of memory");
    }
    return st;
}

/* ------------------------------------------------------------------ */
/* Pinning                                                             */
/* ------------------------------------------------------------------ */

odk_spine_status odk_spine_pin(odk_spine *spine, odk_kernel *kernel,
                               const char *request_json,
                               const char *inputs_ref,
                               const char *replay_class, int64_t timestamp,
                               odk_spine_decision_pin *out,
                               odk_spine_error *err)
{
    char *decision = NULL;
    double confidence = 0.0;
    char *result_json = NULL;
    odk_spine_decision_pin pin;
    odk_spine_registration_ref_list regs;
    char pin_id[64];
    char context_hash[17];
    const char *context_bytes = NULL;
    size_t context_len = 0;
    const reg_brain_entry *brain_entry = NULL;
    size_t i;
    odk_json_value *root;
    odk_error jerr;
    const odk_json_value *v;
    odk_spine_status st;
    pin_entry entry;

    if (spine == NULL || kernel == NULL || request_json == NULL ||
        out == NULL) {
        set_err(err, "null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    if (replay_class == NULL || (strcmp(replay_class, "exact") != 0 &&
                                 strcmp(replay_class, "semantic") != 0 &&
                                 strcmp(replay_class, "foreign") != 0)) {
        set_err(err, "replay_class must be exact, semantic or foreign");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    /* context bytes for the context hash (exact bytes in) */
    root = odk_json_parse(request_json, strlen(request_json), &jerr);
    if (root == NULL) {
        set_errf(err, "request: invalid JSON: %s", jerr.text);
        return ODK_SPINE_ERR_SCHEMA;
    }
    v = odk_json_object_get(root, "context");
    if (v != NULL && v->type == ODK_JSON_STRING) {
        context_bytes = v->string;
        context_len = strlen(context_bytes);
    }
    odk_spine_hash_hex(context_bytes != NULL ? context_bytes : "",
                       context_len, context_hash);
    odk_json_free(root);

    st = odk_spine_evaluate_request(kernel, request_json, &decision,
                                    &confidence, &result_json, err);
    if (st != ODK_SPINE_OK) {
        return st;
    }

    /* brain snapshot attribution: earliest registered snapshot */
    for (i = 0; i < spine->brain_count; i++) {
        if (brain_entry == NULL || spine->brains[i].seq < brain_entry->seq) {
            brain_entry = &spine->brains[i];
        }
    }

    /* customization state recorded in the pin (names + content hashes) */
    st = odk_spine_registrations(spine, &regs, err);
    if (st != ODK_SPINE_OK) {
        free(decision);
        free(result_json);
        return st;
    }

    odk_spine_decision_pin_init(&pin);
    snprintf(pin_id, sizeof(pin_id), "pin-%llu",
             (unsigned long long)(++spine->pin_seq));
    pin.pin_id = strdup(pin_id);
    pin.schema_fp = strdup(spine->schema_fp);
    pin.kernel_version = strdup(ODK_SPINE_KERNEL_VERSION);
    pin.brain_snapshot_id = strdup(brain_entry != NULL
                                       ? brain_entry->snapshot_id
                                       : "");
    pin.context_hash = strdup(context_hash);
    pin.inputs_ref = strdup(inputs_ref != NULL ? inputs_ref : "");
    pin.request_json = strdup(request_json);
    pin.decision = decision; /* transfer */
    pin.confidence = confidence;
    pin.tier = strdup("A");
    pin.replay_class = strdup(replay_class);
    pin.timestamp = timestamp;
    decision = NULL;
    if (regs.count > 0) {
        pin.registrations.items = calloc(regs.count,
                                         sizeof(odk_spine_registration_ref));
        if (pin.registrations.items != NULL) {
            for (i = 0; i < regs.count; i++) {
                pin.registrations.items[i].kind = strdup(regs.items[i].kind);
                pin.registrations.items[i].id = strdup(regs.items[i].id);
                pin.registrations.items[i].version = regs.items[i].version;
                pin.registrations.items[i].content_hash =
                    strdup(regs.items[i].content_hash);
            }
            pin.registrations.count = regs.count;
        }
    }
    odk_spine_registration_ref_list_release(&regs);
    if (pin.pin_id == NULL || pin.schema_fp == NULL ||
        pin.kernel_version == NULL || pin.brain_snapshot_id == NULL ||
        pin.context_hash == NULL || pin.inputs_ref == NULL ||
        pin.request_json == NULL || pin.tier == NULL ||
        pin.replay_class == NULL ||
        (regs.count > 0 && pin.registrations.items == NULL)) {
        free(result_json);
        goto oom;
    }

    /* store the pin (deep copy into the append-only store) */
    memset(&entry, 0, sizeof(entry));
    if (odk_spine_decision_pin_copy(&entry.pin, &pin) != ODK_SPINE_OK) {
        goto oom;
    }
    if (spine->pin_count == spine->pin_cap) {
        size_t ncap = spine->pin_cap == 0 ? 8 : spine->pin_cap * 2;
        pin_entry *ng = realloc(spine->pins, ncap * sizeof(*ng));

        if (ng == NULL) {
            odk_spine_decision_pin_release(&entry.pin);
            goto oom;
        }
        spine->pins = ng;
        spine->pin_cap = ncap;
    }
    spine->pins[spine->pin_count++] = entry;

    /* decision observation write-back (kind "decision") */
    {
        odk_spine_observation obs;

        odk_spine_observation_init(&obs);
        obs.observation_id = strdup(pin_id);
        obs.kind = strdup("decision");
        obs.t = timestamp;
        obs.scope = strdup("");
        obs.subject_ref = strdup(pin_id);
        obs.primitive = strdup("");
        obs.scheme_id = strdup("");
        obs.confidence = confidence;
        obs.tier = strdup("A");
        obs.brain_snapshot_id = strdup(pin.brain_snapshot_id);
        obs.outcome = strdup("");
        obs.kernel_version = strdup(ODK_SPINE_KERNEL_VERSION);
        obs.schema_fp = strdup(spine->schema_fp);
        obs.payload_json = result_json; /* transfer */
        result_json = NULL;
        if (obs.observation_id == NULL || obs.kind == NULL ||
            obs.scope == NULL || obs.subject_ref == NULL ||
            obs.primitive == NULL || obs.scheme_id == NULL ||
            obs.tier == NULL || obs.brain_snapshot_id == NULL ||
            obs.outcome == NULL || obs.kernel_version == NULL ||
            obs.schema_fp == NULL || obs.payload_json == NULL) {
            odk_spine_observation_release(&obs);
            goto oom;
        }
        st = odk_spine_observation_append(spine, &obs, err);
        odk_spine_observation_release(&obs);
        if (st != ODK_SPINE_OK) {
            odk_spine_decision_pin_release(&pin);
            return st;
        }
    }

    /* hand the pin to the caller (ownership moves) */
    memcpy(out, &pin, sizeof(*out));
    memset(&pin, 0, sizeof(pin));
    return ODK_SPINE_OK;

oom:
    free(decision);
    free(result_json);
    odk_spine_decision_pin_release(&pin);
    set_err(err, "out of memory");
    return ODK_SPINE_ERR_MEMORY;
}

/* ------------------------------------------------------------------ */
/* Replay                                                              */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* Report builder (fixed key order; deterministic bytes)               */
/* ------------------------------------------------------------------ */

typedef struct {
    char *data;
    size_t len, cap;
    int oom;
} rbuf;

static void rbuf_init(rbuf *b)
{
    b->cap = 256;
    b->len = 0;
    b->oom = 0;
    b->data = malloc(b->cap);
    if (b->data != NULL) {
        b->data[0] = '\0';
    } else {
        b->oom = 1;
    }
}

static void rbuf_free(rbuf *b)
{
    free(b->data);
    b->data = NULL;
}

static char *rbuf_detach(rbuf *b)
{
    return b->data; /* NULL on OOM */
}

static void rbuf_puts(rbuf *b, const char *s)
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

static void rbuf_put_json_str(rbuf *b, const char *s)
{
    char *lit;

    if (b->oom) {
        return;
    }
    lit = odk_json_string_literal(s != NULL ? s : "",
                                  s != NULL ? strlen(s) : 0);
    if (lit == NULL) {
        b->oom = 1;
        return;
    }
    rbuf_puts(b, lit);
    free(lit);
}

static void rbuf_put_json_bool(rbuf *b, int v)
{
    rbuf_puts(b, v ? "true" : "false");
}

odk_spine_status odk_spine_replay(odk_spine *spine, odk_kernel *kernel,
                                  const char *pin_id, char **report_json,
                                  odk_spine_error *err)
{
    const odk_spine_decision_pin *pin;
    char *rederived = NULL;
    double rederived_confidence = 0.0;
    int decision_match = 0;
    int byte_equal = 0;
    int registrations_match = 1;
    const char *replay_class;
    rbuf b;
    char conf_str[40];
    char *conf_lit;

    if (spine == NULL || kernel == NULL || pin_id == NULL ||
        report_json == NULL) {
        set_err(err, "null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    *report_json = NULL;
    pin = odk_spine_pin_get(spine, pin_id);
    if (pin == NULL) {
        set_errf(err, "unknown pin '%s'", pin_id);
        return ODK_SPINE_ERR_NOT_FOUND;
    }

    /* the active customization state must equal the pinned one */
    {
        odk_spine_registration_ref_list now;

        if (odk_spine_registrations(spine, &now, err) != ODK_SPINE_OK) {
            return ODK_SPINE_ERR_MEMORY;
        }
        if (now.count != pin->registrations.count) {
            registrations_match = 0;
        } else {
            size_t i;

            for (i = 0; i < now.count && registrations_match; i++) {
                if (strcmp(now.items[i].kind,
                           pin->registrations.items[i].kind) != 0 ||
                    strcmp(now.items[i].id,
                           pin->registrations.items[i].id) != 0 ||
                    now.items[i].version !=
                        pin->registrations.items[i].version ||
                    strcmp(now.items[i].content_hash,
                           pin->registrations.items[i].content_hash) != 0) {
                    registrations_match = 0;
                }
            }
        }
        odk_spine_registration_ref_list_release(&now);
    }

    if (strcmp(pin->replay_class, "foreign") == 0) {
        /* recorded lineage only: not re-derivable locally */
        replay_class = "foreign";
    } else {
        odk_spine_status st =
            odk_spine_evaluate_request(kernel, pin->request_json, &rederived,
                                       &rederived_confidence, NULL, err);
        if (st != ODK_SPINE_OK) {
            return st;
        }
        decision_match = strcmp(rederived, pin->decision) == 0;
        /* byte-level comparison of the recorded vs re-derived decision
         * text (the full result document rides in the observation log;
         * the pin's decision field is the authoritative record) */
        byte_equal = decision_match && pin->confidence == rederived_confidence;
        if (strcmp(pin->replay_class, "exact") == 0) {
            replay_class = byte_equal ? "exact" : "mismatch";
        } else { /* semantic */
            replay_class = decision_match ? "semantic" : "mismatch";
        }
    }

    rbuf_init(&b);
    rbuf_puts(&b, "{\"pin_id\":");
    rbuf_put_json_str(&b, pin->pin_id);
    rbuf_puts(&b, ",\"expected_class\":");
    rbuf_put_json_str(&b, pin->replay_class);
    rbuf_puts(&b, ",\"replay_class\":");
    rbuf_put_json_str(&b, replay_class);
    rbuf_puts(&b, ",\"decision_match\":");
    rbuf_put_json_bool(&b, decision_match);
    rbuf_puts(&b, ",\"byte_equal\":");
    rbuf_put_json_bool(&b, byte_equal);
    rbuf_puts(&b, ",\"registrations_match\":");
    rbuf_put_json_bool(&b, registrations_match);
    rbuf_puts(&b, ",\"rederived_decision\":");
    rbuf_put_json_str(&b, replay_class[0] == 'f' ? "" : rederived);
    rbuf_puts(&b, ",\"rederived_confidence\":");
    if (replay_class[0] == 'f') {
        rbuf_puts(&b, "null");
    } else {
        conf_lit = odk_json_number_literal(rederived_confidence);
        rbuf_puts(&b, conf_lit != NULL ? conf_lit : "0.0");
        free(conf_lit);
    }
    rbuf_puts(&b, "}");
    free(rederived);
    (void)conf_str;
    if (b.oom) {
        rbuf_free(&b);
        set_err(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    *report_json = rbuf_detach(&b);
    return ODK_SPINE_OK;
}
