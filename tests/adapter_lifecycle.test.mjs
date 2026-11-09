/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * adapter_lifecycle.test.mjs - Stage 2.1 Workstream D + Stage 2.1.1
 * Workstream B (cleanup ownership & recovery regression tests).
 *
 *   node tests/adapter_lifecycle.test.mjs
 *
 * Regression coverage for the WASM adapter lifecycle against the REAL
 * engine (js/omnidecide.js over dist/omnidecide.wasm):
 *
 *   1.  initialize from uninitialized          10. recreate after shutdown (rejected)
 *   2.  initialize repeatedly (rejected)       11. decision after shutdown (AdapterNotReady)
 *   3.  decision while ready                   12. initialization failure (test double)
 *   4.  recreate with valid configuration      13. recreation failure (test double)
 *   5.  decision after recreation              14. error-state handling
 *   6.  recreate repeatedly                    15. cleanup after failures
 *   7.  shutdown                               16. handle accounting (module counter)
 *   8.  shutdown repeatedly (idempotent)       17. no accumulation of live handles
 *   9.  initialize after shutdown
 *
 * plus the canonical Stage 2.1 regression sequence
 *
 *   initialize -> recreate -> recreate -> decision -> shutdown
 *   -> initialize -> decision -> shutdown
 *
 * after which the module-wide live-kernel count must be ZERO.
 *
 * Stage 2.1.1 adds the cleanup-ownership suite (spec section 5). The
 * adapter must OWN every handle it allocates: a failed release moves
 * the handle into the adapter's pending-cleanup registry (visible via
 * pendingCleanupDetail / lifecycleStats.pendingCleanup), recreation
 * is DEFERRED while unresolved handles remain, shutdown retries every
 * pending entry and reports incomplete shutdown honestly, and
 * retryCleanup() performs the safe public retry. A non-throwing
 * free() is release evidence: the C wrapper (odkw_kernel_free) clears
 * the registry slot before destroying and is a no-op on an already
 * cleared slot, so a retry can NEVER double-free.
 *
 * Required Stage 2.1.1 scenarios (numbered as in the spec):
 *    1 first init                  11 shutdown retries pending
 *    2 repeated init               12 shutdown reports unresolved
 *    3 normal decision             13 repeated shutdown safe
 *    4 normal recreation           14 reinit after complete shutdown
 *    5 recreate w/ cleanup fail    15 repeated recreation no accumulation
 *    6 adapter retains reference   16 replacement init failure
 *    7 cleanup retry succeeds      17 old-instance cleanup failure
 *    8 retry stays pending         18 pending-cleanup retry failure
 *    9 recreation deferred         19 no double-free after partial release
 *   10 shutdown releases active   20 zero adapter-owned handles at end
 *
 * Failure injection: a thin double wraps the REAL kernel and forwards
 * everything; only free() can be made to throw, BEFORE delegating
 * (handle still allocated) or AFTER delegating (release already
 * completed - the partial-release case). Every allocation/release
 * assertion in the passing path reads the REAL module registry via
 * liveKernels(); the fixture registry is used ONLY for an emergency
 * sweep AFTER the assertions, which must always find ZERO orphans -
 * that is the proof the adapter (not the test) owns its handles.
 *
 * A machine-readable summary is written to build/adapter-lifecycle-report.json.
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

/** Module-wide live kernels EXCLUDING the probe used to read the count. */
async function liveKernels() {
    const probe = await OD.OmniDecide.create({ mode: 'ascii', language: 'und' });
    const external = probe.engineCount() - 1;
    probe.free();
    return external;
}

async function rejects(promise, label) {
    try {
        await promise;
        check(false, label + ' (expected a rejection, got none)');
        return null;
    } catch (err) {
        check(true, label);
        return err;
    }
}

const REQ = {
    primitive: 'choice',
    instruction: 'Which investigation lead is best supported by the evidence currently collected?',
    options: ['Investigate the security office', 'Interview the museum curator', 'Examine the delivery records'],
    context: 'The camera feed has a gap. The curator saw the appraiser. A delivery van arrived.'
};

/* Real-adapter accounting invariant (Stage 2.1.1): every handle the
 * adapter allocated is active, pending-cleanup, or confirmed-released. */
function checkInvariant(adapter, label) {
    const s = adapter.lifecycleStats;
    check(s.created === s.released + s.activeHandles + s.pendingCleanup,
        `${label} (created ${s.created} === released ${s.released} + active ${s.activeHandles} + pending ${s.pendingCleanup})`);
}

/**
 * Fault-injecting loader over the REAL engine. Every kernel created
 * through it is registered in `fixture.registry` (for the emergency
 * sweep) and wrapped so `free()` can fail in two modes:
 *   - 'before': throws before touching the real release (the handle
 *     stays allocated in the real registry - the defect scenario);
 *   - 'after': performs the REAL release, then throws (the partial-
 *     release case; a retry must not free the kernel twice).
 * The real release call count is tracked per fixture for no-double-
 * free evidence.
 */
