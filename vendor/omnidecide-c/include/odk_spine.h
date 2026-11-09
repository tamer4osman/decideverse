/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_spine.h - Public C API of the OmniDecide v2.0.0 integration spine.
 *
 * Owner and author: Tamer Osman
 *   GitHub:   https://github.com/tamer4osman
 *   LinkedIn: https://www.linkedin.com/in/tamer4osman
 *
 * The spine is the generic, application-agnostic layer around the
 * deterministic kernel (include/omnidecide.h):
 *
 *   - F7  extension API: registration of scheme sets, decomposition
 *         templates, vocabularies, golden corpora and brain snapshots,
 *         from files or provider callbacks; register -> validate ->
 *         activate atomically; re-registration appends versions.
 *   - F1/F2 classification over registered schemes with temporal
 *         validity (append-only intervals, as-of queries); the Tier A
 *         engine lives in odk-core, Tier B in odk-brain.
 *   - F3  parameterized decomposition over registered templates.
 *   - F4  calibration reports over a decision log.
 *   - F6  decision pinning and replay (exact / semantic / foreign), the
 *         worldState context contract, observation records.
 *   - F5  all cross-boundary record types are generated from
 *         schema/omnidecide.schema (odk-spine/odk_spine_types.h) and are
 *         not redeclared here by hand.
 *
 * Design goals (inherited from the kernel, non-negotiable):
 *   - deterministic: identical input bytes produce identical output bytes,
 *     same run or across runs; no wall clock, no RNG, no locale.
 *   - no training in the spine: the brain stays an external, empirical,
 *     human-inspectable artifact.
 *   - append-only: classification and pinning never overwrite history.
 *   - zero application-domain vocabulary in the core or spine.
 */

#ifndef ODK_SPINE_H
#define ODK_SPINE_H

#include "odk_spine_types.h"
#include "odk_spine_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Kernel version string this spine was built against (ODK_LIBRARY_VERSION). */
#define ODK_SPINE_KERNEL_VERSION ODK_LIBRARY_VERSION

/* ------------------------------------------------------------------ */
/* Handle                                                              */
/* ------------------------------------------------------------------ */

typedef struct odk_spine odk_spine;

/* Create a spine handle. The shipped build contains ZERO registered
 * customizations; everything is supplied through the extension API. */
odk_spine *odk_spine_create(void);
void odk_spine_free(odk_spine *spine);

/* Schema fingerprint of the running build (see odk_spine_base.h). */
const char *odk_spine_schema_fingerprint(const odk_spine *spine);

/* ------------------------------------------------------------------ */
/* F7 - application extension API (the customization seam)             */
/* ------------------------------------------------------------------ */

/* Registration kinds. */
#define ODK_SPINE_KIND_SCHEME_SET "scheme_set"
#define ODK_SPINE_KIND_TEMPLATE "decomposition_template"
#define ODK_SPINE_KIND_VOCABULARY "vocabulary"
#define ODK_SPINE_KIND_CORPUS "corpus"
#define ODK_SPINE_KIND_BRAIN_SNAPSHOT "brain_snapshot"

/* Data-driven loading from JSON text: one record object or an array of
 * record objects (corpora additionally accept JSONL). Lifecycle:
 * register -> validate (schema + referential integrity) -> activate
 * atomically. Re-registration is a versioned append; re-registering the
 * same version with different content is an error. */
odk_spine_status odk_spine_register_json(odk_spine *spine, const char *kind,
                                         const char *json,
                                         const char *source_name,
                                         odk_spine_error *err);

/* Data-driven loading from a file (kind must not be brain_snapshot). */
odk_spine_status odk_spine_register_file(odk_spine *spine, const char *kind,
                                         const char *path,
                                         odk_spine_error *err);

/* Data-driven loading from an application-provided provider callback.
 * The provider hands over malloc'd JSON text in *out_text (ownership
 * passes to the caller of the provider, i.e. the spine frees it) and
 * returns 0 on success, nonzero on failure. */
typedef int (*odk_spine_provider_fn)(void *user, char **out_text,
                                     odk_spine_error *err);

odk_spine_status odk_spine_register_provider(odk_spine *spine,
                                             const char *kind,
                                             odk_spine_provider_fn provider,
                                             void *user,
                                             const char *source_name,
                                             odk_spine_error *err);

