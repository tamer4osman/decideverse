/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * gamelab_village.test.mjs - Stage 3A Node suite for Living Village.
 *
 *   node tests/gamelab_village.test.mjs
 *
 * Nothing is mocked: every decision in this suite runs through the
 * compiled WebAssembly kernel via js/omnidecide.js (the project's own
 * CommonJS integration), either through the pure replay runner with an
 * injected invoke or, in part 2, through the real adapter singleton.
 *
 * Covers (design doc §9):
 *   A. module shape and builder schema validity (docs/API.md)
 *   B. reducer determinism (byte-identical state + event streams)
 *   C. seed behavior (weather consistent per seed, valid values)
 *   D. steward actions: valid, rejected and budget paths
 *   E. rule R1 tested both ways (goal arbitration, fallback)
 *   F. rule R2 tested both ways (mediation verdicts, tie, closure)
 *   G. rule R3 (digest never mutates village state)
 *   H. output-follows-input (real engine, contrasting evidence)
 *   I. full playthrough to the report ending + ending threshold units
 *   J. replay: byte-identical envelopes, identical ending
 *   K. decision-call bound per day and per run
 *   L. failed decision (injected error): consistent state, honest report
 *
 * Machine-readable report: build/village-node-report.json (Job H/G).
 */

import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { mkdirSync, writeFileSync } from 'node:fs';

const require = createRequire(import.meta.url);
const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = dirname(HERE);

/* Real engine source: the project's own CommonJS integration. */
const ODmod = require(join(ROOT, 'js', 'omnidecide.js'));
const OD = ODmod.OmniDecide;

const url = (p) => 'file://' + join(ROOT, p);
const SC = await import(url('gamelab/js/games/living-village/scenario.js'));
const DEC = await import(url('gamelab/js/games/living-village/decisions.js'));
const RP = await import(url('gamelab/js/games/living-village/replay.js'));

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

/* ------------------------------------------------------------------ */
/* Real-kernel test harness                                            */
/* ------------------------------------------------------------------ */

let kernel = null;
let callCount = 0;

function makeInvoke(options = {}) {
    return (request, info) => {
        callCount += 1;
        if (options.failAfter !== undefined && callCount > options.failAfter) {
            return { ok: false, error: { message: 'injected failure', kind: 'InjectedTestError' }, id: 'injected', durationMs: 0 };
        }
        try {
            const envelope = kernel.evaluate(request);
            return { ok: true, envelope, raw: JSON.stringify(envelope), id: 'n', durationMs: 1 };
        } catch (e) {
            return { ok: false, error: e, id: 'n', durationMs: 1 };
        }
    };
}

const VIDS = SC.VILLAGERS.map((v) => v.id);

/**
 * Interactive driver: plays a full run against the real kernel exactly
 * like the module UI does (steward actions, then decay -> 6 goal calls ->
 * dispute check -> digest -> advance), records the replay log and the
 * event label stream. `policy(state, day)` returns steward entries.
 */
function driveRun(seed, policy, options = {}) {
    const invoke = options.invoke || makeInvoke();
    let state = SC.createInitialState(seed);
    state = SC.beginVillage(state).state;
    const log = [{ kind: 'briefing' }];
    const eventLabels = [];
    const push = (label) => eventLabels.push(label);
    callCount = 0;
    let decisionCount = 0;

    for (let d = 1; d <= SC.DAYS; d++) {
        for (const entry of policy(state, d).slice(0, SC.STEWARD_ACTIONS_PER_DAY)) {
            if (entry.kind !== 'action') { log.push(entry); continue; }
            const res = SC.applyStewardAction(state, entry.id, entry.args || {});
            if (!res.ok) return { fail: `${entry.id}: ${res.reason}` };
            state = res.state;
            for (const e of res.events) push(e.label);
            if (res.mediation) {
                const spec = DEC.buildMediationCompare(state);
                const rec = invoke(spec.request, { label: spec.label });
                if (!rec.ok) return { fail: 'mediate: ' + (rec.error && rec.error.message) };
                decisionCount += 1;
                const applied = SC.applyMediationResult(state, rec.envelope);
                state = applied.state;
                for (const e of applied.events) push(e.label);
            }
            log.push(entry);
        }
        log.push({ kind: 'endday' });
        const decay = SC.applyDayDecay(state);
        state = decay.state;
        for (const e of decay.events) push(e.label);
        for (const v of SC.VILLAGERS) {
            const spec = DEC.buildGoalChoice(state, v.id);
            const rec = invoke(spec.request, { label: spec.label });
            if (!rec.ok) return { fail: `goal ${v.id}: ` + (rec.error && rec.error.message) };
            decisionCount += 1;
            const applied = SC.applyGoalResult(state, v.id, rec.envelope);
            state = applied.state;
            for (const e of applied.events) push(e.label);
        }
        const disp = SC.openDisputeIfAny(state);
        state = disp.state;
        for (const e of disp.events) push(e.label);
        const dspec = DEC.buildChronicleDigest(state);
        const drec = invoke(dspec.request, { label: dspec.label });
        if (!drec.ok) return { fail: 'digest: ' + (drec.error && drec.error.message) };
        decisionCount += 1;
        const dapplied = SC.applyDigestResult(state, drec.envelope);
        state = dapplied.state;
        for (const e of dapplied.events) push(e.label);
        const adv = SC.advanceDay(state);
        state = adv.state;
        for (const e of adv.events) push(e.label);
    }
    return { state, log, eventLabels, invoke, decisionCount };
}

