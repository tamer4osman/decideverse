/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * benchmarks.js - Benchmark Overview.
 *
 * Shows the actual measurements collected while this application runs:
 * decision invocation counts, success/failure, min/avg/max execution
 * times measured with the monotonic performance timer, event counts
 * and the recent duration samples. Nothing here is imported from the
 * Stage 1 benchmark files — this is live, session-local telemetry.
 */

import { el, clear, fmtMs } from '../core/dom.js';
import { telemetry } from '../core/telemetry-instance.js';
import { history } from '../core/history-instance.js';
import { adapter } from '../core/adapter.js';
import { bus } from '../core/eventbus.js';

export const benchmarks = {
    id: 'benchmarks',
    title: 'Benchmark Overview',
    route: '#/benchmarks',
    mount(host) {
        const view = el('div', { class: 'view view-benchmarks' });
        host.append(view);
        const unsubs = [bus.on('telemetry:update', render)];

        function render() {
            clear(view);
            const snap = telemetry.snapshot();

            view.append(el('header', { class: 'view-head' },
                el('h1', {}, 'Benchmark overview'),
                el('p', { class: 'lead' },
                    'Live measurements gathered during this Decideverse session — decision invocations are timed ' +
                    'around the actual kernel call with a monotonic timer (performance.now). ' +
                    'Stage 1 release benchmarks live in docs/benchmarks/ and are never mixed in here.')));

            const cards = el('div', { class: 'bench-grid' });
            cards.append(
                benchCard('Decision invocations', [
                    ['Total', String(snap.invocations)],
                    ['Successful', String(snap.success)],
                    ['Failed', String(snap.failed)],
                    ['Rejected before kernel', String(countRejected())]
                ]),
                benchCard('Execution time', [
                    ['Average', fmtMs(snap.avgMs)],
                    ['Minimum', fmtMs(snap.minMs)],
                    ['Maximum', fmtMs(snap.maxMs)],
                    ['Engine init', fmtMs(snap.initMs)]
                ]),
                benchCard('Simulation activity', [
                    ['Simulation events', String(snap.events)],
                    ['Live kernel handles', String(adapter.engineCount() ?? '—')],
                    ['Engine state', adapter.state],
                    ['Text mode', `${adapter.config.mode} / ${adapter.config.language}`]
                ]));
            view.append(cards);

            view.append(samplesCard(snap));
            view.append(el('section', { class: 'card' },
                el('h2', {}, 'Method'),
                el('p', { class: 'muted' },
                    'Every decision invocation is bracketed by performance.now() calls in the adapter — ' +
                    'the measurement covers string marshaling into the WASM heap, the C wrapper call and ' +
                    'the JSON envelope parse, i.e. exactly what an application pays per decision. ' +
                    'The isolated-C raw loop is a Stage 1 benchmark concept and is intentionally not re-measured here. ' +
                    'Counters reset when the page reloads, or on demand:'),
                el('button', { class: 'btn ghost', onclick: () => telemetry.reset() }, 'Reset telemetry')));
        }

        function countRejected() {
            /* Rejections happen in the adapter before the kernel runs. */
            return history.decisions().filter((d) => d.rejectedBeforeKernel).length;
        }

        function benchCard(title, rows) {
            return el('section', { class: 'card bench-card' },
                el('h2', {}, title),
                el('dl', { class: 'kv' },
                    rows.map(([k, v]) => [el('dt', {}, k), el('dd', { class: 'mono' }, v)])));
        }

        function samplesCard(snap) {
            const card = el('section', { class: 'card' },
                el('h2', {}, `Recent decision durations (${snap.samples.length} samples)`),
                el('p', { class: 'muted' }, 'Each bar is one actual kernel invocation in this session.'));
            if (!snap.samples.length) {
                card.append(el('p', { class: 'muted' }, 'No samples yet.'));
                return card;
            }
            const max = Math.max(...snap.samples);
            const bars = el('div', { class: 'bench-bars' },
                snap.samples.map((ms) => el('div', {
                    class: 'bench-bar', title: `${ms.toFixed(3)} ms`,
                    style: `height:${Math.max(4, (ms / max) * 100).toFixed(1)}%`,
                    'aria-label': `${ms.toFixed(3)} ms`
                })));
            card.append(el('div', { class: 'bench-bars-wrap' }, bars),
                el('p', { class: 'mono muted bench-scale' }, `0 – ${fmtMs(max)}`));
            return card;
        }

        render();
        return { unmount() { unsubs.forEach((fn) => fn()); clear(host); } };
    }
};
