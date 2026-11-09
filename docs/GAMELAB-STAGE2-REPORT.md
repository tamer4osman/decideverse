<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# Stage 2 Completion Report — OmniDecide GameLab

Date: 2026-11-09 · Baseline: `omnidecide-wasm-1.0.0` (kernel 2.0.0) ·
Scope: shared GameLab architecture + The Last Detective (first playable game)
+ Decision Inspector, history, benchmarks, reset/replay, tests, docs.

## 1. Result summary

| Completion criterion | Status |
|---|---|
| 1. GameLab launches in a browser | ✅ Chromium 143 (Playwright) verified; WASM fetched with `application/wasm` |
| 2. Dashboard and launcher work | ✅ live engine status, real statistics, 5 modules (1 playable, 4 planned) |
| 3. Real WASM engine initializes | ✅ kernel 2.0.0, backend `embedded-fallback`, no JS fallback |
| 4. The Last Detective playable end-to-end | ✅ briefing → investigation → accusation → resolution |
| 5. Game triggers real OmniDecide decisions | ✅ 5 primitives exercised per playthrough (`choice`, `compare`, `noul`, `extract`, `rank`) |
| 6. Requests and outputs inspectable | ✅ Decision Inspector: submitted input / engine output / game interpretation |
| 7. Simulation history works | ✅ distinguishable event + decision streams, filterable, linked |
| 8. Reset and replay deterministic | ✅ replay re-invokes the real kernel; envelopes byte-identical (5/5) |
| 9. Performance measurements real | ✅ monotonic timing around actual kernel calls, session-local |
| 10. Stage 1 verification suite green | ✅ 78/78 Node + 29/29 browser, untouched |
| 11. Stage 2 tests + E2E pass | ✅ 156/156 Node + 57/57 browser |
| 12. Documentation and screenshots | ✅ 8 docs + entry point + 12 fresh screenshots |

**Total verification: 320 checks, 0 failures across four suites.**

## 2. Architecture and directory structure

Five separated components (full detail: `gamelab/docs/ARCHITECTURE.md`):

- **A. GameLab UI** — `gamelab/js/main.js` (hash router), `views/*`, `core/workspace.js`
- **B. Simulation framework** — `core/session.js`, `clock.js`, `rng.js`, `history.js`, `telemetry.js`, `registry.js`, `eventbus.js`, `dom.js`
- **C. Game modules** — `games/detective/` (playable) + 4 planned registry entries in `games/index.js`
- **D. OmniDecide adapter** — `core/adapter.js` (host-agnostic, injectable engine loader)
- **E. History & telemetry** — shared singletons wired to the adapter

Technology decision: **vanilla ES modules + CSS, zero new dependencies, no
build step** — matching the project's zero-dependency philosophy (UMD API,
static playground, plain Node test scripts). No framework was added.

New files (33 + 12 screenshots + 2 build reports):

```
gamelab/index.html, README.md
gamelab/styles/gamelab.css, detective.css
gamelab/js/main.js
gamelab/js/core/{eventbus,dom,clock,rng,adapter,history,telemetry,registry,session,workspace}.js
gamelab/js/core/{history,telemetry}-instance.js
gamelab/js/views/{dashboard,launcher,simulation,inspector,historyview,benchmarks,about}.js
gamelab/js/games/index.js
gamelab/js/games/detective/{scenario,decisions,replay,module}.js
gamelab/docs/{ARCHITECTURE,ADAPTER,SIMULATION-FRAMEWORK,GAME-MODULES,LAST-DETECTIVE,TESTING,LIMITATIONS,ROADMAP}.md
gamelab/docs/img/*.png (12 screenshots, regenerated each E2E run)
tests/gamelab.test.mjs
tests/gamelab_browser_check.mjs
docs/GAMELAB.md, docs/GAMELAB-STAGE2-REPORT.md (this file)
build/gamelab-browser-check.json
```

