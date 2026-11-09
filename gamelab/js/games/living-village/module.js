/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * module.js - Living Village: Decideverse game module (UI + controller).
 *
 * Owns the village workspace interface: briefing, the day view (steward
 * actions, villager cards, dispute panel, chronicle), end-day arbitration
 * and the village report. All objective rules live in scenario.js (pure
 * reducer); all decision submissions go through the session -> adapter ->
 * real WASM kernel path and are recorded in history/telemetry.
 *
 * The kernel's outputs influence the game ONLY through the declared,
 * transparent rules R1 (goal arbitration), R2 (mediation) and R3
 * (display-only digest) - see the design document and scenario.js.
 *
 * Two ordered logs, two distinct purposes (Stage 2.1 semantics):
 *  - `runLog` (here): the faithful INPUT sequence of the run - briefing,
 *    steward actions, end-days. Replay re-executes exactly this through
 *    the pure reducer; decision requests are rebuilt and genuinely
 *    re-submitted to the real engine.
 *  - session.actionLog (shared): SUCCESSFUL player actions only - begin,
 *    steward actions (including mediation), end-day. The 6 goal
 *    arbitrations and the digest of an end-day are kernel invocations
 *    triggered by that command, not separate player actions; rejected
 *    steward commands never reach the counter.
 *
 * Seed ownership (Stage 2.1): the briefing seed is validated, written to
 * the authoritative session via session.setSeed(), used to build the
 * scenario state, shown by the workspace, kept by reset and reused by
 * replay.
 */

import { el, clear } from '../../core/dom.js';
import { bus } from '../../core/eventbus.js';
import {
    VILLAGE_META, VILLAGERS, DAYS, DEFAULT_SEED, STEWARD_ACTIONS_PER_DAY,
    createInitialState, beginVillage, applyStewardAction, applyDayDecay,
    applyGoalResult, openDisputeIfAny, applyMediationResult, applyDigestResult,
    advanceDay, visitTargetFor, relOf, disputeClaimsFor, meanNeeds
} from './scenario.js';
import { buildGoalChoice, buildMediationCompare, buildChronicleDigest } from './decisions.js';
import { runReplay } from './replay.js';

