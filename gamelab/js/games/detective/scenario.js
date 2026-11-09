/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * scenario.js - "The Missing Museum Diamond" scenario for The Last Detective.
 *
 * PURE and DOM-free: this module owns the objective simulation rules.
 * It never calls the decision engine and never imports the adapter —
 * engine results enter the state only through `applyDecisionResult()`
 * with the envelope the real kernel actually returned. GameLab shows
 * the difference between these objective outcomes and the engine's
 * lexical evaluation at every step.
 *
 * Determinism: the same seed plus the same action/decision sequence
 * produces the same state, the same events and the same ending. The
 * only randomness is cosmetic flavor derived from deriveSeed(seed, step),
 * which is fully deterministic and never touches evidence or outcomes.
 */

/* ------------------------------------------------------------------ */
/* Identity                                                            */
/* ------------------------------------------------------------------ */

export const SCENARIO_VERSION = '1.0.0';
export const DEFAULT_SEED = 20261108;

export const CASE_META = {
    diamond: 'The Meridian Diamond',
    museum: 'Aldermoor Museum of Antiquities',
    event: 'evening gala',
    window: '19:30 – 20:30',
    caseNumber: 'AM-2026-1108'
};

export const SUSPECTS = [
    {
        id: 'adrian-cole',
        name: 'Adrian Cole',
        role: 'Visiting appraiser',
        rankOption: 'Adrian Cole',
        note: 'Requested a private viewing of the diamond; owns the Cole Assay Office.'
    },
    {
        id: 'elena-vasquez',
        name: 'Dr. Elena Vasquez',
        role: 'Museum curator',
        rankOption: 'Elena Vasquez',
        note: 'Controls the display case; was master of ceremonies on stage.'
    },
    {
        id: 'marcus-webb',
        name: 'Marcus Webb',
        role: 'Head of security',
        rankOption: 'Marcus Webb',
        note: 'Runs the camera archive; claims the corridor camera gap was scheduled.'
    },
    {
        id: 'lena-fontaine',
        name: 'Lena Fontaine',
        role: 'Catering manager',
        rankOption: 'Lena Fontaine',
        note: 'Moved freely backstage and at the loading dock all evening.'
    }
];

export const LOCATIONS = {
    'grand-hall': 'Grand Hall',
    'security-office': 'Security Office',
    'curators-office': "Curator's Office",
    'loading-dock': 'Loading Dock',
    'restoration-lab': 'Restoration Lab'
};

/* ------------------------------------------------------------------ */
/* Evidence: the exact texts submitted to the kernel as context        */
/* ------------------------------------------------------------------ */

export const EVIDENCE = {
    'E-00': {
        id: 'E-00',
        title: 'Case report',
        location: null,
        text: 'The Meridian Diamond vanished from the Grand Hall display case during the evening gala, ' +
            'between 19:30 and 20:30. Head of security Marcus Webb reports the recording gap on the ' +
            'corridor camera was caused by scheduled maintenance.'
    },
    'E-01': {
        id: 'E-01',
        title: 'Corridor camera recording gap',
        location: 'security-office',
        text: 'The maintenance corridor camera has a recording gap from 19:38 to 19:52 on gala night.'
    },
    'E-02': {
        id: 'E-02',
        title: "Vasquez: Cole's private viewing",
        location: 'curators-office',
        text: 'Curator Elena Vasquez states that visiting appraiser Adrian Cole requested a private viewing ' +
            'of the Meridian Diamond at 19:30 and was left alone beside the display case.'
    },
    'E-03': {
        id: 'E-03',
        title: 'Guests: technician in the corridor',
        location: 'grand-hall',
        text: 'Guests mention a person in a technician uniform hurrying along the maintenance corridor at 19:45.'
    },
    'E-04': {
        id: 'E-04',
        title: 'Resin replica in the case',
        location: 'grand-hall',
        text: 'The display case held a professionally made resin replica instead of the Meridian Diamond; ' +
            'the replica swap requires an appraiser who was left alone beside the case.'
    },
    'E-04b': {
        id: 'E-04b',
        title: 'Lab: replica is fresh',
        location: 'restoration-lab',
        text: 'The restoration lab confirms the replica is a professional resin copy machined within the last month.'
    },
    'E-05': {
        id: 'E-05',
        title: 'Courier log: parcel signed by Cole',
        location: 'loading-dock',
        text: 'The loading dock courier log records a parcel signed by Adrian Cole at 20:15, addressed to the Cole Assay Office.'
    },
    'E-06': {
        id: 'E-06',
        title: "Fontaine saw Cole in uniform",
        location: 'loading-dock',
        text: 'Catering manager Lena Fontaine saw Adrian Cole in a technician uniform near the loading dock gate at 20:10.'
    },
    'E-07': {
        id: 'E-07',
        title: 'No authorized maintenance',
        location: 'security-office',
        text: 'The maintenance schedule board shows no authorized maintenance for the corridor camera on gala ' +
            'night; the recording gap was not authorized by anyone.'
    },
    'E-08': {
        id: 'E-08',
        title: "Vasquez's stage alibi",
        location: 'curators-office',
        text: 'Curator Elena Vasquez was on the Grand Hall stage delivering remarks from 19:35 to 20:05 in front of eighty guests.'
    }
};

