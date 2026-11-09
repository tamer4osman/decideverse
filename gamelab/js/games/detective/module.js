/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * module.js - The Last Detective: GameLab game module (UI + controller).
 *
 * Owns the workspace interface: case briefing, investigation board,
 * review (decision) actions, accusation panel, ending cards, reset and
 * replay. All objective rules live in scenario.js (pure reducer); all
 * decision submissions go through the session -> adapter -> real WASM
 * kernel path and are recorded in history/telemetry automatically.
 *
 * The engine's outputs influence the game ONLY through the declared,
 * transparent rules (e.g. the +1 hour bonus for following a
 * recommended lead). They never override scenario logic.
 *
 * Two ordered logs, two distinct purposes (Stage 2.1 semantics):
 *  - `runLog` (here): the faithful INPUT sequence of the run - briefing,
 *    investigation actions, decision submissions (including attempts the
 *    engine rejected, so replay can re-submit them byte-faithfully),
 *    accusation, unsolved close. This is what replay re-executes.
 *  - session.actionLog (shared): SUCCESSFUL player actions only - begin,
 *    investigate, decide (accepted, engine-confirmed), accuse,
 *    close-unsolved. This is the workspace "Actions logged" counter.
 *    Rejected commands and failed engine invocations never reach it.
 *
 * Seed ownership (Stage 2.1): the seed the player selects in the
 * briefing is validated, written to the authoritative session via
 * session.setSeed(), used to build the scenario state, shown by the
 * workspace, kept by reset and reused by replay. UI and simulation can
 * never disagree about the seed.
 */

import { el, clear, fmtMs, fmtTime } from '../../core/dom.js';
import { bus } from '../../core/eventbus.js';
import {
    CASE_META, SUSPECTS, LOCATIONS, ACTIONS, EVIDENCE,
    DEFAULT_SEED, BASE_HOURS, MAX_BONUSES,
    createInitialState, applyAction, availableActions, availableReviews,
    beginInvestigation, accuse, closeUnsolved, canAccuse,
    applyConsultationResult, applyDecisionResult, ledger
} from './scenario.js';
import { BUILDERS } from './decisions.js';
import { runReplay } from './replay.js';

