/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * simulation.js - the #/sim/:gameId route.
 *
 * Owns the per-game session lifecycle: creates a SimSession and the
 * game instance, mounts the shared workspace around them, and keeps
 * the live session alive while the user browses other GameLab areas
 * (so they can inspect decisions and history without killing the run).
 * Planned games render their roadmap scope instead of a workspace.
 */

import { el, clear } from '../core/dom.js';
import { getGame } from '../core/registry.js';
import { SimSession } from '../core/session.js';
import { mountWorkspace } from '../core/workspace.js';
import { adapter } from '../core/adapter.js';

/* One live session per game id, kept across navigation. */
const liveSessions = new Map(); /* gameId -> {session, gameInstance, mounted:boolean} */

export const simulation = {
    id: 'simulation',
    title: 'Simulation',
    route: '#/sim/:gameId',
    mount(host, params) {
        const view = el('div', { class: 'view view-sim' });
        host.append(view);

        const gameDef = getGame(params.gameId);
        if (!gameDef) {
            view.append(el('section', { class: 'card' },
                el('h1', {}, 'Unknown simulation'),
                el('p', { class: 'muted' }, `No game module is registered under "${params.gameId}".`),
                el('a', { class: 'btn primary', href: '#/launcher' }, 'Back to launcher')));
            return { unmount() { clear(host); } };
        }

        if (gameDef.status !== 'playable') {
            view.append(el('header', { class: 'view-head' },
                el('h1', {}, gameDef.name),
                el('p', { class: `status-pill st-planned` }, gameDef.plannedFor || 'Planned')));
            view.append(el('section', { class: 'card' },
                el('p', {}, gameDef.blurb),
                el('ul', { class: 'feature-list' }, (gameDef.features || []).map((f) => el('li', {}, f))),
                el('p', { class: 'note' },
                    'Not implemented in Stage 2. Decideverse never ships a fake playable version — ' +
                    'see the roadmap in gamelab/docs/ROADMAP.md.'),
                el('a', { class: 'btn ghost', href: '#/launcher' }, 'Back to launcher')));
            return { unmount() { clear(host); } };
        }

        /* Live session handling ------------------------------------------ */
        let live = liveSessions.get(gameDef.id);
        let workspaceHandle = null;

        function render() {
            clear(view);
            const session = live ? live.session : null;
            view.append(el('header', { class: 'view-head sim-head' },
                el('div', {},
                    el('h1', {}, gameDef.name),
                    el('p', { class: 'lead' }, gameDef.tagline)),
                el('div', { class: 'sim-head-meta' },
                    session
                        ? el('span', { class: 'status-pill st-ready' },
                            `Session ${session.id.replace(gameDef.id + '-', '#')} · step ${session.clock.step}`)
                        : el('span', { class: 'status-pill st-idle' }, 'No active session'),
                    session
                        ? el('a', { class: 'btn ghost small', href: '#/inspector' }, 'Decision Inspector')
                        : null)));

            if (!live) {
                view.append(startCard());
            } else {
                /* Auto-resume the live session across navigation. */
                live.mounted = true;
                const wsHost = el('div', { class: 'ws-host' });
                view.append(wsHost);
                workspaceHandle = mountWorkspace(wsHost, gameDef, live.session, live.gameInstance);
            }
        }

        function startCard() {
            const engineReady = adapter.state === 'ready';
            const card = el('section', { class: 'card intro-card' },
                el('h2', {}, 'Start a session'),
                el('p', {}, gameDef.blurb),
                el('ul', { class: 'feature-list' },
                    (gameDef.features || []).map((f) => el('li', {}, f))),
                el('p', { class: 'muted small' },
                    'The simulation runs in a shared workspace with an event feed, actor panel and ' +
                    'full decision inspection. A deterministic seed drives the scenario; ' +
                    'the same seed and the same actions reproduce the same outcomes.'),
                engineReady
                    ? el('div', { class: 'intro-actions' },
                        el('button', {
                            class: 'btn primary', id: 'dt-start-simulation',
                            onclick: () => { startSession(gameDef); render(); }
                        }, 'Open case file — start session'),
                        el('span', { class: 'muted intro-note' }, 'Engine ready: every decision runs the real WASM kernel.'))
                    : el('div', { class: 'error-box', role: 'alert' },
                        el('strong', {}, 'Engine not ready. '),
                        el('span', {}, 'The decision engine must be initialized before a session can make real decisions. '),
                        el('a', { class: 'panel-link', href: '#/dashboard' }, 'Check engine status →')));
            return card;
        }

        function startSession(def) {
            const session = new SimSession({ game: def.id, seed: def.defaultSeed || 20261108 });
            const gameInstance = def.create();
            liveSessions.set(def.id, { session, gameInstance, mounted: true });
            live = liveSessions.get(def.id);
            gameInstance.start(session);
        }

        function discardSession(def) {
            const cur = liveSessions.get(def.id);
            if (cur && cur.gameInstance && cur.gameInstance.destroy) {
                try { cur.gameInstance.destroy(); } catch (err) { console.error(err); }
            }
            liveSessions.delete(def.id);
            live = null;
        }

        render();

        return {
            unmount() {
                if (workspaceHandle) workspaceHandle.unmount();
                /* Keep the session alive across navigation unless discarded. */
                if (live) live.mounted = false;
                clear(host);
            }
        };
    },

    /* Session access for tests and other views. */
    _liveSessions: liveSessions
};
