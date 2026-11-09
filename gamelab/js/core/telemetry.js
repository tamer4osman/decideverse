/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * telemetry.js - runtime measurements collected during app execution.
 *
 * Every number shown in the Benchmark Overview comes from this module:
 * decision invocations measured with a monotonic timer
 * (performance.now) around the actual kernel call, plus event counts.
 * Nothing here is loaded from old benchmark files and nothing is
 * invented; a fresh session starts at zero.
 *
 * Memory-safe: counters are scalars; duration samples beyond the
 * recent-samples cap are dropped (only aggregate statistics grow).
 */

import { bus } from './eventbus.js';

const MAX_SAMPLES = 500;

export class Telemetry {
    constructor() {
        this.reset();
    }

    reset() {
        this._invocations = 0;
        this._success = 0;
        this._failed = 0;
        this._totalMs = 0;
        this._minMs = null;
        this._maxMs = null;
        this._events = 0;
        this._samples = []; /* last N decision durations */
        this._initMs = null;   /* engine initialization duration */
        bus.emit('telemetry:update', this.snapshot());
    }

    /**
     * Record one completed decision invocation.
     * @param {{ok: boolean, durationMs: number}} rec normalized decision record.
     */
    recordDecision(rec) {
        this._invocations += 1;
        if (rec.ok) this._success += 1;
        else this._failed += 1;
        const ms = Number(rec.durationMs);
        if (Number.isFinite(ms) && ms >= 0) {
            this._totalMs += ms;
            if (this._minMs === null || ms < this._minMs) this._minMs = ms;
            if (this._maxMs === null || ms > this._maxMs) this._maxMs = ms;
            this._samples.push(ms);
            if (this._samples.length > MAX_SAMPLES) this._samples.shift();
        }
        bus.emit('telemetry:update', this.snapshot());
    }

    /** Record one simulation event emission. */
    recordEvent() {
        this._events += 1;
        bus.emit('telemetry:update', this.snapshot());
    }

    /** Record engine initialization duration. */
    recordInit(ms) {
        this._initMs = ms;
        bus.emit('telemetry:update', this.snapshot());
    }

    /**
     * Immutable snapshot for the Benchmark Overview.
     * @returns {{invocations:number, success:number, failed:number,
     *   avgMs:number|null, minMs:number|null, maxMs:number|null,
     *   events:number, initMs:number|null, samples:number[]}}
     */
    snapshot() {
        return {
            invocations: this._invocations,
            success: this._success,
            failed: this._failed,
            avgMs: this._invocations > 0 ? this._totalMs / this._invocations : null,
            minMs: this._minMs,
            maxMs: this._maxMs,
            events: this._events,
            initMs: this._initMs,
            samples: this._samples.slice(-64)
        };
    }
}