/* ------------------------------------------------------------------ */
/* Investigation actions (objective rules: cost, yields, unlocks)      */
/* ------------------------------------------------------------------ */

export const ACTIONS = [
    {
        id: 'hall-case', location: 'grand-hall', title: 'Examine the display case',
        cost: 2, yields: ['E-04']
    },
    {
        id: 'hall-guests', location: 'grand-hall', title: 'Mingle with the gala guests',
        cost: 1, yields: ['E-03']
    },
    {
        id: 'sec-camera', location: 'security-office', title: 'Pull the corridor camera archive',
        cost: 1, yields: ['E-01']
    },
    {
        id: 'sec-board', location: 'security-office', title: 'Check the maintenance schedule board',
        cost: 1, yields: ['E-07']
    },
    {
        id: 'cur-interview', location: 'curators-office', title: 'Interview Dr. Elena Vasquez',
        cost: 1, yields: ['E-02']
    },
    {
        id: 'cur-stage', location: 'curators-office', title: "Verify Vasquez's stage schedule",
        cost: 1, yields: ['E-08']
    },
    {
        id: 'dock-log', location: 'loading-dock', title: 'Inspect the courier log',
        cost: 1, yields: ['E-05']
    },
    {
        id: 'dock-staff', location: 'loading-dock', title: 'Question the dock staff about the gate',
        cost: 1, yields: ['E-06']
    },
    {
        id: 'lab-replica', location: 'restoration-lab', title: 'Ask the lab about the replica materials',
        cost: 1, yields: ['E-04b']
    }
];

export const BASE_HOURS = 9;
export const ACCUSATION_AT_EVIDENCE = 6;  /* E-00 + 5 discovered */
export const MAX_CONSULTATIONS = 3;
export const MAX_BONUSES = 2;

/* ------------------------------------------------------------------ */
/* Initial state                                                       */
/* ------------------------------------------------------------------ */

export function createInitialState(seed = DEFAULT_SEED) {
    return {
        scenarioVersion: SCENARIO_VERSION,
        seed: seed >>> 0,
        step: 0,
        phase: 'briefing', /* briefing | investigation | closed */
        hoursLeft: BASE_HOURS,
        discovered: ['E-00'],
        takenActions: [],
        consultations: 0,
        bonusesGranted: 0,
        pendingBonus: null,       /* {actionId, label} awaiting follow-up */
        brief: null,              /* extract result (array of snippets) */
        rankResult: null,         /* rank result string */
        compareResult: null,      /* compare envelope (statement weighing) */
        claimResult: null,        /* noul envelope (courier claim check) */
        ending: null,             /* {kind:'win'|'wrong'|'unsolved', ...} */
        culpritId: 'adrian-cole'
    };
}