/* Register an application-owned brain snapshot (Tier B). The spine
 * never mutates or frees the brain; snapshot_id must be unique. */
odk_spine_status odk_spine_register_brain(odk_spine *spine, odk_brain *brain,
                                          const char *snapshot_id,
                                          odk_spine_error *err);

/* Deterministic listing of the ACTIVE customization state in
 * registration order — exactly what a decision pin records. Caller
 * releases with odk_spine_registration_ref_list_release(). */
odk_spine_status odk_spine_registrations(const odk_spine *spine,
                                         odk_spine_registration_ref_list *out,
                                         odk_spine_error *err);
void odk_spine_registration_ref_list_release(
    odk_spine_registration_ref_list *list);

/* As-of lookups (F2 semantics): version > 0 fetches that exact version;
 * version == -1 selects, among the versions whose validity interval
 * covers t, the one with the newest valid_from (tie: highest version).
 * The returned pointers stay owned by the spine. */
odk_spine_status odk_spine_scheme_at(const odk_spine *spine, const char *id,
                                     int64_t version, int64_t t,
                                     const odk_spine_scheme **out,
                                     odk_spine_error *err);
odk_spine_status odk_spine_template_at(
    const odk_spine *spine, const char *id, int64_t version, int64_t t,
    const odk_spine_decomposition_template **out, odk_spine_error *err);
odk_spine_status odk_spine_vocabulary_at(
    const odk_spine *spine, const char *id, int64_t version, int64_t t,
    const odk_spine_vocabulary **out, odk_spine_error *err);
const odk_brain *odk_spine_brain_at(const odk_spine *spine,
                                    const char *snapshot_id);

/* ------------------------------------------------------------------ */
/* F1 - multi-scheme classification                                    */
/* ------------------------------------------------------------------ */

/* Classify one item against every ACTIVE scheme whose validity covers t,
 * in registration order: one call, N schemes, one result document.
 *
 * Tier A (deterministic lexical/rule matching, bit-replayable) is always
 * used unless Tier B is enabled AND a brain snapshot is registered.
 * Tier B results are labeled tier "B" and carry the brain snapshot id;
 * Tier A results are labeled "A" with an empty snapshot id.
 *
 * Every classification is written back: an Observation (kind
 * "classification") and an append-only ClassificationInterval are
 * recorded on the spine. The caller owns *out and releases it with
 * odk_spine_classification_result_release(). */
odk_spine_status odk_spine_classify(odk_spine *spine,
                                    const char *subject_ref,
                                    const char *item_text,
                                    const char *scope, int64_t t,
                                    odk_spine_classification_result *out,
                                    odk_spine_error *err);

/* ------------------------------------------------------------------ */
/* F2 - temporal validity                                              */
/* ------------------------------------------------------------------ */

/* As-of classification query: "given scheme scheme_id as it existed at
 * time t, how was subject_ref classified?" Answered from the append-only
 * interval store WITHOUT recomputing: the newest stored interval whose
 * valid_from <= t wins (ties: later append; the store order is the
 * lineage). Reclassification never overwrites — it appends. */
odk_spine_status odk_spine_classification_at(
    const odk_spine *spine, const char *subject_ref, const char *scheme_id,
    int64_t t, const odk_spine_classification_interval **out,
    odk_spine_error *err);

/* Tier B configuration (default: disabled, so pure-replay contexts run
 * Tier A only). Tier B additionally requires a registered brain. */
void odk_spine_set_tier_b(odk_spine *spine, int enabled);
int odk_spine_tier_b_enabled(const odk_spine *spine);

/* ------------------------------------------------------------------ */
/* F3 - parameterized decomposition                                    */
/* ------------------------------------------------------------------ */

/* Decompose an item under a registered template: parameters are resolved
 * against the template's declared contract (unknown names, missing
 * required parameters and wrong types are errors), then the deterministic
 * Tier A extraction rules produce the Components document. The template
 * is resolved as-of t (template_version > 0 pins an exact version;
 * -1 selects the newest valid one). The caller owns *out and releases it
 * with odk_spine_components_release(). */
odk_spine_status odk_spine_decompose(odk_spine *spine,
                                     const char *template_id,
                                     int64_t template_version,
                                     const char *item_text,
                                     const char *params_json, int64_t t,
                                     odk_spine_components *out,
                                     odk_spine_error *err);

