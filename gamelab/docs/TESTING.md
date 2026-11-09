<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# GameLab Testing

Stage 2 keeps **all Stage 1 suites intact and green** and adds its own
suites; Stage 2.1 adds lifecycle / action-tracking / seed-replay regression
coverage; Stage 2.1.1 adds cleanup-ownership / recovery regression coverage.
No Stage 1 test was weakened or skipped.

## Suites

| Suite | Command | Scope | Status |
|---|---|---|---|
| Stage 1 Node functional | `node tests/omnidecide.test.mjs` | kernel integration, primitives, determinism, errors, unicode | **78/78** |
| Stage 1 browser acceptance | `node tests/browser_check.mjs` | playground over Playwright/Chromium | **29/29** |
| Stage 2 Node | `node tests/gamelab.test.mjs` | framework + adapter + scenario + full game path vs the **real kernel** | **156/156** |
| Stage 2 browser E2E | `node tests/gamelab_browser_check.mjs` | the complete GameLab product flow (detective **and** Living Village) + 2.1 + 2.1.1 + 390px regressions | **121/121** |
| Stage 3A Living Village Node | `node tests/gamelab_village.test.mjs` | village reducer determinism, steward paths, rules R1–R3 both ways, output-follows-input, playthroughs, byte-identical replay, module-level action-log/seed/replay semantics — all vs the **real kernel** | **156/156** |
| Stage 2.1 adapter lifecycle | `node tests/adapter_lifecycle.test.mjs` | init/recreate/shutdown/error paths, **cleanup ownership & recovery**, handle accounting vs the **real WASM module** | **158/158** |
| Stage 2.1 action tracking + seed/replay | `node tests/gamelab_state.test.mjs` | the REAL game module in a DOM shim, real session, real engine | **53/53** |

## Setup (once)

```bash
cd tests && npm ci && npx playwright install --with-deps chromium
cd ..
```

## Stage 2 Node suite — what it covers

1. **Application foundation** — event bus (delivery, unsubscribe, isolation),
   deterministic clock, seeded RNG streams, telemetry math (counts,
   avg/min/max, reset), history ring buffers and id stability.
2. **WASM adapter** (real engine, CommonJS module injected as loader):
   - initialization + readiness + measured init duration;
   - valid request mapping for **all six primitives** with real envelopes;
   - result normalization (`raw` string + parsed envelope) and history /
     telemetry wiring;
   - adapter-side validation (unknown primitive, compare arity, topK,
     empty instruction);
   - kernel-side error path (topK 2³² → real `OmniDecideError` status 1 with
     the raw C-authored error envelope) — the engine stays ready;
   - repeated decisions byte-identical; re-init after shutdown reproduces the
     identical envelope; `engineCount` lifecycle; shutdown semantics;
   - initialization-failure handling (broken loader → visible error state, no
     decisions, clean `AdapterNotReady` records).
3. **Simulation framework / The Last Detective scenario** — initial state,
   phase transitions, event streams, evidence discovery, budget and
   re-take rules, decision builder shapes, **seed determinism** (identical
   state and identical events for identical sequences), endings (win / wrong /
   unsolved) and the accusation threshold.
4. **Full playable path + replay against the real engine** — the canonical
   15-step run (briefing, 8 actions, 5 real decisions, accusation), the rank
   alignment check (with full evidence the kernel ranks the actual culprit
   first), replay with **byte-identical envelopes** and identical final state,
   reset determinism, and the engine/simulation separation guard (the win
   ending without any engine involvement).

## Stage 2 browser E2E — the 12 required steps + 2.1 regressions

1. open GameLab → 2. wait for the **real engine** (also asserts WASM fetched
with `application/wasm` MIME and no JS fallback) → 3. launch The Last
Detective → 4. review the case introduction (and select a **custom seed**,
Stage 2.1 C) → 5. discover evidence → 6. trigger a real OmniDecide decision
(`choice` consultation) → 7. inspect the submitted request (exact JSON incl.
the evidence ledger) → 8. inspect the returned result (parsed envelope +
raw bytes) → 9. continue investigating (compare / noul / extract / rank
advisories) → 10. complete the case (accusation panel, correct ending) →
11. reset → 12. replay with a byte-identical report.

Stage 2.1 browser regressions (same suite, real WASM, nothing mocked):

- **A — adapter lifecycle** through the settings UI: recreate (exactly one
  live handle afterwards), a real decision after recreation, shutdown (no
  managed handle, decisions rejected), initialize again (usable again,
  accounting balances).
- **B — action history**: the displayed "Actions logged" counter matches
  the authoritative `session.actionLog` at every stage (3 after begin + 2
  investigations, 4 after the consultation, unchanged after an invalid
  double-submitted action whose rejection appears in the feed, 15 at
  completion, 0 after reset, still 0 after replay).