/* ------------------------------------------------------------------ */
/* Derived views (pure)                                                */
/* ------------------------------------------------------------------ */

export function evidenceCount(state) {
    return state.discovered.length;
}

export function canAccuse(state) {
    return state.phase === 'investigation' && state.discovered.length >= ACCUSATION_AT_EVIDENCE;
}

/** Is the action currently available (not taken, affordable, phase ok)? */
export function isActionAvailable(state, actionId) {
    if (state.phase !== 'investigation') return false;
    const action = ACTIONS.find((a) => a.id === actionId);
    if (!action) return false;
    if (state.takenActions.some((t) => t.id === actionId)) return false;
    return state.hoursLeft >= action.cost;
}

export function availableActions(state) {
    return ACTIONS.filter((a) => isActionAvailable(state, a.id));
}

/**
 * The evidence ledger: the exact context string submitted to the kernel.
 * Deterministic order (E-00, E-01, E-02, ...).
 */
export function ledger(state) {
    return state.discovered
        .map((id) => EVIDENCE[id])
        .filter(Boolean)
        .map((e) => `${e.id}: ${e.text}`)
        .join(' ');
}

/** Review (decision) opportunities with prerequisites — none cost hours. */
export function availableReviews(state) {
    if (state.phase !== 'investigation') return [];
    const reviews = [];
    if (state.discovered.length >= 3 && state.consultations < MAX_CONSULTATIONS) {
        reviews.push({
            id: 'consult-leads', kind: 'leads', title: 'Consult OmniDecide — evaluate current leads',
            hint: 'choice primitive · no investigation cost'
        });
    }
    if (state.discovered.includes('E-01')) {
        reviews.push({
            id: 'weigh-statements', kind: 'compare', title: 'Weigh the conflicting statements about the camera gap',
            hint: 'compare primitive · Webb claims scheduled maintenance; the board says otherwise'
        });
    }
    if (state.discovered.includes('E-05')) {
        reviews.push({
            id: 'claim-check', kind: 'noul', title: 'Verify the courier claim against the case file',
            hint: 'noul primitive · is the courier claim supported by the ledger?'
        });
    }
    if (state.discovered.length >= ACCUSATION_AT_EVIDENCE) {
        reviews.push({
            id: 'case-brief', kind: 'extract', title: 'Assemble the case brief from the ledger',
            hint: 'extract primitive · top-3 most probative items'
        });
        reviews.push({
            id: 'final-ranking', kind: 'rank', title: 'Request a suspect ranking from the evidence',
            hint: 'rank primitive · advisory only'
        });
    }
    return reviews;
}

/* ------------------------------------------------------------------ */
/* Reducer: actions                                                    */
/* ------------------------------------------------------------------ */

const FLAVOR_REMARKS = [
    'A waiter mentions the gallery doors were locked all evening.',
    'A guest remembers the fire exit alarm never went off.',
    'A docent recalls the display case light flickering around 19:40.',
    'A valet says a courier van idled outside well before its scheduled slot.'
];

/**
 * Apply one investigation action.
 * @returns {{ok:boolean, reason?:string, state:object, events:object[]}}
 *          events are specs for the session feed (not yet recorded).
 */
