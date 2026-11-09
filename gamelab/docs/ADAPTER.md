<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# The OmniDecide Adapter

`gamelab/js/core/adapter.js` is the single boundary between GameLab and the
real WebAssembly-backed decision engine. Games never touch the kernel
directly; everything goes through `adapter.invoke()`.

## What it wraps

The **actual** project integration — `js/omnidecide.js` (zero-dependency UMD
API) over `dist/omnidecide.wasm` (OmniDecide C kernel 2.0.0). The adapter
exposes exactly the six upstream primitives:

| Primitive | Purpose | GameLab usage in Stage 2 |
|---|---|---|
| `choice` | pick one of N options | lead consultation |
| `score` | score rubric items | exposed, not used by the detective (single-item rubrics return uninformative values — documented in LIMITATIONS.md) |
| `noul` | nullness / statement support test | courier claim check |
| `rank` | order N options | final suspect ranking |
| `compare` | compare exactly two options | statement weighing |
| `extract` | extract top-K items | case brief assembly |

The adapter does not invent API methods or result fields: `raw` is the
byte-identical envelope **string** produced by the C wrapper; `envelope` is
its parsed form. There is no JavaScript fallback — if the WASM module cannot
load, initialization fails visibly.

## Lifecycle (Stage 2.1; cleanup ownership Stage 2.1.1)

The adapter owns exactly **one managed kernel handle** while ready and
carries an explicit lifecycle state machine:

```
uninitialized ──init()──▶ initializing ──ok──▶ ready
       ▲                      │                 │
       │                  failure               └─recreate()─▶ recreating ──ok──▶ ready
       │                      ▼                        │              └─failure─▶ ready (previous kernel kept)
       └── shutdown() ── shutting-down ──▶ uninitialized ◄────────┘

any state ──init()/recreate() failure paths──▶ error (init only) / ready (recreate, see below)
```

States: `uninitialized` | `initializing` | `ready` | `recreating` |
`shutting-down` | `error` (surfaced on `bus` as `engine:state`).

```js
import { adapter } from '.../core/adapter.js';

adapter.state;            // current lifecycle state (see above)
await adapter.init({ mode: 'ascii', language: 'und' });   // → 'ready' | 'error' | deferred (see below)
await adapter.recreate({ mode, language }); // → { state:'ready', recreated:true } | { recreated:false, error }
await adapter.shutdown();                   // → { state:'uninitialized', released, shutdownComplete, pendingCleanup }
adapter.kernelVersion;    // '2.0.0'
adapter.backend;          // 'embedded-fallback'
adapter.initMs;           // duration of the last successful create
adapter.engineCount();    // live kernels module-wide, read through an owned handle
adapter.lifecycleStats;   // { created, released, recreations, recreateFailures,
                          //   cleanupFailures, cleanupRecoveries, lastCleanupError,
                          //   activeHandles, pendingCleanup } — real observations
adapter.hasPendingCleanup;// true while a failed-release handle is owned
adapter.pendingCleanupDetail; // sanitized registry: [{ reason, attempts, state, sinceMs, error }]
adapter.retryCleanup();   // safe retry of every pending release → { deferred, recovered, remaining, state }
```

### Initialization

- Valid from `uninitialized` and `error` (the retry path).
- While a transition is already running (`initializing`/`recreating`), the
  same in-flight promise is returned — no duplicate creation.
- While `ready`, `init()` is **explicitly rejected** (`Error` pointing to
  `recreate()`). Repeated initialization therefore never allocates a second
  handle — the Stage 2 review's leak path is closed by construction.
- **Allocation policy (Stage 2.1.1):** `init()` first attempts to recover
  pending-cleanup entries (see below). While unresolved handles remain,
  initialization is **deferred** with a `PendingCleanupUnresolved` error —
  the adapter never keeps allocating while cleanup is failing.
- Initialization failure sets state `error`, keeps the diagnostic
  (`adapter.lastError`), flags `initFailureHandled`, and every later
  `invoke()` returns a clean `AdapterNotReady` failure record — no decision
  is ever produced without the kernel.

### Recreation (`recreate(newConfig)`)

Valid only from `ready`. Replacement strategy — **create first, release
after** (Stage 2.1.1 ordering):

