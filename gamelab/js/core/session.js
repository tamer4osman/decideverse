/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * session.js - the shared simulation environment for game modules.
 *
 * A session binds one game module to the shared framework pieces:
 * simulation clock, deterministic RNG, history, telemetry, the
 * OmniDecide adapter and a convenience `decide()` helper that routes
 * decision requests through the real WASM engine while stamping them
 * with game/step metadata and the game's stated interpretation.
 *
 * Games never call the kernel directly - they go through the session,
 * so every actual invocation is recorded, timed and inspectable.
 *
 * Authoritative state (Stage 2.1):
 *  - `seed` is the ONE authoritative scenario seed for this session.
 *    Games take it at start(), change it only through setSeed() (which
 *    rebuilds the derived RNG streams), and the workspace displays this
 *    exact value. It survives reset and drives replay.
 *  - `_log` (actionLog) records SUCCESSFUL PLAYER ACTIONS only - one
 *    entry per accepted player command (begin, investigate, decide,
 *    accuse, close-unsolved). Rejected commands, failed decision
 *    invocations, simulation events and kernel executions are NOT
 *    player actions: they live in the event/decision streams instead.
 *    The workspace "Actions logged" counter displays
 *    `session.actionLog.length` - by definition it matches this log.
 *    A scenario reset starts a new run and clears the log; replay is
 *    an inspection capability and never appends to it.
 */

import { SimClock } from './clock.js';
import { mulberry32, deriveSeed } from './rng.js';
import { adapter } from './adapter.js';
import { history } from './history-instance.js';
import { telemetry } from './telemetry-instance.js';
import { bus } from './eventbus.js';

export class SimSession {
    /**
     * @param {{game:string, seed:number}} spec session identity.
     */
    constructor(spec) {
        this.game = spec.game;
        this.seed = spec.seed >>> 0;
        this.id = `${spec.game}-s${history.nextSession()}`;
        this.clock = new SimClock();
        this.rng = mulberry32(this.seed);
        this._log = []; /* successful player actions (authoritative counter) */
        this.active = false;
        this.startedAt = Date.now();
    }

    /**
     * Replace the authoritative scenario seed (Stage 2.1).
     * Normalizes to an unsigned 32-bit integer, rebuilds the derived RNG
     * stream, and is the ONLY sanctioned way for a game to change the
     * session seed - this keeps session, scenario, workspace display,
     * history and replay in agreement.
     * @param {number} seed new seed (unsigned 32-bit integer).
     * @returns {number} the normalized seed now in force.
     */
    setSeed(seed) {
        this.seed = Number(seed) >>> 0;
        this.rng = mulberry32(this.seed);
        return this.seed;
    }

    /** Deterministic sub-stream for one step. */
    rngFor(step) {
        return mulberry32(deriveSeed(this.seed, step));
    }

    /** Advance the simulation clock one step. */
    tick() {
        return this.clock.tick();
    }

    /**
     * Record one SUCCESSFUL player action into the shared action history.
     * Called exactly once per accepted player command by the game module;
     * rejected commands and failed engine invocations never reach this
     * method, so no double counting is possible.
     * @param {{action:string, args?:object}} entry
     */
    logAction(entry) {
        this._log.push({
            action: entry.action,
            args: entry.args || null,
            step: this.clock.step
        });
    }

    /**
     * Start a fresh action history for a new scenario run (reset/begin).
     * Documented counter semantics: the workspace counter reflects the
     * current run; resetting the scenario starts a new run at zero.
     */
    clearActionLog() {
        this._log = [];
    }

    /** The recorded successful player actions (for inspection/tests). */
    get actionLog() {
        return this._log.slice();
    }

    /** Number of successful player actions in the current run. */
    get actionCount() {
        return this._log.length;
    }

    /**
     * Emit a simulation event (event feed + history + telemetry).
     * @param {{type:string, label:string, detail?:string, meta?:object}} spec
     * @returns {object} stored event record.
     */
    emitEvent(spec) {
        const rec = history.addEvent({
            game: this.game,
            step: this.clock.step,
            type: spec.type,
            label: spec.label,
            detail: spec.detail || '',
            meta: spec.meta || null
        });
        telemetry.recordEvent();
        bus.emit('session:event', rec);
        return rec;
    }

    /**
     * Run one REAL OmniDecide decision and record everything about it.
     *
     * @param {{primitive:string, instruction:string, options?:string[],
     *          context?:string, topK?:number, meta?:boolean, caps?:object}} request
     * @param {{label:string, interpretation:{summary:string, applied:string}, replayOf?:string}} info
     * @returns {object} the normalized decision record (also in history).
     */
    decide(request, info) {
        const record = adapter.invoke(request, {
            game: this.game,
            step: this.clock.step,
            label: info.label,
            interpretation: info.interpretation,
            replayOf: info.replayOf || null
        });
        return record;
    }

    /** Session snapshot for panels. */
    snapshot() {
        return {
            id: this.id,
            game: this.game,
            seed: this.seed,
            step: this.clock.step,
            active: this.active,
            actions: this._log.length,
            elapsedMs: this.clock.elapsedMs,
            startedAt: this.startedAt
        };
    }
}
