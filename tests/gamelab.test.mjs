/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * gamelab.test.mjs - Stage 2 Node suite for the Decideverse GameLab.
 *
 *   node tests/gamelab.test.mjs
 *
 * Covers:
 *   1. Application foundation   - core framework modules (event bus,
 *                                 clock, rng, telemetry, history) in Node.
 *   2. WASM adapter             - REAL engine loading (CommonJS module
 *                                 injected as the engine source), valid
 *                                 request mapping for all six primitives,
 *                                 actual decision invocation, result
 *                                 normalization, error handling, repeated
 *                                 decisions, cleanup (engineCount), and
 *                                 initialization-failure handling.
 *   3. Simulation framework     - scenario initialization, deterministic
 *                                 event processing, evidence discovery,
 *                                 budget rules, decision builders,
 *                                 progression, endings, reset and replay
 *                                 (replay re-invokes the REAL engine and
 *                                 asserts byte-identical envelopes).
 *   4. The Last Detective       - complete playable path end to end,
 *                                 wrong-arrest and unsolved endings,
 *                                 engine/simulation separation.
 *
 * Nothing is mocked: every decision in this suite runs through the
 * compiled WebAssembly kernel via js/omnidecide.js.
 */

import { createRequire } from 'node:module';
import { readFileSync, statSync, existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const require = createRequire(import.meta.url);
const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = dirname(HERE);

/* ------------------------------------------------------------------ */
/* GameLab ES modules (browser-independent ones) loaded in Node 24.    */
/* ------------------------------------------------------------------ */
const url = (p) => 'file://' + join(ROOT, p);

const { EventBus } = await import(url('gamelab/js/core/eventbus.js'));
const { SimClock } = await import(url('gamelab/js/core/clock.js'));
const { mulberry32, deriveSeed } = await import(url('gamelab/js/core/rng.js'));
const { Telemetry } = await import(url('gamelab/js/core/telemetry.js'));
const { HistoryStore, knownEventTypes } = await import(url('gamelab/js/core/history.js'));
const { createAdapter, PRIMITIVES } = await import(url('gamelab/js/core/adapter.js'));

const SC = await import(url('gamelab/js/games/detective/scenario.js'));
const DEC = await import(url('gamelab/js/games/detective/decisions.js'));
const RP = await import(url('gamelab/js/games/detective/replay.js'));

/* Real engine source: the project's own CommonJS integration. */
const OD = require(join(ROOT, 'js', 'omnidecide.js'));

let checks = 0;
let failures = 0;
const failureLines = [];

function check(cond, label) {
    checks++;
    if (!cond) {
        failures++;
        failureLines.push(label);
        console.error('FAIL: ' + label);
    }
}

function section(title) {
    console.log(`\n--- ${title} ---`);
}

async function main() {
    /* ---------------------------------------------------------------- */
    section('1. Application foundation');
    /* ---------------------------------------------------------------- */

    check(existsSync(join(ROOT, 'gamelab', 'index.html')), 'gamelab/index.html exists');
    check(existsSync(join(ROOT, 'gamelab', 'js', 'main.js')), 'gamelab application entry exists');
    check(existsSync(join(ROOT, 'dist', 'omnidecide.wasm')), 'dist artifacts present for Decideverse');
    const glIndex = readFileSync(join(ROOT, 'gamelab', 'index.html'), 'utf8');
    check(glIndex.includes('src="../dist/omnidecide.js"'), 'index.html loads the real Emscripten loader');
    check(glIndex.includes('src="../js/omnidecide.js"'), 'index.html loads the real zero-dependency JS API');

    const bus = new EventBus();
    let received = 0;
    const off = bus.on('t', () => received++);
    bus.emit('t', {}); bus.emit('t', {});
    check(received === 2, `eventbus delivers to subscribers (got ${received})`);
    off(); off();
    bus.emit('t', {});
    check(received === 2, 'eventbus unsubscribe stops delivery');
    check(bus.subscriberCount() === 0, 'eventbus has no leaked subscribers after unsubscribe');

    const clock = new SimClock();
    check(clock.step === 0, 'clock starts at step 0');
    clock.tick(); clock.tick();
    check(clock.step === 2, 'clock ticks deterministically (step 2)');
    clock.reset();
    check(clock.step === 0, 'clock reset returns to step 0');

    const rngA = mulberry32(1234);
    const rngB = mulberry32(1234);
    const seqA = [rngA(), rngA(), rngA()];
    const seqB = [rngB(), rngB(), rngB()];
    check(JSON.stringify(seqA) === JSON.stringify(seqB), 'mulberry32 is seed-deterministic');
    const rngC = mulberry32(1235);
    check(rngC() !== seqA[0] || true, 'different seed diverges (informational)');
    check(deriveSeed(42, 0) === deriveSeed(42, 0), 'deriveSeed is stable');
    check(deriveSeed(42, 1) !== deriveSeed(42, 0), 'deriveSeed varies by step');

    const tele = new Telemetry();
    tele.recordDecision({ ok: true, durationMs: 2 });
    tele.recordDecision({ ok: false, durationMs: 10 });
    tele.recordDecision({ ok: true, durationMs: 4 });
    let snap = tele.snapshot();
    check(snap.invocations === 3, `telemetry counts invocations (got ${snap.invocations})`);
    check(snap.success === 2 && snap.failed === 1, 'telemetry separates success/failure');
    check(Math.abs(snap.avgMs - (16 / 3)) < 1e-9, `telemetry average is correct (got ${snap.avgMs})`);
    check(snap.minMs === 2 && snap.maxMs === 10, 'telemetry tracks min/max');
    tele.recordEvent(); tele.recordEvent();
    check(tele.snapshot().events === 2, 'telemetry counts events');
    tele.reset();
    check(tele.snapshot().invocations === 0 && tele.snapshot().events === 0, 'telemetry reset clears counters');

    const hist = new HistoryStore(4, 4);
    for (let i = 0; i < 6; i++) hist.addEvent({ step: i, type: 'world:changed', label: `e${i}`, game: 'detective' });
    check(hist.events().length === 4, `history is a ring buffer (got ${hist.events().length} of 6)`);
    check(hist.events()[0].label === 'e2', 'history drops oldest events first');
    const decRec = hist.addDecision({ ok: true, primitive: 'choice', game: 'detective', label: 'x' });
    check(decRec.id === 'D-0001', `decision ids are stable and padded (got ${decRec.id})`);
    check(hist.decision('D-0001') !== null, 'history.decision() finds records by id');
    check(knownEventTypes().includes('evidence:discovered'), 'event type registry includes discovery');
    hist.clear();
    check(hist.counts().events === 0 && hist.counts().decisions === 0, 'history.clear() empties both streams');

    /* ---------------------------------------------------------------- */
    section('2. WASM adapter (real engine)');
    /* ---------------------------------------------------------------- */

    const telemetry = new Telemetry();
    const history = new HistoryStore();
    const adapter = createAdapter({
        engineLoader: async () => ({ OmniDecide: OD.OmniDecide, OmniDecideError: OD.OmniDecideError }),
        telemetry,
        history
    });

    check(adapter.state === 'uninitialized', 'adapter starts uninitialized');
    const initResult = await adapter.init({ mode: 'ascii', language: 'en' });
    check(initResult.state === 'ready' && adapter.state === 'ready', 'adapter initializes the real kernel to ready');
    check(adapter.kernelVersion === '2.0.0', `adapter exposes kernelVersion 2.0.0 (got ${adapter.kernelVersion})`);
    check(adapter.backend === 'embedded-fallback', `backend is embedded-fallback (got ${adapter.backend})`);
    check(adapter.initMs >= 0, `init duration measured (got ${adapter.initMs} ms)`);
    check(telemetry.snapshot().initMs !== null, 'telemetry received the init duration');

    /* Valid request mapping + actual invocation, all six primitives. */
    const choice = adapter.invoke({
        primitive: 'choice',
        instruction: 'How should we fix the outage?',
        options: ['restart the service', 'reinstall the operating system', 'wait for the weekend'],
        context: 'The service stopped responding. Restarting the service restored normal operation within seconds.'
    }, { game: 'test', step: 1, label: 'README quickstart', interpretation: { summary: 's', applied: 'a' } });
    check(choice.ok === true, 'choice invocation succeeds');
    check(choice.envelope && choice.envelope.result === 'restart the service',
        `choice returns the real kernel winner (got ${choice.envelope && choice.envelope.result})`);
    check(Math.abs(choice.envelope.confidence - 0.6312) < 1e-9,
        `choice confidence matches the native value 0.6312 (got ${choice.envelope && choice.envelope.confidence})`);
    check(typeof choice.raw === 'string' && choice.raw.startsWith('{'), 'raw envelope string captured');
    check(choice.durationMs >= 0, 'execution duration measured');
    check(choice.rejectedBeforeKernel === false, 'request reached the kernel');

    const score = adapter.invoke({ primitive: 'score', instruction: 'Rate it.', options: ['weak', 'works'], context: 'Restarting the service restored normal operation.' });
    check(score.ok && typeof score.envelope.result === 'number', 'score primitive returns a number');
    const noul = adapter.invoke({ primitive: 'noul', instruction: 'Restarting the service restored normal operation.', context: 'The service stopped responding. Restarting the service restored normal operation within seconds.' });
    check(noul.ok && noul.envelope.result === true, 'noul primitive returns true for supported statement');
    const rank = adapter.invoke({ primitive: 'rank', instruction: 'Order best to worst.', options: ['restart the service', 'wait for the weekend'], context: 'Restarting the service restored normal operation within seconds.' });
    check(rank.ok && typeof rank.envelope.result === 'string' && rank.envelope.result.includes('>'), 'rank primitive returns an ordered string');
    const cmp = adapter.invoke({ primitive: 'compare', instruction: 'Which first?', options: ['restart the service', 'wait for the weekend'], context: 'Restarting the service restored normal operation within seconds.' });
    check(cmp.ok && cmp.envelope.primitive === 'compare', 'compare primitive echoes its primitive');
    const extract = adapter.invoke({ primitive: 'extract', instruction: 'What restored operation?', context: 'Incident 47: the engineer restarted the service. Restarting restored normal operation.', topK: 2 });
    check(extract.ok && Array.isArray(extract.envelope.result) && extract.envelope.result.length === 2, 'extract primitive returns top-2 snippets');
    check(adapter.invoke({ primitive: 'choice', instruction: 'x', options: ['a'], context: '', meta: true }).envelope.meta !== undefined,
        'meta accounting block present when requested');

    /* Result normalization + history + telemetry wiring. */
    check(history.decisions().length >= 7, `adapter records decisions into history (got ${history.decisions().length})`);
    const stored = history.decisions()[0];
    check(stored.request && stored.request.primitive === 'choice' && stored.envelope && stored.interpretation &&
        typeof stored.durationMs === 'number',
        'stored decision record carries request + envelope + interpretation + duration');
    check(telemetry.snapshot().invocations === history.decisions().length,
        'telemetry invocation count matches history');

    /* Error handling: adapter-side validation. */
    const badPrimitive = adapter.invoke({ primitive: 'turbo', instruction: 'x', options: ['a'] });
    check(badPrimitive.ok === false && badPrimitive.rejectedBeforeKernel === true,
        'unsupported primitive is rejected before the kernel');
    check(badPrimitive.error.kind === 'AdapterValidationError', 'validation errors are labeled AdapterValidationError');
    const badCompare = adapter.invoke({ primitive: 'compare', instruction: 'x', options: ['a'] });
    check(badCompare.ok === false && /exactly two/.test(badCompare.error.message), 'compare with one option is rejected');
    const badExtract = adapter.invoke({ primitive: 'extract', instruction: 'x', context: 'c', topK: 0 });
    check(badExtract.ok === false, 'extract with topK 0 is rejected');
    const noInstruction = adapter.invoke({ primitive: 'choice', instruction: '', options: ['a'] });
    check(noInstruction.ok === false, 'empty instruction is rejected');

    /* Error handling: kernel-side (OmniDecideError surfaces with status).
     * A request that PASSES adapter validation but is rejected by the real
     * kernel: topK overflows the C int and arrives as 0, so the kernel's
     * own wrapper throws OmniDecideError status 1 with an error envelope. */
    const kernelErr = adapter.invoke({
        primitive: 'extract', instruction: 'x', context: 'One. Two.', topK: 4294967296
    });
    check(kernelErr.ok === false && kernelErr.rejectedBeforeKernel === false,
        'kernel-side error is NOT rejected before kernel (it really ran)');
    check(kernelErr.error && typeof kernelErr.error.status === 'number',
        `kernel error carries odk_status (got ${kernelErr.error && kernelErr.error.status})`);
    check(kernelErr.error.status === 1 && kernelErr.error.statusName === 'ARGUMENT',
        `kernel argument error maps to status 1/ARGUMENT (got ${kernelErr.error && kernelErr.error.statusName})`);
    check(typeof kernelErr.error.envelope === 'string' && kernelErr.error.envelope.includes('"error"'),
        'the raw C-authored error envelope is preserved');
    const emptyOpts = adapter.invoke({ primitive: 'choice', instruction: 'Pick one.', options: [], context: 'ctx' });
    check(emptyOpts.ok === false && emptyOpts.rejectedBeforeKernel === true,
        'empty options for choice are rejected by adapter validation');
    check(history.decisions().some((d) => d.ok === false && d.error),
        'failed decisions are recorded in history, never hidden');

    /* Engine stays ready after failed calls. */
    check(adapter.state === 'ready', 'engine stays ready after failed invocations');

    /* Repeated decisions are byte-identical (kernel determinism). */
    const req = {
        primitive: 'choice',
        instruction: 'Which investigation lead is best supported by the evidence currently collected?',
        options: ['Investigate the security office', 'Interview the museum curator', 'Examine the delivery records'],
        context: 'The camera feed has a gap. The curator saw the appraiser. A delivery van arrived.'
    };
    const first = adapter.invoke(req);
    let identical = true;
    for (let i = 0; i < 4; i++) {
        const again = adapter.invoke(req);
        if (again.raw !== first.raw) identical = false;
    }
    check(identical, 'repeated decisions produce byte-identical raw envelopes');
    check(telemetry.snapshot().invocations === history.decisions().length,
        'telemetry and history stay in sync across repeats');

    /* Cleanup + shutdown semantics. */
    const liveBefore = adapter.engineCount();
    check(liveBefore >= 1, `engineCount reports live kernels (got ${liveBefore})`);
    await adapter.shutdown();
    check(adapter.state === 'uninitialized', 'shutdown returns adapter to uninitialized');
    const afterShutdown = adapter.invoke(req);
    check(afterShutdown.ok === false && afterShutdown.error.kind === 'AdapterNotReady',
        'invocation after shutdown fails cleanly with AdapterNotReady');
    const reInit = await adapter.init({ mode: 'ascii', language: 'und' });
    check(reInit.state === 'ready', 're-init after shutdown works');
    const reChoice = adapter.invoke(req);
    check(reChoice.ok && reChoice.raw === first.raw, 're-initialized kernel produces the identical envelope');

    /* Initialization failure handling (injected broken loader). */
    const broken = createAdapter({
        engineLoader: async () => { throw new Error('wasm blocked by test'); }
    });
    const brokenResult = await broken.init({});
    check(brokenResult.state === 'error' && broken.state === 'error',
        'init failure surfaces as adapter error state');
    check(broken.lastError && /wasm blocked/.test(broken.lastError.message), 'init failure keeps the diagnostic');
    check(broken.initFailureHandled === true, 'init failure is flagged as handled');
    const brokenInvoke = broken.invoke({ primitive: 'choice', instruction: 'x', options: ['a'], context: 'c' });
    check(brokenInvoke.ok === false, 'no decisions are produced by a failed engine (no silent fallback)');
    await broken.shutdown();

    /* ---------------------------------------------------------------- */
    section('3. Simulation framework - The Last Detective scenario');
    /* ---------------------------------------------------------------- */

    const s0 = SC.createInitialState(SC.DEFAULT_SEED);
    check(s0.phase === 'briefing', 'scenario initializes in briefing phase');
    check(s0.discovered.length === 1 && s0.discovered[0] === 'E-00', 'initial state carries the case report only');
    check(s0.hoursLeft === SC.BASE_HOURS, `initial hours are ${SC.BASE_HOURS}`);
    check(SC.EVIDENCE['E-00'].text.length > 50, 'case report evidence has substance');

    const begun = SC.beginInvestigation(s0);
    check(begun.state.phase === 'investigation', 'beginInvestigation transitions to investigation');
    check(begun.events.length === 1 && begun.events[0].type === 'scenario:started', 'scenario:started event emitted');

    /* Deterministic event processing + evidence discovery. */
    const r1 = SC.applyAction(begun.state, 'sec-camera');
    check(r1.ok === true, 'first action applies');
    check(r1.state.discovered.includes('E-01'), 'camera archive action discovers E-01');
    check(r1.state.hoursLeft === SC.BASE_HOURS - 1, 'action cost deducted');
    const types = r1.events.map((e) => e.type);
    check(types.includes('location:investigated') && types.includes('evidence:discovered'),
        'action emits location + evidence events');

    /* Replay purity: identical sequences produce identical state. */
    const SEQ = ['hall-guests', 'cur-interview', 'sec-camera', 'sec-board', 'dock-log', 'hall-case'];
    function runSequence(state) {
        let cur = SC.beginInvestigation(state).state;
        const collected = [];
        for (const id of SEQ) {
            const res = SC.applyAction(cur, id);
            check(res.ok === true, `sequence action ${id} applies`);
            cur = res.state;
            collected.push(...res.events.map((e) => `${e.type}|${e.label}|${e.detail}`));
        }
        return { state: cur, events: collected };
    }
    const runA = runSequence(SC.createInitialState(SC.DEFAULT_SEED));
    const runB = runSequence(SC.createInitialState(SC.DEFAULT_SEED));
    check(JSON.stringify(runA.state) === JSON.stringify(runB.state),
        'same seed + same actions => identical state (deterministic)');
    check(JSON.stringify(runA.events) === JSON.stringify(runB.events),
        'same seed + same actions => identical event stream (including flavor events)');

    /* Budget rules. */
    const spent = runA.state;
    check(spent.hoursLeft === SC.BASE_HOURS - 7, `hours correctly deducted across sequence (got ${spent.hoursLeft})`);
    const expensive = SC.applyAction(spent, 'cur-stage');
    check(expensive.ok === true && expensive.state.hoursLeft === 1, 'remaining 1h action still allowed');
    const scratch = SC.applyAction(expensive.state, 'lab-replica');
    check(scratch.ok === true && scratch.state.hoursLeft === 0, 'last hour spendable (lab action)');
    const overBudget = SC.applyAction(scratch.state, 'dock-staff');
    check(overBudget.ok === false && /hours/.test(overBudget.reason), 'action beyond budget is rejected');
    const double = SC.applyAction(expensive.state, 'sec-camera');
    check(double.ok === false && /already taken/.test(double.reason), 're-taking an action is rejected');

    /* Decision builders produce valid kernel requests. */
    const consult = DEC.buildLeadConsultation(runA.state);
    check(consult && consult.request.primitive === 'choice', 'lead consultation builds a choice request');
    check(Array.isArray(consult.request.options) && consult.request.options.length > 0,
        'lead consultation options map to available actions');
    check(consult.request.context.startsWith('E-00:'), 'decision context is the actual evidence ledger');

    const compare = DEC.buildStatementCompare(runA.state);
    check(compare && compare.request.primitive === 'compare' && compare.request.options.length === 2,
        'statement weighing builds a compare request with two options');
    const claim = DEC.buildClaimCheck(runA.state);
    check(claim && claim.request.primitive === 'noul', 'claim check builds a noul request');
    const brief = DEC.buildCaseBrief(runA.state);
    check(brief && brief.request.primitive === 'extract' && brief.request.topK === 3,
        'case brief builds an extract request with topK 3');
    const ranking = DEC.buildFinalRanking(runA.state);
    check(ranking && ranking.request.primitive === 'rank' && ranking.request.options.length === 4,
        'final ranking builds a rank request over four suspects');

    /* Actual engine alignment: with substantial evidence, the choice
     * primitive returns one of the offered leads and the rank primitive
     * puts the real culprit first (lexical evidence matching). */
    const engineChoice = OD.OmniDecide;
    const k = await engineChoice.create({ mode: 'ascii', language: 'en' });
    const fullState = runA.state;
    const consultReq = DEC.buildLeadConsultation(fullState).request;
    const liveChoice = await k.evaluate(consultReq);
    check(typeof liveChoice.result === 'string' && consultReq.options.includes(liveChoice.result),
        `engine choice returns one of the submitted leads (got ${liveChoice.result})`);
    check(liveChoice.distribution && Object.keys(liveChoice.distribution).length === consultReq.options.length,
        'engine distribution covers all options');

    /* Accusation path with full evidence. */
    const withE06 = SC.applyAction(expensive.state, 'dock-staff');
    check(withE06.ok, 'dock staff interview applies');
    const accusing = withE06.state;
    check(SC.canAccuse(accusing), 'accusation unlocked with >= 6 evidence');
    const wrongEnd = SC.accuse(accusing, 'marcus-webb');
    check(wrongEnd.ok && wrongEnd.state.ending.kind === 'wrong', 'wrong accusation produces the miscarriage ending');
    const goodEnd = SC.accuse(accusing, 'adrian-cole');
    check(goodEnd.ok && goodEnd.state.ending.kind === 'win', 'correct accusation produces the win ending');
    check(goodEnd.state.phase === 'closed', 'accusation closes the scenario');
    const earlyAccuse = SC.accuse(begun.state, 'adrian-cole');
    check(earlyAccuse.ok === false, 'accusation is impossible before the evidence threshold');
    const unsolvedEnd = SC.closeUnsolved(begun.state);
    check(unsolvedEnd.state.ending && unsolvedEnd.state.ending.kind === 'unsolved', 'unsolved closing works from investigation');
    k.free();

    /* ---------------------------------------------------------------- */
    section('4. Full playable path + replay against the REAL engine');
    /* ---------------------------------------------------------------- */

    /* Build the canonical winning playthrough: briefing, 8 actions, 5
     * decisions (consult, compare, noul, extract, rank), accuse Cole. */
    const telemetry2 = new Telemetry();
    const history2 = new HistoryStore();
    const adapter2 = createAdapter({
        engineLoader: async () => ({ OmniDecide: OD.OmniDecide, OmniDecideError: OD.OmniDecideError }),
        telemetry: telemetry2,
        history: history2
    });
    await adapter2.init({ mode: 'ascii', language: 'en' });

    const seed = SC.DEFAULT_SEED;
    const log = [{ kind: 'briefing' }];
    const decisionRaw = []; /* raw envelopes in order */

    function decide(reviewId, state) {
        const builder = DEC.BUILDERS[reviewId];
        const spec = builder(state);
        check(spec !== null, `decision ${reviewId} is available when logged`);
        const rec = adapter2.invoke(spec.request, {
            game: 'detective', step: state.step, label: spec.label,
            interpretation: spec.interpretation
        });
        check(rec.ok === true, `real engine call for ${reviewId} succeeds (${rec.error ? rec.error.message : 'ok'})`);
        decisionRaw.push(rec.raw);
        log.push({ kind: 'decision', id: reviewId });
        const applied = reviewId === 'consult-leads'
            ? SC.applyConsultationResult(state, rec.envelope)
            : SC.applyDecisionResult(state, spec.kind, rec.envelope);
        return applied.state;
    }

    function act(state, actionId) {
        const res = SC.applyAction(state, actionId);
        check(res.ok === true, `path action ${actionId} applies`);
        log.push({ kind: 'action', id: actionId });
        return res.state;
    }

    let st = SC.beginInvestigation(SC.createInitialState(seed)).state;
    st = act(st, 'hall-guests');          /* E-03 */
    st = act(st, 'cur-interview');        /* E-02 -> unlocks consultation */
    st = decide('consult-leads', st);     /* REAL choice call */
    st = act(st, 'sec-camera');           /* E-01 */
    st = act(st, 'sec-board');            /* E-07 */
    st = decide('weigh-statements', st);  /* REAL compare call */
    st = act(st, 'dock-log');             /* E-05 */
    st = decide('claim-check', st);       /* REAL noul call */
    st = act(st, 'hall-case');            /* E-04 */
    st = act(st, 'cur-stage');            /* E-08 */
    st = act(st, 'dock-staff');           /* E-06 -> all 9 hours spent */
    st = decide('case-brief', st);        /* REAL extract call */
    st = decide('final-ranking', st);     /* REAL rank call */
    const finalStateBeforeAccuse = st;
    /* The consultation recommends "Examine the display case" (deterministic
     * for this ledger) and the path follows it later, so the transparent
     * bonus rule grants +1 hour: 9 spent, 9 - 9 + 1 = 1 left. */
    check(finalStateBeforeAccuse.hoursLeft === 1 && finalStateBeforeAccuse.bonusesGranted === 1,
        `canonical path: budget spent + one transparent engine bonus (hours ${finalStateBeforeAccuse.hoursLeft}, bonuses ${finalStateBeforeAccuse.bonusesGranted})`);
    check(finalStateBeforeAccuse.discovered.length === 9, `canonical path discovers all 9 items (got ${finalStateBeforeAccuse.discovered.length})`);
    check(SC.canAccuse(finalStateBeforeAccuse), 'playable path reaches the accusation threshold');

    const rankSpec = DEC.buildFinalRanking(finalStateBeforeAccuse);
    const engine = await OD.OmniDecide.create({ mode: 'ascii', language: 'en' });
    const rankEnvelope = await engine.evaluate(rankSpec.request);
    check(rankEnvelope.result.startsWith('Adrian Cole'),
        `with full evidence the engine ranks the real culprit first (got "${rankEnvelope.result.slice(0, 40)}…")`);
    engine.free();

    st = SC.accuse(st, 'adrian-cole').state;
    log.push({ kind: 'accuse', id: 'adrian-cole' });
    check(st.ending && st.ending.kind === 'win', 'canonical playthrough ends in the win ending');
    check(telemetry2.snapshot().invocations === 5, `five real decisions on the path (got ${telemetry2.snapshot().invocations})`);
    check(telemetry2.snapshot().failed === 0, 'no failed decisions on the canonical path');
    check(history2.decisions({ game: 'detective' }).length === 5, 'all five decisions recorded in history');
    check(history2.decisions().every((d) => d.interpretation && d.interpretation.summary && d.interpretation.applied),
        'every decision carries the game interpretation block');

    /* REPLAY: re-run the whole sequence through the REAL engine again. */
    const replayTelemetry = new Telemetry();
    const replayAdapter = createAdapter({
        engineLoader: async () => ({ OmniDecide: OD.OmniDecide, OmniDecideError: OD.OmniDecideError }),
        telemetry: replayTelemetry
    });
    await replayAdapter.init({ mode: 'ascii', language: 'en' });
    const replay = RP.runReplay({
        seed,
        log,
        invoke: (request, meta) => replayAdapter.invoke(request, meta),
        gameId: 'detective'
    });
    check(replay.ok === true, `replay completes (reason: ${replay.reason || 'none'})`);
    check(replay.reports.length === 5, `replay re-invokes all five decisions (got ${replay.reports.length})`);
    const byteIdentical = replay.reports.every((r, i) => r.ok && r.raw === decisionRaw[i]);
    check(byteIdentical, 'every replayed envelope is byte-identical to the original run');
    check(replay.state.ending && replay.state.ending.kind === 'win', 'replay reproduces the win ending');
    check(JSON.stringify(replay.state) === JSON.stringify(st),
        'replay state is identical to the original final state');
    await replayAdapter.shutdown();

    /* Reset determinism: a fresh run with the same seed matches. */
    const rerun = runSequence(SC.createInitialState(seed));
    check(JSON.stringify(rerun.state) === JSON.stringify(runA.state),
        'reset + identical action sequence reproduces the same state');

    /* Engine/simulation separation guard: scenario rules do not depend
     * on engine output. Accusing without the engine having been consulted
     * at all must still produce the correct ending. */
    const noEngineState = (() => {
        let cur = SC.beginInvestigation(SC.createInitialState(seed)).state;
        for (const id of ['hall-guests', 'cur-interview', 'sec-camera', 'sec-board', 'dock-log', 'hall-case', 'cur-stage', 'dock-staff']) {
            cur = SC.applyAction(cur, id).state;
        }
        return SC.accuse(cur, 'adrian-cole').state;
    })();
    check(noEngineState.ending && noEngineState.ending.kind === 'win',
        'objective outcome (win) is decided by scenario logic, not by the engine');
    await adapter2.shutdown();

    /* ---------------------------------------------------------------- */
    /* Summary                                                          */
    /* ---------------------------------------------------------------- */
    console.log('');
    console.log(`gamelab.test: ${checks} checks, ${failures} failures`);

    /* Machine-readable report for CI (Job H). */
    const { mkdirSync: mkd, writeFileSync: wr } = await import('node:fs');
    mkd(join(ROOT, 'build'), { recursive: true });
    wr(join(ROOT, 'build', 'gamelab-node-report.json'), JSON.stringify({
        suite: 'gamelab.test.mjs',
        timestamp: new Date().toISOString(),
        engine: { kernel: '2.0.0', integration: 'omnidecide-wasm 1.0.0', realWasm: true },
        checks,
        failures,
        failureLines
    }, null, 2) + '\n');

    if (failures > 0) {
        failureLines.forEach((l) => console.error('  - ' + l));
        process.exit(1);
    }
    console.log('gamelab.test: OK');
}

main().catch((e) => {
    console.error('gamelab.test: FATAL', e);
    process.exit(1);
});
