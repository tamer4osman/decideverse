/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * history.js - session history for simulation events and decisions.
 *
 * Two distinguishable record streams:
 *  - events:   scenario lifecycle and world happenings (typed, with
 *              simulation step, wall timestamp and metadata).
 *  - decisions: every actual OmniDecide invocation with the submitted
 *              request, the raw engine envelope and the game's stated
 *              interpretation.
 *
 * Both streams are ring buffers with a fixed capacity so long sessions
 * cannot grow memory without bound. Records carry monotonically
 * increasing ids for stable cross-references ("see decision D-0007").
 */

import { bus } from './eventbus.js';

const EVENT_TYPES = [
    'scenario:started', 'scenario:completed', 'scenario:reset',
    'evidence:discovered', 'suspect:interviewed', 'location:investigated',
    'decision:requested', 'decision:completed', 'lead:selected',
    'world:changed', 'session:note'
];

export class HistoryStore {
    /**
     * @param {number} eventCapacity ring capacity for events.
     * @param {number} decisionCapacity ring capacity for decisions.
     */
    constructor(eventCapacity = 600, decisionCapacity = 300) {
        this._eventCap = eventCapacity;
        this._decisionCap = decisionCapacity;
        this._events = [];
        this._decisions = [];
        this._nextEventId = 1;
        this._nextDecisionId = 1;
        this._session = 1;
    }

    /** Current session number (bumped on scenario reset). */
    get session() {
        return this._session;
    }

    nextSession() {
        this._session += 1;
        bus.emit('history:session', { session: this._session });
        return this._session;
    }

    /**
     * Record a simulation event.
     * @param {{step:number, type:string, game:string, label:string,
     *          detail?:string, meta?:object}} spec event spec.
     * @returns {object} the stored event record.
     */
    addEvent(spec) {
        if (!spec || typeof spec.type !== 'string') {
            throw new TypeError('history: event requires a type');
        }
        const rec = {
            id: `E-${String(this._nextEventId++).padStart(4, '0')}`,
            session: this._session,
            wallTs: Date.now(),
            step: Number.isFinite(spec.step) ? spec.step : 0,
            game: spec.game || 'gamelab',
            type: spec.type,
            label: spec.label || spec.type,
            detail: spec.detail || '',
            meta: spec.meta || null
        };
        this._events.push(rec);
        if (this._events.length > this._eventCap) this._events.shift();
        bus.emit('history:event', rec);
        return rec;
    }

    /**
     * Record one actual engine decision invocation.
     * @param {object} rec normalized decision record (see adapter).
     * @returns {object} the stored decision record.
     */
    addDecision(rec) {
        const stored = Object.assign({}, rec, {
            id: `D-${String(this._nextDecisionId++).padStart(4, '0')}`,
            session: this._session,
            wallTs: Date.now()
        });
        this._decisions.push(stored);
        if (this._decisions.length > this._decisionCap) this._decisions.shift();
        bus.emit('history:decision', stored);
        return stored;
    }

    /** Chronological events (oldest first), optionally filtered. */
    events(filter) {
        let list = this._events;
        if (filter) {
            if (filter.game) list = list.filter((e) => e.game === filter.game);
            if (filter.type) list = list.filter((e) => e.type === filter.type);
            if (filter.session) list = list.filter((e) => e.session === filter.session);
        }
        return list.slice();
    }

    /** Chronological decisions (oldest first), optionally filtered. */
    decisions(filter) {
        let list = this._decisions;
        if (filter) {
            if (filter.game) list = list.filter((d) => d.game === filter.game);
            if (filter.primitive) list = list.filter((d) => d.primitive === filter.primitive);
            if (filter.session) list = list.filter((d) => d.session === filter.session);
            if (filter.ok !== undefined) list = list.filter((d) => d.ok === filter.ok);
            if (filter.replay !== undefined) list = list.filter((d) => !!d.replay === filter.replay);
        }
        return list.slice();
    }

    /** Look up one decision by id. */
    decision(id) {
        return this._decisions.find((d) => d.id === id) || null;
    }

    /** Look up one event by id. */
    event(id) {
        return this._events.find((e) => e.id === id) || null;
    }

    /** Drop all records (Settings → "Clear history"). */
    clear() {
        this._events = [];
        this._decisions = [];
        bus.emit('history:cleared', {});
    }

    /** Counters for dashboards. */
    counts() {
        return {
            events: this._events.length,
            decisions: this._decisions.length,
            session: this._session
        };
    }
}

/** Known event types (used for filters and labels). */
export function knownEventTypes() {
    return EVENT_TYPES.slice();
}
