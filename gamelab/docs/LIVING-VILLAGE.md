<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# Living Village — Stage 3A design document

*Committed before implementation, per the Stage 3 process. Every number in
here is normative for the implementation and the tests. The engine is the
OmniDecide WASM kernel: a deterministic **lexical evidence matcher**. It is
not an LLM, does not understand the village, and never "knows" anything —
it ranks the textual options we submit against the textual evidence we
submit. This document says what it actually does at every use.*

## 1. Scenario overview

A fixed village of **6 named villagers** is simulated over **7 discrete
days**. The player is the village **steward** with **2 steward actions per
day**. Each day, every villager autonomously pursues one goal, chosen by
the kernel (`choice`) from the villager's own memory and needs as submitted
evidence. Disputes may open between villagers; the steward can mediate them
(`compare` against the shared chronicle). Each day ends with a chronicle
digest (`extract`). After day 7 the village receives a report ending
(thriving / settled / struggling).

## 2. Scenario data

### 2.1 Villagers (fixed roster, order matters for tie-breaks)

| id | name | craft | trait |
|---|---|---|---|
| tamsin | Tamsin Brook | baking | cheerful, talks to everyone first |
| idris | Old Idris | carpentry | slow to trust, remembers slights |
| mara | Mara Voss | herbalism | observant, mediates by nature |
| piet | Piet Alsen | shepherding | stubborn, loyal to old friends |
| juno | Juno Rell | milling | restless, visits often |
| calla | Calla Ember | teaching | patient, keeps the chronicle |

### 2.2 Shared stores

- `grain` starts at 10, `firewood` starts at 8. Integers, never negative.

### 2.3 Villager state (all inside the pure reducer)

- `needs`: `{ hunger, warmth, belonging, craft }`, integers 0–100
  (100 = fully met). Start: hunger 70, warmth 70, belonging 60, craft 55.
- `memory`: array of `{ day, text }`, oldest first, capped at the last
  8 entries per villager (older entries drop off; the cap is deterministic).
- `goal`: today's chosen goal (null before arbitration).

### 2.4 Relationships

`rel[A][B]` integer −10…+10, symmetric, start: tamsin↔juno +3,
piet↔idris +3, mara↔calla +2, every other pair 0.

## 3. Daily flow (strict order, all deterministic)

1. **Steward phase** — player performs 0–2 steward actions (§4).
2. **End day** (player action, always available once the day started):
   1. weather is drawn from the day stream (§6) — clear / overcast / rain /
      cold snap;
   2. needs decay applied (§5);
   3. **goal arbitration**: one real `choice` call per villager (§7.1), in
      roster order; the reducer then applies the chosen goal's objective
      effects (§5);
   4. **dispute check**: if no dispute is open and some pair has
      `rel < −2`, a dispute opens between the pair with the lowest rel
      (tie → roster order); `rel` −6 for the pair on opening;
   5. **chronicle digest**: one real `extract` call over the day's feed
      (§7.3), stored for the chronicle panel (display-only);
   6. day counter +1; day 8 would end the scenario (§8).

### Ambiguity decisions (documented, binding)

- **D1** — a "visit" goal needs a concrete target before arbitration: the
  reducer pre-selects the villager with the **lowest relationship** to the
  actor (tie → roster order) and bakes the name into the option text.
- **D2** — at most **one open dispute** at any time.
- **D3** — if `grain == 0`, the "eat at the common table" goal remains
  selectable but yields no hunger recovery; the memory entry "the granary
  is empty" is appended instead. Honest evidence: the granary state is part
  of the submitted context.
- **D4** — 6 villagers (spec allows 5–7).
- **D5** — 7 days, then the report ending. No early endings.
- **D6** — unused steward actions do not carry over.
- **D7** — the chronicle digest is **display-only**: extraction never
  changes village state (tested by state equality).
- **D8** — dispute opening (final): a pair quarrels when **both** members
  feel lonely (belonging < 40), or when their relationship is strained
  (rel < −2). **D2a** — no *new* disputes open on the final evening.
