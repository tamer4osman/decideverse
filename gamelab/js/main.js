/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * main.js - GameLab bootstrap: navigation, router and engine startup.
 *
 * Loads the real WASM engine through the adapter, then drives a small
 * hash router. Views are plain ES modules with mount/unmount so each
 * route cleans up after itself (no listener leaks). The engine
 * initialization starts immediately — the dashboard reflects live
 * readiness state, including visible failure with retry.
 */

import { el, clear } from './core/dom.js';
import { adapter } from './core/adapter.js';
import { bus } from './core/eventbus.js';
import { dashboard } from './views/dashboard.js';
import { launcher } from './views/launcher.js';
import { simulation } from './views/simulation.js';
import { inspector } from './views/inspector.js';
import { historyview } from './views/historyview.js';
import { benchmarks } from './views/benchmarks.js';
import { about } from './views/about.js';
import './games/index.js'; /* registers the five game modules */

const ROUTES = [dashboard, launcher, simulation, inspector, historyview, benchmarks, about];

const NAV = [
    { href: '#/dashboard', label: 'Dashboard', id: 'dashboard' },
    { href: '#/launcher', label: 'Launcher', id: 'launcher' },
    { href: '#/inspector', label: 'Inspector', id: 'inspector' },
    { href: '#/history', label: 'History', id: 'history' },
    { href: '#/benchmarks', label: 'Benchmarks', id: 'benchmarks' },
    { href: '#/about', label: 'About', id: 'about' }
];

/* ------------------------------------------------------------------ */
/* Shell                                                               */
/* ------------------------------------------------------------------ */

const app = document.getElementById('app');

const shell = el('div', { class: 'shell' },
    el('header', { class: 'topbar' },
        el('a', { class: 'brand', href: '#/dashboard' },
            el('span', { class: 'brand-mark', 'aria-hidden': 'true' }),
            el('span', { class: 'brand-name' }, 'Decide', el('span', { class: 'accent' }, 'verse'))),
        el('nav', { class: 'main-nav', 'aria-label': 'Primary' },
            NAV.map((n) => el('a', { href: n.href, 'data-nav': n.id }, n.label))),
        el('div', { class: 'topbar-status' },
            el('span', { id: 'engine-pill', class: 'status-pill st-uninitialized' }, 'engine: uninitialized'),
            el('span', { id: 'kernel-pill', class: 'muted small' }, ''))),
    el('main', { id: 'view', class: 'view-host', tabindex: '-1' }),
    el('footer', { class: 'footer' },
        el('span', {}, 'Decideverse — powered by omnidecide-wasm 1.0.0 · deterministic decisions in the browser'),
        el('span', { class: 'footer-links' },
            el('a', { href: 'https://github.com/tamer4osman/decideverse' }, 'repository'),
            el('a', { href: '#/about' }, 'about the engine'))));

app.append(shell);

/* ------------------------------------------------------------------ */
/* Router                                                              */
/* ------------------------------------------------------------------ */

let current = { view: null, unsubNav: [] };

function parseHash() {
    const raw = location.hash.replace(/^#\/?/, '');
    const [pathPart, queryPart] = raw.split('?');
    const segments = pathPart.split('/').filter(Boolean);
    const params = Object.fromEntries(new URLSearchParams(queryPart || '').entries());
    return { segments, params };
}

function resolveRoute(segments) {
    if (segments.length === 0) return { view: dashboard, params: {} };
    const [head, ...rest] = segments;
    if (head === 'sim' && rest.length) {
        return { view: simulation, params: { gameId: rest[0] } };
    }
    const route = ROUTES.find((r) => r.id === head);
    if (route && route !== simulation) return { view: route, params: {} };
    return { view: dashboard, params: {} };
}

function renderRoute() {
    const { segments, params } = parseHash();
    const { view, params: routeParams } = resolveRoute(segments, params);

    /* Tear down the previous view. */
    if (current.view && typeof current.view.unmount === 'function') {
        try { current.view.unmount(); } catch (err) { console.error('router: unmount failed', err); }
    }

    const host = document.getElementById('view');
    clear(host);
    const mounted = view.mount(host, routeParams);
    current = { view: mounted, unsubNav: [] };

    /* Highlight nav. */
    document.querySelectorAll('.main-nav a').forEach((a) => {
        const isActive = (segments[0] || 'dashboard') === a.dataset.nav ||
            (a.dataset.nav === 'launcher' && segments[0] === 'sim');
        a.classList.toggle('active', isActive);
    });
    document.querySelector('.view-host').focus({ preventScroll: true });
    window.scrollTo({ top: 0 });
}

window.addEventListener('hashchange', renderRoute);

/* ------------------------------------------------------------------ */
/* Engine status pills                                                 */
/* ------------------------------------------------------------------ */

const pill = document.getElementById('engine-pill');
const kernelPill = document.getElementById('kernel-pill');

function renderPill(state, payload) {
    pill.className = `status-pill st-${state}`;
    const labels = {
        uninitialized: 'engine: uninitialized',
        initializing: 'engine: loading WASM…',
        loading: 'engine: loading WASM…', /* legacy alias */
        ready: 'engine: ready',
        recreating: 'engine: re-creating kernel…',
        'shutting-down': 'engine: releasing kernel…',
        error: 'engine: error'
    };
    pill.textContent = labels[state] || `engine: ${state}`;
    if (state === 'ready' && adapter.state === 'ready') {
        kernelPill.textContent = `kernel ${adapter.kernelVersion} · ${adapter.backend}`;
    } else if (state === 'error') {
        kernelPill.textContent = payload && payload.message
            ? payload.message.slice(0, 64) : 'initialization failed';
    } else {
        kernelPill.textContent = '';
    }
}

bus.on('engine:state', ({ state, payload }) => renderPill(state, payload));
bus.on('engine:ready', () => renderPill('ready'));

/* ------------------------------------------------------------------ */
/* Startup                                                             */
/* ------------------------------------------------------------------ */

if (!location.hash) location.replace('#/dashboard');
renderRoute();
adapter.init({ mode: 'ascii', language: 'und' }).then(() => renderRoute());

/* Test hook (mirrors the Stage 1 __OD_APP__ pattern). */
window.__GAMELAB__ = {
    adapter,
    bus,
    version: '1.0.0',
    routes: ROUTES.map((r) => r.id),
    navigate(hash) { location.hash = hash; renderRoute(); },
    /* Diagnostic access to the live SimSession of a game (default:
     * detective) - lets browser tests assert authoritative session
     * state (seed, action log) instead of trusting rendered text. */
    liveSession(gameId) {
        const entry = simulation._liveSessions.get(gameId || 'detective');
        return entry ? entry.session : null;
    }
};
