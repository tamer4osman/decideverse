<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# Spine golden corpus

The v2.0.0 golden corpus for the new engines (Section 5 of the v2.0.0
brief): every feature landed with its corpus additions in the same
change, and **corpus diffs are reviewed like code** — every diff must be
explained (in the PR body / changelog entry).

## Files

- `inputs.jsonl` — the cases. Each line is one case:
  `{"case_id", "kind", "input"}`. Cases run in file order against one
  spine whose registered customization state is the synthetic example
  fixtures (loaded through the F7 extension API — the harness stays
  generic; corpora and schemes are data).
- `expected.jsonl` — the golden outputs, one line per case, in the same
  order. Output bytes are deterministic.

## Case kinds and what they cover (Section 5 mapping)

| Cases | Feature | Coverage |
|---|---|---|
| `cls-facet-*`, `cls-default` | F1 | per-scheme facet cases and the default label |
| `cls-multi-scheme` | F1 | one call, N schemes, one result document |
| `cls-paraphrase-tierA` vs `cls-paraphrase-tierB` | F1 Tier B | the paraphrase set is the acceptance test for Tier B value-add: Tier A misses, Tier B classifies via vocabulary canonicalization |
| `asof-*`, `reclassify` | F2 | reclassification lineages and as-of queries at multiple times; `asof-missing` covers the not-found path |
| `dec-*` | F3 | per-template decomposition goldens including parameterized variants, window mode, as-of template versions, contract violations |
| `obs-*`, `calibrate` | F4 | a fixed decision log with a known expected report |
| `pin-*`, `replay-*` | F6 | pin → replay → byte-equal round-trips; one `exact`, one `semantic` and one `foreign` pin |
| `ws-*` | F6 | worldState contract validation, valid and invalid |

## Workflow

```sh
make spine-golden-check    # verify outputs are byte-identical
make spine-golden-update   # regenerate expected.jsonl after an INTENTIONAL change
```

After an intentional behavior change: run `spine-golden-update`, inspect
the diff line by line, and explain every changed line in the PR body.
Unexpected diffs are bugs (the corpus has already caught one: the Tier A
engine initially recorded `scheme_version: 0`).