1. validate the new configuration (types adapter-side; semantics kernel-side);
2. **recover pending-cleanup entries first**; if unresolved handles remain,
   the recreation is **deferred** (`recreated: false`, error kind
   `PendingCleanupUnresolved`) — no replacement is allocated while cleanup
   is failing, so repeated recreation can never accumulate handles;
3. create the replacement instance with the REAL API;
4. confirm successful initialization of the replacement;
5. release the previous instance — on success the swap is clean; on failure
   the old handle is **retained in the pending-cleanup registry** (the
   reference is never dropped), the failure is reported
   (`cleanupError` in the result, `engine:cleanup-error` +
   `engine:cleanup-pending` on the bus, `lifecycleStats`), and the result
   carries `cleanupPending: true` — a degraded-but-usable state that is
   never reported as fully clean;
6. swap the managed handle and update readiness + metadata.

If creation fails, **the previous kernel stays active with its previous
configuration**: the result reports `recreated: false` with the
`RecreationFailed` error (underlying `OmniDecideError` status preserved),
`lastError` carries the diagnostic, and the adapter remains genuinely
`ready` — readiness describes the still-working instance and is never
faked. Recovery = retry `recreate()`, or `shutdown()` + `init()`.

### Shutdown (`shutdown()`)

- Releases the managed handle through the real API, then **retries every
  pending-cleanup handle**, and returns to `uninitialized`. Safe from any
  state.
- **Idempotent**: repeated calls never double-release. While pending
  entries exist, every invocation RETRIES them — idempotency means no
  double free, not skipping unfinished cleanup. With nothing owned the
  call is a no-op (`released: false`).
- Any in-flight `init()`/`recreate()` is invalidated via a lifecycle epoch:
  a kernel created by a stale transition is disposed immediately and never
  exposed (its disposal failure lands in the pending registry like any
  other).
- **Completion is only claimed when nothing is owned**: the result reports
  `shutdownComplete: true` and `pendingCleanup: 0` only when the adapter
  holds no handles. Otherwise `incompleteShutdown: true` plus the
  sanitized `pendingCleanupDetail` are returned, the
  `engine:shutdown-incomplete` event fires, and another `shutdown()` (or
  `retryCleanup()`) retries the remaining releases.
- Decisions after shutdown are rejected before the kernel
  (`AdapterNotReady`); reinitialization afterwards works and reproduces
  byte-identical envelopes (kernel determinism) — after a *complete*
  shutdown, or once the pending cleanup has been recovered.

### Handle ownership and pending cleanup (Stage 2.1.1)

Every kernel handle this adapter allocates is owned **exactly once**:

- it is the active `kernel` (`lifecycleStats.activeHandles`), or
- it sits in the internal pending-cleanup registry
  (`lifecycleStats.pendingCleanup`, detail via
  `adapter.pendingCleanupDetail`), or
- its release is confirmed (`lifecycleStats.released`).

Exact accounting invariant (asserted by the regression suite):
`created === released + activeHandles + pendingCleanup`.

A failed `free()` never drops the reference (the Stage 2.1.1 review
finding): the handle is registered as pending with its original error,
its attempt count, and the failing operation as `reason`. Entries leave
the registry **only on release evidence**:

- a non-throwing `free()` — the C wrapper (`odkw_kernel_free`, see
  `src/wrappers/odk_wasm.c`) resolves the registry slot, clears it
  (`g_kernel_live--`) and only then destroys the kernel; a call on an
  already-cleared slot is a no-op, so a retry can never double-free and a
  non-throwing completion always ends with the handle not allocated; or
- the JS wrapper's own `closed` flag — set only after a completed release
  call — which resolves the entry **without** performing any further
  (potentially destructive) call.

`adapter.retryCleanup()` is the safe public retry (also attempted
automatically before any new allocation, and by every `shutdown()`). It
delegates to the same evidence rules; during an in-flight transition it
reports `deferred: true` instead of racing the transition.

Recovery paths in order: `retryCleanup()`, the automatic pre-flight of
`init()`/`recreate()`, and repeated `shutdown()`.

**Engine API limitation (documented, not hidden):** the public engine API
exposes no per-handle existence query — only the module-wide live count
(`odkw_kernel_count`). The adapter therefore cannot externally verify
which specific handle is live while a release keeps failing; it retains
ownership conservatively and reports the unresolved state instead of
claiming a release it cannot evidence. The module-wide count is used as a
supporting diagnostic (`engineCount()` is readable through any owned
handle, active or pending), never as per-handle proof.

