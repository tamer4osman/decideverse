/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * scenario.js - "Living Village" scenario for Decideverse Stage 3A.
 *
 * PURE and DOM-free: this module owns the objective village rules.
 * It never calls the decision engine and never imports the adapter —
 * engine results enter the state only through applyGoalResult(),
 * applyMediationResult() and applyDigestResult() with the envelopes the
 * real kernel actually returned. The kernel is a deterministic lexical
 * evidence matcher: it selects which submitted option the submitted
 * evidence text supports best. It does not understand the village.
 *
 * Declared engine-to-gameplay rules (design doc §7, tested both ways):
 *   R1  goal arbitration (choice): the goal the kernel selects is the
 *       goal the villager pursues that day; this reducer applies that
 *       goal's objective effects. Unmapped results fall back to 'craft'.
 *   R2  dispute mediation (compare): the dispute closes regardless of
 *       the verdict; the supported claim's owner gains belonging +4
 *       ("tie": +2 to both); the pair's relationship +4.
 *   R3  chronicle digest (extract): display-only; never mutates state.
 *
 * Determinism: same seed + same action sequence ⇒ byte-identical state.
 * Weather is the only seed-dependent behavior (design doc §6): derived
 * from local, dependency-free mulberry32/deriveSeed copies. There is no
 * other randomness and no wall-clock time here.
 */

/* ------------------------------------------------------------------ */
/* Identity                                                            */
/* ------------------------------------------------------------------ */

export const SCENARIO_VERSION = '1.0.0';
export const DEFAULT_SEED = 20261109;

export const VILLAGE_META = {
    name: 'Thornbury',
    valley: 'the Aldermoor valley'
};

export const DAYS = 7;              /* D5: seven days, then the report */
export const STEWARD_ACTIONS_PER_DAY = 2;
export const NEEDS_DECAY = { hunger: 8, warmth: 10, warmthCold: 20, belonging: 8, craft: 6 };
export const STORES_START = { grain: 10, firewood: 8 };

/** Fixed roster; array order is the deterministic tie-break order (D1/D4). */
export const VILLAGERS = [
    { id: 'tamsin', name: 'Tamsin Brook', craft: 'baking', trait: 'cheerful, talks to everyone first', good: 'grain' },
    { id: 'idris', name: 'Old Idris', craft: 'carpentry', trait: 'slow to trust, remembers slights', good: 'firewood' },
    { id: 'mara', name: 'Mara Voss', craft: 'herbalism', trait: 'observant, mediates by nature', good: null },
    { id: 'piet', name: 'Piet Alsen', craft: 'shepherding', trait: 'stubborn, loyal to old friends', good: null },
    { id: 'juno', name: 'Juno Rell', craft: 'milling', trait: 'restless, visits often', good: 'grain' },
    { id: 'calla', name: 'Calla Ember', craft: 'teaching', trait: 'patient, keeps the chronicle', good: null }
];

export const WEATHERS = ['clear', 'overcast', 'rain', 'cold snap'];

/** Authored dispute topics per pair (alphabetical pair key). */
const DISPUTE_TOPICS = {
    'idris|piet': 'the shaded pasture that borders both their plots',
    'juno|tamsin': 'the morning queue at the mill',
    'mara|calla': 'the herb garden the school children trampled'
};
const DISPUTE_TOPIC_DEFAULT = 'the shared well and whose turn it is to draw water';

/** Authored, chronicle-adjacent claim pair for mediation (rule R2). */
export function disputeClaimsFor(state) {
    if (!state.dispute) return null;
    const da = VILLAGERS.find((v) => v.id === state.dispute.a);
    const db = VILLAGERS.find((v) => v.id === state.dispute.b);
    return {
        a: `${da.name} claims that the chronicle shows they kept their duties around ${state.dispute.topic}.`,
        b: `${db.name} claims that the chronicle shows they kept their duties around ${state.dispute.topic}.`
    };
}

/* ------------------------------------------------------------------ */
/* Initial state                                                       */
/* ------------------------------------------------------------------ */

