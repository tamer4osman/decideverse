/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * eventbus.js - minimal publish/subscribe bus shared by GameLab modules.
 *
 * Subscriptions return an unsubscribe function so views can detach on
 * unmount (prevents listener leaks). Emitted errors are isolated per
 * handler: one faulty subscriber never breaks the others.
 */

export class EventBus {
    constructor() {
        this._topics = new Map();
    }

    /**
     * Subscribe to a topic.
     * @param {string} topic topic name (e.g. 'engine:ready', 'history:event').
     * @param {(payload: any) => void} handler handler function.
     * @returns {() => void} unsubscribe function (idempotent).
     */
    on(topic, handler) {
        if (typeof handler !== 'function') {
            throw new TypeError('eventbus: handler must be a function');
        }
        if (!this._topics.has(topic)) this._topics.set(topic, new Set());
        const set = this._topics.get(topic);
        set.add(handler);
        let active = true;
        return () => {
            if (!active) return;
            active = false;
            set.delete(handler);
        };
    }

    /**
     * Publish a payload to all subscribers of a topic.
     * @param {string} topic topic name.
     * @param {any} payload event payload.
     */
    emit(topic, payload) {
        const set = this._topics.get(topic);
        if (!set) return;
        for (const handler of [...set]) {
            try {
                handler(payload);
            } catch (err) {
                /* Isolate subscriber failures; report without throwing. */
                console.error(`eventbus: handler for "${topic}" failed`, err);
            }
        }
    }

    /** Number of live subscribers across all topics (leak diagnostics). */
    subscriberCount() {
        let n = 0;
        for (const set of this._topics.values()) n += set.size;
        return n;
    }
}

/** The GameLab-wide bus instance. */
export const bus = new EventBus();
