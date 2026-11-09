<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# The Simulation Framework

The shared framework owns simulation state, time, events, history, telemetry
and session lifecycle. It is deliberately small — enough for five games, not a
general-purpose game engine.

## SimSession (`core/session.js`)

One `SimSession` binds one game run to the framework:

```js
const session = new SimSession({ game: 'detective', seed: 20261108 });
session.id;            // 'detective-s3' (session number from history)
session.clock;         // SimClock — explicit-tick step counter + wall timer
session.rng;           // mulberry32(seed) — deterministic stream
session.rngFor(step);  // derived stream for one step (deriveSeed)
session.seed;          // the ONE authoritative scenario seed (Stage 2.1)
session.setSeed(n);    // change it: normalizes >>> 0, rebuilds the RNG stream
session.logAction({ action, args });  // ONE successful player action
session.actionLog;     // the successful player actions (inspection/tests)
session.actionCount;   // what the workspace "Actions logged" counter renders
session.clearActionLog();  // new run (reset/begin) — documented semantics
session.emitEvent({ type, label, detail, meta });  // feed + history + telemetry
session.decide(request, info);                     // adapter.invoke + metadata
session.snapshot();
```

### Action-history semantics (Stage 2.1)

`session.actionLog` records **successful player actions only** — one entry
per accepted player command (`begin`, `investigate`, `decide`, `accuse`,
`close-unsolved`), stamped with the session step. Four related but distinct
concepts stay separate:

- **Player actions** — the shared action log above; the workspace counter
  renders `session.actionLog.length`, so display and data cannot disagree.
- **Simulation events** — the typed feed/history stream (`emitEvent`);
  rejected commands and world happenings live here, never in the counter.
- **OmniDecide invocations** — every real kernel call (`session.decide`),
  successful or not, recorded with request/envelope in history.
- **Kernel execution results** — the envelope bytes and the game's stated
  interpretation, inspectable in the Decision Inspector.

Rules (enforced by the game module, verified by tests):

- An engine invocation is not automatically a player action: a **failed**
  decision does not count.
- A rejected command (unknown action, no hours, already taken, accusation
  threshold unmet) does not count.
- No double counting: the module logs each accepted command exactly once;
  the game's own `runLog` is a separate record with a separate purpose (see
  LAST-DETECTIVE.md — it is the replay input sequence and intentionally
  also contains failed decision attempts).
- **Reset** starts a new scenario run: the action log clears (the counter
  shows 0) while the previous run stays inspectable via history and replay.
- **Replay** is an inspection capability: it never appends to the action
  log.

### Seed ownership (Stage 2.1)

`session.seed` is the single authoritative scenario seed. Games take it at
`start()`, change it **only** through `setSeed()` (which normalizes to an
unsigned 32-bit integer and rebuilds the derived RNG streams), and the
workspace displays exactly this value. The selected seed flows:
briefing input → validation/normalization → `session.setSeed()` →
scenario initialization → workspace display → history → reset (retained)
→ replay (reused). UI and simulation cannot disagree about the seed.

The sim route (`views/simulation.js`) keeps one live session per game in a
module-level map, so runs survive navigation; leaving the route unmounts the
workspace (bus listeners detached), returning auto-resumes.

## Simulation clock (`core/clock.js`)

- `step` — monotonically increases **only** via `tick()`. No hidden timers:
  deterministic execution means the world never moves on its own.
- Wall time (`start/pause/elapsedMs`) is tracked separately for display.
- Games mirror their own reducer step onto the session clock (the detective
  module ticks per action/accusation/closing).

## Deterministic randomness (`core/rng.js`)

`mulberry32(seed)` provides a reproducible float stream; `deriveSeed(seed,
step)` derives per-step sub-streams. Randomness is for cosmetic flavor only —
it must never decide outcomes. The detective scenario uses it for overheard
remarks; evidence, costs, unlocks and endings are fully deterministic, and the
Node suite asserts that identical seeds produce identical state *and*
identical event streams.

## Events and history (`core/history.js`)

Two distinguishable record streams, each a ring buffer (600 events / 300
decisions):

- **Events** — `{ id 'E-nnnn', session, wallTs, step, game, type, label,
  detail, meta }` with types such as `scenario:started`, `evidence:discovered`,
  `suspect:interviewed`, `location:investigated`, `decision:requested`,
  `decision:completed`, `lead:selected`, `scenario:completed`,
  `scenario:reset`, `world:changed`, `session:note`.
- **Decisions** — the adapter's normalized records, additionally id'd
  (`D-nnnn`), session-stamped and wall-timestamped.

Every record is emitted on the bus (`history:event`, `history:decision`) and
rendered live in the workspace feed, Activity History and dashboard. Events
and decisions are never mixed: the history view tabs them separately, the
inspector renders decisions only.

## Telemetry (`core/telemetry.js`)

Counters and aggregates only — no thresholds, no fabrication:

- decision invocations, successes, failures
- min / avg / max execution time (monotonic timer), engine init duration
- simulation events count
- last 64 duration samples for the benchmark bar strip

`snapshot()` feeds the dashboard, workspace performance panel and Benchmark
Overview. `reset()` clears counters (Settings). Nothing is persisted to
storage: the numbers describe the *current session*.

## Workspace (`core/workspace.js`)

`mountWorkspace(host, gameDef, session, gameInstance)` builds the shared
frame — viewport, controls bar, world status, actors, performance panel, event
feed — and hands the game an environment:

```js
const env = {
  game, session, bus, adapter, history, telemetry,
  viewport, controls,
  panels: { status, actors, perf, feed },
  emitEvent(spec),          // → session.emitEvent
  decide(request, info)     // → session.decide (real kernel call)
};
gameInstance.mount(env);
```

The game implements `{ start(session), mount(env), status(), actors(),
unmount(), destroy() }`. On unmount the workspace detaches its subscriptions
and calls `gameInstance.unmount()` — no listener leaks.

## Registry (`core/registry.js)

`registerGame(def)` validates and freezes definitions; `create(env)` is
required for playable modules. Adding a game is one module folder plus one
registry entry (see GAME-MODULES.md).