### Handle accounting (no invented metrics)

The real API exposes the module-wide live-kernel count
(`odkw_kernel_count`) through `kernel.engineCount()`. While a handle is
owned — active **or pending-cleanup** — `adapter.engineCount()` reports
the module-wide count through it. With nothing owned there is no handle
to ask, so the regression suite
(`tests/adapter_lifecycle.test.mjs`) uses a short-lived **probe kernel**:
live external handles = `probe.engineCount() - 1`. The canonical sequence

```
initialize → recreate → recreate → decision → shutdown
→ initialize → decision → shutdown
```

is asserted to end with **zero** live handles, and the lifecycle counters
(`created` vs `released`) must balance. Before the Stage 2.1 fix this
sequence leaked one handle per recreation (1 → 2 → 2, reproduced against
the real WASM module); after the fix every step holds exactly one handle
and the end state is 0. Before the Stage 2.1.1 fix, a recreation whose
old-instance release failed still orphaned that handle (2 live after
recreation, 1 unreleaseable after shutdown — also reproduced); since the
fix the failing handle is retained, retried and released by the adapter,
and every failure-path scenario ends with zero adapter-owned handles —
verified by the cleanup-ownership suite with an explicit test-fixture
sweep that must find zero orphans.

- The Emscripten module is loaded once per page and shared.
- A failed *decision* never invalidates the kernel (upstream contract):
  the engine stays `ready` after `OmniDecideError` status 1/2/4 records.

## Invocation contract

```js
const rec = adapter.invoke(
  {
    primitive: 'choice',
    instruction: 'Which investigation lead is best supported by the evidence currently collected?',
    options: ['Investigate the security office', 'Interview the museum curator'],
    context: 'E-00: … E-02: …',       // the evidence actually collected
    // topK for extract; caps/meta passthrough supported
  },
  {
    game: 'detective', step: 4,
    label: 'Lead consultation',
    interpretation: { summary: '…', applied: '…' },
    replayOf: null                     // set on replayed invocations
  }
);
```

The normalized record:

```js
{
  seq, game, step, label,
  primitive, request,        // deep copy of what was submitted
  raw,                       // exact envelope string (byte-identical across hosts)
  envelope,                  // parsed { primitive, result, confidence, probability, distribution, meta? }
  ok,                        // true | false
  error,                     // { kind, status, statusName, message, envelope } when !ok
  durationMs,                // monotonic timing around the kernel call
  interpretation,            // the game's declared use of the result
  replayOf,                  // original decision id during replays
  rejectedBeforeKernel       // true → adapter validation rejected it
}
```

Error kinds: `AdapterValidationError` (shape rejected before the kernel),
`AdapterNotReady` (no engine), and the upstream `OmniDecideError` mapped to
`{status, statusName, message, envelope}` — statuses 1 ARGUMENT, 2 INFERENCE,
3 MEMORY, 4 INTERNAL. Failed records are stored in history and counted in
telemetry; nothing is swallowed.

## Validation

Adapter-side validation covers request shape only (object, known primitive,
non-empty string instruction, string options for choice/rank/compare/score,
exactly two options for compare, integer `topK ≥ 1` for extract, string
context). Semantic validation stays kernel-side. This split is verified by
tests: a `topK` of 2³² passes adapter validation and is rejected by the real
kernel wrapper with `OmniDecideError` status 1 — the full error path is
exercised against the WASM module, not mocked.

## Host independence

`createAdapter({ engineLoader })` accepts an async engine source. The browser
default resolves `window.OmniDecide` / `window.OmniDecideError`; the Node test
suite injects the CommonJS module (`require('js/omnidecide.js')`). The same
adapter code paths run in both hosts — see `tests/gamelab.test.mjs` §2.

## Timing

`durationMs` brackets string marshaling into the WASM heap, the wrapper call
and the JSON envelope parse — i.e. exactly what an application pays per
decision — using a monotonic timer (`performance.now`, injectable for tests).
The Stage 1 "isolated C raw loop" is intentionally not re-measured here;
GameLab's Benchmark Overview reports live pipeline measurements only.
