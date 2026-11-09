<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# Game Module Registration Guide

A GameLab game is **one folder plus one registry entry**. The shell, router,
workspace, event feed, Decision Inspector, history and benchmarks are shared —
a module only implements scenario rules and its own interface.

## 1. Create the module folder

```
gamelab/js/games/<your-game>/
├── scenario.js    (recommended) pure reducer: state, rules, data — DOM-free,
│                  importable from Node tests
├── decisions.js   (recommended) pure request builders + interpretations
├── replay.js      (recommended) replay runner with injected invoke()
└── module.js      required: create() factory + workspace controller
```

## 2. Implement the module contract

`module.js` exports a factory returning an instance with:

```js
export function createMyGame() {
    let env = null;
    return {
        start(session) { /* build initial state from session.seed */ },
        mount(environment) { env = environment; render(); },
        status() { return { world: { /* rows for the status panel */ } }; },
        actors() { return [{ name, note, state }]; },
        unmount() { env = null; },
        destroy() { /* final cleanup if any */ }
    };
}
```

Rules of the house:

- **Make decisions through `env.decide(request, info)`** — never call the
  kernel directly, never cache or fake results. Every call is validated,
  timed, recorded and inspectable.
- **Declare the interpretation** — each decision's `info.interpretation =
  { summary, applied }` states what the engine returned and what the game
  will do with it. The Decision Inspector shows this verbatim.
- **Keep outcomes in scenario logic.** The engine advises; the reducer
  decides. Never let an envelope silently override an objective rule; if an
  engine result changes gameplay, do it through a declared, visible rule
  (the detective's "+1 hour for following a recommendation" is the template).
- **Determinism.** Derive any randomness from `session.rngFor(step)`. Same
  seed + same actions must reproduce the same outcomes. Support reset (rebuild
  initial state, snapshot the previous run) and replay (re-execute the logged
  sequence through the pure reducer with real re-invoked decisions).

## 3. Register the game

In `gamelab/js/games/index.js`:

```js
registerGame({
    id: 'my-game',                    // unique; used in routes and records
    name: 'My Game',
    tagline: 'One-line pitch',
    status: 'playable',               // or 'planned'
    accent: '#5fae6f',                // module accent color
    icon: '<svg>…</svg>',             // inline SVG, currentColor
    defaultSeed: 20261108,
    blurb: 'Longer description for launcher and start card.',
    features: ['…', '…'],
    create: () => createMyGame()
});
```

Planned modules omit `create` and carry `plannedFor: 'Planned — Stage N'`;
the launcher and sim route render their scope without faking gameplay.

Nothing else changes: navigation, routes (`#/sim/<id>`), history filters and
launcher cards pick the module up from the registry.

## 4. Test it

- **Node**: import the pure modules under `tests/gamelab.test.mjs` patterns —
  reducer determinism, budget rules, builders, endings, and a full replay that
  re-invokes the real kernel through an injected adapter
  (see `tests/gamelab.test.mjs` §3–§4 for the template).
- **Browser**: extend `tests/gamelab_browser_check.mjs` with a launch-and-play
  flow for the new game (the detective section is the template: launch → play
  → real decision → inspect → complete → reset → replay).

## Reference implementation

`gamelab/js/games/detective/` is the worked example — read it alongside
`docs/LAST-DETECTIVE.md`.
