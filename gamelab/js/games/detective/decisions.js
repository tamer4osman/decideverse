/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * decisions.js - decision-request builders for The Last Detective.
 *
 * Pure functions: given the scenario state they build the exact request
 * object that will be submitted to the real OmniDecide kernel, plus the
 * game's declared interpretation. Because they are pure, replay can
 * rebuild byte-identical requests from a fresh state, and Node tests
 * can execute them against the real WASM engine without a browser.
 */

import {
    ledger, availableActions, SUSPECTS, MAX_CONSULTATIONS
} from './scenario.js';

/** Human labels used in the UI and in history records. */
const LABELS = {
    leads: 'Lead consultation',
    compare: 'Statement weighing',
    noul: 'Courier claim check',
    extract: 'Case brief extraction',
    rank: 'Final suspect ranking'
};

function interpretation(kind, extra) {
    const base = {
        leads: {
            summary: 'The engine chose the lead with the strongest lexical support in the evidence ' +
                'collected so far (choice primitive over the currently available leads).',
            applied: 'If the player\'s next action matches the recommended lead, the museum director grants ' +
                '+1 investigation hour — a transparent rule shown in the feed. The engine does not execute, ' +
                'unlock or forbid anything by itself; up to ' + MAX_CONSULTATIONS + ' consultations per case.'
        },
        compare: {
            summary: 'The engine compared two witness statements about the corridor camera gap against the ' +
                'case file (compare primitive). A "tie" verdict means the wording does not distinguish them.',
            applied: 'Advisory only. Whether the maintenance was authorized is settled objectively by ' +
                'evidence E-07, not by this comparison; the UI marks the supported statement.'
        },
        noul: {
            summary: 'The engine tested whether the courier claim, as worded, is supported by the case ' +
                'file ledger (noul primitive).',
            applied: 'Advisory verification. Evidence E-05 remains the objective fact regardless of the verdict.'
        },
        extract: {
            summary: 'The engine extracted the most probative evidence snippets from the ledger ' +
                '(extract primitive, topK=3).',
            applied: 'The extracted snippets are displayed in the case brief panel; extraction does not ' +
                'create, remove or re-rank actual evidence.'
        },
        rank: {
            summary: 'The engine ordered all four suspects by lexical support in the complete ledger ' +
                '(rank primitive).',
            applied: 'Advisory only. The player\'s accusation alone decides the ending — scenario logic, ' +
                'never the engine, determines who the culprit is.'
        }
    };
    return Object.assign({}, base[kind], extra || {});
}

/** Full request + meta info for the lead consultation (choice). */
export function buildLeadConsultation(state) {
    const leads = availableActions(state);
    if (!leads.length || state.consultations >= MAX_CONSULTATIONS) return null;
    return {
        kind: 'leads',
        label: LABELS.leads,
        request: {
            primitive: 'choice',
            instruction: 'Which investigation lead is best supported by the evidence currently collected?',
            options: leads.map((a) => a.title),
            context: ledger(state)
        },
        interpretation: interpretation('leads'),
        /* maps winner label -> action id for the +1h bonus rule */
        labelToAction: Object.fromEntries(leads.map((a) => [a.title, a.id]))
    };
}

/** Full request + meta info for the statement weighing (compare). */
export function buildStatementCompare(state) {
    if (!state.discovered.includes('E-01')) return null;
    return {
        kind: 'compare',
        label: LABELS.compare,
        request: {
            primitive: 'compare',
            instruction: 'Which statement about the corridor camera gap is consistent with the collected evidence?',
            options: [
                'Head of security Marcus Webb says the recording gap was scheduled maintenance that he authorized himself.',
                'The maintenance schedule board shows no authorized maintenance for the corridor camera on gala night.'
            ],
            context: ledger(state)
        },
        interpretation: interpretation('compare')
    };
}

/** Full request + meta info for the courier claim check (noul). */
export function buildClaimCheck(state) {
    if (!state.discovered.includes('E-05')) return null;
    return {
        kind: 'noul',
        label: LABELS.noul,
        request: {
            primitive: 'noul',
            instruction: 'The loading dock courier log records a parcel signed by Adrian Cole.',
            context: ledger(state)
        },
        interpretation: interpretation('noul')
    };
}

/** Full request + meta info for the case brief (extract). */
export function buildCaseBrief(state) {
    if (state.discovered.length < 6) return null;
    return {
        kind: 'extract',
        label: LABELS.extract,
        request: {
            primitive: 'extract',
            instruction: 'Which evidence items most directly implicate the culprit?',
            context: ledger(state),
            topK: 3
        },
        interpretation: interpretation('extract')
    };
}

/** Full request + meta info for the final suspect ranking (rank). */
export function buildFinalRanking(state) {
    if (state.discovered.length < 6) return null;
    return {
        kind: 'rank',
        label: LABELS.rank,
        request: {
            primitive: 'rank',
            instruction: 'Rank the suspects by how strongly the collected evidence implicates them in the theft of the Meridian Diamond.',
            options: SUSPECTS.map((s) => s.rankOption),
            context: ledger(state)
        },
        interpretation: interpretation('rank')
    };
}

/** Map a review id to its builder. */
export const BUILDERS = {
    'consult-leads': buildLeadConsultation,
    'weigh-statements': buildStatementCompare,
    'claim-check': buildClaimCheck,
    'case-brief': buildCaseBrief,
    'final-ranking': buildFinalRanking
};

/**
 * Rebuild a decision request for a replay log entry.
 * @param {object} state fresh replay state at this point.
 * @param {{kind:'action'|'decision', id:string}} entry replay log entry.
 * @returns {{entry:object, spec:object|null}} spec = builder output.
 */
export function rebuildForReplay(state, entry) {
    if (entry.kind === 'decision') {
        const builder = BUILDERS[entry.id];
        return { entry, spec: builder ? builder(state) : null };
    }
    return { entry, spec: null };
}
