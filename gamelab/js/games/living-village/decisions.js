/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * decisions.js - decision-request builders for Living Village.
 *
 * Pure functions: given the scenario state they build the exact request
 * object that will be submitted to the real OmniDecide kernel, plus the
 * game's declared interpretation (what the engine returned and what the
 * game will do with it). Request shapes follow docs/API.md exactly:
 *   choice  { primitive, instruction, options, context }
 *   compare { primitive, instruction, options (exactly two), context }
 *   extract { primitive, instruction, context, topK }
 *
 * The kernel is a deterministic lexical evidence matcher — these builders
 * submit authored, day-stamped memory/needs/chronicle text as evidence so
 * lexical support between evidence and options is meaningful. No result
 * is ever hard-coded: every envelope comes from the real kernel.
 */

import {
    VILLAGERS, visitTargetFor, goalContext, mediationContext, digestContext,
    disputeClaimsFor, DAYS
} from './scenario.js';

const LABELS = {
    goal: 'Daily goal arbitration',
    mediate: 'Dispute mediation',
    digest: 'Chronicle digest'
};

function interpretation(kind) {
    const base = {
        goal: {
            summary: 'The kernel compared the four authored goal options against the villager\'s ' +
                'day-stamped memory and current needs as submitted evidence (choice primitive) and ' +
                'selected the option with the strongest lexical support.',
            applied: 'Declared rule R1: the selected goal is the goal the villager pursues today; the ' +
                'reducer applies that goal\'s objective effects (eating, firewood, visiting, craft). ' +
                'An unmapped result falls back to "work on their craft". The engine never executes ' +
                'anything itself.'
        },
        mediate: {
            summary: 'The kernel compared the two villagers\' authored claims against the shared ' +
                'chronicle as submitted evidence (compare primitive). A "tie" means the wording does ' +
                'not distinguish them.',
            applied: 'Declared rule R2: the dispute closes regardless of the verdict; the owner of the ' +
                'supported claim gains belonging +4 (a "tie" grants +2 to both); the pair\'s ' +
                'relationship improves by 4. The reducer — not the engine — closes the dispute.'
        },
        digest: {
            summary: 'The kernel extracted the snippets that best summarize today\'s chronicle entries ' +
                '(extract primitive, topK=3).',
            applied: 'Declared rule R3: display-only. The digest is shown in the chronicle panel and ' +
                'never mutates village state.'
        }
    };
    return base[kind];
}

/**
 * Build the goal-arbitration request for one villager (choice, 1 call).
 * The visit target is pre-selected by the reducer (D1) and baked into the
 * option text, so submitted options are stable strings.
 */
export function buildGoalChoice(state, villagerId) {
    const def = VILLAGERS.find((v) => v.id === villagerId);
    if (!def || state.phase !== 'day') return null;
    const target = visitTargetFor(state, villagerId);
    const options = [
        'eat at the common table',
        'gather firewood for the hearth',
        `visit ${target.name}`,
        'work on their craft'
    ];
    return {
        kind: 'goal',
        villagerId,
        label: `${LABELS.goal} — ${def.name} (day ${state.day})`,
        request: {
            primitive: 'choice',
            instruction: `Which goal should ${def.name} pursue today?`,
            options,
            context: goalContext(state, villagerId)
        },
        interpretation: interpretation('goal'),
        goalKeys: ['eat', 'firewood', 'visit', 'craft'],
        visitTargetId: target.id
    };
}

/** Build the dispute-mediation request (compare, exactly two options). */
export function buildMediationCompare(state) {
    if (!state.dispute || state.phase !== 'day') return null;
    const claims = disputeClaimsFor(state);
    const da = VILLAGERS.find((v) => v.id === state.dispute.a);
    const db = VILLAGERS.find((v) => v.id === state.dispute.b);
    return {
        kind: 'mediate',
        label: `${LABELS.mediate} — ${da.name} & ${db.name} (day ${state.day})`,
        request: {
            primitive: 'compare',
            instruction: 'Which claim does the village chronicle support?',
            options: [claims.a, claims.b],
            context: mediationContext(state)
        },
        interpretation: interpretation('mediate'),
        claims
    };
}

/** Build the chronicle digest request (extract, topK=3). */
export function buildChronicleDigest(state) {
    if (state.phase !== 'day') return null;
    return {
        kind: 'digest',
        label: `${LABELS.digest} — day ${state.day}`,
        request: {
            primitive: 'extract',
            instruction: 'Which events best summarize today in the village?',
            context: digestContext(state),
            topK: 3
        },
        interpretation: interpretation('digest')
    };
}

/**
 * Rebuild a decision request for a replay log entry.
 * Log kinds: {kind:'goal', villagerId} | {kind:'mediate'} | {kind:'digest'}.
 * @param {object} state fresh replay state at this point.
 * @param {{kind:string}} entry replay log entry.
 * @returns {{entry:object, spec:object|null}}
 */
export function rebuildForReplay(state, entry) {
    if (entry.kind === 'goal') {
        return { entry, spec: buildGoalChoice(state, entry.villagerId) };
    }
    if (entry.kind === 'mediate') {
        return { entry, spec: buildMediationCompare(state) };
    }
    if (entry.kind === 'digest') {
        return { entry, spec: buildChronicleDigest(state) };
    }
    return { entry, spec: null };
}

/** Small helper for docs/tests: the full per-day call bound (design §7.4). */
export function callsPerDay() {
    return { choice: VILLAGERS.length, extract: 1, compare: '0..1 (only if mediated)', total: `<= ${VILLAGERS.length + 2}` };
}

export const MAX_DAYS = DAYS;