- **D9** — evidence-phrase rule: `goalContext` restates numeric needs as
  felt conditions whose wording lexically echoes the authored goal options
  ("hungers for food to eat at the common table" when hunger < 50 and
  grain remains, "shivers from the cold" when warmth < 35, "feels lonely
  and longs to visit a friend" when belonging < 35, "craves meaningful
  work on their craft" when craft < 35). The phrases derive purely from
  state — the evidence stays honest.
- **D10** — working the craft produces the villager's good: bakers and
  millers replenish the granary (+1 grain), the carpenter the woodshed
  (+1 firewood). A hosted gathering is also a shared meal: hunger +15
  for everyone (2 grain). Hunger decays −8/day (not −12).
- **D11** — ending thresholds (final): `thriving` = mean needs ≥ 45 and
  no open dispute; `struggling` = mean < 30 or an open dispute;
  `settled` = otherwise. Empirically (real kernel, six seeds): attentive
  stewardship settles the village (~34, all disputes resolved); neglect
  leaves an open dispute and a struggling report; thriving is reachable
  only with an excellent stewardship mix and is exercised in the pure
  state tests.

## 4. Steward actions (cost: 1 of the 2 daily actions)

| action | argument | objective effect (reducer-owned) |
|---|---|---|
| `host-gathering` | — | all villagers belonging +10 and hunger +15 (shared meal); `grain` −2 (rejected if grain < 2) |
| `assign-task` | villagerId | target craft +15; produces +1 unit of the villager's good (baking/milling → grain, carpentry → firewood, others: craft only) |
| `mediate-dispute` | — | only when a dispute is open; submits the two claims to `compare` (§7.2) and applies rule **R2** |
| `share-news` | villagerId | target belonging +6, steward-side memory entry "the steward shared news of the valley" appended |

Rejected actions (unknown id, no budget left, mediate without an open
dispute, grain < 2) never reach the action log; they emit a feed event
only. A village report ends the run before day 8; reset/replay follow the
shared Stage 2.1 semantics.

## 5. Needs model (all integers, clamped 0–100)

Decay at end of day, then goal effects:

| need | decay/day | goal/interaction effects |
|---|---|---|
| hunger | −8 (D11) | eat +25 (D3 may null it); host-gathering +15 (shared meal) |
| warmth | −10 (cold snap −20) | gather-firewood +15 |
| belonging | −8 | visit +12 actor / +4 target; host-gathering +10 all; share-news +6 |
| craft | −6 | work-craft +15; assign-task +15 |

Goal side-effects: eat `grain` −1; gather-firewood `firewood` +1;
visit → relationship +3 both, memory entries both sides ("visited X" /
"was visited by X"); work-craft → no store change.

## 6. Determinism strategy

- The **session seed** is the single authoritative seed (Stage 2.1):
  briefing input → validation → `session.setSeed()` → reducer state →
  workspace display → reset (retained) → replay (reused).
- Weather for day *d*: index = `Math.floor(rng() * 4)` with
  `rng = mulberry32(deriveSeed(seed, d * 8 + 1))` — pure, local copies of
  the framework primitives (detective pattern). Weather affects only the
  warmth decay value; it is seed-dependent behavior and is asserted
  consistent per seed.
- Villager streams: `mulberry32(deriveSeed(seed, d * 8 + 2 + index))` for
  cosmetic memory-flavor only; **no outcome randomness** beyond weather.
- No `Math.random()`, no wall-clock time in scenario logic. The reducer is
  DOM-free and importable from Node.
- Same seed + same action sequence ⇒ byte-identical state serialization
  (`JSON.stringify`) and identical event streams.

## 7. Engine use (real request shapes, `docs/API.md` schema)

### 7.1 Goal arbitration — `choice` (6 calls/day)

```js
{
  primitive: 'choice',
  instruction: 'Which goal should <Name> pursue today?',
  options: [
    'eat at the common table',
    'gather firewood for the hearth',
    'visit <Target>',
    'work on their craft'
  ],
  context: '<Name> remembers: <memory entries, day-stamped>. ' +
           'Hunger <n>/100, warmth <n>/100, belonging <n>/100, ' +
           'craft <n>/100. The granary holds <n> grain, the woodshed ' +
           '<n> firewood. Today is <clear|overcast|rain|cold snap>.'
}
```

Envelope → `labelToAction` maps `result` to `eat|firewood|visit|craft`
(prefix match on the submitted option strings, exact keys listed above).
Unmapped/absent result ⇒ documented fallback goal `craft` (a villager can
always work), the envelope is still recorded and shown.

**Declared rule R1 (visible in the feed):** the goal the kernel selects is
the goal the villager pursues that day; the reducer applies that goal's
objective effects. The engine arbitrates *which*; the reducer owns *what it
does*.

### 7.2 Dispute mediation — `compare` (0–1 calls/day, steward action)

```js
{
  primitive: 'compare',
  instruction: 'Which claim does the village chronicle support?',
  options: ['<Claim A, one sentence>', '<Claim B, one sentence>'],
  context: '<day-stamped chronicle entries>'
}
```

Claims are authored one-sentence summaries of each side's grievance, kept
lexically close to the chronicle entries they cite.

**Declared rule R2 (visible in the feed):** the dispute closes regardless
of the verdict; the owner of the supported claim gains belonging +4; a
"tie" grants +2 to both; the pair's `rel` +4 on any verdict. The reducer —
not the engine — closes the dispute and applies the numbers.

### 7.3 Chronicle digest — `extract` (1 call/day)

```js
{
  primitive: 'extract',
  instruction: 'Which events best summarize today in the village?',
  context: '<numbered day-stamped feed lines for the day>',
  topK: 3
}
```

**Declared rule R3:** display-only — the digest is shown in the chronicle
panel and never mutates village state (asserted by test).

### 7.4 Decision-call bound

Exactly 7 calls on a dispute-free day (6 × choice + 1 × extract), 8 when a
dispute is mediated. Full 7-day run: **≤ 56 calls**. No other engine use
exists anywhere in the module.

### 7.5 What the engine does and does not do in this game

- It **selects** a villager's next goal from the submitted memory/needs
  text (lexical support between option wording and evidence wording).
- It **weighs** two authored claims against chronicle entries.
- It **summarizes** the day's feed into three snippets.
- It does **not** know what hunger "is", cannot invent goals, cannot see
  anything except the submitted strings, does not execute anything, and
  its output never silently overrides the reducer: goals are executed only
  through R1's declared effects, disputes close only through R2's declared
  effects, and R3 changes nothing.

## 8. Endings (after day 7, deterministic from final state)

Village report card + ending kind:
- `thriving` — mean of all 24 needs ≥ 45 **and** zero open disputes (D11);
- `struggling` — mean < 30 **or** an open dispute remains;
- `settled` — otherwise.

The report shows: needs table, best/worst relationship pair, disputes
opened/resolved, grain/firewood remaining, chronicle digest count, seed.

## 9. Test plan (`tests/gamelab_village.test.mjs`)

Follows `gamelab.test.mjs` §3–§4 and `gamelab_state.test.mjs` patterns;
real WASM via the CommonJS kernel module; machine-readable report
`build/village-node-report.json`; non-zero exit on failure.

1. Reducer determinism: same seed + same actions ⇒ identical serialized
   state and identical event label streams.
2. Different seeds: valid; weather differs or not — asserted consistent
   per seed (same seed ⇒ same weather sequence; no artificial randomness).
3. Every steward action: valid path, rejected path (budget, unknown id,
   mediate without dispute, grain < 2), day-boundary behavior.
4. R1 tested both ways (goal applied from envelope; fallback for unmapped
   result); R2 both ways (supported claim +4 / tie +2+2; dispute closed in
   all cases); R3 state invariance.
5. Builders produce schema-valid requests (primitive/fields match
   `docs/API.md`); interpretation text present and matching what is applied.
6. **Output-follows-input**: with a memory entry emphasizing a cold,
   fireless hearth, the choice flips to "gather firewood" versus a baseline
   state without it (asserted on the real engine, not mocked).
7. Full playthrough through the **real WASM adapter** to the report ending;
   all three ending kinds reachable and asserted.
8. Reset: snapshots prior run, clears action log, same seed re-briefs;
   replay: re-invokes real decisions, byte-identical envelopes, identical
   ending.
9. Shared action-log semantics: successful actions exactly once; rejected
   actions and failed decisions absent; kernel calls ≠ player actions ≠
   events; workspace counters equal `session.actionLog.length`.
10. Seed synchronization: briefing seed = session seed = reducer seed =
    displayed seed (workspace status + controls note).
11. Injected decision failure: state consistent, event recorded, action
    counter unmoved, module reports honestly.
12. Detective suites unaffected (run unmodified in CI).

## 10. Browser test additions

Extend `tests/gamelab_browser_check.mjs`: launch Living Village from the
launcher, set seed, begin, run steward actions, end days with real
arbitrations, open Decision Inspector on a village decision, reach the
village report, reset, replay, verify counters/seed display, zero console
errors / page errors / failed requests, no horizontal overflow and usable
nav at 390px, launcher still shows five games.
