/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * gamelab_state.test.mjs - Stage 2.1 Workstreams E + F.
 *
 *   node tests/gamelab_state.test.mjs
 *
 * Action-tracking (E) and seed/replay (F) regression coverage, driven
 * through the REAL The Last Detective game module (the actual click
 * handlers, not a logging helper): the module is mounted into a minimal
 * DOM shim and bound to a REAL SimSession whose decisions flow through
 * the SAME shared adapter singleton the browser app uses - initialized
 * here against the REAL WASM engine via the browser-default loader
 * (globalThis.OmniDecide). Every "player action" below is a genuine UI
 * button press routed through module.js -> scenario.js / session.js /
 * adapter.js.
 *
 * Documented semantics under test:
 *   - session.actionLog records SUCCESSFUL player actions exactly once
 *     (begin, investigate, decide, accuse, close-unsolved).
 *   - Rejected commands and failed engine invocations never count.
 *   - The workspace "Actions logged" counter renders
 *     session.actionLog.length - they cannot disagree.
 *   - Reset starts a new run: the shared action log clears, the
 *     effective seed is retained (session + scenario agree).
 *   - Replay never appends to the action log; it re-executes the
 *     recorded runLog with the recorded effective seed and re-submits
 *     rebuilt requests to the REAL engine (byte-identical envelopes).
 *
 * Machine-readable summary: build/gamelab-state-report.json.
 */

import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { mkdirSync, writeFileSync } from 'node:fs';

const require = createRequire(import.meta.url);
const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = dirname(HERE);

const OD = require(join(ROOT, 'js', 'omnidecide.js'));
const { createAdapter } = await import('file://' + join(ROOT, 'gamelab', 'js', 'core', 'adapter.js'));
const { adapter: sharedAdapter } = await import('file://' + join(ROOT, 'gamelab', 'js', 'core', 'adapter.js'));
const { SimSession } = await import('file://' + join(ROOT, 'gamelab', 'js', 'core', 'session.js'));
/* The app wires session.emitEvent/telemetry/decide to the module-level
 * singletons (history-instance / telemetry-instance / adapter); the rig
 * must use the SAME wiring so events, decisions and actions land in one
 * consistent place - exactly like a real page. */
const { history: sharedHistory } = await import('file://' + join(ROOT, 'gamelab', 'js', 'core', 'history-instance.js'));
const { telemetry: sharedTelemetry } = await import('file://' + join(ROOT, 'gamelab', 'js', 'core', 'telemetry-instance.js'));
const SC = await import('file://' + join(ROOT, 'gamelab', 'js', 'games', 'detective', 'scenario.js'));
const RP = await import('file://' + join(ROOT, 'gamelab', 'js', 'games', 'detective', 'replay.js'));

/* ------------------------------------------------------------------ */
/* Minimal DOM shim: just enough for the REAL game module to render.   */
/* ------------------------------------------------------------------ */

const allElements = [];
const byId = new Map();