export function createInitialState(seed = DEFAULT_SEED) {
    const rel = {};
    const bump = (a, b, v) => { rel[`${a}|${b}`] = v; rel[`${b}|${a}`] = v; };
    bump('tamsin', 'juno', 3);
    bump('piet', 'idris', 3);
    bump('mara', 'calla', 2);
    return {
        scenarioVersion: SCENARIO_VERSION,
        seed: seed >>> 0,
        step: 0,
        day: 0,                   /* 0 = briefing; 1..7 = lived days */
        phase: 'briefing',        /* briefing | day | report */
        weather: null,            /* today's weather, set at end-day */
        stores: { grain: STORES_START.grain, firewood: STORES_START.firewood },
        villagers: VILLAGERS.map((v) => ({
            id: v.id,
            needs: { hunger: 70, warmth: 70, belonging: 60, craft: 55 },
            memory: [],
            goal: null,
            lastGoalKey: null
        })),
        rel,
        dispute: null,            /* {a, b, topic, openedDay} */
        disputesOpened: 0,
        disputesResolved: 0,
        stewardActionsLeft: STEWARD_ACTIONS_PER_DAY,
        chronicle: [],            /* [{day, text}] — the objective record */
        digests: [],              /* [{day, snippets:[...]}] (R3, display-only) */
        goalChoices: [],          /* [{day, villagerId, key, confidence}] */
        report: null              /* ending report (phase 'report') */
    };
}

/* ------------------------------------------------------------------ */
/* Pure helpers                                                        */
/* ------------------------------------------------------------------ */

export function villager(state, id) {
    return state.villagers.find((v) => v.id === id) || null;
}

export function relOf(state, a, b) {
    return state.rel[`${a}|${b}`] ?? 0;
}

export function meanNeeds(state) {
    let sum = 0;
    let n = 0;
    for (const v of state.villagers) {
        for (const k of Object.keys(v.needs)) {
            sum += v.needs[k];
            n += 1;
        }
    }
    return n ? sum / n : 0;
}

/** D1: visit target = lowest relationship, tie → roster order, never self. */
export function visitTargetFor(state, villagerId) {
    let best = null;
    for (const v of VILLAGERS) {
        if (v.id === villagerId) continue;
        const r = relOf(state, villagerId, v.id);
        if (best === null || r < best.r) best = v;
    }
    return best;
}

/** The day's chronicle lines (objective record for that day, in order). */
export function dayChronicleLines(state, day) {
    return state.chronicle.filter((c) => c.day === day).map((c) => c.text);
}

export function chronicleText(state, maxDay) {
    return state.chronicle
        .filter((c) => maxDay === undefined || c.day <= maxDay)
        .map((c) => `Day ${c.day}: ${c.text}`)
        .join(' ');
}

/* ------------------------------------------------------------------ */
/* Weather (the only seed-dependent behavior; design doc §6)           */
/* ------------------------------------------------------------------ */

export function weatherFor(state, day) {
    const rng = mulberry32(deriveSeed(state.seed, day * 8 + 1));
    return WEATHERS[Math.floor(rng() * WEATHERS.length)];
}

/* ------------------------------------------------------------------ */
/* Steward actions (player; cost 1 of 2 per day)                       */
/* ------------------------------------------------------------------ */

export const STEWARD_ACTIONS = ['host-gathering', 'assign-task', 'mediate-dispute', 'share-news'];

/**
 * Apply one steward action.
 * @returns {{ok:boolean, reason?:string, state:object, events:object[],
 *            mediation?:boolean}}
 */
