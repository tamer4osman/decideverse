/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_spine_private.h - internal structures shared by the spine modules
 * (registry, classify, decompose, calibrate, pin/replay). Not installed;
 * not part of the public API.
 */

#ifndef ODK_SPINE_PRIVATE_H
#define ODK_SPINE_PRIVATE_H

/* POSIX.1-2008: strdup(). */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "odk_spine.h"

#include <stdarg.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Registry entries (F7): every registration is an append.             */
/* Older versions of the same id are kept and marked inactive; the     */
/* full versioned lineage stays queryable.                             */
/* ------------------------------------------------------------------ */

typedef struct {
    int active;              /* 1 = current version for its id          */
    char content_hash[17];   /* FNV-1a 64 of the registered JSON bytes  */
    char *source_name;       /* file path or provider name              */
    uint64_t seq;            /* registration order (deterministic)      */
    odk_spine_scheme scheme;
} reg_scheme_entry;

typedef struct {
    int active;
    char content_hash[17];
    char *source_name;
    uint64_t seq;
    odk_spine_decomposition_template tmpl;
} reg_template_entry;

typedef struct {
    int active;
    char content_hash[17];
    char *source_name;
    uint64_t seq;
    odk_spine_vocabulary vocab;
} reg_vocab_entry;

typedef struct {
    char content_hash[17];
    char *source_name;
    uint64_t seq;
} reg_corpus_entry;

typedef struct {
    char snapshot_id[129];   /* application-supplied snapshot label     */
    char content_hash[17];
    uint64_t seq;
    odk_brain *brain;        /* NOT owned: the application owns brains  */
} reg_brain_entry;

/* Decision pins (F6): append-only store. */
typedef struct {
    odk_spine_decision_pin pin;
} pin_entry;

/* Classification intervals (F2): append-only store. */
typedef struct {
    odk_spine_classification_interval interval;
} interval_entry;

/* Observation log: append-only store (the record shape of the spine). */
typedef struct {
    odk_spine_observation obs;
} observation_entry;

/* ------------------------------------------------------------------ */
/* The spine handle                                                    */
/* ------------------------------------------------------------------ */

struct odk_spine {
    char schema_fp[17];

    /* F7 registry */
    reg_scheme_entry *schemes;
    size_t scheme_count, scheme_cap;
    reg_template_entry *templates;
    size_t template_count, template_cap;
    reg_vocab_entry *vocabularies;
    size_t vocab_count, vocab_cap;
    reg_corpus_entry *corpora;
    size_t corpus_count, corpus_cap;
    reg_brain_entry *brains;
    size_t brain_count, brain_cap;

    /* append-only stores */
    pin_entry *pins;
    size_t pin_count, pin_cap;
    uint64_t pin_seq;
    interval_entry *intervals;
    size_t interval_count, interval_cap;
    observation_entry *observations;
    size_t observation_count, observation_cap;

    /* configuration */
    int tier_b_enabled;      /* default 0: pure-replay contexts see     */
                             /* Tier A only unless the app opts in      */
    uint64_t seq;            /* monotonic registration counter          */
    uint64_t obs_seq;        /* monotonic observation counter           */
};

/* Registry maintenance (implemented in odk_spine_registry.c). */
void odk_spine_registry_free(struct odk_spine *spine);

/* Shared error helpers (implemented in odk_spine_core.c). */
void odk_spine_set_err(odk_spine_error *err, const char *msg);
void odk_spine_set_errf(odk_spine_error *err, const char *fmt, ...);
#define set_err odk_spine_set_err
#define set_errf odk_spine_set_errf

/* Deep-copy helpers for the generated records; used by the append-only
 * stores (implemented next to their stores). */
odk_spine_status odk_spine_observation_copy(odk_spine_observation *dst,
                                            const odk_spine_observation *src);
odk_spine_status odk_spine_classification_interval_copy(
    odk_spine_classification_interval *dst,
    const odk_spine_classification_interval *src);
odk_spine_status odk_spine_decision_pin_copy(odk_spine_decision_pin *dst,
                                             const odk_spine_decision_pin *src);

/* Shared append into the observation log (implemented in
 * odk_spine_classify.c). */
odk_spine_status odk_spine_observation_append(odk_spine *spine,
                                              const odk_spine_observation *obs,
                                              odk_spine_error *err);

/* Free a map's records and array, then zero it (implemented in
 * odk_spine_core.c). */
void odk_spine_map_release_records(odk_spine_map *m);

/* Internally used file reader (malloc'd NUL-terminated buffer or NULL). */
char *odk_spine_read_file(const char *path, size_t *out_len);

/* Validation helpers shared by the registry and the sidecar. */
odk_spine_status odk_spine_validate_scheme(const odk_spine_scheme *s,
                                           odk_spine_error *err);
odk_spine_status odk_spine_validate_template(
    const odk_spine_decomposition_template *t, odk_spine_error *err);
odk_spine_status odk_spine_validate_vocabulary(const odk_spine_vocabulary *v,
                                               odk_spine_error *err);

#endif /* ODK_SPINE_PRIVATE_H */