class FakeElement {
    constructor(tag) {
        this.nodeType = 1;
        this.tagName = String(tag).toUpperCase();
        this.className = '';
        this.attrs = {};
        this.children = [];
        this.listeners = {};
        this.parentNode = null;
        this.value = '';
        allElements.push(this);
    }
    setAttribute(k, v) {
        this.attrs[k] = String(v);
        if (k === 'id') byId.set(String(v), this);
        if (k === 'value') this.value = String(v);
    }
    getAttribute(k) { return this.attrs[k] ?? null; }
    append(...nodes) {
        for (const n of nodes) {
            if (n === null || n === undefined || n === false) continue;
            const node = n.nodeType ? n : document.createTextNode(String(n));
            if (node.parentNode) node.parentNode.removeChild(node);
            node.parentNode = this;
            this.children.push(node);
        }
    }
    appendChild(n) { this.append(n); return n; }
    removeChild(n) {
        const i = this.children.indexOf(n);
        if (i >= 0) { this.children.splice(i, 1); n.parentNode = null; }
        return n;
    }
    get firstChild() { return this.children[0] || null; }
    get textContent() {
        return this.children.map((c) => c.textContent).join('');
    }
    set textContent(v) { this.children = []; }
    addEventListener(type, fn) { (this.listeners[type] ||= []).push(fn); }
    removeEventListener(type, fn) {
        this.listeners[type] = (this.listeners[type] || []).filter((f) => f !== fn);
    }
    /** Faithful click: a disabled control never fires. Returns a promise
     * for all handler results (async handlers are awaited by tests). */
    click() {
        if (this.attrs.disabled !== undefined) return Promise.resolve([]);
        const results = (this.listeners.click || []).map((fn) => fn({ preventDefault() {}, target: this }));
        return Promise.all(results);
    }
    scrollIntoView() {}
    querySelector() { return null; }
    querySelectorAll() { return []; }
    get classList() {
        const self = this;
        return {
            add: (...c) => { self.className = `${self.className} ${c.join(' ')}`.trim(); },
            remove: (...c) => { self.className = self.className.split(/\s+/).filter((x) => !c.includes(x)).join(' '); },
            toggle: (c, force) => { (force === undefined ? !self.className.includes(c) : force) ? self.classList.add(c) : self.classList.remove(c); },
            contains: (c) => self.className.split(/\s+/).includes(c)
        };
    }
}

globalThis.document = {
    createElement: (tag) => new FakeElement(tag),
    createTextNode: (text) => ({ nodeType: 3, textContent: String(text) }),
    getElementById: (id) => byId.get(id) || null
};

/* Find the most recent element carrying attr="value" (latest render). */
function find(attr, value) {
    for (let i = allElements.length - 1; i >= 0; i--) {
        if (allElements[i].attrs[attr] === value) return allElements[i];
    }
    return null;
}

/* ------------------------------------------------------------------ */
/* Harness: real module + real session + the REAL shared adapter       */
/* ------------------------------------------------------------------ */

const { createDetectiveModule } = await import('file://' + join(ROOT, 'gamelab', 'js', 'games', 'detective', 'module.js'));

/* The browser-default engine loader resolves window.OmniDecide; provide
 * the REAL compiled engine through the same globals and initialize the
 * shared adapter singleton once, exactly like the app shell does. */
globalThis.OmniDecide = OD.OmniDecide;
globalThis.OmniDecideError = OD.OmniDecideError;

let checks = 0;
let failures = 0;
const failureLines = [];
function check(cond, label) {
    checks++;
    if (!cond) { failures++; failureLines.push(label); console.error('FAIL: ' + label); }
}
function section(title) { console.log(`\n--- ${title} ---`); }

function makeRig(seed) {
    /* One consistent history/telemetry per rig: clear the shared singletons
     * (the same stores session.emitEvent and the shared adapter use). */
    sharedHistory.clear();
    sharedTelemetry.reset();
    const session = new SimSession({ game: 'detective', seed });
    const module = createDetectiveModule();
    module.start(session);
    const env = {
        game: { id: 'detective' },
        session,
        viewport: new FakeElement('section'),
        controls: new FakeElement('section'),
        panels: {},
        adapter: sharedAdapter,
        history: sharedHistory,
        telemetry: sharedTelemetry,
        emitEvent: (spec) => session.emitEvent(spec),
        decide: (request, info) => session.decide(request, info)
    };
    module.mount(env);
    return { module, session, adapter: sharedAdapter, history: sharedHistory, telemetry: sharedTelemetry };
}

const CULPRIT = 'adrian-cole';

/** The canonical action/decision sequence, clicked through the real UI.
 * Assumes the scenario has begun (investigation phase). */
async function runCanonical() {
    for (const id of ['hall-guests', 'cur-interview']) await find('data-action-id', id).click();
    await find('data-review-id', 'consult-leads').click();
    for (const id of ['sec-camera', 'sec-board']) await find('data-action-id', id).click();
    await find('data-review-id', 'weigh-statements').click();
    await find('data-action-id', 'dock-log').click();
    await find('data-review-id', 'claim-check').click();
    for (const id of ['hall-case', 'cur-stage', 'dock-staff']) await find('data-action-id', id).click();
    await find('data-review-id', 'case-brief').click();
    await find('data-review-id', 'final-ranking').click();
    await find('data-suspect-id', CULPRIT).click();
}