export function applyStewardAction(state, actionId, args = {}) {
    if (state.phase !== 'day') {
        return { ok: false, reason: 'the village day has not started or is already over', state, events: [] };
    }
    if (!STEWARD_ACTIONS.includes(actionId)) {
        return { ok: false, reason: 'unknown steward action', state, events: [] };
    }
    if (state.stewardActionsLeft <= 0) {
        return { ok: false, reason: 'no steward actions left today', state, events: [] };
    }

    const next = clone(state);
    next.step += 1;
    const events = [];

    if (actionId === 'host-gathering') {
        if (next.stores.grain < 2) {
            return { ok: false, reason: 'not enough grain in the granary (2 needed)', state, events: [] };
        }
        next.stores.grain -= 2;
        next.stewardActionsLeft -= 1;
        for (const v of next.villagers) {
            v.needs.belonging = clamp(v.needs.belonging + 10);
            v.needs.hunger = clamp(v.needs.hunger + 15); /* the gathering is a shared meal */
            v.memory.push({ day: next.day, text: 'shared a meal at the common table' });
        }
        next.chronicle.push({ day: next.day, text: 'The steward hosted a gathering; everyone shared a meal at the common table.' });
        events.push({
            type: 'world:changed',
            label: 'Common-table gathering hosted',
            detail: 'Belonging +10 and hunger +15 for every villager; 2 grain used.',
            meta: { action: actionId }
        });
    } else if (actionId === 'assign-task') {
        const target = villager(next, args.villagerId);
        if (!target) return { ok: false, reason: 'unknown villager', state, events: [] };
        next.stewardActionsLeft -= 1;
        target.needs.craft = clamp(target.needs.craft + 15);
        const def = VILLAGERS.find((v) => v.id === target.id);
        if (def.good) next.stores[def.good] += 1;
        target.memory.push({ day: next.day, text: `worked on their ${def.craft} at the steward's request` });
        next.chronicle.push({ day: next.day, text: `${def.name} worked on their ${def.craft} at the steward's request.` });
        events.push({
            type: 'world:changed',
            label: `Task assigned — ${def.name}`,
            detail: `Craft +15${def.good ? `; ${def.good} +1` : ''}.`,
            meta: { action: actionId, villagerId: target.id }
        });
    } else if (actionId === 'mediate-dispute') {
        if (!next.dispute) {
            return { ok: false, reason: 'no dispute is open to mediate', state, events: [] };
        }
        /* Mediation spends the steward action; the compare envelope is
         * applied afterwards through applyMediationResult (rule R2). */
        next.stewardActionsLeft -= 1;
        events.push({
            type: 'decision:requested',
            label: 'Submitting the dispute to the chronicle (compare)',
            detail: 'The two claims are weighed against the shared chronicle.',
            meta: { action: actionId }
        });
        return { ok: true, state: next, events, mediation: true };
    } else if (actionId === 'share-news') {
        const target = villager(next, args.villagerId);
        if (!target) return { ok: false, reason: 'unknown villager', state, events: [] };
        next.stewardActionsLeft -= 1;
        target.needs.belonging = clamp(target.needs.belonging + 6);
        const def = VILLAGERS.find((v) => v.id === target.id);
        target.memory.push({ day: next.day, text: 'the steward shared news of the valley' });
        next.chronicle.push({ day: next.day, text: `The steward shared news of the valley with ${def.name}.` });
        events.push({
            type: 'world:changed',
            label: `News shared — ${def.name}`,
            detail: 'Belonging +6 for the listener.',
            meta: { action: actionId, villagerId: target.id }
        });
    }

    return { ok: true, state: next, events };
}

/* ------------------------------------------------------------------ */
/* Phase transition: briefing → day 1                                  */
/* ------------------------------------------------------------------ */

/** Begin the village run: day 1 starts with 2 steward actions available. */
export function beginVillage(state) {
    if (state.phase !== 'briefing') return { state, events: [] };
    const next = clone(state);
    next.phase = 'day';
    next.day = 1;
    next.step += 1;
    const events = [{
        type: 'scenario:started',
        label: `Living Village begun — ${VILLAGE_META.name}`,
        detail: `${VILLAGERS.length} villagers, ${DAYS} days ahead, ` +
            `${STEWARD_ACTIONS_PER_DAY} steward actions per day.`,
        meta: { seed: next.seed, scenarioVersion: SCENARIO_VERSION }
    }];
    return { state: next, events };
}

/* ------------------------------------------------------------------ */
/* End-day phases (strict order; design doc §3)                        */
/* ------------------------------------------------------------------ */