export function createDetectiveModule() {
    /* Instance state ---------------------------------------------------- */
    let env = null;
    let state = createInitialState(DEFAULT_SEED);
    let runLog = [];        /* {kind, id} — full reproducible sequence */
    let lastRun = null;     /* {seed, log, decisionCount, ending} snapshot */
    let decisionLog = [];   /* {reviewId, id, raw, ok} in order */
    let replayReport = null;
    let busy = false;       /* guards double clicks while a kernel call runs */

    const instance = {
        /* Called by the simulation route right after creation. */
        start(session) {
            state = createInitialState((session && session.seed) || DEFAULT_SEED);
            runLog = [];
            decisionLog = [];
            replayReport = null;
            lastRun = null;
        },

        /* Called by mountWorkspace with the simulation environment. */
        mount(environment) {
            env = environment;
            render();
            if (state.phase === 'investigation' && state.step === 0) {
                /* resumed session still in briefing state */
            }
        },

        status() {
            return {
                world: {
                    Phase: state.phase,
                    'Hours left': `${state.hoursLeft} of ${BASE_HOURS}` +
                        (state.bonusesGranted ? ` (+${state.bonusesGranted} engine bonus)` : ''),
                    Evidence: `${state.discovered.length} of ${Object.keys(EVIDENCE).length}`,
                    Consultations: `${state.consultations}`,
                    Ending: state.ending ? state.ending.kind : '—'
                }
            };
        },

        actors() {
            return SUSPECTS.map((s) => ({
                name: `${s.name} — ${s.role}`,
                note: s.note,
                state: state.phase === 'closed'
                    ? (s.id === state.culpritId && state.ending && state.ending.kind === 'win' ? 'highlight' : '')
                    : ''
            }));
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
        else if (state.phase === 'investigation') renderInvestigation();
        else renderEnding();
        renderControls();
    }

    function renderBriefing() {
        const seedInput = el('input', {
            class: 'input seed-input', id: 'dt-seed', type: 'number',
            value: String(state.seed), min: '0', max: '4294967295', step: '1',
            'aria-label': 'Scenario seed'
        });
        env.viewport.append(el('div', { class: 'dt-briefing' },
            el('p', { class: 'intro-kicker' }, `Case file ${CASE_META.caseNumber} · Aldermoor Museum · confidential`),
            el('h2', {}, `The Missing ${CASE_META.diamond.replace('The ', '')}`),
            el('p', {}, `${CASE_META.diamond} disappeared from its display case in the Grand Hall of ` +
                `${CASE_META.museum} during the ${CASE_META.event} — somewhere inside the window ` +
                `${CASE_META.window}. The museum is closed, the guest list is sealed, and the press is ` +
                'at the gate. You have until dawn.'),
            el('p', {}, 'Four people moved freely through the building that night. The camera archive has ' +
                'a hole in it. A courier van left the loading dock before it was scheduled to. ' +
                'Everything you need is in the building; you simply have nine investigation hours ' +
                'to find it.'),
            el('ul', { class: 'feature-list' },
                el('li', {}, el('strong', {}, 'Investigate: '), 'spend hours to examine locations and interview people. Each finding enters the evidence ledger.'),
                el('li', {}, el('strong', {}, 'Consult the engine: '), 'at any point you may submit the collected evidence to OmniDecide and see which lead, claim, statement or suspect it supports — and why.'),
                el('li', {}, el('strong', {}, 'Accuse: '), 'once enough evidence is on file, name the thief. The scenario, not the engine, decides who is guilty.')),
            el('div', { class: 'suspect-row' },
                SUSPECTS.map((s) => el('div', { class: 'suspect-chip' },
                    el('span', { class: 'suspect-name' }, s.name),
                    el('span', { class: 'suspect-role' }, s.role)))),
            el('div', { class: 'intro-actions briefing-start' },
                el('label', { class: 'seed-label', for: 'dt-seed' }, 'Seed'),
                seedInput,
                el('button', {
                    class: 'btn primary', id: 'dt-begin-investigation',
                    onclick: () => beginFromBriefing()
                }, 'Begin investigation'))));
    }

    function renderInvestigation() {
        const wrap = el('div', { class: 'dt-investigation' });

        /* Case header */
        wrap.append(el('div', { class: 'dt-case-head' },
            el('div', { class: 'dt-case-title' },
                el('h2', {}, `Case ${CASE_META.caseNumber}`),
                el('span', { class: 'muted' }, CASE_META.diamond + ' — ' + CASE_META.museum)),
            el('div', { class: 'dt-case-meters' },
                el('div', { class: 'meter', id: 'dt-hours', title: 'Investigation hours left' },
                    el('span', { class: 'meter-label' }, 'Hours'),
                    el('span', { class: 'meter-value' }, String(state.hoursLeft))),
                el('div', { class: 'meter', id: 'dt-evidence-count', title: 'Evidence items on file' },
                    el('span', { class: 'meter-label' }, 'Evidence'),
                    el('span', { class: 'meter-value' }, String(state.discovered.length))),
                state.pendingBonus
                    ? el('div', { class: 'meter bonus', id: 'dt-recommendation', title: 'Engine-recommended lead' },
                        el('span', { class: 'meter-label' }, 'Engine recommends'),
                        el('span', { class: 'meter-value small' }, state.pendingBonus.label))
                    : null)));

        /* Investigation board by location */
        const board = el('div', { class: 'dt-board' });
        for (const [locId, locName] of Object.entries(LOCATIONS)) {
            const acts = ACTIONS.filter((a) => a.location === locId);
            const body = el('div', { class: 'dt-loc-actions' },
                acts.map((a) => {
                    const taken = state.takenActions.some((t) => t.id === a.id);
                    const affordable = state.hoursLeft >= a.cost;
                    return el('button', {
                        class: `dt-action ${taken ? 'taken' : ''}`,
                        'data-action-id': a.id,
                        disabled: taken || !affordable || busy,
                        title: taken ? 'Already investigated' : (affordable ? `Costs ${a.cost}h` : 'Not enough hours'),
                        onclick: () => doAction(a.id)
                    },
                        el('span', { class: 'dt-action-title' }, a.title),
                        el('span', { class: 'dt-action-meta' }, taken ? 'done' : `${a.cost}h`));
                }));
            board.append(el('div', { class: 'dt-loc' },
                el('h3', { class: 'dt-loc-name' }, locName),
                body));
        }
        wrap.append(board);

        /* Reviews (real decision opportunities) */
        const reviews = availableReviews(state);
        if (reviews.length) {
            const revWrap = el('div', { class: 'dt-reviews' },
                el('h3', { class: 'panel-title' }, 'Consult the decision engine'),
                el('p', { class: 'muted small' },
                    'Each consultation submits your actual collected evidence to the OmniDecide WASM kernel ' +
                    'and is recorded in the Decision Inspector.'),
                reviews.map((r) => el('button', {
                    class: 'dt-review', 'data-review-id': r.id, disabled: busy,
                    onclick: () => doReview(r.id)
                },
                    el('span', { class: 'dt-review-title' }, r.title),
                    el('span', { class: 'dt-review-hint' }, r.hint))));
            wrap.append(revWrap);
        }

        /* Engine advisory panel */
        const advisory = advisoryPanel();
        if (advisory) wrap.append(advisory);

        /* Case brief (extract result) */
        if (state.brief && state.brief.length) {
            wrap.append(el('div', { class: 'dt-brief card-inset' },
                el('h3', { class: 'panel-title' }, 'Case brief (engine extraction)'),
                el('ol', { class: 'extract-list' },
                    state.brief.map((s) => el('li', {}, String(s))))));
        }

        /* Evidence ledger */
        wrap.append(el('div', { class: 'dt-ledger' },
            el('h3', { class: 'panel-title' }, `Evidence ledger (${state.discovered.length})`),
            el('ul', { class: 'dt-evidence-list' },
                state.discovered.map((id) => {
                    const e = EVIDENCE[id];
                    return el('li', { class: 'dt-evidence-item', 'data-evidence-id': id },
                        el('span', { class: 'mono ev-id' }, e.id),
                        el('span', { class: 'ev-title' }, e.title),
                        el('span', { class: 'ev-text' }, e.text));
                }))));

        /* Accusation panel */
        if (canAccuse(state)) {
            wrap.append(el('div', { class: 'dt-accusation' },
                el('h3', { class: 'panel-title' }, 'Name the thief'),
                el('p', { class: 'muted small' },
                    'Enough evidence is on file. Your accusation ends the case — the scenario decides who is guilty, ' +
                    'whatever the engine\'s ranking says.'),
                el('div', { class: 'dt-accuse-row' },
                    SUSPECTS.map((s) => el('button', {
                        class: 'btn accuse', 'data-suspect-id': s.id, disabled: busy,
                        onclick: () => doAccuse(s.id)
                    }, `Accuse ${s.name}`))),
                el('button', {
                    class: 'btn ghost small', id: 'dt-close-unsolved', disabled: busy,
                    onclick: () => doUnsolved()
                }, 'Close the case unsolved')));
        }

        env.viewport.append(wrap);
    }

    function advisoryPanel() {
        const bits = [];
        if (state.compareResult) {
            const r = state.compareResult.result;
            bits.push(el('div', { class: 'advice-line' },
                el('span', { class: 'advice-kind' }, 'compare'),
                r === 'tie'
                    ? el('span', {}, 'The engine could not distinguish the two statements ("tie", confidence ' + state.compareResult.confidence.toFixed(4) + '). Objective rule: E-07 settles it.')
                    : el('span', {}, `The engine finds more support for: "${r}" (confidence ${state.compareResult.confidence.toFixed(4)}).`)));
        }
        if (state.claimResult) {
            bits.push(el('div', { class: 'advice-line' },
                el('span', { class: 'advice-kind' }, 'noul'),
                el('span', {},
                    `Courier claim ${state.claimResult.result === true ? 'SUPPORTED' : 'NOT SUPPORTED'} by the ledger ` +
                    `(confidence ${state.claimResult.confidence.toFixed(4)}).`)));
        }
        if (state.rankResult) {
            bits.push(el('div', { class: 'advice-line' },
                el('span', { class: 'advice-kind' }, 'rank'),
                el('span', {}, `Engine ranking: ${state.rankResult} — advisory only, you decide.`)));
        }
        if (!bits.length) return null;
        return el('div', { class: 'dt-advisory' },
            el('h3', { class: 'panel-title' }, 'Engine advisories (shown transparently)'),
            bits);
    }

    function renderEnding() {
        const e = state.ending;
        const kindClass = e.kind === 'win' ? 'win' : (e.kind === 'wrong' ? 'wrong' : 'cold');
        const decisionsThisRun = decisionLog.length;
        env.viewport.append(el('div', { class: `dt-ending ${kindClass}`, id: 'dt-ending' },
            el('p', { class: 'ending-kicker' },
                e.kind === 'win' ? 'Resolution' : (e.kind === 'wrong' ? 'Miscarriage' : 'Unresolved')),
            el('h2', {}, e.title),
            el('p', {}, e.text),
            el('dl', { class: 'kv two-col ending-stats' },
                el('dt', {}, 'Evidence on file'), el('dd', {}, `${state.discovered.length} of ${Object.keys(EVIDENCE).length}`),
                el('dt', {}, 'Hours left'), el('dd', {}, String(state.hoursLeft)),
                el('dt', {}, 'Engine decisions'), el('dd', {}, String(decisionsThisRun)),
                el('dt', {}, 'Seed'), el('dd', {}, String(state.seed))),
            el('div', { class: 'intro-actions' },
                el('button', { class: 'btn primary', id: 'dt-reset-ending', onclick: () => doReset() }, 'Reset the case'),
                lastRun ? el('button', { class: 'btn ghost', id: 'dt-replay-from-ending', onclick: () => doReplay() }, 'Replay this run') : null)));
    }

    function renderControls() {
        clear(env.controls);
        env.controls.append(
            el('button', {
                class: 'btn ghost small', id: 'dt-reset', disabled: state.phase === 'briefing',
                onclick: () => doReset()
            }, 'Reset'),
            el('button', {
                class: 'btn ghost small', id: 'dt-replay',
                disabled: !(lastRun || runLog.length) || busy,
                title: 'Re-run the recorded action sequence through the real engine',
                onclick: () => doReplay()
            }, 'Replay run'),
            el('a', { class: 'btn ghost small', href: '#/inspector' }, 'Decision Inspector'),
            el('a', { class: 'btn ghost small', href: '#/history' }, 'Activity history'),
            el('span', { class: 'muted small controls-note' },
                `seed ${state.seed} · step ${state.step} · ledger ${ledger(state).length} chars`));
    }

    /* ---------------------------------------------------------------- */
    /* Gameplay operations                                              */
    /* ---------------------------------------------------------------- */

    function beginFromBriefing() {
        const input = document.getElementById('dt-seed');
        /* Seed validation/normalization (documented): non-numeric or
         * empty input falls back to the scenario default; numeric input
         * is normalized to its unsigned 32-bit value. The selected seed
         * is written to the AUTHORITATIVE session so the workspace,
         * history, reset and replay all use the same value. */
        const parsed = input ? Number.parseInt(input.value, 10) : NaN;
        const seed = Number.isFinite(parsed) ? parsed >>> 0 : DEFAULT_SEED;
        env.session.setSeed(seed);
        state = createInitialState(seed);
        runLog = [];
        decisionLog = [];
        replayReport = null;
        env.session.clearActionLog(); /* a new scenario run starts a fresh action history */
        const res = beginInvestigation(state);
        state = res.state;
        runLog.push({ kind: 'briefing' });
        env.session.logAction({ action: 'begin', args: { seed } });
        for (const spec of res.events) env.emitEvent(spec);
        bus.emit('game:updated', {});
        render();
    }

    function doAction(actionId) {
        const res = applyAction(state, actionId);
        if (!res.ok) {
            /* Rejected commands are NOT successful player actions: the
             * shared action counter must not move (event only). */
            env.emitEvent({ type: 'session:note', label: 'Action rejected', detail: res.reason });
            render();
            return;
        }
        state = res.state;
        runLog.push({ kind: 'action', id: actionId });
        env.session.logAction({ action: 'investigate', args: { id: actionId } });
        env.session.tick(); /* keep the shared sim clock in step with the reducer */
        for (const spec of res.events) env.emitEvent(spec);
        bus.emit('game:updated', {});
        render();
    }

    async function doReview(reviewId) {
        const builder = BUILDERS[reviewId];
        const spec = builder(state);
        if (!spec || busy) return;
        busy = true;
        render(); /* disable buttons while the kernel call runs */

        env.emitEvent({
            type: 'decision:requested',
            label: `Submitting to OmniDecide — ${spec.label}`,
            detail: `${spec.request.primitive} · ${spec.request.options ? spec.request.options.length + ' options · ' : ''}` +
                `context ${spec.request.context.length} chars`,
            meta: { reviewId }
        });

        const rec = env.decide(spec.request, {
            label: spec.label,
            interpretation: spec.interpretation
        });

        runLog.push({ kind: 'decision', id: reviewId });
        decisionLog.push({ reviewId, id: rec.id, raw: rec.raw, ok: rec.ok });

        /* Player-action semantics: submitting a decision is a player
         * action when it was ACCEPTED and produced a real engine record
         * (rec.ok). A failed engine invocation is an event, not a
         * successful player action - the run log keeps the attempt for
         * faithful replay, the action counter does not move. */
        if (rec.ok) {
            env.session.logAction({ action: 'decide', args: { reviewId } });
            const applied = reviewId === 'consult-leads'
                ? applyConsultationResult(state, rec.envelope)
                : applyDecisionResult(state, spec.kind, rec.envelope);
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
        bus.emit('game:updated', {});
        render();
    }

    function doAccuse(suspectId) {
        if (busy) return;
        const res = accuse(state, suspectId);
        if (!res.ok) {
            env.emitEvent({ type: 'session:note', label: 'Accusation rejected', detail: res.reason });
            render();
            return;
        }
        state = res.state;
        runLog.push({ kind: 'accuse', id: suspectId });
        env.session.logAction({ action: 'accuse', args: { id: suspectId } });
        env.session.tick();
        for (const spec of res.events) env.emitEvent(spec);
        snapshotRun();
        bus.emit('game:updated', {});
        render();
    }

    function doUnsolved() {
        if (busy) return;
        const res = closeUnsolved(state);
        state = res.state;
        runLog.push({ kind: 'unsolved' });
        env.session.logAction({ action: 'close-unsolved', args: {} });
        env.session.tick();
        for (const spec of res.events) env.emitEvent(spec);
        snapshotRun();
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
                ending: state.ending ? state.ending.kind : null
            };
        }
    }

    function doReset() {
        snapshotRun();
        const seed = state.seed;
        /* Reset semantics (documented): the SAME effective seed stays
         * authoritative (session + scenario + workspace agree), the
         * scenario restarts in the briefing phase, and the shared action
         * history starts fresh because a new run begins. The previous
         * run remains inspectable via history and `Replay run`. */
        env.session.setSeed(seed);
        state = createInitialState(seed);
        runLog = [];
        decisionLog = [];
        replayReport = null;
        env.session.clearActionLog();
        env.emitEvent({
            type: 'scenario:reset',
            label: 'Scenario reset',
            detail: `State rebuilt from seed ${seed}. Previous run kept in history for inspection.`,
            meta: { seed }
        });
        bus.emit('game:updated', {});
        render();
    }

    async function doReplay() {
        if (busy) return;
        const source = lastRun ||
            { seed: state.seed, log: runLog.slice(), decisions: decisionLog.slice(), ending: state.ending ? state.ending.kind : null };
        if (!source.log.length) return;
        busy = true;
        render();

        env.emitEvent({
            type: 'session:note',
            label: 'Replay started',
            detail: `Re-running ${source.log.length} logged steps with seed ${source.seed}. ` +
                'Decision requests are rebuilt and re-submitted to the real engine.',
            meta: { seed: source.seed, steps: source.log.length }
        });

        const t0 = performance.now();
        const result = runReplay({
            seed: source.seed,
            log: source.log,
            invoke: (request, meta) => env.adapter.invoke(request, meta),
            gameId: 'detective'
        });
        const elapsed = performance.now() - t0;

        /* Compare replayed decision envelopes to the originals in order. */
        let identical = 0;
        let compared = 0;
        const originals = source.decisions || [];
        for (let i = 0; i < result.reports.length; i++) {
            const rep = result.reports[i];
            const orig = originals[i];
            if (!orig) continue;
            compared += 1;
            if (rep.ok && orig.ok && rep.raw === orig.raw) identical += 1;
        }

        const endingSame = result.ok && source.ending
            ? endingKind(result.state) === source.ending
            : null; /* n/a when replaying an unfinished run */

        replayReport = {
            ok: result.ok,
            reason: result.reason || null,
            steps: source.log.length,
            decisions: result.reports.length,
            identical,
            compared,
            endingSame,
            elapsedMs: elapsed
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
        if (replayReport) showReplayReport();
    }

    function endingKind(s) { return s && s.ending ? s.ending.kind : null; }

    function showReplayReport() {
        const r = replayReport;
        const host = env.viewport.querySelector('.dt-replay-report-host') ||
            env.viewport.appendChild(el('div', { class: 'dt-replay-report-host' }));
        clear(host);
        host.append(el('div', {
            class: `card-inset dt-replay-report ${r.ok ? 'ok' : 'err'}`, id: 'dt-replay-report'
        },
            el('h3', { class: 'panel-title' }, r.ok ? 'Replay report' : 'Replay failed'),
            el('dl', { class: 'kv two-col' },
                el('dt', {}, 'Steps re-executed'), el('dd', {}, String(r.steps)),
                el('dt', {}, 'Decisions re-invoked'), el('dd', {}, String(r.decisions)),
                el('dt', {}, 'Byte-identical envelopes'), el('dd', {}, `${r.identical} / ${r.compared}`),
                el('dt', {}, 'Ending matches original'), el('dd', {}, r.endingSame === null ? 'n/a (run unfinished)' : (r.endingSame ? 'yes' : 'no')),
                el('dt', {}, 'Replay wall time'), el('dd', {}, fmtMs(r.elapsedMs)),
                el('dt', {}, 'Failures'), el('dd', {}, r.reason || 'none')),
            el('p', { class: 'muted small' },
                'Replay re-submits rebuilt requests to the real WASM kernel — it is a Decideverse simulation ' +
                'capability, not a kernel feature. Determinism comes from the pure scenario reducer plus ' +
                'the kernel\'s byte-identical envelopes.')));
        host.scrollIntoView({ behavior: 'smooth', block: 'nearest' });
    }

    return instance;
}
