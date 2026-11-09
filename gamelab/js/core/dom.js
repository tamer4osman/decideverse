/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * dom.js - tiny DOM helpers for the GameLab UI.
 *
 * GameLab is deliberately dependency-free: these helpers replace a
 * framework. `el()` builds elements declaratively; `fmt*` helpers keep
 * rendering consistent across views.
 */

/**
 * Create an element.
 * @param {string} tag HTML tag name.
 * @param {object} [attrs] attributes (`class` accepted as className alias).
 * @param {...(Node|string|null|undefined|Array)} children children to append.
 * @returns {HTMLElement}
 */
export function el(tag, attrs, ...children) {
    const node = document.createElement(tag);
    if (attrs) {
        for (const [key, value] of Object.entries(attrs)) {
            if (value === null || value === undefined || value === false) continue;
            if (key === 'class') node.className = value;
            else if (key === 'dataset') Object.assign(node.dataset, value);
            else if (key.startsWith('on') && typeof value === 'function') {
                node.addEventListener(key.slice(2).toLowerCase(), value);
            } else if (key === 'html') {
                node.innerHTML = value; /* trusted, authored markup only */
            } else {
                node.setAttribute(key, String(value));
            }
        }
    }
    append(node, children);
    return node;
}

function append(node, children) {
    for (const child of children) {
        if (child === null || child === undefined || child === false) continue;
        if (Array.isArray(child)) { append(node, child); continue; }
        node.append(child.nodeType ? child : document.createTextNode(String(child)));
    }
}

/** Remove all children of a node. */
export function clear(node) {
    while (node.firstChild) node.removeChild(node.firstChild);
    return node;
}

/** Escape a string for safe interpolation into non-trusted contexts. */
export function esc(text) {
    const div = document.createElement('div');
    div.textContent = String(text);
    return div.innerHTML;
}

/** Format milliseconds for display (e.g. "1.84 ms"). */
export function fmtMs(ms) {
    if (ms === null || ms === undefined || Number.isNaN(ms)) return '—';
    if (ms >= 100) return `${ms.toFixed(1)} ms`;
    if (ms >= 1) return `${ms.toFixed(2)} ms`;
    return `${(ms * 1000).toFixed(0)} µs`;
}

/** Format a wall-clock timestamp as HH:MM:SS. */
export function fmtTime(ts) {
    const d = new Date(ts);
    const p = (n) => String(n).padStart(2, '0');
    return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}`;
}

/** Clamp helper used by panels. */
export function clamp(n, lo, hi) {
    return Math.min(hi, Math.max(lo, n));
}

/** Monotonic wall timer (performance.now when present). */
export function now() {
    return (typeof performance !== 'undefined' && typeof performance.now === 'function')
        ? performance.now()
        : Date.now();
}