/** 3.1–3.2: weather + needs decay, onto a fresh clone. */
export function applyDayDecay(state) {
    if (state.phase !== 'day') return { ok: false, reason: 'not in the day phase', state, events: [] };
    const next = clone(state);
    next.step += 1;
    next.weather = weatherFor(next, next.day);
    const cold = next.weather === 'cold snap';
    for (const v of next.villagers) {
        v.needs.hunger = clamp(v.needs.hunger - NEEDS_DECAY.hunger);
        v.needs.warmth = clamp(v.needs.warmth - (cold ? NEEDS_DECAY.warmthCold : NEEDS_DECAY.warmth));
        v.needs.belonging = clamp(v.needs.belonging - NEEDS_DECAY.belonging);
        v.needs.craft = clamp(v.needs.craft - NEEDS_DECAY.craft);
        v.goal = null;
        v.lastGoalKey = null;
    }
    next.chronicle.push({ day: next.day, text: `The day dawned ${next.weather}.` });
    const events = [{
        type: 'world:changed',
        label: `Day ${next.day}: ${next.weather}`,
        detail: `Needs decay applied (warmth −${cold ? NEEDS_DECAY.warmthCold : NEEDS_DECAY.warmth}).`,
        meta: { weather: next.weather, day: next.day }
    }];
    return { ok: true, state: next, events };
}

/**
 * R1: record one goal-arbitration envelope (choice) for one villager and
 * apply the goal's objective effects. Unmapped results fall back to
 * 'craft' (documented).
 */
export function applyGoalResult(state, villagerId, envelope) {
    const next = clone(state);
    const v = villager(next, villagerId);
    if (!v) return { state, events: [] };
    const result = envelope && typeof envelope.result === 'string' ? envelope.result : null;
    let key = 'craft';                 /* documented fallback (R1) */
    let targetId = null;
    if (result === 'eat at the common table') key = 'eat';
    else if (result === 'gather firewood for the hearth') key = 'firewood';
    else if (result === 'work on their craft') key = 'craft';
    else if (result && result.startsWith('visit ')) {
        const name = result.slice('visit '.length);
        const def = VILLAGERS.find((x) => x.name === name);
        if (def && def.id !== villagerId) {
            key = 'visit';
            targetId = def.id;
        }
    }

    const def = VILLAGERS.find((x) => x.id === villagerId);
    const events = [];
    let detail = '';
    if (key === 'eat') {
        if (next.stores.grain >= 1) {
            next.stores.grain -= 1;
            v.needs.hunger = clamp(v.needs.hunger + 25);
            v.memory.push({ day: next.day, text: 'shared a meal at the common table' });
            detail = 'Ate at the common table (hunger +25, grain −1).';
        } else {
            v.memory.push({ day: next.day, text: 'the granary is empty' }); /* D3 */
            detail = 'found the granary empty and went without a meal.';
        }
    } else if (key === 'firewood') {
        next.stores.firewood += 1;
        v.needs.warmth = clamp(v.needs.warmth + 15);
        v.memory.push({ day: next.day, text: 'gathered firewood for the hearth' });
        detail = 'Gathered firewood (warmth +15, firewood +1).';
    } else if (key === 'visit' && targetId) {
        const t = villager(next, targetId);
        const tdef = VILLAGERS.find((x) => x.id === targetId);
        v.needs.belonging = clamp(v.needs.belonging + 12);
        t.needs.belonging = clamp(t.needs.belonging + 4);
        bumpRel(next, villagerId, targetId, 3);
        v.memory.push({ day: next.day, text: `visited ${tdef.name}` });
        t.memory.push({ day: next.day, text: `was visited by ${def.name}` });
        detail = `Visited ${tdef.name} (belonging +12/+4, relationship +3).`;
    } else {
        v.needs.craft = clamp(v.needs.craft + 15);
        v.memory.push({ day: next.day, text: `worked on their ${def.craft}` });
        /* D10: working the craft produces the villager's good — bakers and
         * millers replenish the granary, the carpenter the woodshed. */
        let prod = '';
        if (def.good) {
            next.stores[def.good] += 1;
            prod = `; ${def.good} +1`;
        }
        detail = `Worked on their ${def.craft} (craft +15${prod}).`;
    }

    v.goal = detail;
    v.lastGoalKey = key;
    next.goalChoices.push({ day: next.day, villagerId, key, confidence: envelope ? envelope.confidence : null });
    next.chronicle.push({ day: next.day, text: `${def.name}: ${detail}` });
    events.push({
        type: 'world:changed',
        label: `${def.name} pursues: ${key === 'visit' && targetId ? `visit ${VILLAGERS.find((x) => x.id === targetId).name}` : key}`,
        detail: `${detail} (kernel choice, confidence ${envelope ? Number(envelope.confidence).toFixed(4) : 'n/a'})`,
        meta: { rule: 'R1', villagerId, goalKey: key, confidence: envelope ? envelope.confidence : null }
    });
    return { state: next, events };
}