/** Full playthrough from the briefing: set the seed through the REAL
 * briefing input, click Begin, then run the canonical sequence. */
async function playthrough(seed) {
    const seedInput = document.getElementById('dt-seed');
    seedInput.value = String(seed);
    await find('id', 'dt-begin-investigation').click();
    await runCanonical();
}

async function main() {
    await sharedAdapter.init({ mode: 'ascii', language: 'en' });
    check(sharedAdapter.state === 'ready', 'harness. the shared adapter (same singleton the app uses) is ready on the real engine');

    /* ---------------------------------------------------------------- */
    section('E. Shared action tracking through the real game module');
    /* ---------------------------------------------------------------- */

    const rig = makeRig(SC.DEFAULT_SEED);
    const { module, session, history } = rig;

    /* E1: one successful action produces exactly one shared entry. */
    check(session.actionCount === 0, 'E1. the counter starts at zero');
    await find('id', 'dt-begin-investigation').click();
    check(session.actionCount === 1, `E1. begin investigation = 1 action (got ${session.actionCount})`);
    check(session.actionLog[0].action === 'begin', 'E1. the entry is the documented "begin" action');
    await find('data-action-id', 'hall-guests').click();
    check(session.actionCount === 2, `E1. one successful investigation = +1 (got ${session.actionCount})`);
    check(session.actionLog[1].action === 'investigate' && session.actionLog[1].args.id === 'hall-guests',
        'E1. the entry identifies the investigate action');

    /* E2: several successful actions produce the correct count. */
    for (const id of ['cur-interview', 'sec-camera', 'sec-board']) {
        await find('data-action-id', id).click();
    }
    check(session.actionCount === 5, `E2. five successful actions counted (got ${session.actionCount})`);

    /* E3: a rejected action does not increment - reproduced exactly like
     * the browser double-click: the first click on an available button
     * succeeds and re-renders; the second click on the same (now stale,
     * still-enabled) DOM node reaches the real handler and the reducer
     * rejects it ("action already taken"). */
    const dblBtn = find('data-action-id', 'cur-stage'); /* available, not yet taken */
    const beforeReject = session.actionCount;
    await dblBtn.click();                                   /* success: +1 */
    check(session.actionCount === beforeReject + 1, 'E3. the first (valid) click of the double-click succeeds');
    await dblBtn.click();                                   /* stale node: rejected */
    check(session.actionCount === beforeReject + 1,
        `E3. the rejected stale click does not increment (got ${session.actionCount})`);
    check(history.events().some((e) => e.type === 'session:note' && /rejected/i.test(e.label)),
        'E3. the rejection is visible in the event stream instead');

    /* E4: an unsuccessful decision invocation is NOT a player action.
     * The SHARED engine singleton is shut down, so the module's next
     * real submission fails exactly as it would in a broken browser. */
    const decisionsBeforeFailure = history.decisions().length;
    await sharedAdapter.shutdown();
    const reviewBtn = find('data-review-id', 'weigh-statements');
    check(reviewBtn !== null, 'E4. a review button is present');
    await reviewBtn.click();
    const failedRec = history.decisions().slice(decisionsBeforeFailure)[0];
    check(failedRec && failedRec.ok === false && failedRec.error.kind === 'AdapterNotReady',
        'E4. the decision invocation failed against the down engine (real record)');
    check(session.actionCount === beforeReject + 1,
        `E4. the failed decision is NOT counted as a player action (stays ${beforeReject + 1}, got ${session.actionCount})`);
    const dbg = module._debug();
    check(dbg.runLog.some((e) => e.kind === 'decision'),
        'E4. the run log keeps the attempt for faithful replay (documented divergence)');
    await sharedAdapter.init({ mode: 'ascii', language: 'en' });
    check(sharedAdapter.state === 'ready', 'E4. the shared engine is back up');

    /* E5: a successful decision IS tracked with documented semantics. */
    const okReview = find('data-review-id', 'weigh-statements');
    await okReview.click();
    check(session.actionCount === beforeReject + 2,
        `E5. an accepted decision adds exactly one action (got ${session.actionCount})`);
    check(session.actionLog[session.actionCount - 1].action === 'decide',
        'E5. the entry is the documented "decide" action');

    /* E6: no double counting - runLog and actionLog are distinct records
     * whose documented divergence is exactly the failed attempt. */
    const dbg2 = module._debug();
    const failedAttempts = history.decisions({ ok: false }).length;
    check(dbg2.runLog.length === session.actionCount + failedAttempts,
        `E6. run log = successful actions + failed attempts (${dbg2.runLog.length} = ${session.actionCount} + ${failedAttempts})`);
    const okDecisions = history.decisions({ ok: true }).length;
    const decideActions = session.actionLog.filter((a) => a.action === 'decide').length;
    check(okDecisions === decideActions,
        `E6. exactly one shared action per successful engine decision (${okDecisions} decisions, ${decideActions} actions)`);

    /* E7: the workspace counter value equals the authoritative history. */
    const wsCounter = session.actionLog.length; /* what workspace.js renders */
    check(wsCounter === session.actionCount,
        'E7. the rendered counter (actionLog.length) matches the authoritative history');

    /* E8: finish the case; completion action counted; reset semantics.
     * (cur-stage was already taken by the E3 double-click; that click is
     * rejected by the reducer and, per the documented semantics, counts
     * nothing and breaks nothing.) */
    for (const id of ['hall-case', 'cur-stage', 'dock-staff']) await find('data-action-id', id).click();
    await find('data-review-id', 'case-brief').click();
    await find('data-review-id', 'final-ranking').click();
    await find('data-suspect-id', CULPRIT).click();
    const dbgEnd = module._debug();
    check(dbgEnd.state.ending && dbgEnd.state.ending.kind === 'win', 'E8. the playthrough reached the win ending');
    const failedNow = history.decisions({ ok: false }).length;
    check(session.actionCount === dbgEnd.runLog.length - failedNow,
        `E8. run steps = actions + failed attempts at completion (${dbgEnd.runLog.length} = ${session.actionCount} + ${failedNow})`);

    /* E8 reset: new run, counter cleared, seed retained. */
    await find('id', 'dt-reset-ending').click();
    check(session.actionCount === 0, `E8. reset starts a fresh run: counter 0 (got ${session.actionCount})`);
    const dbgReset = module._debug();
    check(dbgReset.state.phase === 'briefing', 'E8. the scenario is back in the briefing phase');
    check(dbgReset.lastRun && dbgReset.lastRun.log.length > 0,
        'E8. the previous run remains replayable (lastRun kept)');
    check(session.seed === SC.DEFAULT_SEED, 'E8. reset retains the documented effective seed');

    /* ---------------------------------------------------------------- */
    section('F. Seed ownership and deterministic replay');
    /* ---------------------------------------------------------------- */

    /* F1: default seed initializes correctly. */
    check(session.seed === SC.DEFAULT_SEED, `F1. the session kept the default seed (got ${session.seed})`);
    check(SC.createInitialState().seed === SC.DEFAULT_SEED, 'F1. scenario default seed initializes correctly');

    /* F2/F3: custom seed replaces default; session matches scenario. */
    const CUSTOM = 31337;
    const rig2 = makeRig(SC.DEFAULT_SEED);
    const { module: m2, session: s2 } = rig2;
    check(s2.seed === SC.DEFAULT_SEED, 'F2. a fresh session starts with the default seed');
    const input2 = document.getElementById('dt-seed');
    input2.value = String(CUSTOM);
    await find('id', 'dt-begin-investigation').click();
    check(s2.seed === CUSTOM, `F2. the selected seed became the authoritative session seed (got ${s2.seed})`);
    check(m2._debug().state.seed === CUSTOM, 'F3. the scenario state carries the same seed');
    check(s2.seed === m2._debug().state.seed, 'F3. session seed matches scenario seed');

    /* F4: the value the workspace displays is the session seed. */
    const displayedSeed = s2.seed; /* workspace.js renders String(session.seed) */
    check(displayedSeed === CUSTOM && m2._debug().state.seed === displayedSeed,
        'F4. the displayed (session) seed equals the effective scenario seed');

    /* Finish rig2's canonical run with the custom seed (while rig2's
     * elements are still the freshest in the shim registry). */
    await runCanonical();
    const dbg2End = m2._debug();
    check(dbg2End.state.ending && dbg2End.state.ending.kind === 'win',
        'F5. the custom-seed playthrough completes with the win ending');
    check(dbg2End.lastRun.seed === CUSTOM, `F6. the recorded run carries the effective seed (got ${dbg2End.lastRun.seed})`);
    check(s2.seed === CUSTOM, 'F6. the session seed equals the recorded run seed');

    /* F6/F7: replay uses the recorded seed and reproduces the outcome. */
    const actionCountBeforeReplay = s2.actionCount;
    const originals = dbg2End.decisionLog.map((d) => d.raw);
    const replay = RP.runReplay({
        seed: dbg2End.lastRun.seed,
        log: dbg2End.lastRun.log,
        invoke: (request, meta) => sharedAdapter.invoke(request, meta),
        gameId: 'detective'
    });
    check(replay.ok === true, `F7. replay completes (reason: ${replay.reason || 'none'})`);
    check(replay.reports.length === 5 && replay.reports.every((r, i) => r.ok && r.raw === originals[i]),
        'F7. every replayed envelope is byte-identical (real kernel re-execution)');
    check(JSON.stringify(replay.state) === JSON.stringify(dbg2End.state),
        'F7. replay reproduces the exact final scenario state');
    check(s2.actionCount === actionCountBeforeReplay,
        `F7. replay never appends to the shared action log (got ${s2.actionCount})`);

    /* F5b: reset after completion retains the seed and stays replayable. */
    await find('id', 'dt-reset-ending').click();
    check(s2.seed === CUSTOM && m2._debug().state.seed === CUSTOM,
        'F5b. reset retains the effective seed (session + scenario agree)');

    /* F9: seed validation + normalization rules (documented), via the
     * REAL briefing input of a fresh rig. */
    const rig3 = makeRig(SC.DEFAULT_SEED);
    const { module: m3, session: s3 } = rig3;
    const input3 = document.getElementById('dt-seed');
    input3.value = 'not-a-number';
    await find('id', 'dt-begin-investigation').click();
    check(s3.seed === SC.DEFAULT_SEED && m3._debug().state.seed === SC.DEFAULT_SEED,
        'F9. non-numeric seed input falls back to the documented default (session + scenario agree)');
    await find('id', 'dt-reset').click();
    document.getElementById('dt-seed').value = '-1';
    await find('id', 'dt-begin-investigation').click();
    check(s3.seed === 4294967295 && m3._debug().state.seed === 4294967295,
        `F9. negative seeds normalize to their unsigned 32-bit value (got ${s3.seed})`);
    await find('id', 'dt-reset').click();
    document.getElementById('dt-seed').value = '4294967296';
    await find('id', 'dt-begin-investigation').click();
    check(s3.seed === 0 && m3._debug().state.seed === 0,
        `F9. out-of-range seeds wrap per the unsigned 32-bit rule (got ${s3.seed})`);

    /* F10: a failed scenario initialization leaves consistent metadata:
     * with the briefing input missing, the documented default applies and
     * session + scenario stay consistent. */
    const el4 = document.getElementById('dt-seed');
    el4.parentNode.removeChild(el4);
    byId.delete('dt-seed');
    await find('id', 'dt-begin-investigation').click();
    check(s3.seed === SC.DEFAULT_SEED && m3._debug().state.seed === SC.DEFAULT_SEED,
        'F10. a missing seed input falls back to the default; session + scenario stay consistent');

    /* F7b: identical seed + identical ordered actions => identical
     * outcomes (full playthrough of a fresh rig, real engine). */
    const rig5 = makeRig(CUSTOM);
    const { module: m5 } = rig5;
    await playthrough(CUSTOM);
    const dbg5 = m5._debug();
    check(JSON.stringify(dbg5.state) === JSON.stringify(dbg2End.state),
        'F7b. the same seed + same action sequence produces the identical final state');
    check(dbg5.decisionLog.every((d, i) => d.raw === dbg2End.decisionLog[i].raw),
        'F7b. the engine envelopes are byte-identical across runs');

    /* F8: different seeds differ ONLY where the scenario defines seeded
     * variation (the cosmetic flavor remarks); evidence, hours and the
     * ending stay identical. */
    const SEQF = ['hall-guests', 'cur-interview', 'sec-camera', 'sec-board', 'dock-log', 'hall-case'];
    function flavorSignature(seed) {
        const flavor = [];
        let st = SC.beginInvestigation(SC.createInitialState(seed)).state;
        for (const id of SEQF) {
            const res = SC.applyAction(st, id);
            for (const ev of res.events) if (ev.type === 'world:changed') flavor.push(ev.detail);
            st = res.state;
        }
        return flavor.join('|');
    }
    function coreSignature(seed) {
        let st = SC.beginInvestigation(SC.createInitialState(seed)).state;
        for (const id of SEQF) st = SC.applyAction(st, id).state;
        return JSON.stringify({ d: st.discovered, h: st.hoursLeft, t: st.takenActions.map((a) => a.id) });
    }
    let seedA = null; let seedB = null;
    for (let s = 1; s < 120 && seedB === null; s++) {
        for (let t = s + 1; t < 121; t++) {
            if (flavorSignature(s) !== flavorSignature(t)) { seedA = s; seedB = t; break; }
        }
    }
    check(seedB !== null, 'F8. two seeds with different defined flavor variation exist');
    if (seedB !== null) {
        check(coreSignature(seedA) === coreSignature(seedB),
            'F8. different seeds do NOT change evidence, budget or taken actions (no artificial randomness)');
        check(flavorSignature(seedA) !== flavorSignature(seedB),
            'F8. different seeds differ exactly in the defined flavor stream');
    }

    /* E9: replay reconstructs the expected action sequence. */
    check(dbg2End.lastRun.log[0].kind === 'briefing' &&
        dbg2End.lastRun.log.filter((e) => e.kind === 'action').length === 8 &&
        dbg2End.lastRun.log.filter((e) => e.kind === 'decision').length === 5 &&
        dbg2End.lastRun.log[dbg2End.lastRun.log.length - 1].kind === 'accuse',
        'E9. the replay log reconstructs briefing + 8 actions + 5 decisions + accusation');

    /* Shut the shared engine down; the suites leave no live handles. */
    await sharedAdapter.shutdown();
    const probe = await OD.OmniDecide.create({ mode: 'ascii', language: 'und' });
    const leftover = probe.engineCount() - 1;
    probe.free();
    check(leftover === 0, `cleanup. no kernel handles remain after the suite (got ${leftover})`);
    check(createAdapter !== undefined, 'harness. adapter factory still exported for other suites');

    /* ---------------------------------------------------------------- */
    console.log('');
    console.log(`gamelab_state.test: ${checks} checks, ${failures} failures`);
    mkdirSync(join(ROOT, 'build'), { recursive: true });
    writeFileSync(join(ROOT, 'build', 'gamelab-state-report.json'), JSON.stringify({
        suite: 'gamelab_state.test.mjs',
        timestamp: new Date().toISOString(),
        engine: { kernel: '2.0.0', integration: 'omnidecide-wasm 1.0.0', realWasm: true, realModule: true },
        checks, failures, failureLines
    }, null, 2) + '\n');
    if (failures > 0) {
        failureLines.forEach((l) => console.error('  - ' + l));
        process.exit(1);
    }
    console.log('gamelab_state.test: OK');
}

main().catch((e) => {
    console.error('gamelab_state.test: FATAL', e);
    process.exit(1);
});
