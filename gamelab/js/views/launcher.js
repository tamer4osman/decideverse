/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * launcher.js - simulation launcher.
 *
 * Presents the five Decideverse games. Two are playable (The Last
 * Detective, Living Village); the other three render as honest "Planned"
 * modules with their roadmap scope - clicking one opens the roadmap
 * detail, never a fake playable version.
 */

import { el, clear } from '../core/dom.js';
import { allGames } from '../core/registry.js';

export const launcher = {
    id: 'launcher',
    title: 'Simulation Launcher',
    route: '#/launcher',
    mount(host) {
        const view = el('div', { class: 'view view-launcher' });
        host.append(view);

        function render() {
            clear(view);
            view.append(el('header', { class: 'view-head' },
                el('h1', {}, 'Simulation launcher'),
                el('p', { class: 'lead' },
                    'Five games share one simulation framework and one real decision engine. ' +
                    'The Last Detective and Living Village are playable today; ' +
                    'the other three are planned scopes, not playable stubs.')));

            const grid = el('div', { class: 'launcher-grid' });
            for (const g of allGames()) grid.append(gameCard(g));
            view.append(grid);
            view.append(detailPanel());
        }

        function gameCard(g) {
            const playable = g.status === 'playable';
            const card = el('article', {
                class: `game-card ${playable ? 'playable' : 'planned'}`,
                style: `--game-accent: ${g.accent}`
            },
                el('div', { class: 'game-icon', 'aria-hidden': 'true', html: g.icon }),
                el('div', { class: 'game-card-body' },
                    el('h2', { class: 'game-name' }, g.name),
                    el('p', { class: 'game-tagline' }, g.tagline),
                    el('p', { class: 'game-blurb muted' }, g.blurb),
                    el('ul', { class: 'game-features' },
                        (g.features || []).map((f) => el('li', {}, f)))),
                el('div', { class: 'game-card-foot' },
                    el('span', { class: `status-pill st-${playable ? 'ready' : 'planned'}` },
                        playable ? `Playable — ${g.playableStage || 'Stage 2'}` : (g.plannedFor || 'Planned')),
                    playable
                        ? el('a', { class: 'btn primary', href: `#/sim/${g.id}` }, 'Open simulation')
                        : el('a', { class: 'btn ghost', href: `#/launcher?planned=${g.id}` }, 'Scope')));
            return card;
        }

        function detailPanel() {
            const planned = new URLSearchParams(location.hash.split('?')[1] || '').get('planned');
            const g = planned ? allGames().find((x) => x.id === planned) : null;
            if (!g) return el('section', { class: 'card roadmap-note' },
                el('h2', {}, 'Adding a game module'),
                el('p', { class: 'muted' },
                    'Each game owns its scenario rules and entities; the workspace, event feed, decision ' +
                    'inspector, history and benchmarks are shared. A new game is one module under ' +
                    'gamelab/js/games/<id>/ plus a registry entry — see gamelab/docs/GAME-MODULES.md.'));
            return el('section', { class: 'card planned-detail', id: 'planned-detail' },
                el('div', { class: 'card-head' },
                    el('h2', {}, `${g.name} — planned scope`),
                    el('span', { class: 'status-pill st-planned' }, g.plannedFor || 'Planned')),
                el('p', {}, g.blurb),
                el('ul', { class: 'feature-list' }, (g.features || []).map((f) => el('li', {}, f))),
                el('p', { class: 'note' },
                    'This panel describes the roadmap. Decideverse does not ship placeholder gameplay for ' +
                    'unimplemented games.'));
        }

        render();
        return { unmount() { clear(host); } };
    }
};