/** 3.4: open a dispute if thresholds are met (D2: at most one open).
 *
 * Opening conditions (design doc D8): a pair quarrels when BOTH villagers
 * feel lonely (belonging < 40) — untended villages argue — or when the
 * relationship is strained (rel < −2) even if everyone is comfortable.
 * A steward who keeps belonging up prevents disputes.
 */
export function openDisputeIfAny(state) {
    if (state.dispute) return { state, events: [] };
    if (state.day >= DAYS) return { state, events: [] }; /* D2a: no new disputes on the final evening */
    let worst = null;
    for (let i = 0; i < VILLAGERS.length; i++) {
        for (let j = i + 1; j < VILLAGERS.length; j++) {
            const a = VILLAGERS[i].id;
            const b = VILLAGERS[j].id;
            const r = relOf(state, a, b);
            const lonely = villager(state, a).needs.belonging < 40 &&
                villager(state, b).needs.belonging < 40;
            if ((lonely || r < -2) && (worst === null || r < worst.r)) {
                worst = { a, b, r };
            }
        }
    }
    if (!worst) return { state, events: [] };
    const next = clone(state);
    next.step += 1;
    const topic = DISPUTE_TOPICS[`${worst.a}|${worst.b}`] || DISPUTE_TOPIC_DEFAULT;
    next.dispute = { a: worst.a, b: worst.b, topic, openedDay: next.day };
    next.disputesOpened += 1;
    bumpRel(next, worst.a, worst.b, -6);
    const da = VILLAGERS.find((v) => v.id === worst.a);
    const db = VILLAGERS.find((v) => v.id === worst.b);
    villager(next, worst.a).memory.push({ day: next.day, text: `quarreled with ${db.name} over ${topic}` });
    villager(next, worst.b).memory.push({ day: next.day, text: `quarreled with ${da.name} over ${topic}` });
    next.chronicle.push({ day: next.day, text: `${da.name} and ${db.name} quarreled over ${topic}.` });
    const events = [{
        type: 'world:changed',
        label: `Dispute: ${da.name} and ${db.name}`,
        detail: `Over ${topic}. The steward may mediate (compare against the chronicle).`,
        meta: { rule: 'R2', a: worst.a, b: worst.b }
    }];
    return { state: next, events };
}

/**
 * R2: apply one mediation compare envelope. The dispute closes regardless
 * of the verdict; supported claim's owner +4 belonging ("tie": +2 both);
 * pair relationship +4.
 */
