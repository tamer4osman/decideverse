/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * rng.js - deterministic seeded pseudo-random number generator.
 *
 * GameLab scenarios must be reproducible: the same seed plus the same
 * action sequence produces identical outcomes. mulberry32 is small,
 * fast and fully deterministic across hosts. It is used for cosmetic
 * variety inside scenarios only - never for decision outcomes, which
 * are pure scenario logic plus the OmniDecide kernel.
 */

/**
 * Create a seeded generator.
 * @param {number} seed 32-bit integer seed.
 * @returns {() => number} function producing floats in [0, 1).
 */
export function mulberry32(seed) {
    let a = seed >>> 0;
    return function () {
        a |= 0;
        a = (a + 0x6D2B79F5) | 0;
        let t = Math.imul(a ^ (a >>> 15), 1 | a);
        t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

/**
 * Derive a child seed from a base seed and a step counter so each
 * simulation step has its own deterministic stream.
 * @param {number} seed base seed.
 * @param {number} step simulation step.
 * @returns {number} derived 32-bit seed.
 */
export function deriveSeed(seed, step) {
    let h = (seed ^ (step * 0x9E3779B9)) >>> 0;
    h = Math.imul(h ^ (h >>> 16), 0x45d9f3b) >>> 0;
    h = Math.imul(h ^ (h >>> 16), 0x45d9f3b) >>> 0;
    return (h ^ (h >>> 16)) >>> 0;
}
