<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# Spine API reference (v2.0.0)

The kernel API (`choice`, `score`, `noul`, `rank`, `compare`, `extract`)
is documented in [API.md](API.md) and is unchanged in 2.0.0. This
document covers the integration spine (`include/odk_spine.h`), the
records are the generated ones from `odk-spine/odk_spine_types.h`
(single source of truth: `schema/omnidecide.schema`).

Every call reports through `odk_spine_error` (kernel style) and returns
`odk_spine_status`:

| Status | Meaning |
|---|---|
| `ODK_SPINE_OK` | success |
| `ODK_SPINE_ERR_ARGUMENT` | API misuse: NULL, bad JSON, bad name |
| `ODK_SPINE_ERR_MEMORY` | allocation failure |
| `ODK_SPINE_ERR_SCHEMA` | record failed schema validation |
| `ODK_SPINE_ERR_REFERENTIAL` | dangling reference in registered data |
| `ODK_SPINE_ERR_NOT_FOUND` | unknown id, version or time interval |
| `ODK_SPINE_ERR_STATE` | illegal lifecycle transition |
| `ODK_SPINE_ERR_RANGE` | numeric value outside the exact range |

## Handle

```c
odk_spine *odk_spine_create(void);   /* zero registered customizations */
void odk_spine_free(odk_spine *spine);
const char *odk_spine_schema_fingerprint(const odk_spine *spine);
```

## F1 — classification

```c
odk_spine_status odk_spine_classify(odk_spine *spine,
                                    const char *subject_ref,
                                    const char *item_text,
                                    const char *scope, int64_t t,
                                    odk_spine_classification_result *out,
                                    odk_spine_error *err);
```

One call, N schemes, one result document: every ACTIVE scheme whose
validity covers `t`, in registration order. Tier A (deterministic
lexical/rule matching, bit-replayable) always runs; Tier B (brain-assisted
paraphrase resilience) runs only when enabled AND a brain snapshot is
registered. Results are labeled `tier: "A"|"B"` and Tier B results carry
the `brain_snapshot_id`. Every classification is written back as an
Observation plus an append-only ClassificationInterval.

Confidence model (deterministic): rule match with all terms → 0.9;
any-term rule → `0.5 + 0.4 * matched/total`; no rule → 0.5. Tier B link
evidence weighs 0.5 per term. The engine is word-bounded and case-folded
(ASCII), with no stemming — paraphrase resilience is Tier B's job, which
is exactly the documented v1 weakness this design addresses.

## F2 — temporal validity

```c
odk_spine_status odk_spine_classification_at(
    const odk_spine *spine, const char *subject_ref, const char *scheme_id,
    int64_t t, const odk_spine_classification_interval **out,
    odk_spine_error *err);
```

As-of classification query: answered from the append-only interval store
WITHOUT recomputing. The newest stored interval with `valid_from <= t`
wins (ties: later append — the store order is the lineage).
Reclassification never overwrites; it appends.

As-of lookups for registered data (version `-1` = newest valid at `t`):
`odk_spine_scheme_at`, `odk_spine_template_at`, `odk_spine_vocabulary_at`,
`odk_spine_brain_at`.

## F3 — decomposition

```c
odk_spine_status odk_spine_decompose(odk_spine *spine,
                                     const char *template_id,
                                     int64_t template_version, /* -1 = as-of */
                                     const char *item_text,
                                     const char *params_json, int64_t t,
                                     odk_spine_components *out,
                                     odk_spine_error *err);
```

Parameters resolve against the template's declared contract (unknown
names, missing required parameters, wrong types: errors). Extraction
modes: `sentence` (sentences containing any term), `before`/`after`/
`around` (character window around the first match), `keyvalue` (lines
whose key matches a term). A required component left empty is a contract
violation (error). Output bytes are deterministic; on failure `*out` is
left zeroed.

## F4 — calibration

```c
odk_spine_status odk_spine_observation_ingest(odk_spine *spine,
                                              const char *observation_json,
                                              odk_spine_error *err);
odk_spine_status odk_spine_calibrate(odk_spine *spine,
                                     const char *generated_from,
                                     odk_spine_calibration_report *out,
                                     odk_spine_error *err);
```

Reports compare predicted confidence against observed outcome rate, per
primitive, per scheme, per tier, binned into ten confidence buckets, with
sample counts and per-bin calibration error. Two runs over the same log
produce byte-identical reports.

## F6 — pinning, replay, worldState

```c
odk_spine_status odk_spine_pin(odk_spine *spine, odk_kernel *kernel,
                               const char *request_json,
                               const char *inputs_ref,
                               const char *replay_class, int64_t timestamp,
                               odk_spine_decision_pin *out,
                               odk_spine_error *err);
odk_spine_status odk_spine_replay(odk_spine *spine, odk_kernel *kernel,
                                  const char *pin_id, char **report_json,
                                  odk_spine_error *err);
odk_spine_status odk_spine_worldstate_validate(const char *json,
                                               odk_spine_error *err);
```

See [REPLAY.md](REPLAY.md) for the pin record, the replay classes and
the report shape. `timestamp` is caller-supplied — nothing in the spine
reads the wall clock. The worldState contract validates the rendered
context shape; `scope` stays opaque and is never interpreted.

## Sidecar runtime

```sh
build/odk_sidecar                  # JSONL over stdio
build/odk_sidecar --http 8130      # HTTP/1.1 on 127.0.0.1:8130 (POST /rpc)
build/odk_sidecar --http 8130 --data examples/synthetic/data
```

One request per line (stdio) or per POST body (HTTP):
`{"id":"...","op":"...","params":{...}}` →
`{"id":"...","ok":true,"result":{...}}` or
`{"id":"...","ok":false,"error":"..."}`. Responses are byte-identical
for identical requests given the same registered state; the id is echoed
verbatim. Ops: `ping`, `register`, `registrations`, `evaluate`,
`classify`, `decompose`, `pin`, `replay`, `calibrate`,
`worldstate.validate`, `observation.ingest`. HTTP responses carry CORS
headers (no auth, no tenancy: out of scope for 2.0.0).

## WASM target

`make wasm` builds `build/omnidecide.wasm` via emscripten with the same
primitive surface and the same determinism (fp-contract stays off).
Host-imposed limits of the shipped profile: single-threaded, no
filesystem (brain snapshots move through memory exports), sidecar
surfaces are host-side only.

## Benchmarks

`make bench` runs the 2.0.0 baseline: Tier A decision latency budget
≤ 1 ms @ 2 KB context (measured 0.28 ms on the CI host), classification
throughput baseline ≈ 22,000 classifications/s @ 2 KB, pin+replay
round-trip ≈ 0.7 ms. Any regression > 10 % requires written
justification in the PR.
