/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * replay.js - deterministic replay runner for Living Village.
 *
 * Replay is a Decideverse simulation capability (NOT a kernel feature):
 * it re-executes a recorded steward-action/day sequence through the same
 * pure reducer against the same seed. Decision requests are rebuilt by
 * the pure builders and genuinely re-submitted to the real engine —
 * kernel determinism then guarantees byte-identical envelopes. The
 * invoke function is a parameter, so the runner works in the browser
 * (adapter injected) and in Node tests (CommonJS kernel injected).
 *
 * Replay log format (the faithful INPUT sequence of a run):
 *   {kind:'briefing'}
 *   {kind:'action', id:'host-gathering'|'share-news', args?}
 *   {kind:'action', id:'assign-task', args:{villagerId}}
 *   {kind:'action', id:'mediate-dispute'}       → one compare call
 *   {kind:'endday'}                             → 6 choice + 1 extract call
 */

import {
    createInitialState, applyStewardAction, applyDayDecay, applyGoalResult,
    openDisputeIfAny, applyMediationResult, applyDigestResult, advanceDay,
    beginVillage, VILLAGERS
} from './scenario.js';
import { buildGoalChoice, buildMediationCompare, buildChronicleDigest } from './decisions.js';

/**
 * Run a full replay.
 * @param {object} opts
 * @param {number} opts.seed scenario seed.
 * @param {Array<{kind:string, id?:string, args?:object, villagerId?:string}>} opts.log
 * @param {(request:object, meta:object) => object} opts.invoke adapter invoke
 *        (records decisions in history/telemetry when a full adapter is passed).
 * @param {string} [opts.gameId] game id stamped on decision metadata.
 * @returns {{ok:boolean, reason?:string, state:object, reports:object[]}}
 *          reports: one per replayed decision {kind, villagerId?, raw, ok, error}.
 */
export function runReplay(opts) {
    const { seed, log, invoke } = opts;
    if (!Array.isArray(log)) return { ok: false, reason: 'replay log missing', state: null, reports: [] };
    const gameId = opts.gameId || 'living-village';

    let state = createInitialState(seed);
    const reports = [];

    const submit = (spec, meta) => {
        const rec = invoke(spec.request, meta);
        reports.push({
            kind: spec.kind,
            villagerId: spec.villagerId || null,
            ok: rec.ok,
            raw: rec.raw,
            error: rec.error ? rec.error.message : null,
            durationMs: rec.durationMs
        });
        return rec;
    };

    for (const entry of log) {
        if (entry.kind === 'briefing') {
            if (state.phase !== 'briefing') {
                return { ok: false, reason: 'briefing out of order', state, reports };
            }
            state = beginVillage(state).state;
            continue;
        }
        if (entry.kind === 'action') {
            const res = applyStewardAction(state, entry.id, entry.args || {});
            if (!res.ok) return { ok: false, reason: `action "${entry.id}" no longer applicable: ${res.reason}`, state, reports };
            state = res.state;
            if (res.mediation) {
                const spec = buildMediationCompare(state);
                if (!spec) return { ok: false, reason: 'mediation request unavailable during replay', state, reports };
                const rec = submit(spec, {
                    game: gameId,
                    step: state.step,
                    label: `[replay] ${spec.label}`,
                    interpretation: spec.interpretation
                });
                if (!rec.ok) {
                    return { ok: false, reason: `mediation decision failed: ${rec.error ? rec.error.message : 'unknown'}`, state, reports };
                }
                state = applyMediationResult(state, rec.envelope).state;
            }
            continue;
        }
        if (entry.kind === 'endday') {
            const decay = applyDayDecay(state);
            if (!decay.ok) return { ok: false, reason: decay.reason || 'day decay failed', state, reports };
            state = decay.state;
            /* goal arbitration: one real choice call per villager, roster order */
            for (const v of VILLAGERS) {
                const spec = buildGoalChoice(state, v.id);
                if (!spec) return { ok: false, reason: `goal request unavailable for ${v.id}`, state, reports };
                const rec = submit(spec, {
                    game: gameId,
                    step: state.step,
                    label: `[replay] ${spec.label}`,
                    interpretation: spec.interpretation
                });
                if (!rec.ok) {
                    return { ok: false, reason: `goal decision for ${v.id} failed: ${rec.error ? rec.error.message : 'unknown'}`, state, reports };
                }
                state = applyGoalResult(state, v.id, rec.envelope).state;
            }
            /* dispute check */
            const disp = openDisputeIfAny(state);
            state = disp.state;
            /* chronicle digest: one real extract call */
            const digest = buildChronicleDigest(state);
            if (!digest) return { ok: false, reason: 'digest request unavailable', state, reports };
            const drec = submit(digest, {
                game: gameId,
                step: state.step,
                label: `[replay] ${digest.label}`,
                interpretation: digest.interpretation
            });
            if (!drec.ok) {
                return { ok: false, reason: `digest decision failed: ${drec.error ? drec.error.message : 'unknown'}`, state, reports };
            }
            state = applyDigestResult(state, drec.envelope).state;
            const adv = advanceDay(state);
            state = adv.state;
            continue;
        }
        return { ok: false, reason: `unknown replay entry kind "${entry.kind}"`, state, reports };
    }

    return { ok: true, state, reports };
}