Modified files: **none** — every Stage 1 file (`dist/`, `js/`, `web/`, `src/`,
`vendor/`, `scripts/`, `.github/`, Stage 1 tests and docs) is byte-untouched;
all Stage 2 work is additive.

## 3. Application startup

```bash
python3 -m http.server 8000        # repository root, HTTP(S) required
# open http://127.0.0.1:8000/gamelab/index.html
```

Script order is contractual: `dist/omnidecide.js` (Emscripten loader) →
`js/omnidecide.js` (UMD API) → `gamelab/js/main.js` (ES modules). The engine
initializes immediately; the dashboard reflects loading/ready/error live, with
a retry action on failure. No decisions can exist without the kernel.

## 4. Actual WASM integration details

- The adapter calls `kernel.evaluateRaw(request)` — the byte-identical
  envelope string from the C wrapper — parses it for display, and times the
  whole pipeline (marshal → wrapper → parse) with a monotonic timer.
- Validation is layered: adapter shape checks first, kernel semantics second.
  Verified against the real module: a `topK` of 2³² passes the adapter and is
  rejected by the kernel wrapper with `OmniDecideError` status 1 (ARGUMENT)
  plus the raw C-authored error envelope — preserved, recorded, never hidden.
- Initialization failure (e.g. blocked `.wasm`) surfaces as a visible error
  state on every page with a retry; invocations then fail cleanly with
  `AdapterNotReady` records. No silent fallback anywhere.
- Lifecycle: one shared Emscripten module, one kernel handle, `engineCount()`
  shown in the UI, settings changes re-create the handle through the real
  adapter path.

## 5. The Last Detective — gameplay description

Case AM-2026-1108: the Meridian Diamond vanishes from the Aldermoor Museum
during the evening gala. Nine investigation hours across five locations; ten
evidence items (E-00 … E-08, E-04b) whose exact texts form the ledger
submitted to the kernel; four suspects. Decision moments: lead consultation
(`choice`, with the transparent "+1 hour if followed" rule), statement
weighing (`compare`, tie handled honestly), courier claim check (`noul`),
case brief (`extract` topK 3), suspect ranking (`rank`). Accusation ends the
case; the scenario — never the engine — decides the culprit. With the full
ledger the kernel's `rank` genuinely puts Adrian Cole first
(`Adrian Cole > Elena Vasquez > Marcus Webb > Lena Fontaine`, confidence
0.0354, shown honestly). Wrong arrest and unsolved endings exist. Reset
snapshots the run; replay re-executes it through the pure reducer with real
re-invoked kernel calls. Full design: `gamelab/docs/LAST-DETECTIVE.md`.

## 6. Example decision requests and actual outputs

All below are real kernel envelopes captured during verification.

**Lead consultation (early game, `choice`):**
```json
request:  {"primitive":"choice",
           "instruction":"Which investigation lead is best supported by the evidence currently collected?",
           "options":["Pull the corridor camera archive","Interview Dr. Elena Vasquez","Inspect the courier log","Examine the display case"],
           "context":"E-00: … E-03: … E-02: …"}
response: {"primitive":"choice","result":"Examine the display case","confidence":0.3255,
           "probability":0.3255,"distribution":{"Examine the display case":0.3255,…}}
```

**Statement weighing (`compare`):**
```json
response: {"primitive":"compare",
           "result":"The maintenance schedule board shows no authorized maintenance for the corridor camera on gala night.",
           "confidence":0.666}
```

**Courier claim check (`noul`):**
```json
response: {"primitive":"noul","result":true,"confidence":0.8532}
```

**Final suspect ranking (`rank`, full 1290-char ledger):**
```json
response: {"primitive":"rank",
           "result":"Adrian Cole > Elena Vasquez > Marcus Webb > Lena Fontaine",
           "confidence":0.0354,"probability":0.3256,
           "distribution":{"Adrian Cole":0.3256,"Elena Vasquez":0.23,"Marcus Webb":0.225,"Lena Fontaine":0.2194}}
```

