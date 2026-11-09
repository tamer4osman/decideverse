<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# Benchmarks

> **Decideverse context:** engine-layer performance evidence — the
> responsiveness budget every Decideverse game arbitration runs inside.
> The engine is finished at 1.0.0; these numbers are its recorded
> snapshot.

Tools: `tests/node_bench.mjs` (Node) and `tests/browser_bench.mjs`
(Chromium via Playwright). Both drive the **shared** workload library
`js/benchmark.js` inside the page/process against the real
`dist/omnidecide.wasm` — no synthetic JavaScript workloads, no mocked
decisions, no reused historical numbers. Every run writes fresh
machine-readable JSON to `docs/benchmarks/` plus a timestamped copy.

```bash
node tests/node_bench.mjs --scale standard
node tests/browser_bench.mjs --scale standard
node tests/node_bench.mjs --scale quick     # reduced iterations
node tests/node_bench.mjs --scale standard --json /tmp/out.json
```

## Workloads (standard scale)

| Workload | Target context | Iterations |
|---|---:|---:|
| small | ~200 bytes | 2000 |
| medium | ~2 KB | 800 |
| large | ~20 KB | 150 |
| unicode (中文) | ~2 KB | 800 |

`--scale quick` divides iterations (300/150/40/150) for smoke runs.
Each workload is a `choice` decision over a generated context sized to
the target (the suite asserts the sizes in the functional tests).

## What is measured

**Initialization** — WASM module load, kernel creation, first-decision
latency (reported in the JSON `init` block and in the console header).

**Per workload** — iterations, context size, average, median, p95,
p99, min, max, decisions/s, total time.

**Two timings per workload, never conflated:**

1. **Pipeline** — full JavaScript→WASM decision: string marshaling
   into the WASM heap + wrapper call + JSON envelope parse. This is
   what an application pays per decision.
2. **Isolated C (`rawLoop`)** — the identical request pre-marshaled
   once, then the C evaluation looped without per-call JavaScript
   marshaling. This isolates wrapper + kernel cost.

The isolated C number is **not** described as isolated kernel-only
time either: it includes the wrapper's envelope composition. Both
labels travel with the data (JSON `notes` array and the console
footer).

## Integrity rules

- Fresh execution only; no thresholds are imposed (numbers are
  host-dependent — measure, don't gate).
- Environment metadata embedded in every JSON: host, Node version,
  Emscripten version, kernel provenance, browser version (browser
  runs), WASM response metadata (browser runs).
- Results live next to the code: `docs/benchmarks/node-bench.json`,
  `docs/benchmarks/browser-bench.json`.

## Reference numbers (release host, standard scale)

Recorded in `docs/WASM-STATUS.md` §7 (Node: median 0.053 ms small →
1.802 ms large, 14.6k → 554 ops/s; Chromium: 0.100 ms → 1.900 ms,
14.3k → 527 ops/s). Treat them as one machine's snapshot, not a
contract.