function makeFaultyLoader(fixture) {
    return async () => ({
        OmniDecide: {
            create: async (config) => {
                const real = await OD.OmniDecide.create(config);
                fixture.registry.push(real);
                const wrap = {
                    get kernelVersion() { return real.kernelVersion; },
                    get backend() { return real.backend; },
                    get closed() { return real.closed; },
                    evaluateRaw: (r) => real.evaluateRaw(r),
                    engineCount: () => real.engineCount(),
                    free: () => {
                        if (fixture.mode === 'before' && fixture.failing) {
                            throw new Error('simulated old-instance free() failure (before release)');
                        }
                        real.free();
                        fixture.realFreeCalls += 1;
                        if (fixture.mode === 'after' && fixture.failing) {
                            throw new Error('simulated free() failure (after the real release completed)');
                        }
                    }
                };
                return wrap;
            },
            version: OD.OmniDecide.version
        },
        OmniDecideError: OD.OmniDecideError
    });
}

function makeFixture() {
    return { failing: false, mode: 'before', registry: [], realFreeCalls: 0 };
}

/** Emergency fixture sweep - runs AFTER the assertions of a scenario.
 * Returns the number of handles the TEST had to release; in every
 * passing scenario this must be 0 (the adapter already owned cleanup). */
function emergencySweep(fixture) {
    let swept = 0;
    for (const k of fixture.registry) {
        if (!k.closed) { k.free(); swept += 1; }
    }
    return swept;
}

