/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * historyview.js - Activity History.
 *
 * Chronological, distinguishable streams of simulation events and
 * decision records. Events and decisions are different record types;
 * each row carries simulation step, wall time, type and metadata.
 * Decisions link straight into the Decision Inspector.
 */

import { el, clear, fmtTime, fmtMs } from '../core/dom.js';
import { history } from '../core/history-instance.js';
import { bus } from '../core/eventbus.js';

export const historyview = {
    id: 'history',
    title: 'Activity History',
    route: '#/history',
    mount(host) {
        const view = el('div', { class: 'view view-history' });
        host.append(view);
        const unsubs = [
            bus.on('history:event', render),
            bus.on('history:decision', render),
            bus.on('history:cleared', render)
        ];

        let tab = 'events';
        let typeFilter = '';

        function render() {
            clear(view);
            view.append(el('header', { class: 'view-head' },
                el('h1', {}, 'Activity history'),
                el('p', { class: 'lead' },
                    'Chronological record of this browser session: simulation events and actual ' +
                    'decision invocations, kept as separate, distinguishable streams.')));

            const counts = history.counts();
            const tabs = el('div', { class: 'tab-row', role: 'tablist' },
                el('button', {
                    class: `tab ${tab === 'events' ? 'active' : ''}`, role: 'tab',
                    onclick: () => { tab = 'events'; render(); }
                }, `Simulation events (${counts.events})`),
                el('button', {
                    class: `tab ${tab === 'decisions' ? 'active' : ''}`, role: 'tab',
                    onclick: () => { tab = 'decisions'; render(); }
                }, `Decisions (${counts.decisions})`));

            const types = tab === 'events'
                ? [...new Set(history.events().map((e) => e.type))].sort()
                : [...new Set(history.decisions().map((d) => d.primitive || 'rejected'))].sort();
            const select = el('select', {
                class: 'select filter-select', 'aria-label': 'Filter by type',
                onchange: (e) => { typeFilter = e.target.value; render(); }
            }, el('option', { value: '' }, 'All types'),
                types.map((t) => el('option', {
                    value: t, selected: t === typeFilter ? '' : null
                }, t)));

            view.append(el('div', { class: 'toolbar' }, tabs, select));

            if (tab === 'events') renderEvents(view);
            else renderDecisions(view);
        }

        function renderEvents(view) {
            const events = history.events(typeFilter ? { type: typeFilter } : null).slice().reverse();
            const card = el('section', { class: 'card' },
                el('p', { class: 'muted' },
                    `${events.length} event${events.length === 1 ? '' : 's'} (newest first). Events describe what happened in the simulation world.`));
            if (!events.length) {
                card.append(el('p', { class: 'muted' }, 'No events recorded yet.'));
            } else {
                const list = el('ol', { class: 'history-list' });
                for (const ev of events) {
                    list.append(el('li', { class: 'history-item' },
                        el('div', { class: 'hi-row1' },
                            el('span', { class: 'mono hi-id' }, ev.id),
                            el('span', { class: 'pill p-event' }, ev.type),
                            el('span', { class: 'hi-label' }, ev.label),
                            el('span', { class: 'hi-meta' },
                                `session ${ev.session} · step ${ev.step} · ${fmtTime(ev.wallTs)}`)),
                        ev.detail ? el('div', { class: 'hi-detail' }, ev.detail) : null));
                }
                card.append(list);
            }
            view.append(card);
        }

        function renderDecisions(view) {
            const filter = typeFilter ? { primitive: typeFilter } : null;
            const decisions = history.decisions(filter).slice().reverse();
            const card = el('section', { class: 'card' },
                el('p', { class: 'muted' },
                    `${decisions.length} decision${decisions.length === 1 ? '' : 's'} (newest first). Decisions are actual kernel invocations — click one to open it in the Decision Inspector.`));
            if (!decisions.length) {
                card.append(el('p', { class: 'muted' }, 'No decisions recorded yet.'));
            } else {
                const list = el('ol', { class: 'history-list' });
                for (const d of decisions) {
                    list.append(el('li', { class: 'history-item clickable' },
                        el('a', { class: 'history-link', href: `#/inspector?id=${d.id}` },
                            el('div', { class: 'hi-row1' },
                                el('span', { class: 'mono hi-id' }, d.id),
                                el('span', { class: `pill p-decision ${d.ok ? 'ok' : 'err'}` }, d.primitive || 'rejected'),
                                el('span', { class: 'hi-label' }, d.label),
                                el('span', { class: 'hi-meta' },
                                    `session ${d.session} · step ${d.step === null ? '—' : d.step} · ${fmtMs(d.durationMs)}`)),
                            el('div', { class: 'hi-detail' },
                                d.ok
                                    ? `result: ${summarize(d)} · confidence ${d.envelope && typeof d.envelope.confidence === 'number' ? d.envelope.confidence.toFixed(4) : '—'}`
                                    : (d.error ? `error: ${d.error.message}` : 'error')))));
                }
                card.append(list);
            }
            view.append(card);
        }

        function summarize(d) {
            if (!d.envelope) return '—';
            const r = d.envelope.result;
            if (Array.isArray(r)) return `[${r.length} items]`;
            if (typeof r === 'string') return r.length > 60 ? r.slice(0, 60) + '…' : r;
            return String(r);
        }

        render();
        return { unmount() { unsubs.forEach((fn) => fn()); clear(host); } };
    }
};