/** Canonical steward policy (the one documented and asserted here):
 * mediate any open dispute, assign a producer daily, host while grain
 * lasts, otherwise share news with the loneliest villager. */
function policySteward(state, d) {
    const acts = [];
    if (state.dispute) acts.push({ kind: 'action', id: 'mediate-dispute' });
    acts.push({ kind: 'action', id: 'assign-task', args: { villagerId: d % 2 ? 'tamsin' : 'juno' } });
    if (state.stores.grain >= 6) acts.push({ kind: 'action', id: 'host-gathering' });
    else {
        const loneliest = state.villagers.reduce((w, v) => (v.needs.belonging < w.needs.belonging ? v : w));
        acts.push({ kind: 'action', id: 'share-news', args: { villagerId: loneliest.id } });
    }
    return acts;
}

function policyNone() {
    return [];
}

const SEED = 20261109;

/* ------------------------------------------------------------------ */
/* Part-2 machinery: minimal DOM shim + real module rig                */
/* (pattern and semantics follow tests/gamelab_state.test.mjs)         */
/* ------------------------------------------------------------------ */

const { adapter: sharedAdapter } = await import(url('gamelab/js/core/adapter.js'));
const { SimSession } = await import(url('gamelab/js/core/session.js'));
const { history: sharedHistory } = await import(url('gamelab/js/core/history-instance.js'));
const { telemetry: sharedTelemetry } = await import(url('gamelab/js/core/telemetry-instance.js'));
const { createLivingVillageModule } = await import(url('gamelab/js/games/living-village/module.js'));

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
    click() {
        if (this.attrs.disabled !== undefined) return Promise.resolve([]);
        const results = (this.listeners.click || []).map((fn) => fn({ preventDefault() {}, target: this }));
        return Promise.all(results);
    }
    scrollIntoView() {}
    querySelector(sel) {
        if (sel.startsWith('.')) {
            return this.children.find((c) => c.className && c.className.split(/\s+/).includes(sel.slice(1))) || null;
        }
        if (sel.startsWith('#')) return this.children.find((c) => c.attrs.id === sel.slice(1)) || null;
        return null;
    }
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
        if (attr === 'class') {
            if (allElements[i].className && allElements[i].className.split(/\s+/).includes(value)) return allElements[i];
        } else if (allElements[i].attrs[attr] === value) return allElements[i];
    }
    return null;
}