export function applyMediationResult(state, envelope) {
    if (!state.dispute) return { state, events: [] };
    const next = clone(state);
    next.step += 1;
    const result = envelope && typeof envelope.result === 'string' ? envelope.result : null;
    const claims = disputeClaimsFor(state);
    const da = VILLAGERS.find((v) => v.id === next.dispute.a);
    const db = VILLAGERS.find((v) => v.id === next.dispute.b);
    let detail = '';
    if (result === claims.a) {
        villager(next, next.dispute.a).needs.belonging =
            clamp(villager(next, next.dispute.a).needs.belonging + 4);
        detail = `${da.name}'s claim is supported by the chronicle (belonging +4).`;
    } else if (result === claims.b) {
        villager(next, next.dispute.b).needs.belonging =
            clamp(villager(next, next.dispute.b).needs.belonging + 4);
        detail = `${db.name}'s claim is supported by the chronicle (belonging +4).`;
    } else {
        villager(next, next.dispute.a).needs.belonging =
            clamp(villager(next, next.dispute.a).needs.belonging + 2);
        villager(next, next.dispute.b).needs.belonging =
            clamp(villager(next, next.dispute.b).needs.belonging + 2);
        detail = 'The chronicle cannot distinguish the claims ("tie"): belonging +2 to both.';
    }
    bumpRel(next, next.dispute.a, next.dispute.b, 4);
    villager(next, next.dispute.a).memory.push({ day: next.day, text: `the dispute with ${db.name} was resolved` });
    villager(next, next.dispute.b).memory.push({ day: next.day, text: `the dispute with ${da.name} was resolved` });
    next.chronicle.push({ day: next.day, text: `The dispute between ${da.name} and ${db.name} was resolved. ${detail}` });
    next.dispute = null;
    next.disputesResolved += 1;
    const events = [{
        type: 'world:changed',
        label: 'Dispute resolved',
        detail: `${detail} (kernel compare, confidence ${envelope ? Number(envelope.confidence).toFixed(4) : 'n/a'})`,
        meta: { rule: 'R2', confidence: envelope ? envelope.confidence : null }
    }];
    return { state: next, events };
}

/**
 * R3: record one chronicle digest envelope (extract). Display-only.
 */
export function applyDigestResult(state, envelope) {
    const next = clone(state);
    const snippets = envelope && Array.isArray(envelope.result) ? envelope.result.map(String) : [];
    next.digests.push({ day: next.day, snippets });
    const events = [{
        type: 'world:changed',
        label: `Chronicle digest for day ${next.day}`,
        detail: `${snippets.length} snippets extracted; display-only (rule R3).`,
        meta: { rule: 'R3', day: next.day }
    }];
    return { state: next, events };
}

/** 3.6: advance the day; after DAYS days compute the report ending. */
export function advanceDay(state) {
    const next = clone(state);
    next.step += 1;
    next.day += 1;
    next.stewardActionsLeft = STEWARD_ACTIONS_PER_DAY;
    if (next.day > DAYS) {
        next.phase = 'report';
        next.report = villageReport(next);
        const events = [{
            type: 'scenario:completed',
            label: `Village report — ${next.report.kind}`,
            detail: next.report.summary,
            meta: { ending: next.report.kind, meanNeeds: next.report.meanNeeds }
        }];
        return { state: next, events, ended: true };
    }
    const events = [{
        type: 'world:changed',
        label: `Morning of day ${next.day}`,
        detail: `${STEWARD_ACTIONS_PER_DAY} steward actions available.`,
        meta: { day: next.day }
    }];
    return { state: next, events, ended: false };
}

/* ------------------------------------------------------------------ */
/* Endings (design doc §8)                                             */
/* ------------------------------------------------------------------ */

export function villageReport(state) {
    const mean = Math.round(meanNeeds(state) * 10) / 10;
    let best = null;
    let worst = null;
    for (let i = 0; i < VILLAGERS.length; i++) {
        for (let j = i + 1; j < VILLAGERS.length; j++) {
            const a = VILLAGERS[i].id;
            const b = VILLAGERS[j].id;
            const r = relOf(state, a, b);
            if (!best || r > best.r) best = { a, b, r };
            if (!worst || r < worst.r) worst = { a, b, r };
        }
    }
    let kind;
    if (mean >= 45 && !state.dispute) kind = 'thriving';
    else if (mean < 30 || state.dispute) kind = 'struggling';
    else kind = 'settled';
    return {
        kind,
        meanNeeds: mean,
        bestPair: best ? `${nameOf(best.a)} & ${nameOf(best.b)} (+${best.r})` : '—',
        worstPair: worst ? `${nameOf(worst.a)} & ${nameOf(worst.b)} (${worst.r})` : '—',
        disputesOpened: state.disputesOpened,
        disputesResolved: state.disputesResolved,
        grain: state.stores.grain,
        firewood: state.stores.firewood,
        digestCount: state.digests.length,
        seed: state.seed,
        summary: `After ${DAYS} days the mean need satisfaction is ${mean}/100; ` +
            `disputes opened ${state.disputesOpened}, resolved ${state.disputesResolved}. ` +
            `The granary holds ${state.stores.grain} grain, the woodshed ${state.stores.firewood} firewood.`
    };
}