/* ------------------------------------------------------------------ */
/* F4 - calibration reporting                                          */
/* ------------------------------------------------------------------ */

/* Ingest one Observation record (JSON) into the spine's append-only
 * observation log. Used by the surrounding storage stack to stream a
 * decision log back for calibration and replay; the record is validated
 * against the schema and stored verbatim. */
odk_spine_status odk_spine_observation_ingest(odk_spine *spine,
                                              const char *observation_json,
                                              odk_spine_error *err);

/* Compute a calibration report (F4) over the spine's observation log:
 * predicted confidence vs observed outcome rate, per primitive, per
 * scheme, per tier, binned into ten confidence buckets. Two runs over
 * the same log produce byte-identical reports. The caller owns *out and
 * releases it with odk_spine_calibration_report_release(). */
odk_spine_status odk_spine_calibrate(odk_spine *spine,
                                     const char *generated_from,
                                     odk_spine_calibration_report *out,
                                     odk_spine_error *err);

/* ------------------------------------------------------------------ */
/* F6 - decision pinning, replay and the worldState contract           */
/* ------------------------------------------------------------------ */

/* Pin one decision: evaluate the exact request document
 * {primitive, instruction, options?, context, top_k?} with the kernel,
 * record an append-only DecisionPin (schema fingerprint, kernel version,
 * brain snapshot, context hash, the request bytes, the decision, the
 * caller-supplied timestamp and the registered customization state by
 * name + content hash) and write back a decision Observation. The
 * caller owns *out and releases it with
 * odk_spine_decision_pin_release(). replay_class is chosen by the
 * caller: "exact" (Tier A, bit-level), "semantic" (same decision,
 * different bytes) or "foreign" (recorded lineage only). */
odk_spine_status odk_spine_pin(odk_spine *spine, odk_kernel *kernel,
                               const char *request_json,
                               const char *inputs_ref,
                               const char *replay_class, int64_t timestamp,
                               odk_spine_decision_pin *out,
                               odk_spine_error *err);

/* Replay a pinned decision: load pin + pinned request bytes ->
 * re-derive with the kernel -> compare -> report. The report document
 * (malloc'd JSON, caller frees) has fixed key order and carries the
 * replay class verdict: exact / semantic / mismatch, or "foreign" for
 * pins whose replay_class is foreign (recorded lineage is not
 * re-derivable locally). */
odk_spine_status odk_spine_replay(odk_spine *spine, odk_kernel *kernel,
                                  const char *pin_id, char **report_json,
                                  odk_spine_error *err);

/* Evaluate an exact request document (the same shape a pin records):
 * {primitive, instruction, options?, context, top_k?}. Returns the
 * decision string, its confidence and the kernel's own result JSON
 * (both malloc'd, caller frees). Exposed for sidecars and integrations
 * that re-derive pinned decisions. */
odk_spine_status odk_spine_evaluate_request(odk_kernel *kernel,
                                            const char *request_json,
                                            char **decision,
                                            double *confidence,
                                            char **result_json,
                                            odk_spine_error *err);

/* Pin store read access. */
size_t odk_spine_pin_count(const odk_spine *spine);
const odk_spine_decision_pin *odk_spine_pin_at(const odk_spine *spine,
                                               size_t i);
const odk_spine_decision_pin *odk_spine_pin_get(const odk_spine *spine,
                                                const char *pin_id);

/* The worldState(variables, scope, t) contract: validate a rendered
 * context document against the WorldStateContext schema. scope is
 * opaque and application-defined (it must parse as a JSON object, array
 * or string; its content is never interpreted). The spine validates the
 * shape; it does NOT implement storage. */
odk_spine_status odk_spine_worldstate_validate(const char *json,
                                               odk_spine_error *err);

/* ------------------------------------------------------------------ */
/* Append-only stores (read access for the surrounding stack)          */
/* ------------------------------------------------------------------ */

size_t odk_spine_observation_count(const odk_spine *spine);
const odk_spine_observation *odk_spine_observation_at(const odk_spine *spine,
                                                      size_t i);
size_t odk_spine_interval_count(const odk_spine *spine);
const odk_spine_classification_interval *odk_spine_interval_at(
    const odk_spine *spine, size_t i);

#ifdef __cplusplus
}
#endif

#endif /* ODK_SPINE_H */