export function applyAction(state, actionId) {
    const action = ACTIONS.find((a) => a.id === actionId);
    if (!action) return { ok: false, reason: 'unknown action', state, events: [] };
    if (state.phase !== 'investigation') {
        return { ok: false, reason: 'scenario is not in the investigation phase', state, events: [] };
    }
    if (state.takenActions.some((t) => t.id === actionId)) {
        return { ok: false, reason: 'action already taken', state, events: [] };
    }
    if (state.hoursLeft < action.cost) {
        return { ok: false, reason: 'not enough investigation hours', state, events: [] };
    }

    /* Deep clone (structuredClone keeps this pure and cheap). */
    const next = clone(state);
    next.step += 1;
    next.hoursLeft -= action.cost;
    next.takenActions.push({ id: actionId, step: next.step });

    const events = [];

    /* Transparent bonus rule: following the engine-recommended lead grants
     * +1 hour from the museum director. The engine never does this itself. */
    if (next.pendingBonus && next.pendingBonus.actionId === actionId &&
        next.bonusesGranted < MAX_BONUSES) {
        next.hoursLeft += 1;
        next.bonusesGranted += 1;
        events.push({
            type: 'lead:selected',
            label: `Followed the engine-recommended lead — the director grants +1 hour`,
            detail: `Recommendation "${next.pendingBonus.label}" was followed; hours now ${next.hoursLeft}.`,
            meta: { recommendedBy: 'omnidecide', actionId }
        });
        next.pendingBonus = null;
    }

    events.push({
        type: 'location:investigated',
        label: `${LOCATIONS[action.location]} — ${action.title}`,
        detail: `Cost ${action.cost}h; hours left ${next.hoursLeft}.`,
        meta: { actionId, location: action.location, cost: action.cost }
    });

    for (const evidenceId of action.yields) {
        if (!next.discovered.includes(evidenceId)) {
            next.discovered.push(evidenceId);
            const ev = EVIDENCE[evidenceId];
            events.push({
                type: 'evidence:discovered',
                label: `${evidenceId} — ${ev.title}`,
                detail: ev.text,
                meta: { evidenceId }
            });
        }
    }

    /* Deterministic cosmetic flavor: never affects evidence or outcomes. */
    const rng = mulberry32(deriveSeed(next.seed, next.step));
    if (rng() < 0.6) {
        const remark = FLAVOR_REMARKS[Math.floor(rng() * FLAVOR_REMARKS.length)];
        events.push({
            type: 'world:changed',
            label: 'Overheard on the floor',
            detail: remark,
            meta: { flavor: true }
        });
    }

    return { ok: true, state: next, events };
}

/* ------------------------------------------------------------------ */
/* Reducer: decision results (from the REAL engine, via the module)    */
/* ------------------------------------------------------------------ */

/**
 * Record the outcome of a lead consultation (choice envelope).
 * @param {object} state current state.
 * @param {object} envelope the actual engine envelope.
 * @returns {{state:object, events:object[]}}
 */
export function applyConsultationResult(state, envelope) {
    const next = clone(state);
    next.consultations += 1;
    const winner = envelope && typeof envelope.result === 'string' ? envelope.result : null;
    const action = winner ? ACTIONS.find((a) => a.title === winner) : null;
    next.pendingBonus = action
        ? { actionId: action.id, label: winner }
        : null;
    const events = [{
        type: 'decision:completed',
        label: `Lead consultation: ${winner ? `"${winner}"` : 'no clear winner'}`,
        detail: action
            ? 'Following this lead next grants +1 investigation hour (transparent rule).'
            : 'The recommended lead did not map to an available action; no bonus is offered.',
        meta: { confidence: envelope ? envelope.confidence : null, pendingBonus: next.pendingBonus }
    }];
    return { state: next, events };
}

/**
 * Record the outcome of any other decision kind.
 * kinds: compare | noul | extract | rank
 */
export function applyDecisionResult(state, kind, envelope) {
    const next = clone(state);
    const events = [];
    if (kind === 'compare') {
        next.compareResult = envelope;
        events.push({
            type: 'decision:completed',
            label: 'Statement weighing complete',
            detail: envelope && envelope.result === 'tie'
                ? 'The engine returned "tie": the two statements are lexically indistinguishable. You must weigh them yourself.'
                : `The engine found more support for: "${envelope && envelope.result}".`,
            meta: { confidence: envelope ? envelope.confidence : null }
        });
    } else if (kind === 'noul') {
        next.claimResult = envelope;
        events.push({
            type: 'decision:completed',
            label: `Courier claim ${envelope && envelope.result === true ? 'holds' : 'not supported'}`,
            detail: 'noul checks the claim wording against the ledger; evidence E-05 remains the objective fact either way.',
            meta: { confidence: envelope ? envelope.confidence : null }
        });
    } else if (kind === 'extract') {
        next.brief = Array.isArray(envelope && envelope.result) ? envelope.result.slice() : null;
        events.push({
            type: 'decision:completed',
            label: 'Case brief assembled (top-3 extraction)',
            detail: 'The brief lists the extracted snippets; it does not create or remove evidence.',
            meta: {}
        });
    } else if (kind === 'rank') {
        next.rankResult = typeof (envelope && envelope.result) === 'string' ? envelope.result : null;
        events.push({
            type: 'decision:completed',
            label: 'Suspect ranking received (advisory)',
            detail: 'The ranking is advice only; your accusation alone decides the ending.',
            meta: { confidence: envelope ? envelope.confidence : null }
        });
    } else {
        return { state, events: [] };
    }
    return { state: next, events };
}

