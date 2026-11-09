/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * dashboard.js - GameLab landing view.
 *
 * Real data only: engine status comes from the live adapter, decision
 * statistics from telemetry, recent activity from history. When the
 * engine failed to initialize the dashboard shows the failure
 * prominently with a retry action - never a fake "ready" state.
 */

import { el, clear, fmtMs, fmtTime } from '../core/dom.js';
import { adapter } from '../core/adapter.js';
import { history } from '../core/history-instance.js';
import { telemetry } from '../core/telemetry-instance.js';
import { allGames } from '../core/registry.js';
import { bus } from '../core/eventbus.js';

export const dashboard = {
    id: 'dashboard',
    title: 'Dashboard',
    route: '#/dashboard',
    mount(host) {
        const view = el('div', { class: 'view view-dashboard' });
        host.append(view);

        const unsubs = [
            bus.on('engine:state', render),
            bus.on('engine:ready', render),
            bus.on('telemetry:update', renderStats),
            bus.on('history:event', renderRecent),
            bus.on('history:decision', renderStats)
        ];

        function render() {
            clear(view);
            view.append(hero());
            view.append(engineCard());
            view.append(statsRow());
            view.append(simGrid());
            view.append(recentCard());
        }

        function renderStats() {
            /* Re-render only the stats strip for cheap live updates. */
            const strip = view.querySelector('.stats-row');
            if (strip) strip.replaceWith(statsRow());
        }

        function renderRecent() {
            const card = view.querySelector('.recent-card');
            if (card) card.replaceWith(recentCard());
        }

        /* ---------------------------------------------------------------- */
        function hero() {
            return el('section', { class: 'hero' },
                el('div', { class: 'hero-mark' },
                    el('span', { class: 'hero-logo', 'aria-hidden': 'true', html: htmlLogo() }),
                    el('h1', {}, 'Decide', el('span', { class: 'accent' }, 'verse')),
                    el('p', { class: 'tagline' },
                        'Interactive simulations where deterministic, evidence-based decision evaluation drives games.')),
                el('div', { class: 'hero-actions' },
                    el('a', { class: 'btn primary', href: '#/launcher' }, 'Open launcher'),
                    el('a', { class: 'btn ghost', href: '#/sim/detective' }, 'Play The Last Detective'),
                    el('a', { class: 'btn ghost', href: '#/about' }, 'About the engine')));
        }

        function engineCard() {
            const state = adapter.state;
            const card = el('section', {
                class: `card engine-card st-${state}`, 'aria-label': 'Engine status'
            });
            card.append(el('div', { class: 'card-head' },
                el('h2', {}, 'Decision engine'),
                el('span', { class: `status-pill st-${state}` }, state)));

            if (state === 'ready') {
                card.append(el('dl', { class: 'kv two-col' },
                    el('dt', {}, 'Kernel'), el('dd', {}, `OmniDecide C kernel ${adapter.kernelVersion} (WebAssembly)`),
                    el('dt', {}, 'Backend'), el('dd', {}, adapter.backend),
                    el('dt', {}, 'Integration'), el('dd', {}, 'omnidecide-wasm 1.0.0 — zero-dependency UMD API'),
                    el('dt', {}, 'Initialization'), el('dd', {}, `${fmtMs(adapter.initMs)} (${adapter.config.mode} mode, language "${adapter.config.language}")`),
                    el('dt', {}, 'Live kernels'), el('dd', {}, String(adapter.engineCount() ?? '—')),
                    el('dt', {}, 'Primitives'), el('dd', {}, adapter.PRIMITIVES.join(' · '))));
                card.append(el('p', { class: 'note' },
                    'Every decision in Decideverse is produced by the real compiled C kernel. No JavaScript fallback exists.'));
            } else if (state === 'initializing' || state === 'loading' || state === 'recreating') {
                card.append(el('p', { class: 'note' }, state === 'recreating'
                    ? 'Re-creating the kernel: the replacement is created and confirmed before the previous instance is released…'
                    : 'Loading WebAssembly module and creating the kernel…'));
            } else if (state === 'shutting-down') {
                card.append(el('p', { class: 'note' }, 'Releasing the managed kernel handle…'));
            } else if (state === 'error') {
                card.append(el('div', { class: 'error-box', role: 'alert' },
                    el('strong', {}, 'Engine initialization failed. '),
                    el('span', {}, adapter.lastError ? adapter.lastError.message : 'Unknown error.'),
                    el('p', { class: 'note' },
                        'Without the real WASM engine there are no decisions, no simulations and no fallback. ' +
                        'Serve this page over HTTP(S) with dist/omnidecide.wasm reachable.')),
                    el('button', {
                        class: 'btn primary', onclick: () => adapter.init(adapter.config)
                    }, 'Retry initialization'));
            } else {
                card.append(el('p', { class: 'note' }, 'Engine not initialized yet.'),
                    el('button', { class: 'btn primary', onclick: () => adapter.init(adapter.config) }, 'Initialize engine'));
            }
            return card;
        }

        function statsRow() {
            const snap = telemetry.snapshot();
            const counts = history.counts();
            const stat = (value, label, cls) => el('div', { class: `stat ${cls || ''}` },
                el('span', { class: 'stat-value' }, value),
                el('span', { class: 'stat-label' }, label));
            return el('div', { class: 'stats-row' },
                stat(String(snap.invocations), 'Decision invocations'),
                stat(snap.invocations ? `${Math.round((snap.success / snap.invocations) * 100)}%` : '—', 'Successful'),
                stat(snap.avgMs !== null ? fmtMs(snap.avgMs) : '—', 'Avg decision time'),
                stat(String(counts.events), 'Simulation events'),
                stat(String(counts.session), 'Session #'));
        }

        function simGrid() {
            const games = allGames();
            return el('section', { class: 'card', 'aria-label': 'Available simulations' },
                el('div', { class: 'card-head' },
                    el('h2', {}, 'Simulations'),
                    el('a', { class: 'panel-link', href: '#/launcher' }, 'Launcher →')),
                el('div', { class: 'mini-game-grid' },
                    games.map((g) => el('a', {
                        class: `mini-game st-${g.status}`,
                        href: g.status === 'playable' ? `#/sim/${g.id}` : '#/launcher',
                        style: `--game-accent: ${g.accent}`
                    },
                        el('span', { class: 'mini-game-name' }, g.name),
                        el('span', { class: 'mini-game-status' },
                            g.status === 'playable' ? 'Playable' : 'Planned')))));
        }

        function recentCard() {
            const card = el('section', { class: 'card recent-card', 'aria-label': 'Recent activity' },
                el('div', { class: 'card-head' },
                    el('h2', {}, 'Recent activity'),
                    el('a', { class: 'panel-link', href: '#/history' }, 'History →')));
            const events = history.events().slice(-7).reverse();
            const decisions = history.decisions().slice(-3).reverse();
            if (!events.length && !decisions.length) {
                card.append(el('p', { class: 'muted' },
                    'Nothing yet. Launch a simulation and its events and decisions will appear here.'));
                return card;
            }
            const list = el('ul', { class: 'recent-list' });
            for (const d of decisions) {
                list.append(el('li', { class: 'recent-item' },
                    el('span', { class: `pill p-decision ${d.ok ? 'ok' : 'err'}` }, d.primitive),
                    el('span', { class: 'recent-label' }, d.label),
                    el('span', { class: 'recent-meta' }, `${d.game} · ${fmtTime(d.wallTs)}`),
                    el('a', { class: 'panel-link', href: `#/inspector?id=${d.id}` }, 'inspect')));
            }
            for (const ev of events) {
                list.append(el('li', { class: 'recent-item' },
                    el('span', { class: `pill p-event` }, ev.type),
                    el('span', { class: 'recent-label' }, ev.label),
                    el('span', { class: 'recent-meta' }, `${ev.game} · step ${ev.step} · ${fmtTime(ev.wallTs)}`)));
            }
            card.append(list);
            return card;
        }

        render();

        return {
            unmount() { unsubs.forEach((fn) => fn()); clear(host); }
        };
    }
};

function htmlLogo() {
    /* Inline SVG: crosshair + decision fork — Decideverse wordmark. */
    return `<svg viewBox="0 0 48 48" width="46" height="46" role="img" aria-label="Decideverse logo">
      <defs>
        <linearGradient id="glg" x1="0" y1="0" x2="1" y2="1">
          <stop offset="0" stop-color="#39d0c4"/><stop offset="1" stop-color="#2273b8"/>
        </linearGradient>
      </defs>
      <rect x="2" y="2" width="44" height="44" rx="10" fill="none" stroke="url(#glg)" stroke-width="2.5"/>
      <circle cx="24" cy="24" r="4" fill="url(#glg)"/>
      <path d="M24 20V9M24 28v11M20 24H9M28 24h11" stroke="url(#glg)" stroke-width="2.5" stroke-linecap="round"/>
      <circle cx="9" cy="9" r="2.4" fill="#39d0c4"/><circle cx="39" cy="9" r="2.4" fill="#2273b8"/>
      <circle cx="9" cy="39" r="2.4" fill="#2273b8"/><circle cx="39" cy="39" r="2.4" fill="#39d0c4"/>
    </svg>`;
}