- **C — seed synchronization**: the selected custom seed (4711) shows in
  the workspace, matches the session, survives reset and stays untouched by
  replay.

Additional checks: dashboard engine card, launcher module states (1 playable /
4 planned, no fake playables), history tabs, benchmark overview fed by real
samples, About settings applying for real, responsive layout at 390×844 with
no horizontal overflow, initialization-failure page (WASM blocked → visible
error state, no decisions), zero console errors, zero page errors, zero failed
requests.

Fresh screenshots are written to `gamelab/docs/img/` on every run; a machine
readable report lands in `build/gamelab-browser-check.json`.

## Reproduce everything

```bash
node tests/omnidecide.test.mjs
node tests/gamelab.test.mjs
node tests/adapter_lifecycle.test.mjs
node tests/gamelab_state.test.mjs
node tests/gamelab_village.test.mjs
node tests/browser_check.mjs
node tests/gamelab_browser_check.mjs
# or: ./scripts/verify-release.sh   (Stage 1 gates, unchanged)
```

## Stage 2.1 regression suites — what they cover

### `adapter_lifecycle.test.mjs` (real WASM, doubles only for isolated error paths)

1. initialize from uninitialized (exactly one live handle); 2. repeated
`init()` while ready is explicitly rejected and allocates nothing; 3.
decision while ready; 4. `recreate()` with a valid configuration (previous
handle released — still exactly one); 5. decision after recreation
(byte-identical envelope); 6. repeated recreation (no accumulation); 7.
shutdown; 8. repeated shutdown (no double free); 9. initialize after
shutdown; 10. recreate after shutdown is rejected; 11. decision after
shutdown → `AdapterNotReady`; 12. initialization failure (no hidden
allocation); 13. recreation failure (previous kernel stays genuinely ready,
`RecreationFailed` preserves the kernel status, recovery works); 14.
error-state retry via `init()`; 15. cleanup failures recorded and reported
(never swallowed), including a shutdown whose `free()` throws; plus the
shutdown-during-initialization epoch race (the stale create disposes
itself) and the canonical spec sequence ending with **zero live kernel
handles**.

**Stage 2.1.1 cleanup-ownership coverage** (same suite): a recreation whose
old-instance `free()` fails keeps the handle **adapter-owned** in the
pending-cleanup registry (reason, attempt count and original error
visible); recreation is **deferred** (`PendingCleanupUnresolved`) and
allocates nothing while a handle is unresolved; `retryCleanup()` performs
the safe retry — it stays pending when the failure persists and releases
the handle once it clears; `shutdown()` retries pending entries, reports
`incompleteShutdown` honestly and never claims completion while a handle
is owned; initialization is deferred until recovery; a **partial release**
(free() completing on the C side, then throwing) is resolved from release
evidence with **no second real free** (no double-free); repeated
recreation with intermittent failures never accumulates handles; every
scenario asserts the exact ownership invariant
`created === released + activeHandles + pendingCleanup` against the real
module counter, and a test-fixture emergency sweep — run only AFTER the
assertions — must always find **zero orphaned handles**, proving the
adapter (not the test) owned every cleanup.

Handle accounting uses the REAL module counter (`odkw_kernel_count`)
through a short-lived probe kernel — no invented metrics, no mock replacing
the WASM lifecycle.

### `gamelab_state.test.mjs` (real game module + real session + real engine)

The suite mounts the actual `createDetectiveModule()` into a minimal DOM
shim and clicks the real handlers, with decisions flowing through the same
shared adapter singleton the browser app uses:

- one successful action = exactly one shared action-history entry; several
  actions = the exact count; a rejected action (reproduced as the browser
  double-click: stale node → reducer rejection) never increments; a failed
  engine invocation (real `AdapterNotReady` record after a real shutdown)
  is not a player action; an accepted decision counts once; no double
  counting (runLog = actions + failed attempts); the workspace counter
  equals the authoritative log; reset starts a new run at 0 and keeps the
  previous run replayable; the replay log reconstructs briefing + 8 actions
  + 5 decisions + accusation.
- seed ownership: default seed initializes; a custom briefing seed becomes
  the authoritative session seed and the scenario seed; the workspace
  displays the effective seed; reset retains it; replay reuses it and
  reproduces byte-identical envelopes plus the exact final state; identical
  seed + sequence = identical outcome; different seeds differ ONLY in the
  defined flavor stream (never in evidence, budget or endings); invalid
  seeds follow the documented rules (non-numeric → default, out-of-range →
  unsigned 32-bit wrap); a missing briefing input falls back consistently.

## Rules

- Essential tests are never silently skipped; an unmet expectation fails the
  suite with a nonzero exit status (same policy as Stage 1).
- Every "real engine" claim in a test means the compiled WASM module ran —
  the Node suite injects the project's own CommonJS integration, the browser
  suites load `dist/omnidecide.wasm` over HTTP with the correct MIME type.