**Case brief (`extract` topK 3):** returns the replica, the curator's private
viewing statement and the unauthorized-maintenance evidence (exact snippets
visible in the workspace screenshot).

## 7. Test results

| Suite | Checks | Result |
|---|---|---|
| `tests/omnidecide.test.mjs` (Stage 1, untouched) | 78 | ✅ 0 failures |
| `tests/browser_check.mjs` (Stage 1, untouched) | 29 | ✅ 0 failures |
| `tests/gamelab.test.mjs` (Stage 2 Node) | 156 | ✅ 0 failures |
| `tests/gamelab_browser_check.mjs` (Stage 2 E2E) | 57 | ✅ 0 failures |

The Stage 2 Node suite covers the framework modules, the adapter against the
real engine (all six primitives, validation, kernel error path, determinism,
re-init, shutdown, init failure), scenario determinism and budget rules, the
canonical 15-step playthrough with five real decisions, byte-identical replay
and the engine/simulation separation guard. The browser E2E implements the
required 12-step flow plus failure handling, responsive and hygiene checks.
Nothing was skipped; no Stage 1 test was weakened.

## 8. Browser verification

Playwright + Chromium 143 (headless), fresh runs: real WASM fetch with
`application/wasm` MIME (HTTP 200), engine ready in the top bar, full game
path, zero console errors, zero page errors, zero failed network requests,
initialization-failure page (WASM blocked) shows the error state and produces
no decisions. Report: `build/gamelab-browser-check.json`. Screenshots:
`gamelab/docs/img/gamelab-*.png` (dashboard, launcher, briefing,
investigation, workspace, inspector, history, benchmarks, ending, replay,
about, responsive-390).

## 9. Performance observations

Measured live (monotonic timer around the actual kernel call; Node 24,
this verification host):

| Measurement | Value |
|---|---|
| Engine initialization (Node) | 3.29 ms |
| Engine initialization (browser, E2E run) | 12.20 ms (dashboard display) |
| Full-ledger `rank` decision (1290 chars, 100 samples) | min 0.394 ms · p50 0.443 ms · p90 0.553 ms · mean 0.466 ms |
| Throughput, same workload | ≈ 2,144 decisions/s |
| In-game observed durations (browser, E2E) | ≈ 0.7–3.7 ms per decision (first-call effects included) |
| Full replay (15 steps incl. 5 real decisions) | ≈ 5 ms wall |

GameLab's Benchmark Overview shows only these session-local measurements —
never the Stage 1 release benchmarks, which use a different methodology
(isolated-C raw loop) and live separately in `docs/benchmarks/`.

## 10. Known limitations

Summarized in `gamelab/docs/LIMITATIONS.md`: lexical matching is not
understanding; low rank confidence is normal and shown honestly; single-item
`score` rubrics are uninformative (primitive exposed but not used by the
detective); `compare` may return `"tie"`; no persistence (page-memory
sessions); one kernel handle; session-local benchmarks; fixed scenario
content; the four planned games are scopes, not stubs; Stage 2 CI wiring is
left additive and optional.

## 11. Recommendations for Stage 3

1. **Living Village** first (fully specified in `gamelab/docs/ROADMAP.md`) —
   it exercises `choice` arbitration over generated goals plus memory/needs
   modeling, the widest new coverage with the existing framework unchanged.
2. Wire `tests/gamelab.test.mjs` into the existing CI pipeline as a new
   parallel job, then the Playwright E2E (additive jobs; existing seven jobs
   untouched).
3. Add persistence (localStorage export of session history + decision logs)
   and a JSON decision-log export button in the inspector.
4. Consider a detective case editor to demonstrate that engine output follows
   submitted evidence — a strong honest-engine showcase.
5. Keep the purity boundary (pure scenario/decision modules, injected
   adapters) — it is what made deterministic replay and Node-side real-kernel
   testing possible in Stage 2.
