/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * clock.js - deterministic simulation clock.
 *
 * GameLab separates two notions of time:
 *  - simulation step: a monotonically increasing counter advanced only
 *    by explicit `tick()` calls. Every event and decision is stamped
 *    with the step at which it happened; replay restores the same
 *    step sequence.
 *  - wall time: real host time, captured when an event is recorded.
 *
 * The clock itself never advances on its own: deterministic execution
 * means no hidden timers drive the simulation.
 */

import { bus } from './eventbus.js';

export class SimClock {
    constructor() {
        this._step = 0;
        this._startedAt = null;
        this._accumulatedMs = 0;
        this._running = false;
    }

    /** Begin a new run: step counter back to zero, session timer started. */
    reset() {
        this._step = 0;
        this._startedAt = null;
        this._accumulatedMs = 0;
        this._running = false;
        bus.emit('clock:reset', { step: 0 });
    }

    /** Start (or resume) the wall-time session timer. */
    start() {
        if (this._running) return;
        this._startedAt = (typeof performance !== 'undefined') ? performance.now() : Date.now();
        this._running = true;
    }

    /** Pause the wall-time session timer. */
    pause() {
        if (!this._running) return;
        this._accumulatedMs += ((typeof performance !== 'undefined') ? performance.now() : Date.now()) - this._startedAt;
        this._startedAt = null;
        this._running = false;
    }

    /**
     * Advance one simulation step.
     * @returns {number} the new step.
     */
    tick() {
        this._step += 1;
        bus.emit('clock:tick', { step: this._step });
        return this._step;
    }

    /** Current simulation step. */
    get step() {
        return this._step;
    }

    /** Whether the wall timer is running. */
    get running() {
        return this._running;
    }

    /** Elapsed wall milliseconds attributed to the current session. */
    get elapsedMs() {
        const live = this._running
            ? ((typeof performance !== 'undefined') ? performance.now() : Date.now()) - this._startedAt
            : 0;
        return this._accumulatedMs + live;
    }
}