export function createLivingVillageModule() {
    /* Instance state ---------------------------------------------------- */
    let env = null;
    let state = createInitialState(DEFAULT_SEED);
    let runLog = [];        /* {kind, id, args} — full reproducible sequence */
    let lastRun = null;     /* {seed, log, decisionCount, ending} snapshot */
    let decisionLog = [];   /* {kind, villagerId, raw, ok} in order */
    let replayReport = null;
    let busy = false;

    const instance = {
        start(session) {
            state = createInitialState((session && session.seed) || DEFAULT_SEED);
            runLog = [];
            decisionLog = [];
            replayReport = null;
            lastRun = null;
        },

        mount(environment) {
            env = environment;
            render();
        },

        status() {
            return {
                world: {
                    Day: state.phase === 'report' ? `report (after ${DAYS} days)` : state.day,
                    Weather: state.weather || '—',
                    Grain: state.stores.grain,
                    Firewood: state.stores.firewood,
                    'Mean needs': state.phase === 'briefing' ? '—' : `${Math.round(meanNeeds(state) * 10) / 10}/100`,
                    Dispute: state.dispute ? 'OPEN — mediate!' : 'none',
                    Ending: state.report ? state.report.kind : '—'
                }
            };
        },

        actors() {
            return VILLAGERS.map((def) => {
                const v = state.villagers.find((x) => x.id === def.id);
                return {
                    name: `${def.name} — ${def.craft}`,
                    note: def.trait,
                    state: state.dispute && (state.dispute.a === def.id || state.dispute.b === def.id)
                        ? 'highlight' : ''
                };
            });
        },

        unmount() { env = null; },
        destroy() { env = null; },

        /* exposed for the Node-facing test surface */
        _debug: () => ({ state, runLog, decisionLog, lastRun })
    };

    /* ---------------------------------------------------------------- */
    /* Rendering                                                        */
    /* ---------------------------------------------------------------- */

    function render() {
        if (!env) return;
        clear(env.viewport);
        if (state.phase === 'briefing') renderBriefing();
        else if (state.phase === 'day') renderDay();
        else renderReport();
        renderControls();
    }

    function renderBriefing() {
        const seedInput = el('input', {
            class: 'input seed-input', id: 'lv-seed', type: 'number',
            value: String(state.seed), min: '0', max: '4294967295', step: '1',
            'aria-label': 'Scenario seed'
        });
        env.viewport.append(el('div', { class: 'lv-briefing' },
            el('p', { class: 'intro-kicker' }, `The village of ${VILLAGE_META.name} · ${VILLAGE_META.valley} · stage 3 simulation`),
            el('h2', {}, 'Seven days in Thornbury'),
            el('p', {}, `Six villagers live in ${VILLAGE_META.name}: Tamsin the baker, Old Idris the carpenter, ` +
                'Mara the herbalist, Piet the shepherd, Juno the miller and Calla the teacher. They eat, keep warm, ' +
                'seek company and take pride in their work — and they remember what happens to them. As steward you ' +
                'have two actions each day: host a gathering, assign a task, mediate a dispute or share news.'),
            el('p', {}, 'Every evening each villager decides what to pursue the next day — and every one of those ' +
                'decisions is a real request to the OmniDecide WASM kernel, built from that villager\'s memory and ' +
                'needs. The kernel is a deterministic lexical evidence matcher: it selects the option its submitted ' +
                'evidence supports best. It advises through the declared rules R1–R3; the reducer applies every effect.'),
            el('ul', { class: 'feature-list' },
                el('li', {}, el('strong', {}, 'Steward: '), 'two actions per day — host a gathering (a shared meal), assign a task, mediate a dispute, share news.'),
                el('li', {}, el('strong', {}, 'Villagers: '), 'private memory, four needs, a relationship web; daily goal arbitration through the kernel.'),
                el('li', {}, el('strong', {}, 'Chronicle: '), 'the objective record of the run; the engine digests it for the daily panel and weighs claims against it.')),
            el('div', { class: 'intro-actions briefing-start' },
                el('label', { class: 'seed-label', for: 'lv-seed' }, 'Seed'),
                seedInput,
                el('button', {
                    class: 'btn primary', id: 'lv-begin',
                    onclick: () => beginFromBriefing()
                }, 'Begin day 1'))));
    }

    function renderDay() {
        const wrap = el('div', { class: 'lv-day' });

        /* Village header */
        wrap.append(el('div', { class: 'dt-case-head' },
            el('div', { class: 'dt-case-title' },
                el('h2', {}, `Day ${state.day} in ${VILLAGE_META.name}`),
                el('span', { class: 'muted' }, `Yesterday's weather: ${state.weather} · mean needs ${Math.round(meanNeeds(state) * 10) / 10}/100`)),
            el('div', { class: 'dt-case-meters' },
                el('div', { class: 'meter', id: 'lv-grain', title: 'Grain in the granary' },
                    el('span', { class: 'meter-label' }, 'Grain'),
                    el('span', { class: 'meter-value' }, String(state.stores.grain))),
                el('div', { class: 'meter', id: 'lv-firewood', title: 'Firewood in the woodshed' },
                    el('span', { class: 'meter-label' }, 'Firewood'),
                    el('span', { class: 'meter-value' }, String(state.stores.firewood))),
                state.dispute
                    ? el('div', { class: 'meter bonus', id: 'lv-dispute-meter', title: 'A dispute is open' },
                        el('span', { class: 'meter-label' }, 'Dispute'),
                        el('span', { class: 'meter-value small' }, 'open — mediate'))
                    : null)));

        /* Steward actions */
        wrap.append(el('div', { class: 'lv-steward card-inset' },
            el('h3', { class: 'panel-title' }, `Steward actions (${state.stewardActionsLeft} of ${STEWARD_ACTIONS_PER_DAY} left today)`),
            el('div', { class: 'lv-steward-actions' },
                el('button', {
                    class: 'btn ghost small', 'data-steward-action': 'host-gathering',
                    disabled: busy || state.stewardActionsLeft <= 0 || state.stores.grain < 2,
                    title: 'A shared meal: hunger +15 and belonging +10 for everyone; 2 grain',
                    onclick: () => doStewardAction('host-gathering', {})
                }, 'Host a gathering (2 grain)'),
                el('button', {
                    class: 'btn ghost small', 'data-steward-action': 'mediate-dispute',
                    disabled: busy || state.stewardActionsLeft <= 0 || !state.dispute,
                    title: 'Weigh the two claims against the chronicle (compare)',
                    onclick: () => doStewardAction('mediate-dispute', {})
                }, 'Mediate the dispute'),
                el('span', { class: 'muted small' }, 'assign-task and share-news sit on each villager card:'),
                el('button', {
                    class: 'btn primary', id: 'lv-endday', disabled: busy,
                    onclick: () => doEndDay()
                }, 'End the day'))));

        /* Assign-task / share-news per villager (small buttons on cards) */
        /* Villager cards */
        const grid = el('div', { class: 'lv-grid' });
        for (const def of VILLAGERS) {
            const v = state.villagers.find((x) => x.id === def.id);
            const target = visitTargetFor(state, def.id);
            const relTarget = target ? `${target.name} (${relOf(state, def.id, target.id) >= 0 ? '+' : ''}${relOf(state, def.id, target.id)})` : '—';
            grid.append(el('div', { class: 'card lv-villager', 'data-villager': def.id },
                el('h3', { class: 'panel-title' }, def.name),
                el('p', { class: 'muted small' }, `${def.craft} · ${def.trait}`),
                needsBar('hunger', v.needs.hunger),
                needsBar('warmth', v.needs.warmth),
                needsBar('belonging', v.needs.belonging),
                needsBar('craft', v.needs.craft),
                el('p', { class: 'muted small lv-goal', title: 'Today\'s goal (kernel-arbitrated)' },
                    v.goal ? `Today: ${v.goal}` : 'The evening will tell.'),
                el('p', { class: 'muted small' }, `Closest bond: ${relTarget}`),
                el('details', { class: 'lv-memory' },
                    el('summary', {}, `Memory (${v.memory.length})`),
                    el('ul', { class: 'feature-list' },
                        v.memory.slice(-4).map((m) => el('li', {}, `day ${m.day}: ${m.text}`)))),
                el('div', { class: 'lv-card-actions' },
                    el('button', {
                        class: 'btn ghost small', 'data-assign': def.id,
                        disabled: busy || state.stewardActionsLeft <= 0,
                        title: 'Craft +15 and produce their good',
                        onclick: () => doStewardAction('assign-task', { villagerId: def.id })
                    }, 'Assign task'),
                    el('button', {
                        class: 'btn ghost small', 'data-share': def.id,
                        disabled: busy || state.stewardActionsLeft <= 0,
                        title: 'Belonging +6 for the listener',
                        onclick: () => doStewardAction('share-news', { villagerId: def.id })
                    }, 'Share news'))));
        }
        wrap.append(grid);

        /* Dispute panel */
        if (state.dispute) {
            const da = VILLAGERS.find((v) => v.id === state.dispute.a);
            const db = VILLAGERS.find((v) => v.id === state.dispute.b);
            wrap.append(el('div', { class: 'card-inset lv-dispute', id: 'lv-dispute' },
                el('h3', { class: 'panel-title' }, `Open dispute: ${da.name} & ${db.name}`),
                el('p', { class: 'muted small' }, `Over ${state.dispute.topic}. Mediating submits both claims to the ` +
                    'kernel (compare) against the chronicle; the reducer applies rule R2.'),
                el('button', {
                    class: 'btn primary', id: 'lv-mediate', disabled: busy || state.stewardActionsLeft <= 0,
                    onclick: () => doStewardAction('mediate-dispute', {})
                }, 'Mediate now')));
        }

        /* Chronicle + latest digest */
        wrap.append(el('div', { class: 'lv-chronicle card-inset' },
            el('h3', { class: 'panel-title' }, 'Village chronicle'),
            el('ol', { class: 'extract-list', id: 'lv-chronicle-list' },
                state.chronicle.slice(-12).map((c) => el('li', {}, `day ${c.day}: ${c.text}`))),
            state.digests.length
                ? el('p', { class: 'muted small', id: 'lv-digest' },
                    `Latest digest (day ${state.digests[state.digests.length - 1].day}, kernel-extracted): ` +
                    state.digests[state.digests.length - 1].snippets.join(' | '))
                : el('p', { class: 'muted small', id: 'lv-digest' }, 'The first digest appears after day 1.')));

        env.viewport.append(wrap);
    }

    function needsBar(label, value) {
        return el('div', { class: 'meter', title: `${label}: ${value}/100` },
            el('span', { class: 'meter-label' }, label),
            el('span', { class: 'meter-value' }, String(value)));
    }

    function renderReport() {
        const r = state.report;
        env.viewport.append(el('div', { class: `dt-ending ${r.kind === 'thriving' ? 'win' : (r.kind === 'struggling' ? 'wrong' : 'cold')} lv-report`, id: 'lv-report' },
            el('p', { class: 'ending-kicker' },
                r.kind === 'thriving' ? 'The village thrives' : (r.kind === 'struggling' ? 'The village struggles' : 'The village settles')),
            el('h2', {}, `Village report — ${r.kind}`),
            el('p', {}, r.summary),
            el('dl', { class: 'kv two-col ending-stats' },
                el('dt', {}, 'Mean needs'), el('dd', {}, `${r.meanNeeds}/100`),
                el('dt', {}, 'Closest bond'), el('dd', {}, r.bestPair),
                el('dt', {}, 'Strained pair'), el('dd', {}, r.worstPair),
                el('dt', {}, 'Disputes opened / resolved'), el('dd', {}, `${r.disputesOpened} / ${r.disputesResolved}`),
                el('dt', {}, 'Grain / firewood'), el('dd', {}, `${r.grain} / ${r.firewood}`),
                el('dt', {}, 'Chronicle digests'), el('dd', {}, String(r.digestCount)),
                el('dt', {}, 'Seed'), el('dd', {}, String(r.seed))),
            el('div', { class: 'intro-actions' },
                el('button', { class: 'btn primary', id: 'lv-reset-report', onclick: () => doReset() }, 'Reset the village'),
                lastRun ? el('button', { class: 'btn ghost', id: 'lv-replay-report', onclick: () => doReplay() }, 'Replay this run') : null)));
    }

    function renderControls() {
        clear(env.controls);
        env.controls.append(
            el('button', {
                class: 'btn ghost small', id: 'lv-reset', disabled: state.phase === 'briefing',
                onclick: () => doReset()
            }, 'Reset'),
            el('button', {
                class: 'btn ghost small', id: 'lv-replay',
                disabled: !(lastRun || runLog.length) || busy,
                title: 'Re-run the recorded steward sequence through the real engine',
                onclick: () => doReplay()
            }, 'Replay run'),
            el('a', { class: 'btn ghost small', href: '#/inspector' }, 'Decision Inspector'),
            el('a', { class: 'btn ghost small', href: '#/history' }, 'Activity history'),
            el('span', { class: 'muted small controls-note' },
                `seed ${state.seed} · step ${state.step} · day ${state.day}`));
    }

    /* ---------------------------------------------------------------- */
    /* Gameplay operations                                              */
    /* ---------------------------------------------------------------- */

    function beginFromBriefing() {
        const input = document.getElementById('lv-seed');
        /* Seed validation/normalization (documented): non-numeric or empty
         * input falls back to the scenario default; numeric input is
         * normalized to its unsigned 32-bit value and written to the
         * AUTHORITATIVE session. */
        const parsed = input ? Number.parseInt(input.value, 10) : NaN;
        const seed = Number.isFinite(parsed) ? parsed >>> 0 : DEFAULT_SEED;
        env.session.setSeed(seed);
        state = createInitialState(seed);
        runLog = [];
        decisionLog = [];
        replayReport = null;
        env.session.clearActionLog(); /* a new scenario run starts a fresh action history */
        const res = beginVillage(state);
        state = res.state;
        runLog.push({ kind: 'briefing' });
        env.session.logAction({ action: 'begin', args: { seed } });
        for (const spec of res.events) env.emitEvent(spec);
        bus.emit('game:updated', {});
        render();
    }

    async function doStewardAction(actionId, args) {
        if (busy) return;
        const res = applyStewardAction(state, actionId, args);
        if (!res.ok) {
            /* Rejected commands are NOT successful player actions: the
             * shared action counter must not move (event only). */
            env.emitEvent({ type: 'session:note', label: 'Steward action rejected', detail: res.reason });
            render();
            return;
        }
        state = res.state;
        runLog.push({ kind: 'action', id: actionId, args });
        env.session.logAction({ action: 'steward', args: { id: actionId, ...args } });
        env.session.tick();
        for (const spec of res.events) env.emitEvent(spec);

        if (res.mediation) {
            const spec = buildMediationCompare(state);
            if (!spec) { render(); return; }
            busy = true;
            render();
            env.emitEvent({
                type: 'decision:requested',
                label: `Submitting to OmniDecide — ${spec.label}`,
                detail: `compare · 2 claims · context ${spec.request.context.length} chars`,
                meta: { kind: 'mediate' }
            });
            const rec = env.decide(spec.request, { label: spec.label, interpretation: spec.interpretation });
            decisionLog.push({ kind: 'mediate', villagerId: null, id: rec.id, raw: rec.raw, ok: rec.ok });
            if (rec.ok) {
                const applied = applyMediationResult(state, rec.envelope);
                state = applied.state;
                for (const spec2 of applied.events) env.emitEvent(spec2);
            } else {
                env.emitEvent({
                    type: 'session:note',
                    label: 'Engine returned an error',
                    detail: rec.error ? `${rec.error.kind}: ${rec.error.message}` : 'unknown error',
                    meta: { decisionId: rec.id }
                });
            }
            busy = false;
        }
        bus.emit('game:updated', {});
        render();
    }

    async function doEndDay() {
        if (busy) return;
        busy = true;
        render();
        /* The end-day command is one player action; the goal arbitrations
         * and the digest it triggers are kernel invocations recorded by
         * the session/inspector, not separate player actions. */
        env.session.logAction({ action: 'end-day', args: { day: state.day } });

        runLog.push({ kind: 'endday' });
        const decay = applyDayDecay(state);
        state = decay.state;
        for (const spec of decay.events) env.emitEvent(spec);

        for (const def of VILLAGERS) {
            const spec = buildGoalChoice(state, def.id);
            const rec = env.decide(spec.request, { label: spec.label, interpretation: spec.interpretation });
            decisionLog.push({ kind: 'goal', villagerId: def.id, id: rec.id, raw: rec.raw, ok: rec.ok });
            if (rec.ok) {
                const applied = applyGoalResult(state, def.id, rec.envelope);
                state = applied.state;
                for (const spec2 of applied.events) env.emitEvent(spec2);
            } else {
                env.emitEvent({
                    type: 'session:note',
                    label: 'Goal arbitration failed',
                    detail: rec.error ? `${rec.error.kind}: ${rec.error.message}` : 'unknown error',
                    meta: { decisionId: rec.id, villagerId: def.id }
                });
            }
        }

        const disp = openDisputeIfAny(state);
        state = disp.state;
        for (const spec of disp.events) env.emitEvent(spec);

        const digest = buildChronicleDigest(state);
        const drec = env.decide(digest.request, { label: digest.label, interpretation: digest.interpretation });
        decisionLog.push({ kind: 'digest', villagerId: null, id: drec.id, raw: drec.raw, ok: drec.ok });
        if (drec.ok) {
            const applied = applyDigestResult(state, drec.envelope);
            state = applied.state;
            for (const spec2 of applied.events) env.emitEvent(spec2);
        } else {
            env.emitEvent({
                type: 'session:note',
                label: 'Chronicle digest failed',
                detail: drec.error ? `${drec.error.kind}: ${drec.error.message}` : 'unknown error',
                meta: { decisionId: drec.id }
            });
        }

        const adv = advanceDay(state);
        state = adv.state;
        for (const spec of adv.events) env.emitEvent(spec);
        env.session.tick();
        if (adv.ended) snapshotRun();
        busy = false;
        bus.emit('game:updated', {});
        render();
    }

    function snapshotRun() {
        if (runLog.length) {
            lastRun = {
                seed: state.seed,
                log: runLog.slice(),
                decisions: decisionLog.slice(),
                decisionCount: decisionLog.length,
                ending: state.report ? state.report.kind : null
            };
        }
    }

    function doReset() {
        snapshotRun();
        const seed = state.seed;
        /* Reset semantics (documented): the SAME effective seed stays
         * authoritative, the village restarts in the briefing phase, and
         * the shared action history starts fresh. The previous run remains
         * inspectable via history and `Replay run`. */
        env.session.setSeed(seed);
        state = createInitialState(seed);
        runLog = [];
        decisionLog = [];
        replayReport = null;
        env.session.clearActionLog();
        env.emitEvent({
            type: 'scenario:reset',
            label: 'Village reset',
            detail: `State rebuilt from seed ${seed}. Previous run kept in history for inspection.`,
            meta: { seed }
        });
        bus.emit('game:updated', {});
        render();
    }

    function doReplay() {
        if (busy) return;
        const source = lastRun ||
            { seed: state.seed, log: runLog.slice(), decisions: decisionLog.slice(), ending: state.report ? state.report.kind : null };
        if (!source.log.length) return;
        busy = true;
        render();

        env.emitEvent({
            type: 'session:note',
            label: 'Replay started',
            detail: `Re-running ${source.log.length} logged steps with seed ${source.seed}. ` +
                'Goal and digest requests are rebuilt and re-submitted to the real engine.',
            meta: { seed: source.seed, steps: source.log.length }
        });

        const result = runReplay({
            seed: source.seed,
            log: source.log,
            invoke: (request, meta) => env.adapter.invoke(request, meta),
            gameId: 'living-village'
        });

        /* Compare replayed decision envelopes to the originals in order. */
        let identical = 0;
        let compared = 0;
        const originals = source.decisions || [];
        for (let i = 0; i < result.reports.length; i++) {
            const rp = result.reports[i];
            const orig = originals[i];
            if (!orig) continue;
            compared += 1;
            if (rp.ok && orig.ok && rp.raw === orig.raw) identical += 1;
        }
        const endingSame = result.ok && source.ending
            ? (result.state.report ? result.state.report.kind : null) === source.ending
            : null;

        replayReport = {
            ok: result.ok,
            reason: result.reason || null,
            steps: source.log.length,
            decisions: result.reports.length,
            identical,
            compared,
            endingSame
        };

        env.emitEvent({
            type: 'session:note',
            label: result.ok ? 'Replay complete' : 'Replay aborted',
            detail: result.ok
                ? `${result.reports.length} decisions re-invoked; ${identical}/${compared} envelopes byte-identical; ` +
                  `ending ${endingSame ? 'matches' : 'DIFFERS from'} the original run.`
                : `Reason: ${result.reason}`,
            meta: { replay: replayReport }
        });

        busy = false;
        render();
        showReplayReport();
    }

    function showReplayReport() {
        const r = replayReport;
        const host = env.viewport.querySelector('.lv-replay-report-host') ||
            env.viewport.appendChild(el('div', { class: 'lv-replay-report-host' }));
        clear(host);
        host.append(el('div', {
            class: `card-inset lv-replay-report ${r.ok ? 'ok' : 'err'}`, id: 'lv-replay-report'
        },
            el('h3', { class: 'panel-title' }, r.ok ? 'Replay report' : 'Replay failed'),
            el('dl', { class: 'kv two-col' },
                el('dt', {}, 'Steps re-executed'), el('dd', {}, String(r.steps)),
                el('dt', {}, 'Decisions re-invoked'), el('dd', {}, String(r.decisions)),
                el('dt', {}, 'Byte-identical envelopes'), el('dd', {}, `${r.identical} / ${r.compared}`),
                el('dt', {}, 'Ending matches original'),
                el('dd', {}, r.endingSame === null ? 'n/a (run unfinished)' : (r.endingSame ? 'yes' : 'no')),
                el('dt', {}, 'Failures'), el('dd', {}, r.reason || 'none')),
            el('p', { class: 'muted small' },
                'Replay re-submits rebuilt requests to the real WASM kernel — it is a Decideverse simulation ' +
                'capability, not a kernel feature. Determinism comes from the pure scenario reducer plus ' +
                'the kernel\'s byte-identical envelopes.')));
    }

    return instance;
}