function nameOf(id) { return VILLAGERS.find((v) => v.id === id).name; }

/* ------------------------------------------------------------------ */
/* Context texts submitted to the kernel (evidence honesty)            */
/* ------------------------------------------------------------------ */

/** The exact context string for one villager's goal arbitration. */
export function goalContext(state, villagerId) {
    const def = VILLAGERS.find((v) => v.id === villagerId);
    const v = villager(state, villagerId);
    const mem = v.memory.slice(-8)
        .map((m) => `day ${m.day}: ${m.text}`)
        .join('; ') || 'no memories yet';
    /* Derived, state-honest phrases (design doc D9): the numeric needs
     * are restated as felt conditions whose wording lexically echoes the
     * authored goal options, so lexical support is meaningful. */
    const feel = [];
    if (v.needs.hunger < 50) {
        feel.push(state.stores.grain >= 1
            ? 'hungers for food to eat at the common table'
            : 'hungers for food but the granary is empty');
    }
    if (v.needs.warmth < 35) feel.push('shivers from the cold');
    if (v.needs.belonging < 35) feel.push('feels lonely and longs to visit a friend');
    if (v.needs.craft < 35) feel.push('craves meaningful work on their craft');
    const feelText = feel.length ? `${def.name} ${feel.join(' and ')}. ` : '';
    return `${def.name} is a villager of ${VILLAGE_META.name} (${def.craft}, ${def.trait}). ` +
        `${def.name} remembers: ${mem}. ${feelText}` +
        `Hunger ${v.needs.hunger}/100, warmth ${v.needs.warmth}/100, belonging ${v.needs.belonging}/100, ` +
        `craft ${v.needs.craft}/100. The granary holds ${state.stores.grain} grain, the woodshed ` +
        `${state.stores.firewood} firewood. Today is ${state.weather}.`;
}

/** The exact context string for the mediation compare (the chronicle). */
export function mediationContext(state) {
    return chronicleText(state, state.day) || 'The chronicle is still empty.';
}

/** The exact context string for the day's digest extract. */
export function digestContext(state) {
    const lines = dayChronicleLines(state, state.day);
    return lines.map((t, i) => `${i + 1}. ${t}`).join(' ') || 'Nothing happened today.';
}

/* ------------------------------------------------------------------ */
/* Internal helpers (dependency-free, Node-importable)                 */
/* ------------------------------------------------------------------ */

function clamp(n) { return Math.max(0, Math.min(100, n)); }

function bumpRel(state, a, b, delta) {
    const key = `${a}|${b}`;
    state.rel[key] = Math.max(-10, Math.min(10, (state.rel[key] ?? 0) + delta));
    state.rel[`${b}|${a}`] = state.rel[key];
}

function clone(state) {
    return JSON.parse(JSON.stringify(state));
}

function mulberry32(seed) {
    let a = seed >>> 0;
    return function () {
        a |= 0;
        a = (a + 0x6D2B79F5) | 0;
        let t = Math.imul(a ^ (a >>> 15), 1 | a);
        t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

function deriveSeed(seed, step) {
    let h = (seed ^ (step * 0x9E3779B9)) >>> 0;
    h = Math.imul(h ^ (h >>> 16), 0x45d9f3b) >>> 0;
    h = Math.imul(h ^ (h >>> 16), 0x45d9f3b) >>> 0;
    return (h ^ (h >>> 16)) >>> 0;
}
