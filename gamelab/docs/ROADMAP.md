<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# Roadmap — Decideverse games

**Shipped:** The Last Detective (Stage 2) · Living Village (Stage 3A).
**Next:** Tactical Command (Stage 3B). After that: The Last Outpost,
Rumors & Kingdoms.

The launcher presents unshipped games as planned modules; Decideverse
never ships fake playables. Each design below reuses the shared framework
unchanged — a new game is a module folder, a registry entry, pure
scenario/decision files and tests (see GAME-MODULES.md).

## Stage 3 — Living Village — **SHIPPED (Stage 3A)**
*Autonomous villagers with memory, needs and relationships. Playable today; design and
implemented rules: LIVING-VILLAGE.md. Tactical Command (below) is the remaining Stage 3 scope.*

- Villager agents with private memory graphs and a relationship web that
  changes through interactions.
- Need/goal generation from world state (hunger, warmth, belonging, craft).
- Daily schedule arbitration through `choice` ("Which goal should Tamsin
  pursue next?") with the villager's memory as submitted evidence.
- Dispute mediation through `compare` (two villagers' claims against the
  shared chronicle).
- A village chronicle assembled from the event feed; `extract` powers the
  "today in the village" digest.
- Deterministic day seeds; replayable days.

## Stage 3B — Tactical Command — **NEXT**
*Squad-level coordination under time pressure.*

- Grid-based engagements: squad members with roles/loadouts/readiness, threats
  with confidence levels, objectives with priorities.
- Threat assessment via `compare` ("Which contact is the greater risk given
  observed evidence?"), objective prioritization via `rank`, engagement orders
  via `choice`.
- Order-of-operations constraints (ammo, suppression, cover) stay entirely in
  scenario logic; the engine never overrides fire-discipline rules.
- After-action review: every arbitration inspectable in the Decision
  Inspector with the squad's observed evidence as context.

## Stage 4 — The Last Outpost
*Survival resource allocation against a ticking clock.*

- Resource ledger with decay/spoilage; weather and threat cycles from
  deterministic seeds.
- Per-cycle allocation arbitration via `choice`/`rank` (build / repair /
  stockpile / expedition) with the colony's status report as evidence.
- Expedition resolution via `compare` (two routes against scouting reports).
- Survival ledger and cycle reports; replayable cycles.

## Stage 4 — Rumors & Kingdoms
*Factions, rumors and the reliability of information.*

- Rumor propagation between factions with time-decayed reliability —
  time-sensitive evidence is this game's core twist.
- Source credibility modeled in scenario logic; the engine receives the rumor
  text **plus** credibility annotations as evidence.
- Alliance / accusation / war arbitrations via `choice`/`rank`; treaty
  verification via `noul` against treaty texts.
- Treaty timeline visualization; deterministic diplomacy seeds.

## Engineering candidates (any stage)

- Persist sessions/history (localStorage or IndexedDB) and export decision
  logs as JSON.
- ~~Wire the two Stage 2 suites into the existing CI pipeline as new jobs~~ —
  **done**: Jobs H (Node) and I (browser E2E) run the Decideverse suites on
  every push, additive to the engine jobs; Stage 3A extended both with the
  Living Village coverage.
- Case editor for the detective (author alternative evidence sets) to
  demonstrate that engine output follows submitted evidence, not hardcoded
  answers.
- A "raw request console" for power users to submit arbitrary requests to the
  kernel through the adapter (all six primitives, caps/meta toggles).
