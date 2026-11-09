/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * workspace.js - the reusable simulation workspace.
 *
 * Every playable game runs inside this consistent shell:
 *
 *   ┌──────────────────────────────────────────┬───────────────┐
 *   │  viewport (game-specific interface)      │ world status  │
 *   │                                          │ actors        │
 *   ├──────────────────────────────────────────┤ performance   │
 *   │  controls (game-provided)                │               │
 *   ├──────────────────────────────────────────┴───────────────┤
 *   │  event feed (chronological, clickable)                   │
 *   └──────────────────────────────────────────────────────────┘
 *
 * The game module owns the viewport and controls; the workspace owns
 * the frame, the live panels and unmount hygiene (bus listeners are
 * detached when the user navigates away - no leaks).
 */

import { el, clear, fmtMs, fmtTime } from './dom.js';
import { bus } from './eventbus.js';
import { adapter } from './adapter.js';
import { history } from './history-instance.js';
import { telemetry } from './telemetry-instance.js';

/**
 * Run a game module inside a host element.
 * @param {HTMLElement} host container for the whole workspace.
 * @param {object} gameDef registry definition (playable).
 * @param {object} session the SimSession for this run.
 * @param {object} gameInstance object created by the module's create(env).
 * @returns {{unmount: () => void}} workspace handle.
 */
export function mountWorkspace(host, gameDef, session, gameInstance) {
    const unsubs = [];

    const viewport = el('section', { class: 'ws-viewport', 'aria-label': 'Simulation viewport' });
    const statusPanel = el('section', { class: 'ws-panel ws-status', 'aria-label': 'World status' });
    const actorsPanel = el('section', { class: 'ws-panel ws-actors', 'aria-label': 'Actors' });
    const perfPanel = el('section', { class: 'ws-panel ws-perf', 'aria-label': 'Performance' });
    const feedPanel = el('section', { class: 'ws-panel ws-feed', 'aria-label': 'Event feed' });
    const controls = el('section', { class: 'ws-controls', 'aria-label': 'Simulation controls' });

    const grid = el('div', { class: 'ws-grid' },
        el('div', { class: 'ws-main' },
            viewport,
            controls),
        el('aside', { class: 'ws-side' },
            statusPanel, actorsPanel, perfPanel),
        el('div', { class: 'ws-bottom' }, feedPanel));

    const env = {
        game: gameDef,
        session,
        viewport,
        controls,
        panels: {
            status: statusPanel,
            actors: actorsPanel,
            perf: perfPanel,
            feed: feedPanel
        },
        bus,
        adapter,
        history,
        telemetry,
        emitEvent: (spec) => session.emitEvent(spec),
        decide: (request, info) => session.decide(request, info)
    };

    /* The game renders its interface into the viewport + controls. */
    gameInstance.mount(env);

    /* Live panels ----------------------------------------------------- */
    function renderStatus() {
        clear(statusPanel);
        statusPanel.append(el('h3', { class: 'panel-title' }, 'World status'));
        const snap = gameInstance.status ? gameInstance.status() : session.snapshot();
        const rows = [
            ['Session', session.id],
            ['Seed', String(session.seed)],
            ['Simulation step', String(session.clock.step)],
            ['Actions logged', String(session.actionLog.length)],
            ['Elapsed', fmtMs(session.clock.elapsedMs)]
        ];
        const titles = {
            'Seed': 'The authoritative scenario seed for this session (set in the briefing; kept by reset and replay)',
            'Actions logged': 'Successful player actions in the current run. Rejected commands and failed engine calls never count; reset starts a new run at 0; replay never adds.'
        };
        for (const [k, v] of Object.entries(snap.world || {})) rows.push([k, v]);
        statusPanel.append(el('dl', { class: 'kv' },
            rows.map(([k, v]) => [
                el('dt', {}, k),
                el('dd', { title: titles[k] || null, 'data-status-key': k }, String(v))
            ])));
    }

    function renderActors() {
        clear(actorsPanel);
        actorsPanel.append(el('h3', { class: 'panel-title' }, 'Actors'));
        const actors = gameInstance.actors ? gameInstance.actors() : [];
        if (!actors.length) {
            actorsPanel.append(el('p', { class: 'muted' }, 'No actor data in this phase.'));
            return;
        }
        const list = el('ul', { class: 'actor-list' });
        for (const a of actors) {
            list.append(el('li', { class: `actor ${a.state || ''}`.trim() },
                el('span', { class: 'actor-name' }, a.name),
                a.note ? el('span', { class: 'actor-note' }, a.note) : null));
        }
        actorsPanel.append(list);
    }

    function renderPerf() {
        clear(perfPanel);
        const snap = telemetry.snapshot();
        perfPanel.append(el('h3', { class: 'panel-title' }, 'Performance'),
            el('dl', { class: 'kv' },
                el('dt', {}, 'Decisions'), el('dd', {}, `${snap.invocations} (${snap.failed} failed)`),
                el('dt', {}, 'Avg / min / max'),
                el('dd', {}, `${fmtMs(snap.avgMs)} / ${fmtMs(snap.minMs)} / ${fmtMs(snap.maxMs)}`),
                el('dt', {}, 'Engine init'), el('dd', {}, fmtMs(adapter.initMs)),
                el('dt', {}, 'Live kernels'), el('dd', {}, String(adapter.engineCount() ?? '—')),
                el('dt', {}, 'Events'), el('dd', {}, String(snap.events))),
            el('a', { class: 'panel-link', href: '#/benchmarks' }, 'Benchmark overview →'));
    }

    function renderFeed() {
        clear(feedPanel);
        feedPanel.append(el('h3', { class: 'panel-title' }, 'Event feed'),
            el('a', { class: 'panel-link feed-link', href: '#/history' }, 'Activity history →'));
        const events = history.events({ game: gameDef.id }).slice(-60).reverse();
        const list = el('ul', { class: 'feed-list', 'aria-live': 'polite' });
        for (const ev of events) {
            list.append(el('li', { class: `feed-item ev-${ev.type.replace(/[^a-z0-9]+/gi, '-')}` },
                el('span', { class: 'feed-step' }, `s${ev.step}`),
                el('span', { class: 'feed-time' }, fmtTime(ev.wallTs)),
                el('span', { class: `feed-type t-${ev.type.split(':')[0]}` }, ev.type),
                el('span', { class: 'feed-label' }, ev.label)));
        }
        feedPanel.append(list);
    }

    renderStatus();
    renderActors();
    renderPerf();
    renderFeed();

    unsubs.push(bus.on('clock:tick', renderStatus));
    unsubs.push(bus.on('session:event', (rec) => {
        if (rec.game === gameDef.id) { renderFeed(); renderStatus(); }
    }));
    unsubs.push(bus.on('history:decision', () => { renderPerf(); }));
    unsubs.push(bus.on('telemetry:update', () => { renderPerf(); }));
    unsubs.push(bus.on('game:updated', () => { renderStatus(); renderActors(); }));

    host.append(grid);

    return {
        unmount() {
            unsubs.forEach((fn) => fn());
            try { gameInstance.unmount && gameInstance.unmount(); } catch (err) {
                console.error('workspace: game unmount failed', err);
            }
            clear(host);
        }
    };
}