function makeRig(seed) {
    sharedHistory.clear();
    sharedTelemetry.reset();
    const session = new SimSession({ game: 'living-village', seed });
    const mod = createLivingVillageModule();
    mod.start(session);
    const env = {
        game: { id: 'living-village' },
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
    mod.mount(env);
    return { module: mod, session, adapter: sharedAdapter, history: sharedHistory, telemetry: sharedTelemetry, env };
}

async function main() {
    kernel = await OD.create({ mode: 'ascii', language: 'und' });

    /* ---------------------------------------------------------------- */
    section('A. Module shape and builder schema validity (docs/API.md)');
    /* ---------------------------------------------------------------- */

    for (const name of ['createInitialState', 'beginVillage', 'applyStewardAction', 'applyDayDecay',
        'applyGoalResult', 'openDisputeIfAny', 'applyMediationResult', 'applyDigestResult',
        'advanceDay', 'villageReport', 'goalContext', 'mediationContext', 'digestContext',
        'buildGoalChoice', 'buildMediationCompare', 'buildChronicleDigest', 'runReplay']) {
        const mod = name === 'runReplay' ? RP : (name.startsWith('build') ? DEC : SC);
        check(typeof mod[name] === 'function', `A. ${name} is exported`);
    }
    check(SC.VILLAGERS.length === 6, 'A. six named villagers (D4)');
    check(SC.DAYS === 7, 'A. seven days (D5)');

    let s = SC.beginVillage(SC.createInitialState(SEED)).state;
    s = SC.applyDayDecay(s).state;
    const goalReq = DEC.buildGoalChoice(s, 'tamsin');
    check(goalReq.request.primitive === 'choice', 'A. goal builder uses the choice primitive');
    check(Array.isArray(goalReq.request.options) && goalReq.request.options.length === 4,
        'A. goal builder submits four options');
    check(typeof goalReq.request.instruction === 'string' && goalReq.request.instruction.length > 0,
        'A. goal builder carries an instruction');
    check(typeof goalReq.request.context === 'string' && goalReq.request.context.length > 0,
        'A. goal builder carries an evidence context');
    check(goalReq.request.topK === undefined && goalReq.request.caps === undefined,
        'A. choice request carries no extract-only fields');
    check(goalReq.interpretation && typeof goalReq.interpretation.summary === 'string'
        && typeof goalReq.interpretation.applied === 'string' && goalReq.interpretation.applied.includes('R1'),
        'A. goal interpretation declares rule R1');
    check(goalReq.goalKeys.join(',') === 'eat,firewood,visit,craft', 'A. goal key order matches options');

    /* dispute + mediation builder: force a lonely village */
    let sLonely = SC.beginVillage(SC.createInitialState(SEED)).state;
    for (let i = 0; i < 5; i++) sLonely = SC.applyDayDecay(sLonely).state;
    sLonely = SC.openDisputeIfAny(sLonely).state;
    check(sLonely.dispute !== null, 'A. a lonely village opens a dispute (D8)');
    const medReq = DEC.buildMediationCompare(sLonely);
    check(medReq.request.primitive === 'compare', 'A. mediation builder uses the compare primitive');
    check(medReq.request.options.length === 2, 'A. compare submits exactly two claims');
    check(medReq.interpretation.applied.includes('R2'), 'A. mediation interpretation declares rule R2');

    const digReq = DEC.buildChronicleDigest(sLonely);
    check(digReq.request.primitive === 'extract', 'A. digest builder uses the extract primitive');
    check(digReq.request.topK === 3, 'A. digest extract uses topK 3');
    check(digReq.interpretation.applied.includes('R3'), 'A. digest interpretation declares rule R3');

    check(DEC.buildGoalChoice(SC.createInitialState(SEED), 'tamsin') === null,
        'A. goal builder refuses the briefing phase');
    check(DEC.buildMediationCompare(sLonely) !== null && DEC.buildMediationCompare(SC.createInitialState(SEED)) === null,
        'A. mediation builder exists only while a dispute is open');
    const bound = DEC.callsPerDay();
    check(bound.choice === 6 && bound.extract === 1, 'A. documented call bound: 6 choice + 1 extract per day');

    /* ---------------------------------------------------------------- */
    section('B. Reducer determinism (byte-identical state + events)');
    /* ---------------------------------------------------------------- */

    const run1 = driveRun(SEED, policySteward);
    check(!run1.fail, `B. canonical steward run completes (${run1.fail || 'ok'})`);
    const run2 = driveRun(SEED, policySteward);
    check(!run2.fail, `B. second canonical run completes (${run2.fail || 'ok'})`);
    check(JSON.stringify(run1.state) === JSON.stringify(run2.state),
        'B. same seed + same actions produce byte-identical state');
    check(JSON.stringify(run1.eventLabels) === JSON.stringify(run2.eventLabels),
        'B. event label streams are identical across runs');

    /* ---------------------------------------------------------------- */
    section('C. Seed behavior (weather is the seed-dependent layer)');
    /* ---------------------------------------------------------------- */

    const seqA = [];
    const sA = SC.createInitialState(SEED);
    for (let d = 1; d <= SC.DAYS; d++) seqA.push(SC.weatherFor(sA, d));
    const seqB = [];
    const sB = SC.createInitialState(SEED);
    for (let d = 1; d <= SC.DAYS; d++) seqB.push(SC.weatherFor(sB, d));
    check(JSON.stringify(seqA) === JSON.stringify(seqB), 'C. same seed yields the same weather sequence');
    check(seqA.every((w) => SC.WEATHERS.includes(w)), 'C. every drawn weather is a valid weather');
    const seqC = [];
    const sC = SC.createInitialState(777);
    for (let d = 1; d <= SC.DAYS; d++) seqC.push(SC.weatherFor(sC, d));
    check(JSON.stringify(seqA) !== JSON.stringify(seqC), 'C. a different seed draws differently (observed)');
    const runD = driveRun(777, policySteward);
    check(!runD.fail && runD.state.report.kind === run1.state.report.kind,
        'C. different seed: valid run with a consistent report kind');

    /* ---------------------------------------------------------------- */
    section('D. Steward actions: valid, rejected, budget paths');
    /* ---------------------------------------------------------------- */

    let d1 = SC.beginVillage(SC.createInitialState(SEED)).state;
    check(d1.stewardActionsLeft === 2, 'D. two steward actions per day (D6)');
    let r = SC.applyStewardAction(d1, 'share-news', { villagerId: 'tamsin' });
    check(r.ok === true, 'D. share-news succeeds on a valid villager');
    d1 = r.state;
    check(d1.stewardActionsLeft === 1, 'D. the action consumed one of two daily actions');
    check(villagerNeed(d1, 'tamsin', 'belonging') === 66, 'D. share-news raised belonging by 6 (60+6)');
    check(d1.villagers[0].memory.some((m) => m.text === 'the steward shared news of the valley'),
        'D. share-news wrote the documented memory entry');
    r = SC.applyStewardAction(d1, 'share-news', { villagerId: 'tamsin' });
    check(r.ok === true, 'D. second action of the day succeeds');
    d1 = r.state;
    r = SC.applyStewardAction(d1, 'share-news', { villagerId: 'tamsin' });
    check(r.ok === false && r.reason === 'no steward actions left today',
        'D. a third action is rejected (budget exhausted)');
    check(d1.stewardActionsLeft === 0, 'D. rejected actions do not change the budget');
    r = SC.applyStewardAction(d1, 'host-gathering');
    check(r.ok === false && /no steward actions left/.test(r.reason), 'D. budget applies to host-gathering too');
    r = SC.applyStewardAction(d1, 'witchcraft');
    check(r.ok === false && r.reason === 'unknown steward action', 'D. unknown actions are rejected');
    r = SC.applyStewardAction(d1, 'assign-task', { villagerId: 'tamsin' });
    check(r.ok === false, 'D. even valid actions are rejected when the budget is gone');
    /* unknown villager: checked on a fresh day (budget is checked first) */
    let d4 = SC.beginVillage(SC.createInitialState(SEED)).state;
    r = SC.applyStewardAction(d4, 'share-news', { villagerId: 'nobody' });
    check(r.ok === false && r.reason === 'unknown villager', 'D. unknown villagers are rejected');
    r = SC.applyStewardAction(d4, 'assign-task', { villagerId: 'nobody' });
    check(r.ok === false && r.reason === 'unknown villager', 'D. assign-task rejects unknown villagers too');
    /* mediate without dispute */
    r = SC.applyStewardAction(d1, 'mediate-dispute');
    check(r.ok === false && r.reason === 'no steward actions left today',
        'D. mediation needs budget (checked before the dispute check)');
    let d2 = SC.beginVillage(SC.createInitialState(SEED)).state;
    r = SC.applyStewardAction(d2, 'mediate-dispute');
    check(r.ok === false && r.reason === 'no dispute is open to mediate',
        'D. mediation without an open dispute is rejected');
    /* host-gathering grain rule */
    let d3 = SC.beginVillage(SC.createInitialState(SEED)).state;
    d3.stores.grain = 1;
    r = SC.applyStewardAction(d3, 'host-gathering');
    check(r.ok === false && /not enough grain/.test(r.reason), 'D. hosting is rejected when grain < 2');
    check(d3.stores.grain === 1, 'D. rejected hosting leaves the granary untouched');
    /* briefing-phase rejection */
    r = SC.applyStewardAction(SC.createInitialState(SEED), 'share-news', { villagerId: 'tamsin' });
    check(r.ok === false && /not started/.test(r.reason), 'D. steward actions are rejected in the briefing');
    /* rejected actions emit no chronicle lines */
    const chronBefore = d3.chronicle.length;
    SC.applyStewardAction(d3, 'host-gathering');
    check(d3.chronicle.length === chronBefore, 'D. rejected actions write no chronicle entries');

    /* ---------------------------------------------------------------- */
    section('E. Rule R1 both ways (goal arbitration, real + fallback)');
    /* ---------------------------------------------------------------- */

    let e1 = SC.beginVillage(SC.createInitialState(SEED)).state;
    e1 = SC.applyDayDecay(e1).state;
    const eatEnv = { result: 'eat at the common table', confidence: 0.5 };
    let e1r = SC.applyGoalResult(e1, 'tamsin', eatEnv);
    check(e1r.state.villagers[0].lastGoalKey === 'eat', 'E. R1: the eat result maps to the eat goal');
    check(e1r.state.stores.grain === e1.stores.grain - 1, 'E. R1: eating consumed one grain');
    check(villagerNeed(e1r.state, 'tamsin', 'hunger') === Math.min(100, villagerNeed(e1, 'tamsin', 'hunger') + 25),
        'E. R1: eating raised hunger by 25');
    check(e1r.events[0].meta.rule === 'R1', 'E. R1: the feed event declares the rule');

    const fireEnv = { result: 'gather firewood for the hearth', confidence: 0.5 };
    const e2r = SC.applyGoalResult(e1, 'idris', fireEnv);
    check(e2r.state.villagers[1].lastGoalKey === 'firewood', 'E. R1: firewood result maps to firewood goal');
    check(e2r.state.stores.firewood === e1.stores.firewood + 1, 'E. R1: firewood gathered into the store');

    const visitEnv = { result: 'visit Juno Rell', confidence: 0.5 };
    const e3r = SC.applyGoalResult(e1, 'tamsin', visitEnv);
    check(e3r.state.villagers[0].lastGoalKey === 'visit', 'E. R1: visit result maps to the visit goal');
    check(SC.relOf(e3r.state, 'tamsin', 'juno') === SC.relOf(e1, 'tamsin', 'juno') + 3,
        'E. R1: visiting raised the relationship by 3');
    check(e3r.state.villagers[0].memory.some((m) => m.text.includes('visited Juno Rell')),
        'E. R1: the visitor wrote a memory entry');
    check(e3r.state.villagers.find((v) => v.id === 'juno').memory.some((m) => m.text.includes('was visited by Tamsin Brook')),
        'E. R1: the host wrote a memory entry');

    const craftEnv = { result: 'work on their craft', confidence: 0.5 };
    const e4r = SC.applyGoalResult(e1, 'tamsin', craftEnv);
    check(e4r.state.villagers[0].lastGoalKey === 'craft', 'E. R1: craft result maps to the craft goal');
    check(e4r.state.stores.grain === e1.stores.grain + 1, 'E. R1/D10: the baker replenished the granary');

    const junkEnv = { result: 'something entirely unmapped', confidence: 0.1 };
    const e5r = SC.applyGoalResult(e1, 'mara', junkEnv);
    check(e5r.state.villagers[2].lastGoalKey === 'craft',
        'E. R1: an unmapped result falls back to craft (documented)');
    check(e5r.events[0].meta.goalKey === 'craft', 'E. R1: the fallback is visible in the feed event');

    /* real kernel path: the envelope the engine actually returned */
    const specReal = DEC.buildGoalChoice(e1, 'tamsin');
    const recReal = kernel.evaluate(specReal.request);
    const e6r = SC.applyGoalResult(e1, 'tamsin', recReal);
    check(e6r.state.villagers[0].lastGoalKey !== null, 'E. R1: a real envelope applies to the reducer');
    check(e6r.state.goalChoices[e6r.state.goalChoices.length - 1].confidence === recReal.confidence,
        'E. R1: the recorded confidence is the real envelope confidence');

    /* ---------------------------------------------------------------- */
    section('F. Rule R2 both ways (mediation verdicts, tie, closure)');
    /* ---------------------------------------------------------------- */

    const m0 = sLonely; /* has an open dispute */
    const claims = SC.disputeClaimsFor(m0);
    const pairA = m0.dispute.a;
    const pairB = m0.dispute.b;
    const belongingA = villagerNeed(m0, pairA, 'belonging');
    const relBefore = SC.relOf(m0, pairA, pairB);

    const rA = SC.applyMediationResult(m0, { result: claims.a, confidence: 0.6 });
    check(rA.state.dispute === null, 'F. R2: the dispute closes on a supported claim');
    check(villagerNeed(rA.state, pairA, 'belonging') === Math.min(100, belongingA + 4),
        'F. R2: the supported claim owner gains belonging +4');
    check(SC.relOf(rA.state, pairA, pairB) === Math.min(10, relBefore + 4),
        'F. R2: the pair relationship improves by 4');
    check(rA.state.disputesResolved === m0.disputesResolved + 1, 'F. R2: the resolution is counted');

    const m1 = SC.openDisputeIfAny(JSON.parse(JSON.stringify(m0))).state;
    check(m1.dispute !== null, 'F. a dispute can be reopened for the next case');
    const rB = SC.applyMediationResult(m1, { result: claims.b, confidence: 0.6 });
    check(villagerNeed(rB.state, pairB, 'belonging') === Math.min(100, villagerNeed(m1, pairB, 'belonging') + 4),
        'F. R2: the other claim can win symmetrically');
    check(rB.state.dispute === null, 'F. R2: closure works for either verdict');

    const m2 = SC.openDisputeIfAny(JSON.parse(JSON.stringify(m0))).state;
    const rTie = SC.applyMediationResult(m2, { result: 'tie', confidence: 0.4 });
    check(villagerNeed(rTie.state, pairA, 'belonging') === Math.min(100, villagerNeed(m2, pairA, 'belonging') + 2)
        && villagerNeed(rTie.state, pairB, 'belonging') === Math.min(100, villagerNeed(m2, pairB, 'belonging') + 2),
        'F. R2: a tie grants belonging +2 to both');
    check(rTie.state.dispute === null, 'F. R2: a tie closes the dispute too');
    check(rTie.events[0].meta.rule === 'R2', 'F. R2: the feed event declares the rule');

    /* real kernel mediation end to end */
    const mSpec = DEC.buildMediationCompare(m0);
    const mRec = kernel.evaluate(mSpec.request);
    check(mRec.primitive === 'compare', 'F. the real kernel answered the compare request');
    const mReal = SC.applyMediationResult(m0, mRec);
    check(mReal.state.dispute === null, 'F. R2: the real envelope closes the dispute (any verdict)');

    /* ---------------------------------------------------------------- */
    section('G. Rule R3 (digest is display-only)');
    /* ---------------------------------------------------------------- */

    const g0 = SC.beginVillage(SC.createInitialState(SEED)).state;
    g0.weather = SC.weatherFor(g0, 1);
    SC.applyDayDecay(g0);
    const gState = SC.applyDayDecay(SC.beginVillage(SC.createInitialState(SEED)).state).state;
    const gBefore = JSON.stringify(gState);
    const gSpec = DEC.buildChronicleDigest(gState);
    const gRec = kernel.evaluate(gSpec.request);
    const gAfter = SC.applyDigestResult(gState, gRec);
    const gClone = JSON.parse(JSON.stringify(gAfter.state));
    gClone.digests = gState.digests; /* only the digest record may differ */
    check(JSON.stringify(gClone) === gBefore,
        'G. R3: applying a digest mutates nothing but the digest record');
    check(Array.isArray(gAfter.state.digests[gAfter.state.digests.length - 1].snippets),
        'G. R3: the snippets are recorded for the chronicle panel');

    /* ---------------------------------------------------------------- */
    section('H. Output follows input (real engine, contrasting evidence)');
    /* ---------------------------------------------------------------- */

    const probeReq = (mut) => {
        const st = SC.applyDayDecay(SC.beginVillage(SC.createInitialState(SEED)).state).state;
        mut(st);
        return {
            primitive: 'choice',
            instruction: 'Which goal should Tamsin Brook pursue today?',
            options: ['eat at the common table', 'gather firewood for the hearth',
                'visit Old Idris', 'work on their craft'],
            context: SC.goalContext(st, 'tamsin')
        };
    };
    const coldEnvelope = kernel.evaluate(probeReq((st) => {
        const v = SC.villager(st, 'tamsin');
        v.needs.warmth = 18;
        v.memory.push({ day: 1, text: 'the hearth fire is dying and the woodshed is empty' });
        st.weather = 'cold snap';
    }));
    check(coldEnvelope.result === 'gather firewood for the hearth',
        `H. cold, fire-focused evidence selects the firewood goal (got "${coldEnvelope.result}")`);
    const lonelyEnvelope = kernel.evaluate(probeReq((st) => {
        const v = SC.villager(st, 'tamsin');
        v.needs.warmth = 85; v.needs.hunger = 85; v.needs.belonging = 15;
        v.memory.push({ day: 1, text: 'has not spoken with Old Idris for days and misses company' });
        st.weather = 'clear';
    }));
    check(lonelyEnvelope.result === 'visit Old Idris',
        `H. warm, lonely evidence selects the visit goal (got "${lonelyEnvelope.result}")`);
    check(coldEnvelope.result !== lonelyEnvelope.result,
        'H. changing the submitted evidence changed the kernel result (no hardcoding)');

    /* ---------------------------------------------------------------- */
    section('I. Full playthrough + ending thresholds');
    /* ---------------------------------------------------------------- */

    check(run1.state.phase === 'report' && run1.state.report !== null,
        'I. the canonical run reaches the village report');
    const rep = run1.state.report;
    check(rep.kind === 'settled',
        `I. the canonical steward run settles the village (got "${rep.kind}", mean ${rep.meanNeeds})`);
    check(rep.disputesResolved === rep.disputesOpened,
        `I. the steward resolved every dispute (${rep.disputesOpened}/${rep.disputesResolved})`);
    check(rep.seed === SEED && rep.digestCount === SC.DAYS,
        'I. the report records the seed and one digest per day');

    const neg = driveRun(SEED, policyNone);
    check(!neg.fail, `I. the neglect run completes (${neg.fail || 'ok'})`);
    check(neg.state.report.kind === 'struggling',
        `I. neglect leaves the village struggling (got "${neg.state.report.kind}", ` +
        `mean ${neg.state.report.meanNeeds}, open dispute ${neg.state.dispute !== null})`);

    /* pure threshold units: craft states for every ending kind */
    const mk = (meanNeeds, withDispute) => {
        const st = SC.createInitialState(SEED);
        st.phase = 'report';
        st.day = SC.DAYS + 1;
        const per = Math.round(meanNeeds);
        for (const v of st.villagers) {
            v.needs = { hunger: per, warmth: per, belonging: per, craft: per };
        }
        if (withDispute) st.dispute = { a: 'tamsin', b: 'idris', topic: 'x', openedDay: 7 };
        return st;
    };
    check(SC.villageReport(mk(50, false)).kind === 'thriving', 'I. mean >= 45, no dispute -> thriving');
    check(SC.villageReport(mk(32, false)).kind === 'settled', 'I. 30 <= mean < 45, no dispute -> settled');
    check(SC.villageReport(mk(28, false)).kind === 'struggling', 'I. mean < 30 -> struggling');
    check(SC.villageReport(mk(40, true)).kind === 'struggling', 'I. an open dispute forces struggling');
    check(SC.villageReport(mk(50, true)).kind === 'struggling', 'I. an open dispute beats a high mean');

    /* ---------------------------------------------------------------- */
    section('J. Replay: byte-identical envelopes, identical ending');
    /* ---------------------------------------------------------------- */

    const replayRun = RP.runReplay({ seed: SEED, log: run1.log, invoke: makeInvoke(), gameId: 'living-village' });
    check(replayRun.ok === true, `J. the recorded run replays cleanly (${replayRun.reason || 'ok'})`);
    check(JSON.stringify(replayRun.state) === JSON.stringify(run1.state),
        'J. the replayed state is byte-identical to the original run');
    check(replayRun.reports.length === run1.decisionCount,
        `J. the replay re-invoked the same number of decisions (${replayRun.reports.length})`);
    check(replayRun.reports.every((rp) => rp.ok),
        'J. every replayed decision succeeded on the real kernel');

    /* ---------------------------------------------------------------- */
    section('K. Decision-call bound');
    /* ---------------------------------------------------------------- */

    const stewardMediations = run1.log.filter((e, i) =>
        e.kind === 'action' && e.id === 'mediate-dispute').length;
    check(run1.decisionCount === SC.DAYS * (SC.VILLAGERS.length + 1) + stewardMediations,
        `K. call accounting: 7*(6+1)+${stewardMediations} = ${run1.decisionCount}`);
    check(run1.decisionCount <= 56, 'K. the design bound of 56 calls per run holds');
    check(replayRun.reports.length === run1.decisionCount,
        'K. replay re-invoked exactly the recorded call count');

    /* ---------------------------------------------------------------- */
    section('L. Failed decision (injected error)');
    /* ---------------------------------------------------------------- */

    callCount = 0;
    const failRun = driveRun(SEED, policySteward, { invoke: makeInvoke({ failAfter: 3 }) });
    check(failRun.fail !== undefined, 'L. the driver reports the injected failure honestly');
    const failReplay = RP.runReplay({ seed: SEED, log: run1.log, invoke: makeInvoke({ failAfter: 2 }) });
    check(failReplay.ok === false, 'L. replay aborts on a failed decision');
    check(typeof failReplay.reason === 'string' && failReplay.reason.length > 0,
        'L. the abort reason is human-readable');
    check(failReplay.reports.some((rp) => rp.ok === false && rp.error),
        'L. the failing decision is in the report with its error');
    const failedSpec = failReplay.reports.find((rp) => rp.ok === false);
    check(failReplay.state.phase === 'day' || failReplay.state.phase === 'briefing' ||
        failReplay.state.phase === 'report',
        'L. the interrupted state remains a valid scenario phase');
    check(SC.meanNeeds(failReplay.state) > 0 && failReplay.state.villagers.length === 6,
        'L. the interrupted state remains a consistent village');

    /* clean shutdown of the suite kernel */
    kernel.free();

    /* ---------------------------------------------------------------- */
    section('M. Module wiring: registry, briefing seed sync, action log');
    /* ---------------------------------------------------------------- */

    /* Part 2 drives the REAL module through a minimal DOM shim, a REAL
     * SimSession and the REAL shared adapter singleton - exactly like
     * gamelab_state.test.mjs does for the detective. */
    globalThis.OmniDecide = OD;
    globalThis.OmniDecideError = ODmod.OmniDecideError;
    await sharedAdapter.init({ mode: 'ascii', language: 'en' });
    check(sharedAdapter.state === 'ready', 'M. the shared adapter is ready on the real engine');

    /* Importing games/index.js performs the registrations (module side
     * effect), exactly like the app shell does. */
    await import(url('gamelab/js/games/index.js'));
    const { allGames } = await import(url('gamelab/js/core/registry.js'));
    const games = allGames();
    check(games.length === 5, `M. the launcher registry still presents five games (got ${games.length})`);
    const playable = games.filter((g) => g.status === 'playable');
    check(playable.length === 2, `M. two playable games (got ${playable.length})`);
    check(playable.some((g) => g.id === 'living-village'), 'M. living-village is registered playable');
    check(playable.every((g) => typeof g.create === 'function'), 'M. every playable game has a create factory');

    const rig = makeRig(SEED);
    {
        const { module: mod, session } = rig;
        check(session.actionCount === 0, 'M. the shared action counter starts at zero');
        const seedInput = document.getElementById('lv-seed');
        seedInput.value = '424242';
        await find('id', 'lv-begin').click();
        check(session.seed === 424242, `M. the briefing seed reached the authoritative session (got ${session.seed})`);
        check(mod._debug().state.seed === 424242, 'M. the scenario initialized from the effective seed');
        check(session.actionCount === 1, `M. begin is one successful action (got ${session.actionCount})`);
        check(session.actionLog[0].action === 'begin', 'M. the entry is the documented begin action');

        const assignBtn = find('data-assign', 'tamsin');
        await assignBtn.click();
        check(session.actionCount === 2, `M. an accepted steward action counts once (got ${session.actionCount})`);
        check(session.actionLog[1].action === 'steward' && session.actionLog[1].args.id === 'assign-task',
            'M. the entry identifies the steward action and its argument');
        const before = mod._debug().state.stores.grain;
        check(before === 11, `M. the baker produced one grain (got ${before})`);

        await find('data-assign', 'idris').click();
        check(session.actionCount === 3, 'M. the second steward action counts');
        await find('data-assign', 'mara').click();
        check(session.actionCount === 3, 'M. a rejected steward action does NOT count');

        const decisionsBefore = sharedHistory.decisions().length;
        await find('id', 'lv-endday').click();
        check(session.actionCount === 4, `M. end-day is one player action (got ${session.actionCount})`);
        const newDecisions = sharedHistory.decisions().slice(decisionsBefore);
        check(newDecisions.length === 7, `M. end-day produced 7 real kernel invocations (got ${newDecisions.length})`);
        check(newDecisions.every((d) => d.ok), 'M. every end-day decision succeeded on the real kernel');
        check(newDecisions.filter((d) => d.request.primitive === 'choice').length === 6,
            'M. six choice arbitrations per day');
        check(newDecisions[6].request.primitive === 'extract', 'M. the day ends with the chronicle extract');
        check(newDecisions.every((d) => d.game === 'living-village'),
            'M. decisions are stamped with the living-village game id');
        check(newDecisions.every((d) => d.interpretation && typeof d.interpretation.applied === 'string'),
            'M. every decision carries the declared interpretation (Inspector-visible)');

        const note = find('class', 'controls-note');
        check(note && note.textContent.includes('424242'),
            'M. the workspace controls display the session seed');
    }

    /* ---------------------------------------------------------------- */
    section('N. Reset, replay and honest failure through the module');
    /* ---------------------------------------------------------------- */

    {
        const { module: mod, session } = rig;
        await find('id', 'lv-endday').click();
        await find('id', 'lv-reset').click();
        check(session.actionCount === 0, `N. reset clears the shared action log (got ${session.actionCount})`);
        check(mod._debug().state.phase === 'briefing', 'N. reset returns the village to the briefing');
        check(session.seed === 424242, 'N. reset retains the effective seed');

        const decisionsBefore = sharedHistory.decisions().length;
        await find('id', 'lv-replay').click();
        const reportEl = find('id', 'lv-replay-report');
        check(reportEl !== null, 'N. the replay report panel renders');
        check(reportEl.textContent.includes('byte-identical envelopes'), 'N. the report compares envelopes');
        check(session.actionCount === 0, 'N. replay never appends to the shared action log');
        check(sharedHistory.decisions().length > decisionsBefore,
            'N. replay re-invoked real decisions (recorded in history)');
        const stateAfterReplay = JSON.parse(JSON.stringify(mod._debug().state));
        check(stateAfterReplay.phase === 'briefing' && stateAfterReplay.seed === 424242,
            'N. replay leaves the module state where it was');
    }

    {
        const rigFail = makeRig(SC.DEFAULT_SEED);
        const { module: mod, session } = rigFail;
        await find('id', 'lv-begin').click();
        const realDecide = rigFail.env.decide;
        let injected = false;
        rigFail.env.decide = (request, info) => {
            if (!injected) {
                injected = true;
                return { ok: false, error: { kind: 'InjectedTestError', message: 'injected' }, id: 'x', durationMs: 0 };
            }
            return realDecide(request, info);
        };
        const countBefore = session.actionCount;
        const decisionsBefore = sharedHistory.decisions().length;
        await find('id', 'lv-endday').click();
        check(session.actionCount === countBefore + 1,
            'N. the end-day command itself still counts as the player action');
        const newDecisions = sharedHistory.decisions().slice(decisionsBefore);
        /* The injected failure bypasses session.decide, so it never reaches
         * the adapter's history - the module reports it through its own
         * decision log and a feed event instead (documented semantics). */
        check(newDecisions.filter((d) => d.ok).length === 6,
            `N. the other 6 arbitrations of the day still ran on the real kernel (got ` +
            `${newDecisions.filter((d) => d.ok).length})`);
        check(mod._debug().decisionLog.some((d) => d.ok === false),
            'N. the injected failure is kept in the module decision log');
        check(sharedHistory.events().some((e) => e.label === 'Goal arbitration failed'),
            'N. the module reports the failed arbitration as a feed event');
        const dbg = mod._debug();
        check(dbg.state.phase === 'day' && dbg.state.day === 2,
            'N. the village day completed with consistent state despite the failure');
    }

    console.log(`\ngamelab_village.test: ${checks} checks, ${failures} failures`);

    /* Machine-readable report for CI (Job H + Job G gate). */
    mkdirSync(join(ROOT, 'build'), { recursive: true });
    writeFileSync(join(ROOT, 'build', 'village-node-report.json'), JSON.stringify({
        suite: 'gamelab_village.test.mjs',
        timestamp: new Date().toISOString(),
        engine: { kernel: '2.0.0', integration: 'omnidecide-wasm 1.0.0', realWasm: true },
        game: 'living-village',
        checks,
        failures,
        failureLines
    }, null, 2) + '\n');

    if (failures > 0) {
        failureLines.forEach((l) => console.error('  - ' + l));
        process.exit(1);
    }
    console.log('gamelab_village.test: OK');
}

function villagerNeed(state, id, need) {
    return state.villagers.find((v) => v.id === id).needs[need];
}

main().catch((e) => {
    console.error('gamelab_village.test: FATAL', e);
    process.exit(1);
});
