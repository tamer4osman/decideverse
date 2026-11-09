/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * adapter.js - GameLab's OmniDecide adapter.
 *
 * The single boundary between GameLab simulations and the REAL
 * WebAssembly-backed decision engine (`js/omnidecide.js` over
 * `dist/omnidecide.wasm`). Responsibilities:
 *
 *  - A well-defined lifecycle with explicit states:
 *        uninitialized -> initializing -> ready -> recreating -> ready
 *        any state -----> shutting-down -> uninitialized
 *        init/recreate failure -> error (init) / ready (recreate, see below)
 *    Exactly ONE managed kernel handle exists while ready; recreation
 *    creates the replacement FIRST, confirms it, and only then releases
 *    the previous instance, so the managed handle is never abandoned
 *    without a release (Stage 2.1 fix for the handle leak).
 *  - Adapter-OWNED handles (Stage 2.1.1): every kernel handle this
 *    adapter allocates - the active instance AND any instance whose
 *    release failed - stays in an internal ownership registry until
 *    release is CONFIRMED. A failed free() never orphans a handle: it
 *    moves to the pending-cleanup registry, is retried safely
 *    (odkw_kernel_free is slot-idempotent, so a retry can never
 *    double-free), and blocks further allocation until resolved.
 *  - Access to the six supported decision primitives, and nothing else.
 *  - Input validation BEFORE the kernel is bothered (shape checks;
 *    semantic validation stays kernel-side).
 *  - Safe repeated invocation; a failed call never invalidates the
 *    kernel (upstream contract: invalid input keeps the engine ready).
 *  - Error handling: OmniDecideError status/message/raw envelope are
 *    preserved, never swallowed, never replaced by a fallback. Cleanup
 *    failures are recorded and reported, never silently ignored.
 *  - Result normalization for display (parsed envelope + raw string).
 *  - Execution timing with a monotonic timer around the kernel call.
 *  - Decision metadata (kernel version, backend, integration version).
 *
 * The adapter never simulates the engine, never hardcodes winners and
 * never invents result fields: `raw` is the byte-identical envelope
 * string produced by the C wrapper; `envelope` is its parsed form.
 *
 * Host independence: the engine source is injectable. The browser
 * default resolves `window.OmniDecide`; Node tests inject the CommonJS
 * module (see tests/*.test.mjs).
 *
 * Handle accounting: the real API exposes the module-wide live-kernel
 * count through `kernel.engineCount()` (C: odkw_kernel_count). The
 * adapter reads it through the managed handle, or - while it still
 * owns pending-cleanup handles - through one of those; with no owned
 * handle at all there is nothing to ask, so tests use a short-lived
 * probe kernel and read the module counter through it (see
 * tests/adapter_lifecycle.test.mjs).
 *
 * Release semantics of the REAL engine (verified in src/wrappers/
 * odk_wasm.c, odkw_kernel_free): the wrapper resolves the registry
 * slot, clears it (g_kernel_live--) and only then destroys the kernel;
 * a call on an already-cleared slot is a no-op. A non-throwing
 * Kernel.free() therefore always ends with the handle NOT allocated -
 * either this call released it, or it had been released before - which
 * is the evidence the adapter requires before dropping ownership. The
 * public API exposes NO per-handle existence query (only the
 * module-wide count), so where a retry cannot be performed the
 * adapter keeps ownership and reports the unresolved state.
 */

import { bus } from './eventbus.js';
import { now } from './dom.js';
import { history } from './history-instance.js';
import { telemetry } from './telemetry-instance.js';

export const PRIMITIVES = ['choice', 'score', 'noul', 'rank', 'compare', 'extract'];

const STATUS_NAMES = {
    0: 'OK',
    1: 'ARGUMENT',
    2: 'INFERENCE',
    3: 'MEMORY',
    4: 'INTERNAL'
};

/**
 * Create an adapter bound to an engine source.
 * @param {object} [opts]
 * @param {() => Promise<{OmniDecide:object, OmniDecideError:Function}>} [opts.engineLoader]
 *        async engine source resolver (defaults to browser globals).
 * @param {() => number} [opts.now] monotonic timer injection (tests).
 * @param {object} [opts.telemetry] optional telemetry sink (Telemetry instance).
 * @param {object} [opts.history] optional history sink (HistoryStore instance).
 */
export function createAdapter(opts = {}) {
    const engineLoader = opts.engineLoader || loadBrowserEngine;
    const timer = opts.now || now;
    const telemetry = opts.telemetry || null;
    const history = opts.history || null;

    /* Lifecycle: uninitialized | initializing | ready | recreating |
     * shutting-down | error. Exactly one managed kernel while ready. */
    let state = 'uninitialized';
    let kernel = null;
    let OmniDecideError = null;
    let lastError = null;
    let config = { mode: 'ascii', language: 'und' };
    let initStartedAt = null;
    let initMs = null;
    let initFailureHandled = false;
    let decisionCounter = 0;

    /* Lifecycle bookkeeping (real observations, not invented metrics). */
    let epoch = 0;            /* bumped by every transition; stale creates self-release */
    let inflight = null;      /* { promise } dedupes concurrent init/recreate */
    const lifecycle = {
        created: 0,           /* kernel instances successfully created (managed + transient stale) */
        released: 0,          /* kernel instances whose release is CONFIRMED */
        recreations: 0,       /* successful recreate() swaps */
        recreateFailures: 0,  /* recreate() attempts that failed (incl. policy-deferred) */
        cleanupFailures: 0,   /* free() calls that threw */
        cleanupRecoveries: 0, /* pending handles whose release was later confirmed */
        lastCleanupError: null
    };

    /* Adapter OWNERSHIP REGISTRY (Stage 2.1.1). Every handle this
     * adapter allocates is owned exactly once: either it is the active
     * `kernel`, or it sits in `pendingCleanup` after a failed release.
     * Entries leave this registry ONLY on release evidence (a non-
     * throwing free() or the wrapper's own closed flag). Invariant over
     * all managed handles: created === released + active + pending. */
    let pendingCleanup = [];  /* [{ kernel, reason, error, attempts, state, since }] */

    function setState(next, payload) {
        state = next;
        bus.emit('engine:state', { state, payload: payload || null });
    }

    async function loadEngineSource() {
        const src = await engineLoader();
        if (!src || typeof src.OmniDecide?.create !== 'function' || !src.OmniDecideError) {
            throw new Error('adapter: engine source lacks OmniDecide.create / OmniDecideError');
        }
        return src;
    }

    /**
     * Initialize the kernel. Valid from `uninitialized` and `error`.
     * - While a transition is already running (initializing/recreating)
     *   the same in-flight promise is returned (no duplicate creation).
     * - While `ready` the transition is explicitly REJECTED: call
     *   recreate(newConfig) to change the configuration. Repeated
     *   initialization therefore never allocates a second handle.
     * @param {{mode?:string, language?:string}} [opts] text mode + language.
     * @returns {Promise<{state:string}>} final adapter state.
     */
    async function init(initOpts) {
        if (state === 'initializing' || state === 'recreating') {
            return inflight ? inflight.promise : { state };
        }
        if (state === 'ready') {
            throw new Error(
                'adapter: init() while ready is an invalid transition - ' +
                'the managed kernel already exists; call recreate(newConfig) to change settings');
        }
        /* Stage 2.1.1 allocation policy: recover pending cleanup BEFORE
         * any new allocation, and refuse to allocate while unresolved
         * handles remain (no unbounded accumulation while cleanup is
         * failing). retryCleanup() / shutdown() are the recovery paths. */
        const preflight = recoverPendingCleanup('init');
        if (preflight.remaining > 0) {
            const blocked = {
                kind: 'PendingCleanupUnresolved', status: null, statusName: null,
                message: `adapter: initialization deferred - ${preflight.remaining} kernel handle(s) ` +
                    'await cleanup recovery; call retryCleanup() (or shutdown()) first',
                envelope: null,
                pendingCleanup: pendingCleanup.length
            };
            lastError = blocked;
            bus.emit('engine:error', Object.assign({ recoverable: true, operation: 'init' }, blocked));
            return { state, error: blocked };
        }
        if (initOpts) config = { mode: initOpts.mode || 'ascii', language: initOpts.language || 'und' };
        lastError = null;
        initFailureHandled = false;

        /* Defensive cleanup: a live handle must never be abandoned, even
         * on the (unexpected) ready-kernel-plus-error path. A failing
         * release lands in the pending registry - and then blocks this
         * initialization just like the preflight above. */
        if (kernel) releaseManagedKernel('init');
        if (pendingCleanup.length > 0) {
            const blocked = {
                kind: 'PendingCleanupUnresolved', status: null, statusName: null,
                message: `adapter: initialization deferred - ${pendingCleanup.length} kernel handle(s) ` +
                    'failed release during the defensive cleanup; call retryCleanup() first',
                envelope: null,
                pendingCleanup: pendingCleanup.length
            };
            lastError = blocked;
            bus.emit('engine:error', Object.assign({ recoverable: true, operation: 'init' }, blocked));
            return { state, error: blocked };
        }

        const myEpoch = ++epoch;
        setState('initializing');
        initStartedAt = timer();
        const promise = (async () => {
            try {
                const src = await loadEngineSource();
                const created = await src.OmniDecide.create(config);
                if (myEpoch !== epoch) {
                    /* shutdown()/recreate() raced this create: dispose of
                     * the fresh handle immediately, never expose it. The
                     * transient handle is accounted like every other
                     * adapter allocation. */
                    lifecycle.created += 1;
                    disposeKernel(created, 'stale init create');
                    return { state, stale: true };
                }
                OmniDecideError = src.OmniDecideError;
                kernel = created;
                lifecycle.created += 1;
                initMs = timer() - initStartedAt;
                if (telemetry) telemetry.recordInit(initMs);
                setState('ready');
                bus.emit('engine:ready', {
                    kernelVersion: kernel.kernelVersion,
                    backend: kernel.backend,
                    integrationVersion: src.OmniDecide.version,
                    initMs,
                    config: Object.assign({}, config)
                });
                return { state: 'ready' };
            } catch (err) {
                initFailureHandled = true;
                lastError = describeError(err);
                /* Only take the error state if this attempt still owns the
                 * lifecycle (a racing shutdown already reset the epoch). */
                if (myEpoch === epoch) setState('error', lastError);
                bus.emit('engine:error', lastError);
                return { state: myEpoch === epoch ? 'error' : state };
            }
        })();
        inflight = { promise };
        try {
            return await promise;
        } finally {
            if (inflight && inflight.promise === promise) inflight = null;
        }
    }

    /**
     * Replace the managed kernel with a new one for a new configuration.
     * Valid ONLY from `ready` (a kernel must exist to be replaced; from
     * `uninitialized`/`error` call init()).
     *
     * Replacement strategy (create-first, release-after):
     *   1. validate the new configuration (types; semantics stay kernel-side);
     *   2. create the replacement instance with the REAL API;
     *   3. confirm successful initialization of the replacement;
     *   4. release the previous instance (cleanup failures recorded, never swallowed);
     *   5. swap the managed handle and update readiness + metadata.
     *
     * If creation fails, the previous kernel stays active with its
     * previous configuration: the result reports `recreated: false` with
     * the error, `lastError` carries the diagnostic, and the adapter
     * remains genuinely `ready` (readiness is never faked - it describes
     * the still-working previous instance).
     *
     * @param {{mode?:string, language?:string}} [opts] new text mode + language.
     * @returns {Promise<{state:string, recreated:boolean, error?:object}>}
     */
    async function recreate(recreateOpts) {
        if (state === 'initializing' || state === 'recreating') {
            return inflight ? inflight.promise : { state, recreated: false };
        }
        if (state !== 'ready' || !kernel) {
            throw new Error(
                `adapter: recreate() requires a ready engine (state: ${state}) - call init() first`);
        }
        const nextConfig = recreateOpts
            ? { mode: recreateOpts.mode || 'ascii', language: recreateOpts.language || 'und' }
            : Object.assign({}, config);
        /* Adapter-side type validation before anything is allocated;
         * unknown mode/language values are semantically rejected by the
         * real kernel, which surfaces through the failure path below. */
        if (typeof nextConfig.mode !== 'string' || nextConfig.mode.length === 0) {
            throw new Error('adapter: recreate() mode must be a non-empty string');
        }
        if (typeof nextConfig.language !== 'string' || nextConfig.language.length === 0) {
            throw new Error('adapter: recreate() language must be a non-empty string');
        }

        /* Stage 2.1.1 allocation policy (spec: repeated recreation must
         * not accumulate handles): recover existing pending entries
         * FIRST; if unresolved handles remain, DEFER the recreation -
         * no replacement is allocated while cleanup is failing. The
         * adapter stays genuinely ready on the current kernel. */
        const preflight = recoverPendingCleanup('recreate');
        if (preflight.remaining > 0) {
            lifecycle.recreateFailures += 1;
            const deferred = {
                kind: 'PendingCleanupUnresolved', status: null, statusName: null,
                message: `adapter: recreation deferred - ${preflight.remaining} kernel handle(s) ` +
                    'await cleanup recovery; call retryCleanup() (or shutdown()) and retry',
                envelope: null,
                pendingCleanup: pendingCleanup.length
            };
            lastError = deferred;
            bus.emit('engine:error', Object.assign({ recoverable: true, operation: 'recreate' }, deferred));
            return { state: 'ready', recreated: false, error: deferred };
        }

        const previous = kernel;
        const myEpoch = ++epoch;
        setState('recreating');
        const startedAt = timer();
        const promise = (async () => {
            try {
                const src = await loadEngineSource();
                const created = await src.OmniDecide.create(nextConfig);
                if (myEpoch !== epoch) {
                    /* shutdown() won the race: release the replacement
                     * immediately; shutdown owns the final state. The
                     * transient handle is accounted like every other
                     * adapter allocation. */
                    lifecycle.created += 1;
                    disposeKernel(created, 'stale recreate create');
                    return { state, recreated: false, stale: true };
                }
                /* Replacement confirmed - now release the previous
                 * instance so no handle is ever abandoned. A failing
                 * release lands in the pending-cleanup registry: the
                 * reference is RETAINED (Stage 2.1.1), the swap still
                 * happens, and the result reports the degraded cleanup
                 * state instead of a fully clean lifecycle. */
                let cleanupError = null;
                if (previous && !previous.closed) {
                    cleanupError = releaseKernelInstance(previous, 'recreate');
                }
                kernel = created;
                lifecycle.created += 1;
                lifecycle.recreations += 1;
                config = nextConfig;
                lastError = null;
                initMs = timer() - startedAt;
                if (telemetry) telemetry.recordInit(initMs);
                setState('ready');
                bus.emit('engine:ready', {
                    kernelVersion: kernel.kernelVersion,
                    backend: kernel.backend,
                    integrationVersion: src.OmniDecide.version,
                    initMs,
                    config: Object.assign({}, config),
                    recreated: true,
                    cleanupPending: pendingCleanup.length
                });
                const result = { state: 'ready', recreated: true };
                if (cleanupError) {
                    result.cleanupError = cleanupError;
                    result.cleanupPending = true;
                    result.pendingCleanup = pendingCleanup.length;
                }
                return result;
            } catch (err) {
                lifecycle.recreateFailures += 1;
                const failure = describeError(err);
                failure.kind = 'RecreationFailed'; /* status/statusName/message/envelope preserved */
                if (myEpoch === epoch) {
                    /* Recoverable: the previous kernel is untouched and
                     * still answers requests. Stay genuinely ready and
                     * report the failure honestly. */
                    kernel = previous && !previous.closed ? previous : kernel;
                    lastError = failure;
                    setState('ready');
                }
                bus.emit('engine:error', Object.assign({ recoverable: true, operation: 'recreate' }, failure));
                return { state: myEpoch === epoch ? 'ready' : state, recreated: false, error: failure };
            }
        })();
        inflight = { promise };
        try {
            return await promise;
        } finally {
            if (inflight && inflight.promise === promise) inflight = null;
        }
    }

    function describeError(err) {
        if (err && typeof err === 'object' && 'status' in err) {
            return {
                kind: 'OmniDecideError',
                status: err.status,
                statusName: STATUS_NAMES[err.status] || `STATUS_${err.status}`,
                message: err.message || String(err),
                envelope: err.envelope || null
            };
        }
        return { kind: 'Error', status: null, statusName: null, message: (err && err.message) || String(err), envelope: null };
    }

    /* ------------------------------------------------------------------ */
    /* Ownership & cleanup (Stage 2.1.1)                                   */
    /*                                                                     */
    /* Every kernel handle allocated by this adapter is owned exactly      */
    /* once: as the active `kernel` or as a pending-cleanup entry. A       */
    /* failed free() NEVER drops the reference - it registers the handle   */
    /* in the pending registry with the original error, and the entry      */
    /* leaves the registry only on release evidence:                       */
    /*   - a non-throwing free() (the wrapper clears the slot before       */
    /*     destroying; a retry on an already-cleared slot is a no-op, so   */
    /*     a retry can never double-free), or                              */
    /*   - the wrapper's own `closed` flag (set only after a completed     */
    /*     release call).                                                  */
    /* The public engine API exposes no per-handle existence query, so     */
    /* where no retry can be performed the entry stays owned and the       */
    /* unresolved state is reported (never claimed as released).           */
    /* ------------------------------------------------------------------ */

    /** One real release attempt through the instance's public free().
     * Returns null on confirmed release, the described error otherwise.
     * Never throws; failures are counted and reported, never swallowed. */
    function attemptRelease(instance, reason) {
        if (!instance) return null;
        try {
            instance.free();
            lifecycle.released += 1;
            return null;
        } catch (err) {
            lifecycle.cleanupFailures += 1;
            const described = describeError(err);
            described.reason = reason;
            lifecycle.lastCleanupError = described;
            console.error(`adapter: kernel release failed (${reason})`, err);
            bus.emit('engine:cleanup-error', described);
            return described;
        }
    }

    /** Register a failed-release handle in the ownership registry.
     * The reference is kept reachable; only release evidence removes it. */
    function registerPendingCleanup(instance, reason, error) {
        pendingCleanup.push({
            kernel: instance,
            reason,
            error,
            attempts: 1,
            state: 'pending',
            since: timer()
        });
        bus.emit('engine:cleanup-pending', {
            reason,
            pending: pendingCleanup.length,
            error
        });
    }

    /** Release one kernel instance through the real API. On failure the
     * handle is RETAINED in the pending-cleanup registry (never dropped,
     * never silently discarded) and the error is returned to the caller. */
    function releaseKernelInstance(instance, reason) {
        const error = attemptRelease(instance, reason);
        if (error && instance) registerPendingCleanup(instance, reason, error);
        return error;
    }

    function releaseManagedKernel(reason) {
        const k = kernel;
        kernel = null;
        if (k) return releaseKernelInstance(k, reason);
        return null;
    }

    /** Best-effort disposal of a transient (stale) handle. Failures are
     * retained in the pending registry exactly like managed ones. */
    function disposeKernel(instance, reason) {
        if (!instance) return null;
        return releaseKernelInstance(instance, reason);
    }

    /** Reconcile-or-release ONE pending entry (safe retry).
     *  - closed === true: the wrapper completed a release earlier (it
     *    only sets the flag after a non-throwing call) => resolved with
     *    no further call - no destructive operation is performed.
     *  - otherwise: exactly one retry through free(). A non-throwing
     *    completion is release evidence by the wrapper's slot protocol
     *    (live slot -> released now; cleared slot -> harmless no-op). */
    function resolvePendingEntry(entry, retryReason) {
        if (entry.kernel.closed === true) {
            lifecycle.released += 1;
            lifecycle.cleanupRecoveries += 1;
            return null;
        }
        const error = attemptRelease(entry.kernel, `retry:${entry.reason}:${retryReason}`);
        if (error === null) {
            lifecycle.cleanupRecoveries += 1;
        } else {
            entry.attempts += 1;
            entry.error = error;
            entry.state = 'pending';
        }
        return error;
    }

    /** Attempt safe cleanup of every pending entry. Entries leave the
     * registry ONLY on release evidence; failures stay owned with their
     * updated error and attempt count. Synchronous (free() is sync). */
    function recoverPendingCleanup(triggerReason) {
        if (pendingCleanup.length === 0) {
            return { recovered: 0, remaining: 0 };
        }
        const remaining = [];
        let recovered = 0;
        for (const entry of pendingCleanup) {
            const error = resolvePendingEntry(entry, triggerReason);
            if (error === null) {
                entry.state = 'released';
                recovered += 1;
            } else {
                remaining.push(entry);
            }
        }
        pendingCleanup = remaining;
        if (recovered > 0) {
            bus.emit('engine:cleanup-recovered', {
                recovered,
                remaining: pendingCleanup.length,
                reason: triggerReason
            });
        }
        return { recovered, remaining: pendingCleanup.length };
    }

    /** Sanitized pending-cleanup diagnostics (no kernel references). */
    function pendingCleanupDetail() {
        return pendingCleanup.map((entry) => ({
            reason: entry.reason,
            attempts: entry.attempts,
            state: entry.state,
            sinceMs: Math.max(0, Math.round(timer() - entry.since)),
            error: entry.error ? {
                kind: entry.error.kind,
                status: entry.error.status,
                statusName: entry.error.statusName,
                message: entry.error.message
            } : null
        }));
    }

    /* ------------------------------------------------------------------ */
    /* Adapter-side shape validation (semantic checks stay kernel-side).   */
    /* ------------------------------------------------------------------ */
    function validateRequest(request) {
        if (!request || typeof request !== 'object' || Array.isArray(request)) {
            return 'request must be an object';
        }
        if (!PRIMITIVES.includes(request.primitive)) {
            return `unsupported primitive "${request.primitive}" (supported: ${PRIMITIVES.join(', ')})`;
        }
        if (typeof request.instruction !== 'string' || request.instruction.length === 0) {
            return 'instruction must be a non-empty string';
        }
        const needsOptions = ['choice', 'rank', 'compare', 'score'].includes(request.primitive);
        if (needsOptions) {
            if (!Array.isArray(request.options) || request.options.length === 0) {
                return `${request.primitive} requires a non-empty options array`;
            }
            for (const opt of request.options) {
                if (typeof opt !== 'string') return 'options must all be strings';
            }
        }
        if (request.primitive === 'compare' && request.options.length !== 2) {
            return 'compare requires exactly two options';
        }
        if (request.primitive === 'noul' && request.options) {
            return 'noul does not take options';
        }
        if (request.primitive === 'extract') {
            if (request.topK !== undefined && (!Number.isInteger(request.topK) || request.topK < 1)) {
                return 'extract requires topK as an integer >= 1';
            }
        }
        if (request.context !== undefined && typeof request.context !== 'string') {
            return 'context must be a string';
        }
        return null;
    }

    /**
     * Invoke one real decision. Returns a normalized record. The record
     * is pushed to the history sink when one is attached; telemetry is
     * updated either way. Calls during any non-ready lifecycle state
     * (including after shutdown) are rejected before the kernel with a
     * clean AdapterNotReady record - a released handle is never used.
     *
     * @param {object} request kernel request (primitive/instruction/options/context/topK/meta).
     * @param {object} [meta] GameLab metadata {game, step, label, interpretation, replayOf}.
     * @returns {object} normalized decision record.
     */
    function invoke(request, meta) {
        decisionCounter += 1;
        const seq = decisionCounter;
        const gameMeta = meta || {};

        /* Pre-kernel validation. */
        const invalid = validateRequest(request);
        if (invalid) {
            const rec = {
                seq,
                game: gameMeta.game || 'gamelab',
                step: Number.isFinite(gameMeta.step) ? gameMeta.step : null,
                label: gameMeta.label || 'Decision',
                primitive: (request && request.primitive) || null,
                request: safeClone(request) || null,
                raw: null,
                envelope: null,
                ok: false,
                error: { kind: 'AdapterValidationError', status: null, statusName: 'ARGUMENT', message: invalid, envelope: null },
                durationMs: 0,
                interpretation: gameMeta.interpretation || null,
                replayOf: gameMeta.replayOf || null,
                rejectedBeforeKernel: true
            };
            if (telemetry) telemetry.recordDecision(rec);
            if (history) history.addDecision(rec);
            bus.emit('decision:completed', rec);
            return rec;
        }

        if (state !== 'ready' || !kernel || kernel.closed) {
            const rec = {
                seq,
                game: gameMeta.game || 'gamelab',
                step: Number.isFinite(gameMeta.step) ? gameMeta.step : null,
                label: gameMeta.label || 'Decision',
                primitive: request.primitive,
                request: safeClone(request),
                raw: null,
                envelope: null,
                ok: false,
                error: {
                    kind: 'AdapterNotReady', status: null, statusName: null,
                    message: `engine not ready (state: ${state})`, envelope: null
                },
                durationMs: 0,
                interpretation: gameMeta.interpretation || null,
                replayOf: gameMeta.replayOf || null,
                rejectedBeforeKernel: true
            };
            if (telemetry) telemetry.recordDecision(rec);
            if (history) history.addDecision(rec);
            bus.emit('decision:completed', rec);
            return rec;
        }

        bus.emit('decision:requested', { request: safeClone(request), meta: gameMeta, seq });

        /* One kernel invocation; evaluateRaw returns the byte-identical
         * envelope string, which we parse for display. Timed monotonically. */
        const t0 = timer();
        let raw = null;
        let envelope = null;
        let error = null;
        try {
            raw = kernel.evaluateRaw(request);
            envelope = JSON.parse(raw);
        } catch (err) {
            error = describeError(err);
            /* `err.envelope` is the raw C-authored error envelope string. */
            if (error.envelope === null && err && typeof err.envelope === 'string') {
                error.envelope = err.envelope;
                try { error.parsedEnvelope = JSON.parse(err.envelope); } catch { /* keep string */ }
            }
        }
        const durationMs = timer() - t0;

        const rec = {
            seq,
            game: gameMeta.game || 'gamelab',
            step: Number.isFinite(gameMeta.step) ? gameMeta.step : null,
            label: gameMeta.label || 'Decision',
            primitive: request.primitive,
            request: safeClone(request),
            raw,
            envelope,
            ok: !error,
            error,
            durationMs,
            interpretation: gameMeta.interpretation || null,
            replayOf: gameMeta.replayOf || null,
            rejectedBeforeKernel: false
        };
        if (telemetry) telemetry.recordDecision(rec);
        if (history) history.addDecision(rec);
        bus.emit('decision:completed', rec);
        return rec;
    }

    /** Convenience passthroughs to the six primitives (validated + timed). */
    function invokePrimitive(primitive, args, meta) {
        return invoke(buildRequest(primitive, args), meta);
    }

    function buildRequest(primitive, args) {
        const req = { primitive, instruction: args.instruction, context: args.context || '' };
        if (['choice', 'rank', 'compare', 'score'].includes(primitive)) req.options = args.options;
        if (primitive === 'extract') req.topK = args.topK;
        if (args.meta !== undefined) req.meta = args.meta;
        if (args.caps !== undefined) req.caps = args.caps;
        return req;
    }

    /**
     * Release the managed kernel, recover every pending-cleanup handle,
     * and return to `uninitialized`.
     *
     * Safe to call from any state and any number of times. With no
     * owned handle at all it is a no-op (no double release, no
     * duplicate event); while pending-cleanup entries exist, EVERY
     * invocation retries them - idempotency means no double free, not
     * skipping unfinished cleanup. Any in-flight init/recreate is
     * invalidated via the lifecycle epoch and its freshly created
     * handle is disposed (failures land in the pending registry).
     * Subsequent decisions fail cleanly with AdapterNotReady - a
     * released handle is never used.
     *
     * Shutdown claims completion ONLY when no adapter-owned handle
     * remains: otherwise the result reports `shutdownComplete: false`
     * / `incompleteShutdown: true` with the sanitized pending detail,
     * and another shutdown() (or retryCleanup()) retries the release.
     *
     * @returns {Promise<{state:string, released:boolean,
     *          shutdownComplete:boolean, pendingCleanup:number,
     *          cleanupError?:object, incompleteShutdown?:boolean}>}
     */
    async function shutdown() {
        epoch += 1; /* invalidates any in-flight create; stale handles self-release */
        inflight = null;
        const hadKernel = !!kernel;
        const hadPending = pendingCleanup.length > 0;
        if (!hadKernel && !hadPending && state === 'uninitialized') {
            return { state: 'uninitialized', released: false, shutdownComplete: true, pendingCleanup: 0 };
        }
        const cleanupError = releaseManagedKernel('shutdown');
        const recovery = recoverPendingCleanup('shutdown');
        setState('uninitialized');
        const remaining = pendingCleanup.length;
        const result = {
            state: 'uninitialized',
            released: hadKernel,
            shutdownComplete: remaining === 0,
            pendingCleanup: remaining
        };
        if (cleanupError) result.cleanupError = cleanupError;
        if (remaining > 0) {
            result.incompleteShutdown = true;
            result.pendingCleanupDetail = pendingCleanupDetail();
            bus.emit('engine:shutdown-incomplete', { pending: remaining });
        }
        if (hadKernel || hadPending) {
            bus.emit('engine:released', {
                cleanupError: cleanupError || null,
                shutdownComplete: result.shutdownComplete
            });
        }
        return result;
    }

    /** Safe public retry of every pending-cleanup release (Stage 2.1.1).
     * During an in-flight transition the attempt is deferred: the running
     * transition owns cleanup until it settles. */
    function retryCleanup() {
        if (state === 'initializing' || state === 'recreating') {
            return { deferred: true, state, recovered: 0, remaining: pendingCleanup.length };
        }
        const result = recoverPendingCleanup('retryCleanup');
        return Object.assign({ deferred: false, state }, result);
    }

    /** Live kernel handles in the WASM module (leak diagnostics), read
     * through the managed handle, or - while the adapter still owns a
     * pending-cleanup handle - through that. Null when nothing is owned
     * (there is no instance to ask; tests use a probe kernel). */
    function engineCount() {
        if (kernel) return kernel.engineCount();
        if (pendingCleanup.length > 0) return pendingCleanup[0].kernel.engineCount();
        return null;
    }

    function safeClone(value) {
        try { return JSON.parse(JSON.stringify(value)); } catch { return null; }
    }

    return {
        /* lifecycle */
        init,
        shutdown,
        recreate,
        /* invocation */
        invoke,
        invokePrimitive,
        validateRequest,
        /* introspection */
        get state() { return state; },
        get kernelVersion() { return kernel ? kernel.kernelVersion : null; },
        get backend() { return kernel ? kernel.backend : null; },
        get config() { return Object.assign({}, config); },
        get initMs() { return initMs; },
        get initFailureHandled() { return initFailureHandled; },
        get lastError() { return lastError; },
        /** Real lifecycle observations since adapter creation. */
        get lifecycleStats() {
            return {
                created: lifecycle.created,
                released: lifecycle.released,
                recreations: lifecycle.recreations,
                recreateFailures: lifecycle.recreateFailures,
                cleanupFailures: lifecycle.cleanupFailures,
                cleanupRecoveries: lifecycle.cleanupRecoveries,
                lastCleanupError: lifecycle.lastCleanupError,
                /* Real ownership accounting: every handle this adapter
                 * allocated is active, pending-cleanup, or confirmed-
                 * released. Exact invariant (asserted by tests):
                 * created === released + activeHandles + pendingCleanup. */
                activeHandles: kernel ? 1 : 0,
                pendingCleanup: pendingCleanup.length
            };
        },
        /** Sanitized pending-cleanup registry (no kernel references). */
        get pendingCleanupDetail() {
            return pendingCleanupDetail();
        },
        get hasPendingCleanup() {
            return pendingCleanup.length > 0;
        },
        /** Safe retry of all pending releases (see retryCleanup()). */
        retryCleanup,
        engineCount,
        PRIMITIVES,
        STATUS_NAMES
    };
}

/** Browser default engine source: globals installed by the two script tags. */
function loadBrowserEngine() {
    const g = (typeof self !== 'undefined') ? self : globalThis;
    if (!g.OmniDecide || !g.OmniDecideError) {
        return Promise.reject(new Error(
            'adapter: window.OmniDecide not found - include dist/omnidecide.js and js/omnidecide.js before GameLab'));
    }
    return Promise.resolve({ OmniDecide: g.OmniDecide, OmniDecideError: g.OmniDecideError });
}

/** Shared singleton used by views and games (wired to shared history/telemetry). */
export const adapter = createAdapter({ telemetry, history });
