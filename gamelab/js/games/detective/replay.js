/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * replay.js - deterministic replay runner for The Last Detective.
 *
 * Replay is a GameLab simulation capability (NOT a kernel feature): it
 * re-executes a recorded action/decision sequence through the same
 * pure reducer against the same seed. Decision requests are rebuilt by
 * the pure builders and genuinely re-submitted to the real engine —
 * kernel determinism then guarantees byte-identical envelopes. This
 * runner works in the browser (adapter injected) and in Node tests
 * (CommonJS-injected adapter), because the invoke function is a
 * parameter.
 *
 * The replay log format:
 *   {kind:'action',   id:<actionId>}
 *   {kind:'decision', id:<reviewId>}
 *   {kind:'briefing'}
 *   {kind:'accuse',   id:<suspectId>}
 *   {kind:'unsolved'}
 */

import {
    createInitialState, applyAction, applyConsultationResult, applyDecisionResult,
    beginInvestigation, accuse, closeUnsolved, canAccuse
} from './scenario.js';
import { BUILDERS } from './decisions.js';

/**
 * Run a full replay.
 * @param {object} opts
 * @param {number} opts.seed scenario seed.
 * @param {Array<{kind:string, id:string}>} opts.log recorded sequence.
 * @param {(request:object, meta:object) => object} opts.invoke adapter invoke
 *        (records decisions in history/telemetry when a full adapter is passed).
 * @param {string} [opts.gameId] game id stamped on decision metadata.
 * @returns {{ok:boolean, reason?:string, state:object, reports:object[]}}
 *          reports: one per replayed decision {id, requestId, raw, ok, error}.
 */
export function runReplay(opts) {
    const { seed, log, invoke } = opts;
    if (!Array.isArray(log)) return { ok: false, reason: 'replay log missing', state: null, reports: [] };
    const gameId = opts.gameId || 'detective';

    let state = createInitialState(seed);
    const reports = [];

    for (const entry of log) {
        if (entry.kind === 'briefing') {
            state = beginInvestigation(state).state;
            continue;
        }
        if (entry.kind === 'action') {
            const res = applyAction(state, entry.id);
            if (!res.ok) return { ok: false, reason: `action "${entry.id}" no longer applicable: ${res.reason}`, state, reports };
            state = res.state;
            continue;
        }
        if (entry.kind === 'decision') {
            const builder = BUILDERS[entry.id];
            const spec = builder ? builder(state) : null;
            if (!spec) return { ok: false, reason: `decision "${entry.id}" no longer available`, state, reports };
            const rec = invoke(spec.request, {
                game: gameId,
                step: state.step,
                label: `[replay] ${spec.label}`,
                interpretation: spec.interpretation,
                replayOf: entry.replayOf || null
            });
            reports.push({
                id: entry.id,
                ok: rec.ok,
                raw: rec.raw,
                error: rec.error ? rec.error.message : null,
                durationMs: rec.durationMs
            });
            if (!rec.ok) {
                return { ok: false, reason: `decision "${entry.id}" failed: ${rec.error ? rec.error.message : 'unknown'}`, state, reports };
            }
            state = (entry.id === 'consult-leads')
                ? applyConsultationResult(state, rec.envelope).state
                : applyDecisionResult(state, spec.kind, rec.envelope).state;
            continue;
        }
        if (entry.kind === 'accuse') {
            if (!canAccuse(state)) {
                return { ok: false, reason: 'accusation not possible during replay', state, reports };
            }
            const res = accuse(state, entry.id);
            if (!res.ok) return { ok: false, reason: res.reason, state, reports };
            state = res.state;
            continue;
        }
        if (entry.kind === 'unsolved') {
            state = closeUnsolved(state).state;
            continue;
        }
        return { ok: false, reason: `unknown replay entry kind "${entry.kind}"`, state, reports };
    }

    return { ok: true, state, reports };
}
