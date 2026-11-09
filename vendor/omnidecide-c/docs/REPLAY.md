<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# Replay semantics (v2.0.0)

Total replay (invariant I3): **every decision produced by v2.x must be
re-derivable from its pin plus the pinned inputs.** Replay results are
compared and reported, never assumed.

## The pin

A `DecisionPin` is an append-only record containing, at minimum:

| Field | Meaning |
|---|---|
| `schema_fp` | schema fingerprint — keeps historical pins interpretable after schema evolution |
| `kernel_version` | the library version that made the decision |
| `brain_snapshot_id` | the registered brain snapshot at decision time ("" when none) |
| `context_hash` | FNV-1a 64 of the exact context bytes |
| `inputs_ref` | application-side reference to the pinned inputs |
| `request_json` | the exact request document — the kernel input, byte for byte |
| `decision`, `confidence` | the recorded outcome |
| `tier` | "A" or "B" |
| `replay_class` | declared by the application at pin time |
| `timestamp` | caller-supplied; the kernel never reads the wall clock |
| `registrations` | the active customization state by name + content hash |

`request_json` riding in the pin is what makes replay self-contained;
`inputs_ref` remains the application's pointer into its own storage.

## Replay classes

The application declares the class at pin time; replay reports the
verdict:

- **`exact`** — the decision was produced by Tier A only. Replay
  re-derives with the same kernel: bit-level identity is required. The
  verdict is `exact` (decision text and confidence identical) or
  `mismatch`.
- **`semantic`** — the decision was brain-assisted or otherwise allowed
  to differ in bytes. Replay re-derives and requires the SAME decision;
  byte differences are permitted. Verdict `semantic` or `mismatch`.
- **`foreign`** — the decision came from an external engine, recorded
  for lineage only. It is NOT re-derivable locally; replay reports
  `foreign` without running the kernel.

Replay refuses silently-wrong environments: if the current active
customization set differs from the pin's `registrations` (name, version
or content hash), the report's `registrations_match` is `false`.

## The report

`odk_spine_replay` returns a deterministic JSON document:

```json
{"pin_id":"pin-1","expected_class":"exact","replay_class":"exact",
 "decision_match":true,"byte_equal":true,"registrations_match":true,
 "rederived_decision":"restart the service","rederived_confidence":0.6312}
```

- `expected_class` — what the pin declared (`replay_class` field).
- `replay_class` — the verdict: `exact`, `semantic`, `foreign` or
  `mismatch`.
- `decision_match` / `byte_equal` — text-level and bit-level comparison
  of the recorded vs re-derived decision.
- `registrations_match` — the customization state equals the pinned one.
- `rederived_decision` — the re-derived decision text (`""` for foreign pins, absent from replay).
- `rederived_confidence` — the re-derived confidence score (`null` for foreign pins).

Two replays of the same pin produce byte-identical reports.

## How to replay

```c
odk_spine_replay(spine, kernel, "pin-1", &report_json, &err);
```

or through the sidecar:

```
{"id":"1","op":"replay","params":{"pin_id":"pin-1"}}
```

## What replay does NOT cover

- `foreign` decisions (by definition).
- Pins whose customization state cannot be reconstructed — the sidecar
  verifies `registrations_match` and reports it; it never fakes a
  verdict.
- Schema evolution: old pins stay interpretable thanks to `schema_fp`,
  but a pin made under schema X is replayed with the semantics of the
  schema the running build was generated from; the fingerprint in the
  report/pin pairing is how an auditor notices the difference.