async function main() {
    const baseline = await liveKernels();
    check(baseline === 0, `no kernels are live before the suite (got ${baseline})`);

    /* ---------------------------------------------------------------- */
    section('A. Real WASM lifecycle (no doubles)');
    /* ---------------------------------------------------------------- */

    const adapter = createAdapter({
        engineLoader: async () => ({ OmniDecide: OD.OmniDecide, OmniDecideError: OD.OmniDecideError })
    });

    /* 1. Initialize from uninitialized. */
    check(adapter.state === 'uninitialized', '1. adapter starts uninitialized');
    const init1 = await adapter.init({ mode: 'ascii', language: 'en' });
    check(init1.state === 'ready' && adapter.state === 'ready', '1. initialize from uninitialized reaches ready');
    check(await liveKernels() === 1, '1. exactly one managed kernel handle is live after init');
    check(adapter.engineCount() === 1, `1. engineCount() through the managed handle reports 1 (got ${adapter.engineCount()})`);
    check(adapter.lifecycleStats.created === 1 && adapter.lifecycleStats.released === 0,
        '1. lifecycle accounting: created 1, released 0');

    /* 2. Initialize repeatedly -> explicit invalid transition, no new handle. */
    const reinitErr = await rejects(adapter.init({ mode: 'unicode', language: 'en' }),
        '2. init() while ready is explicitly rejected (invalid transition)');
    check(reinitErr && /recreate/.test(reinitErr.message), '2. the rejection points to recreate() as the sanctioned path');
    check(await liveKernels() === 1, '2. repeated initialization allocated NO additional kernel handle');
    check(adapter.state === 'ready', '2. adapter remains ready after the rejected re-init');

    /* 3. Decision while ready. */
    const d1 = adapter.invoke(REQ, { game: 'lifecycle-test', step: 1, label: 'while ready' });
    check(d1.ok === true && typeof d1.raw === 'string', '3. decision executes while ready');
    check(await liveKernels() === 1, '3. a decision does not change the handle count');

    /* 4. Recreate with a valid configuration. */
    const rec1 = await adapter.recreate({ mode: 'unicode', language: 'en' });
    check(rec1.recreated === true && rec1.state === 'ready', '4. recreate() with valid config reports recreated');
    check(adapter.config.mode === 'unicode', `4. active config is the new one (mode ${adapter.config.mode})`);
    check(await liveKernels() === 1, '4. recreate released the previous handle - still exactly ONE live kernel');
    check(adapter.lifecycleStats.created === 2 && adapter.lifecycleStats.released === 1,
        '4. lifecycle accounting: created 2, released 1');
    check(rec1.cleanupPending === undefined && adapter.hasPendingCleanup === false,
        '4. a clean recreation reports no pending cleanup');

    /* 5. Decision after recreation. */
    const d2 = adapter.invoke(REQ, { game: 'lifecycle-test', step: 2, label: 'after recreate' });
    check(d2.ok === true, '5. decision executes after recreation');
    check(d2.raw === d1.raw, '5. the recreated kernel produces the byte-identical envelope (determinism)');

    /* 6. Recreate repeatedly. */
    const rec2 = await adapter.recreate({ mode: 'ascii', language: 'und' });
    const rec3 = await adapter.recreate({ mode: 'ascii', language: 'en' });
    check(rec2.recreated && rec3.recreated, '6. repeated recreation succeeds');
    check(await liveKernels() === 1, '6. repeated recreation never accumulates handles (still exactly 1)');
    check(adapter.lifecycleStats.recreations === 3 && adapter.lifecycleStats.recreateFailures === 0,
        '6. lifecycle accounting: 3 successful recreations, 0 failures');
    const d3 = adapter.invoke(REQ, { game: 'lifecycle-test', step: 3, label: 'after re-recreates' });
    check(d3.ok && d3.raw === d1.raw, '6. decisions after repeated recreation stay byte-identical');
    checkInvariant(adapter, '6. ownership invariant holds after repeated recreation');

    /* 7. Shutdown. */
    const sd1 = await adapter.shutdown();
    check(sd1.released === true && sd1.state === 'uninitialized' && adapter.state === 'uninitialized',
        '7. shutdown releases the managed kernel and returns to uninitialized');
    check(sd1.shutdownComplete === true && sd1.pendingCleanup === 0,
        '7. shutdown reports complete (no pending cleanup)');
    check(await liveKernels() === 0, '7. zero live kernels after shutdown');

    /* 8. Shutdown repeatedly (idempotent, no double free). */
    const sd2 = await adapter.shutdown();
    check(sd2.released === false && adapter.state === 'uninitialized',
        '8. repeated shutdown is a safe no-op (released: false)');
    check(sd2.shutdownComplete === true, '8. the repeated no-op shutdown still reports complete');
    check(await liveKernels() === 0, '8. repeated shutdown caused no double-free (still 0 live)');
    check(adapter.lifecycleStats.released === 4, `8. exactly 4 handles were ever released (got ${adapter.lifecycleStats.released})`);

    /* 11. Decision after shutdown (checked before re-init, per spec order). */
    const d4 = adapter.invoke(REQ, { game: 'lifecycle-test', step: 4, label: 'after shutdown' });
    check(d4.ok === false && d4.error.kind === 'AdapterNotReady' && d4.rejectedBeforeKernel === true,
        '11. decision after shutdown is rejected before the kernel (AdapterNotReady)');

    /* 9. Initialize after shutdown. */
    const init2 = await adapter.init({ mode: 'ascii', language: 'en' });
    check(init2.state === 'ready', '9. initialize after shutdown works');
    check(await liveKernels() === 1, '9. re-initialization holds exactly one live handle');
    const d5 = adapter.invoke(REQ, { game: 'lifecycle-test', step: 5, label: 'after re-init' });
    check(d5.ok && d5.raw === d1.raw, '9. re-initialized kernel answers byte-identically');

    /* 10. Recreate after shutdown -> invalid transition. */
    await adapter.shutdown();
    const recErr = await rejects(adapter.recreate({ mode: 'unicode', language: 'en' }),
        '10. recreate() after shutdown is rejected (requires ready)');
    check(await liveKernels() === 0, '10. the rejected recreate allocated nothing');

    /* ---------------------------------------------------------------- */
    section('B. Canonical Stage 2.1 regression sequence (spec order)');
    /* ---------------------------------------------------------------- */
    /* initialize -> recreate -> recreate -> decision -> shutdown
       -> initialize -> decision -> shutdown => zero live handles. */

    const seqAdapter = createAdapter({
        engineLoader: async () => ({ OmniDecide: OD.OmniDecide, OmniDecideError: OD.OmniDecideError })
    });
    await seqAdapter.init({ mode: 'ascii', language: 'en' });
    check(await liveKernels() === 1, 'seq. initialize -> 1 live handle');
    await seqAdapter.recreate({ mode: 'unicode', language: 'en' });
    check(await liveKernels() === 1, 'seq. recreate -> still 1 live handle (old released)');
    await seqAdapter.recreate({ mode: 'ascii', language: 'en' });
    check(await liveKernels() === 1, 'seq. recreate again -> still 1 live handle');
    const seqD1 = seqAdapter.invoke(REQ, { game: 'lifecycle-test', label: 'seq decision 1' });
    check(seqD1.ok, 'seq. decision executes');
    await seqAdapter.shutdown();
    check(await liveKernels() === 0, 'seq. shutdown -> 0 live handles');
    await seqAdapter.init({ mode: 'ascii', language: 'en' });
    check(await liveKernels() === 1, 'seq. initialize again -> 1 live handle');
    const seqD2 = seqAdapter.invoke(REQ, { game: 'lifecycle-test', label: 'seq decision 2' });
    check(seqD2.ok && seqD2.raw === seqD1.raw, 'seq. decision after re-init is byte-identical');
    await seqAdapter.shutdown();
    const finalLive = await liveKernels();
    check(finalLive === 0, `seq. final shutdown -> NO managed kernel handles remain (got ${finalLive})`);
    /* 2 inits + 2 recreations = 4 creations; 2 recreations + 2 shutdowns = 4 releases. */
    check(seqAdapter.lifecycleStats.created === 4 && seqAdapter.lifecycleStats.released === 4,
        `seq. accounting balances: created ${seqAdapter.lifecycleStats.created}, released ${seqAdapter.lifecycleStats.released}`);
    checkInvariant(seqAdapter, 'seq. ownership invariant holds at the end of the canonical sequence');

    /* ---------------------------------------------------------------- */
    section('C. Isolated error paths (marked test doubles)');
    /* ---------------------------------------------------------------- */

    /* 12. Initial initialization failure (double: loader always throws). */
    section('C1. initialization failure');
    const broken = createAdapter({
        engineLoader: async () => { throw new Error('wasm blocked by test'); }
    });
    const brokenResult = await broken.init({});
    check(brokenResult.state === 'error' && broken.state === 'error', '12. init failure surfaces as error state');
    check(broken.lastError && /wasm blocked/.test(broken.lastError.message), '12. the diagnostic is preserved');
    check(broken.lifecycleStats.created === 0, '12. no handle was created by the failed init');
    check(await liveKernels() === 0, '12. failed initialization left no hidden allocation');
    const brokenRec = broken.invoke(REQ);
    check(brokenRec.ok === false && brokenRec.error.kind === 'AdapterNotReady', '12. failed engine produces no decisions');

    /* 14. Error-state handling: init() is the retry path from error. */
    const retryAdapter = createAdapter({
        engineLoader: async () => ({ OmniDecide: OD.OmniDecide, OmniDecideError: OD.OmniDecideError })
    });
    const firstFail = await retryAdapter.init({ mode: 'unicode', language: 'xx' });
    check(firstFail.state === 'error', '14. an invalid configuration lands in the error state');
    const retry = await retryAdapter.init({ mode: 'ascii', language: 'und' });
    check(retry.state === 'ready', '14. init() from the error state is the documented retry path');
    check(retryAdapter.lifecycleStats.cleanupFailures === 0, '14. the retry path performed no cleanup');
    await retryAdapter.shutdown();
    check(await liveKernels() === 0, '14. the failed-then-retried cycle left no hidden allocation');

    /* 16. Recreation failure (double: the first create succeeds, later
     * creates fail; the previous kernel must survive untouched). */
    section('C2. recreation failure (replacement initialization fails)');
    let failCreate = false;
    const flaky = createAdapter({
        engineLoader: async () => ({
            OmniDecide: {
                create: async (config) => {
                    if (failCreate) throw new OmniFail();
                    return OD.OmniDecide.create(config);
                },
                version: OD.OmniDecide.version
            },
            OmniDecideError: OD.OmniDecideError
        })
    });
    function OmniFail() {
        const e = new OD.OmniDecideError(3, 'omnidecide.js: out of memory writing string argument (simulated)');
        return e;
    }
    await flaky.init({ mode: 'ascii', language: 'en' });
    const originalConfig = flaky.config.mode;
    failCreate = true;
    const failedRec = await flaky.recreate({ mode: 'unicode', language: 'en' });
    check(failedRec.recreated === false && failedRec.error && failedRec.error.kind === 'RecreationFailed',
        '16. failed recreation reports recreated:false with a RecreationFailed error');
    check(failedRec.error.status === 3 && failedRec.error.statusName === 'MEMORY',
        '16. the underlying OmniDecideError status is preserved');
    check(flaky.state === 'ready', '16. the adapter stays genuinely ready on the PREVIOUS kernel (no fake readiness)');
    check(flaky.config.mode === originalConfig, '16. the active configuration is still the previous one');
    const stillWorking = flaky.invoke(REQ);
    check(stillWorking.ok === true, '16. the previous kernel still answers decisions after a failed recreation');
    check(flaky.lifecycleStats.recreateFailures === 1 && flaky.lifecycleStats.created === 1,
        '16. accounting: 1 recreate failure, no phantom creation');
    check(await liveKernels() === 1, '16. failed recreation leaked nothing (still exactly 1 live handle)');
    failCreate = false;

    /* Recovery: a valid recreation after a failed one. */
    const recovered = await flaky.recreate({ mode: 'unicode', language: 'en' });
    check(recovered.recreated === true && flaky.config.mode === 'unicode',
        '16. a valid recreation after a failed one succeeds');
    check(await liveKernels() === 1, '16. recovery keeps exactly one live handle');
    await flaky.shutdown();

    /* ---------------------------------------------------------------- */
    section('D. Cleanup ownership: recreation keeps the failed handle (Stage 2.1.1 spec 5/6/17)');
    /* ---------------------------------------------------------------- */

    const dirtyFixture = makeFixture();
    const dirty = createAdapter({ engineLoader: makeFaultyLoader(dirtyFixture) });
    await dirty.init({ mode: 'ascii', language: 'en' });
    dirtyFixture.failing = true;
    const dirtySwap = await dirty.recreate({ mode: 'unicode', language: 'en' });

    /* 5 + 17: recreation succeeds, the old-instance release failure is
     * reported explicitly and never swallowed. */
    check(dirtySwap.recreated === true && dirtySwap.cleanupError !== null,
        '5. recreate() succeeds but reports the old-instance cleanup failure explicitly');
    check(dirtySwap.cleanupPending === true && dirtySwap.pendingCleanup === 1,
        '5. the recreation result exposes the degraded (not fully clean) cleanup state');
    check(dirty.lifecycleStats.cleanupFailures === 1 && dirty.lifecycleStats.lastCleanupError !== null,
        '5. the cleanup failure is recorded in the lifecycle accounting');

    /* 6: the adapter RETAINS the reference - the handle is not orphaned. */
    check(dirty.hasPendingCleanup === true && dirty.lifecycleStats.pendingCleanup === 1,
        '6. the failed handle stays adapter-owned (pending-cleanup registry holds it)');
    check(dirty.pendingCleanupDetail.length === 1 &&
          dirty.pendingCleanupDetail[0].reason === 'recreate' &&
          dirty.pendingCleanupDetail[0].attempts === 1 &&
          dirty.pendingCleanupDetail[0].state === 'pending',
        '6. the pending entry exposes reason, attempt count and pending state');
    check(dirty.pendingCleanupDetail[0].error.message.includes('before release'),
        '6. the ORIGINAL cleanup error is preserved in the pending detail');
    check(await liveKernels() === 2, '6. the real registry still holds BOTH handles (active + pending) - nothing abandoned');
    check(dirty.engineCount() === 2, '6. engineCount() is readable through adapter ownership while a handle is pending');
    checkInvariant(dirty, '6. ownership invariant: created 2 === released 0 + active 1 + pending 1');

    /* The replacement remains correctly managed. */
    const dirtyDecision = dirty.invoke(REQ, { game: 'lifecycle-test', label: 'on replacement' });
    check(dirtyDecision.ok === true, '6. the replacement kernel answers decisions (correctly managed)');
    check(dirty.state === 'ready', '6. the adapter stays ready on the replacement');

    /* 9: recreation is DEFERRED while the unresolved handle exists. */
    const deferredRec = await dirty.recreate({ mode: 'ascii', language: 'en' });
    check(deferredRec.recreated === false && deferredRec.error &&
          deferredRec.error.kind === 'PendingCleanupUnresolved',
        '9. recreation is deferred with PendingCleanupUnresolved while a handle is unresolved');
    check(await liveKernels() === 2, '9. the deferred recreation allocated NO replacement (still 2 handles)');
    check(dirty.state === 'ready' && dirty.invoke(REQ).ok === true,
        '9. the adapter stays ready and usable through the deferral');
    check(dirty.lifecycleStats.recreateFailures === 1,
        '9. the deferred attempt is honestly counted as a recreate failure');

    /* 8: a retry while the failure persists stays pending (attempt increments). */
    const attemptsBeforeRetry = dirty.pendingCleanupDetail[0].attempts;
    const pendingRetry = dirty.retryCleanup();
    check(pendingRetry.recovered === 0 && pendingRetry.remaining === 1,
        '8. cleanup retry with persisting failure recovers nothing and keeps the entry pending');
    check(dirty.pendingCleanupDetail[0].attempts === attemptsBeforeRetry + 1,
        '8. the failed retry increments the recorded attempt count');
    check(await liveKernels() === 2, '8. the failed retry left both handles in the real registry');

    /* 7: once the failure clears, the safe retry releases the handle. */
    dirtyFixture.failing = false;
    const okRetry = dirty.retryCleanup();
    check(okRetry.recovered === 1 && okRetry.remaining === 0,
        '7. cleanup retry releases the pending handle and empties the registry');
    check(dirty.hasPendingCleanup === false && dirty.lifecycleStats.pendingCleanup === 0,
        '7. the adapter drops ownership only after confirmed release');
    check(await liveKernels() === 1, '7. the real registry holds exactly the active handle after recovery');
    check(dirty.lifecycleStats.cleanupRecoveries === 1,
        '7. the recovery is visible in the lifecycle accounting (cleanupRecoveries)');
    checkInvariant(dirty, '7. ownership invariant after recovery: created === released + active');

    const afterRetryShutdown = await dirty.shutdown();
    check(afterRetryShutdown.shutdownComplete === true && (await liveKernels()) === 0,
        '7. shutdown after recovery is complete and leaves zero live handles');
    check(emergencySweep(dirtyFixture) === 0,
        '7. fixture sweep after full shutdown finds NO orphans - the adapter owned every recovery');

    /* ---------------------------------------------------------------- */
    section('E. Critical Stage 2.1.1 regression sequence (spec 5.1)');
    /* ---------------------------------------------------------------- */
    /* initialize -> decision
     * inject one old-instance release failure -> recreate
     *   assert: replacement managed, old handle adapter-owned,
     *           cleanup-pending visible, nothing abandoned
     * retry cleanup
     *   assert: old handle released, pending registry empty,
     *           only the active instance remains
     * decision -> shutdown
     *   assert: active released, live handles zero
     * initialize -> decision -> shutdown
     *   assert: live handles zero */

    const seqFixture = makeFixture();
    const seq = createAdapter({ engineLoader: makeFaultyLoader(seqFixture) });

    await seq.init({ mode: 'ascii', language: 'en' });
    const seqA = seq.invoke(REQ, { game: 'lifecycle-test', label: 'critical 1' });
    check(seqA.ok === true, 'crit. initialize + decision work');
    check(await liveKernels() === 1, 'crit. exactly one live handle after init+decision');

    seqFixture.failing = true; /* inject ONE old-instance release failure */
    const critRec = await seq.recreate({ mode: 'unicode', language: 'en' });
    check(critRec.recreated === true && critRec.cleanupError !== null,
        'crit. recreate reports the injected cleanup failure');
    check(seq.lifecycleStats.pendingCleanup === 1 &&
          seq.pendingCleanupDetail[0].reason === 'recreate',
        'crit. the old handle remains adapter-owned with a visible cleanup-pending state');
    check(await liveKernels() === 2, 'crit. no handle was silently abandoned (real registry holds both)');
    const critD1 = seq.invoke(REQ, { game: 'lifecycle-test', label: 'critical 2' });
    check(critD1.ok === true && critD1.raw === seqA.raw,
        'crit. the replacement is correctly managed (real, deterministic decisions)');

    seqFixture.failing = false;
    const critRetry = seq.retryCleanup();
    check(critRetry.recovered === 1 && critRetry.remaining === 0,
        'crit. retry cleanup releases the old handle');
    check(seq.lifecycleStats.pendingCleanup === 0 && seq.pendingCleanupDetail.length === 0,
        'crit. the pending-cleanup registry is empty after the retry');
    check(await liveKernels() === 1, 'crit. only the active instance remains in the real registry');

    const critD2 = seq.invoke(REQ, { game: 'lifecycle-test', label: 'critical 3' });
    check(critD2.ok === true && critD2.raw === seqA.raw, 'crit. decision after recovery is byte-identical');
    const critSd = await seq.shutdown();
    check(critSd.shutdownComplete === true && critSd.released === true,
        'crit. shutdown releases the active instance and reports complete');
    check(await liveKernels() === 0, 'crit. underlying live handle count is zero after shutdown');

    await seq.init({ mode: 'ascii', language: 'en' });
    const critD3 = seq.invoke(REQ, { game: 'lifecycle-test', label: 'critical 4' });
    check(critD3.ok === true && critD3.raw === seqA.raw, 'crit. reinitialize + decision work (byte-identical)');
    const critSd2 = await seq.shutdown();
    check(critSd2.shutdownComplete === true, 'crit. final shutdown reports complete');
    check(await liveKernels() === 0, 'crit. underlying live handle count is zero after the final shutdown');
    checkInvariant(seq, 'crit. ownership invariant through the whole critical sequence');
    check(emergencySweep(seqFixture) === 0, 'crit. fixture sweep finds NO orphans (adapter-owned cleanup proven)');

    /* ---------------------------------------------------------------- */
    section('F. Shutdown semantics with pending handles (spec 11/12/13)');
    /* ---------------------------------------------------------------- */

    /* 11: shutdown retries pending cleanup and completes once clear. */
    const sdFixture = makeFixture();
    const sd = createAdapter({ engineLoader: makeFaultyLoader(sdFixture) });
    await sd.init({ mode: 'ascii', language: 'en' });
    sdFixture.failing = true;
    const sdDirty = await sd.shutdown();
    check(sdDirty.released === true && sdDirty.incompleteShutdown === true &&
          sdDirty.shutdownComplete === false && sdDirty.pendingCleanup === 1,
        '12. shutdown reports the unresolved resources honestly (incomplete, not "fully released")');
    check(Array.isArray(sdDirty.pendingCleanupDetail) && sdDirty.pendingCleanupDetail[0].reason === 'shutdown',
        '12. the incomplete-shutdown result carries the sanitized pending detail');
    check(await liveKernels() === 1, '12. the failed shutdown handle is still live - and still adapter-owned');
    check(sd.hasPendingCleanup === true, '12. the adapter keeps ownership after the incomplete shutdown');

    /* 13 + 11: repeated shutdown is safe and RETRIES the pending entry. */
    const sdDirty2 = await sd.shutdown();
    check(sdDirty2.released === false && sdDirty2.shutdownComplete === false && sdDirty2.pendingCleanup === 1,
        '13. repeated shutdown with persisting failure stays safe and honest');
    check(await liveKernels() === 1, '13. repeated shutdown caused no double-free (still exactly 1 live)');

    sdFixture.failing = false;
    const sdRecovered = await sd.shutdown(); /* the retry path of shutdown */
    check(sdRecovered.shutdownComplete === true && sdRecovered.pendingCleanup === 0,
        '11. a repeated shutdown retries the pending cleanup and completes');
    check(await liveKernels() === 0, '11. the adapter (not the fixture) released the pending handle');
    check(sd.lifecycleStats.cleanupRecoveries === 1, '11. the shutdown recovery is accounted');
    check(emergencySweep(sdFixture) === 0, '11. fixture sweep finds NO orphans');

    /* 14: reinitialization after a COMPLETE shutdown works. */
    const afterComplete = await sd.init({ mode: 'ascii', language: 'en' });
    check(afterComplete.state === 'ready' && (await liveKernels()) === 1,
        '14. reinitialization after complete shutdown works');
    check(sd.invoke(REQ).ok === true, '14. the reinitialized engine answers real decisions');
    await sd.shutdown();
    check(await liveKernels() === 0, '14. clean shutdown at the end of section F');

    /* ---------------------------------------------------------------- */
    section('G. Allocation policy: init/recreate deferred while unresolved (spec 9/14)');
    /* ---------------------------------------------------------------- */

    const polFixture = makeFixture();
    const pol = createAdapter({ engineLoader: makeFaultyLoader(polFixture) });
    await pol.init({ mode: 'ascii', language: 'en' });
    polFixture.failing = true;
    const polDirty = await pol.shutdown(); /* leaves one unresolved handle */
    check(polDirty.pendingCleanup === 1, 'G. setup: dirty shutdown leaves one unresolved handle');

    /* 14: init is deferred while pending handles exist (no allocation
     * while cleanup is failing). */
    polFixture.failing = true;
    const blockedInit = await pol.init({ mode: 'ascii', language: 'en' });
    check(blockedInit.error && blockedInit.error.kind === 'PendingCleanupUnresolved',
        '14. initialization is deferred with PendingCleanupUnresolved while a handle is unresolved');
    check(pol.state === 'uninitialized' && (await liveKernels()) === 1,
        '14. the deferred init allocated NOTHING (still only the pending handle)');
    check(pol.invoke(REQ).ok === false, '14. no decisions can be produced in the deferred state');

    /* Recovery, then init works. */
    polFixture.failing = false;
    const polRetry = pol.retryCleanup();
    check(polRetry.recovered === 1 && polRetry.remaining === 0, '14. retryCleanup clears the pending handle');
    const polInit = await pol.init({ mode: 'ascii', language: 'en' });
    check(polInit.state === 'ready' && (await liveKernels()) === 1,
        '14. initialization works after the pending cleanup is resolved');
    checkInvariant(pol, '14. ownership invariant after deferred-init recovery');
    await pol.shutdown();

    /* ---------------------------------------------------------------- */
    section('H. Partial release and no double-free (spec 19)');
    /* ---------------------------------------------------------------- */

    /* Mode 'after': the double performs the REAL release, THEN throws.
     * The adapter must treat a later retry as safe: the wrapper's closed
     * flag proves the release completed, so NO second real free() may
     * reach the kernel. */
    const partFixture = makeFixture();
    partFixture.mode = 'after';
    const part = createAdapter({ engineLoader: makeFaultyLoader(partFixture) });
    await part.init({ mode: 'ascii', language: 'en' });
    partFixture.failing = true;
    const partSwap = await part.recreate({ mode: 'unicode', language: 'en' });
    check(partSwap.recreated === true && partSwap.cleanupError !== null,
        '19. the partial-release failure is still reported (never swallowed)');
    check(partFixture.realFreeCalls === 1,
        '19. the real release ran exactly once during the failed attempt');
    check(await liveKernels() === 1,
        '19. the registry already reflects the completed release (no leak, no double count)');

    const partRetry = part.retryCleanup();
    check(partRetry.recovered === 1 && partRetry.remaining === 0,
        '19. the retry resolves the entry from release evidence');
    check(partFixture.realFreeCalls === 1,
        '19. NO second real free() reached the kernel - no double-free');
    check(await liveKernels() === 1, '19. exactly the active handle remains live');
    check(part.lifecycleStats.pendingCleanup === 0, '19. the pending entry is gone after evidence-based resolution');
    checkInvariant(part, '19. ownership invariant after partial-release recovery');
    await part.shutdown();
    check(await liveKernels() === 0, '19. clean shutdown after partial-release recovery');
    check(emergencySweep(partFixture) === 0,
        '19. fixture sweep after full shutdown finds NO orphans (no double-free, nothing abandoned)');

    /* ---------------------------------------------------------------- */
    section('I. Repeated recreation with intermittent cleanup failures (spec 15)');
    /* ---------------------------------------------------------------- */

    const intFixture = makeFixture();
    const acc = createAdapter({ engineLoader: makeFaultyLoader(intFixture) });
    await acc.init({ mode: 'ascii', language: 'en' });
    for (let i = 0; i < 4; i++) {
        intFixture.failing = (i % 2 === 0); /* alternate: fail, recover, fail, recover */
        const r = await acc.recreate({ mode: i % 2 ? 'ascii' : 'unicode', language: 'en' });
        if (acc.hasPendingCleanup) acc.retryCleanup(); /* recovery attempt each round */
        checkInvariant(acc, `15. ownership invariant after recreation round ${i + 1}`);
        check(await liveKernels() <= 2, `15. round ${i + 1}: at most active + one pending handle (no accumulation)`);
    }
    intFixture.failing = false;
    acc.retryCleanup();
    await acc.shutdown();
    const accFinal = await liveKernels();
    check(accFinal === 0, `15. intermittent failures never accumulated handles (final live: ${accFinal})`);
    check(acc.lifecycleStats.created === acc.lifecycleStats.released,
        `15. accounting balances (created ${acc.lifecycleStats.created}, released ${acc.lifecycleStats.released})`);
    check(emergencySweep(intFixture) === 0, '15. fixture sweep finds NO orphans');

    /* ---------------------------------------------------------------- */
    section('J. Shutdown racing an in-flight initialization (epoch guard)');
    /* ---------------------------------------------------------------- */
    let releaseLoader;
    const loaderGate = new Promise((resolve) => { releaseLoader = resolve; });
    const raceFixture = makeFixture();
    const racer = createAdapter({
        engineLoader: () => loaderGate.then(() => ({ OmniDecide: {
            create: async (config) => {
                const real = await OD.OmniDecide.create(config);
                raceFixture.registry.push(real);
                return {
                    get kernelVersion() { return real.kernelVersion; },
                    get backend() { return real.backend; },
                    get closed() { return real.closed; },
                    evaluateRaw: (r) => real.evaluateRaw(r),
                    engineCount: () => real.engineCount(),
                    free: () => { real.free(); raceFixture.realFreeCalls += 1; }
                };
            },
            version: OD.OmniDecide.version
        }, OmniDecideError: OD.OmniDecideError }))
    });
    const racing = racer.init({ mode: 'ascii', language: 'en' }); /* stuck in the loader */
    check(racer.state === 'initializing', 'race. adapter is initializing while the loader is pending');
    await racer.shutdown(); /* wins the race */
    check(racer.state === 'uninitialized', 'race. shutdown during initialization returns to uninitialized');
    releaseLoader(); /* the stale create now resolves */
    const racedResult = await racing;
    check(racedResult.stale === true || racer.state === 'uninitialized',
        'race. the stale initialization result never reports readiness');
    check(racer.state === 'uninitialized', 'race. adapter stays uninitialized after the stale create arrives');
    const raceLive = await liveKernels();
    check(raceLive === 0, `race. the stale kernel handle was disposed, none remain (got ${raceLive})`);
    check(racer.lifecycleStats.created === 1 && racer.lifecycleStats.released === 1,
        'race. the transient stale handle is honestly accounted (created 1, released 1)');
    check(emergencySweep(raceFixture) === 0, 'race. fixture sweep finds NO orphans');
    const afterRace = await racer.init({ mode: 'ascii', language: 'en' });
    check(afterRace.state === 'ready', 'race. the adapter can initialize normally after the race');
    await racer.shutdown();

    /* ---------------------------------------------------------------- */
    section('K. Final handle accounting');
    /* ---------------------------------------------------------------- */
    const endLive = await liveKernels();
    check(endLive === 0, `17/20. no live kernel handles remain at the end of the suite (got ${endLive})`);
    const total = await OD.OmniDecide.create({ mode: 'ascii', language: 'und' });
    check(total.engineCount() === 1, '16. the module counter itself is reachable and exact (probe only)');
    total.free();

    /* ---------------------------------------------------------------- */
    /* Summary + machine-readable report                                */
    /* ---------------------------------------------------------------- */
    console.log('');
    console.log(`adapter_lifecycle.test: ${checks} checks, ${failures} failures`);
    mkdirSync(join(ROOT, 'build'), { recursive: true });
    writeFileSync(join(ROOT, 'build', 'adapter-lifecycle-report.json'), JSON.stringify({
        suite: 'adapter_lifecycle.test.mjs',
        stage: '2.1.1',
        timestamp: new Date().toISOString(),
        engine: { kernel: '2.0.0', integration: 'omnidecide-wasm 1.0.0', realWasm: true },
        coverage: {
            stage21: 'lifecycle states, canonical sequence, error paths, epoch race',
            stage211: 'cleanup ownership, pending registry, safe retry, deferred allocation, incomplete shutdown, no double-free'
        },
        checks,
        failures,
        failureLines
    }, null, 2) + '\n');
    if (failures > 0) {
        failureLines.forEach((l) => console.error('  - ' + l));
        process.exit(1);
    }
    console.log('adapter_lifecycle.test: OK');
}

main().catch((e) => {
    console.error('adapter_lifecycle.test: FATAL', e);
    process.exit(1);
});
