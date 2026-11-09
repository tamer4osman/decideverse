/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * registry.js - GameLab game-module registry.
 *
 * Each game registers a definition. Playable modules provide a
 * `create(env)` factory that the workspace calls with a simulation
 * environment; planned modules carry roadmap metadata instead. Adding
 * a new game = adding a module under js/games/<id>/ and one registry
 * entry - no shell changes required.
 */

import { bus } from './eventbus.js';

const games = new Map();

/**
 * Register a game definition.
 * @param {{id:string, name:string, tagline:string, status:'playable'|'planned',
 *          accent:string, icon:string, blurb:string,
 *          features?:string[], plannedFor?:string,
 *          create?:(env:object)=>object}} def
 */
export function registerGame(def) {
    if (!def || !def.id) throw new TypeError('registry: game definition requires an id');
    if (games.has(def.id)) throw new Error(`registry: duplicate game id "${def.id}"`);
    if (def.status === 'playable' && typeof def.create !== 'function') {
        throw new Error(`registry: playable game "${def.id}" requires a create(env) factory`);
    }
    games.set(def.id, Object.freeze(Object.assign({}, def)));
    bus.emit('registry:changed', { id: def.id });
}

export function getGame(id) {
    return games.get(id) || null;
}

export function allGames() {
    return [...games.values()];
}

export function playableGames() {
    return allGames().filter((g) => g.status === 'playable');
}

export function plannedGames() {
    return allGames().filter((g) => g.status === 'planned');
}