/* ------------------------------------------------------------------ */
/* Reducer: phase transitions and endings                              */
/* ------------------------------------------------------------------ */

export function beginInvestigation(state) {
    if (state.phase !== 'briefing') return { state, events: [] };
    const next = clone(state);
    next.phase = 'investigation';
    const events = [{
        type: 'scenario:started',
        label: 'Investigation begun — The Missing Museum Diamond',
        detail: `${CASE_META.diamond} missing from ${CASE_META.museum}; window ${CASE_META.window}. ` +
            `${BASE_HOURS} investigation hours allocated.`,
        meta: { seed: next.seed, scenarioVersion: SCENARIO_VERSION }
    }];
    return { state: next, events };
}

export function accuse(state, suspectId) {
    if (!canAccuse(state)) {
        return { ok: false, reason: 'not enough evidence to accuse yet', state, events: [] };
    }
    if (!SUSPECTS.some((s) => s.id === suspectId)) {
        return { ok: false, reason: 'unknown suspect', state, events: [] };
    }
    const next = clone(state);
    next.phase = 'closed';
    next.step += 1;
    const suspect = SUSPECTS.find((s) => s.id === suspectId);
    const correct = suspectId === next.culpritId;

    if (correct) {
        next.ending = {
            kind: 'win',
            title: 'Case closed — the Meridian Diamond is recovered',
            text: 'Adrian Cole is arrested at the Cole Assay Office before the courier parcel is opened. ' +
                'Inside: the Meridian Diamond, wrapped in anti-static foam. Cole, left alone beside the ' +
                'display case during his private viewing, swapped in the resin replica and slipped out ' +
                'through the maintenance corridor during the camera gap he knew about.',
            suspectId
        };
    } else {
        const culprit = SUSPECTS.find((s) => s.id === next.culpritId);
        next.ending = {
            kind: 'wrong',
            title: `Case closed — wrong arrest: ${suspect.name}`,
            text: `${suspect.name} is detained, but the evidence does not hold together and the prosecution ` +
                `collapses within a day. The real culprit — ${culprit.name}, ${culprit.role} — is long gone, ` +
                'and so is the Meridian Diamond.',
            suspectId
        };
    }
    const events = [{
        type: 'scenario:completed',
        label: next.ending.title,
        detail: next.ending.text,
        meta: { ending: next.ending.kind, evidenceCount: next.discovered.length }
    }];
    return { ok: true, state: next, events };
}

export function closeUnsolved(state) {
    if (state.phase !== 'investigation') return { state, events: [] };
    const next = clone(state);
    next.phase = 'closed';
    next.step += 1;
    next.ending = {
        kind: 'unsolved',
        title: 'The case went cold',
        text: 'You hand the file back to the Aldermoor curator with your apologies. The Meridian Diamond ' +
            'remains missing, and the insurance assessors take over.',
        suspectId: null
    };
    const events = [{
        type: 'scenario:completed',
        label: next.ending.title,
        detail: next.ending.text,
        meta: { ending: 'unsolved', evidenceCount: next.discovered.length }
    }];
    return { state: next, events };
}

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

function clone(state) {
    return JSON.parse(JSON.stringify(state));
}

/* Local, dependency-free copies of the rng primitives so this module
 * stays importable in Node without the browser bundle. */
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
