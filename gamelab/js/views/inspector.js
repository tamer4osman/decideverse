/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * inspector.js - the Decision Inspector.
 *
 * Every actual OmniDecide invocation is inspectable here. The detail
 * view strictly separates three things:
 *
 *   SUBMITTED INPUT  - what GameLab sent to the kernel.
 *   ENGINE OUTPUT    - what the real WASM integration returned
 *                      (parsed envelope + raw byte-identical string).
 *   GAME INTERPRETATION - how the simulation used that output.
 *
 * The inspector never fabricates explanations of the kernel's internal
 * reasoning: it shows the data the adapter captured and the game's
 * declared interpretation.
 */

import { el, clear, fmtMs, fmtTime, esc } from '../core/dom.js';
import { history } from '../core/history-instance.js';
import { adapter } from '../core/adapter.js';
import { bus } from '../core/eventbus.js';

export const inspector = {
    id: 'inspector',
    title: 'Decision Inspector',
    route: '#/inspector',
    mount(host, params) {
        const view = el('div', { class: 'view view-inspector' });
        host.append(view);
        const unsubs = [bus.on('history:decision', () => renderList())];

        const selectedId = params.id || null;

        function renderList() {
            clear(view);
            view.append(el('header', { class: 'view-head' },
                el('h1', {}, 'Decision Inspector'),
                el('p', { class: 'lead' },
                    'Every actual OmniDecide invocation: the exact submitted request, the exact engine ' +
                    'response, and how the simulation used the result.')));

            if (adapter.state !== 'ready') {
                view.append(el('div', { class: `engine-strip st-${adapter.state}` },
                    `Engine state: ${adapter.state}`, adapter.lastError ? ` — ${adapter.lastError.message}` : ''));
            }

            const decisions = history.decisions().slice().reverse();
            const listCard = el('section', { class: 'card inspector-list-card' },
                el('div', { class: 'card-head' },
                    el('h2', {}, `Decisions (${decisions.length})`),
                    el('span', { class: 'muted' }, 'Newest first')));
            if (!decisions.length) {
                listCard.append(el('p', { class: 'muted' },
                    'No decisions yet. Decisions appear here the moment a simulation submits a request ' +
                    'to the engine — or when an input is rejected by validation.'));
            } else {
                const tbl = el('table', { class: 'data-table' },
                    el('thead', {}, el('tr', {},
                        el('th', {}, 'ID'), el('th', {}, 'Time'), el('th', {}, 'Sim / step'),
                        el('th', {}, 'Primitive'), el('th', {}, 'Label'),
                        el('th', {}, 'Result'), el('th', {}, 'Duration'), el('th', {}, ''))),
                    el('tbody', {}, decisions.map((d) => row(d))));
                listCard.append(tbl);
            }
            view.append(listCard);

            if (selectedId) {
                const rec = history.decision(selectedId);
                if (rec) view.append(detailCard(rec));
                else view.append(el('section', { class: 'card' },
                    el('p', { class: 'muted' }, `No decision with id ${selectedId} in this session history.`)));
            } else {
                const last = decisions[0];
                if (last) view.append(detailCard(last));
            }
        }

        function row(d) {
            return el('tr', { class: d.ok ? 'ok' : 'err' },
                el('td', { class: 'mono' }, d.id),
                el('td', {}, fmtTime(d.wallTs)),
                el('td', {}, `${d.game}${d.step !== null ? ` · s${d.step}` : ''}`),
                el('td', {}, el('span', { class: 'pill p-decision' }, d.primitive || '—')),
                el('td', {}, d.label),
                el('td', {}, d.ok
                    ? el('span', { class: 'result-ok' }, summarizeResult(d.envelope))
                    : el('span', { class: 'result-err' }, d.error ? d.error.kind : 'error')),
                el('td', { class: 'mono' }, fmtMs(d.durationMs)),
                el('td', {}, el('a', { class: 'btn ghost small', href: `#/inspector?id=${d.id}` }, 'Inspect')));
        }

        function summarizeResult(env) {
            if (!env) return '—';
            if (env.primitive === 'extract' && Array.isArray(env.result)) {
                return `top-${env.result.length} extracted`;
            }
            if (typeof env.result === 'boolean') return env.result ? 'true' : 'false';
            if (typeof env.result === 'number') return String(env.result);
            if (typeof env.result === 'string') {
                return env.result.length > 42 ? env.result.slice(0, 42) + '…' : env.result;
            }
            return JSON.stringify(env.result).slice(0, 42);
        }

        function detailCard(d) {
            const card = el('section', { class: 'card inspector-detail', id: 'decision-detail' });
            card.append(el('div', { class: 'card-head' },
                el('h2', {}, `Decision ${d.id}`),
                el('div', { class: 'detail-badges' },
                    el('span', { class: `status-pill ${d.ok ? 'st-ready' : 'st-error'}` }, d.ok ? 'engine response' : 'error'),
                    d.replayOf ? el('span', { class: 'pill p-replay' }, `replay of ${d.replayOf}`) : null,
                    d.rejectedBeforeKernel ? el('span', { class: 'pill p-reject' }, 'rejected before kernel') : null)));

            /* Meta strip --------------------------------------------------- */
            card.append(el('dl', { class: 'kv two-col meta-strip' },
                el('dt', {}, 'Simulation'), el('dd', {}, d.game),
                el('dt', {}, 'Simulation step'), el('dd', {}, d.step === null ? '—' : String(d.step)),
                el('dt', {}, 'Decision type'), el('dd', {}, d.primitive || '—'),
                el('dt', {}, 'Wall clock'), el('dd', {}, fmtTime(d.wallTs)),
                el('dt', {}, 'Execution duration'), el('dd', {}, fmtMs(d.durationMs)),
                el('dt', {}, 'Kernel'), el('dd', {},
                    adapter.state === 'ready' ? `${adapter.kernelVersion} · ${adapter.backend}` : '—')));

            /* 1. SUBMITTED INPUT ------------------------------------------- */
            const input = el('div', { class: 'io-block' },
                el('h3', { class: 'io-title io-input' }, 'Submitted input'),
                el('p', { class: 'io-caption' }, 'The exact request Decideverse sent to the OmniDecide kernel.'),
                el('pre', { class: 'code' }, JSON.stringify(d.request, null, 2)));
            card.append(input);

            /* 2. ENGINE OUTPUT ---------------------------------------------- */
            const out = el('div', { class: 'io-block' },
                el('h3', { class: 'io-title io-output' }, 'Engine output'),
                el('p', { class: 'io-caption' },
                    'What the real WASM integration returned — the C-authored envelope and its raw bytes.'));
            if (d.ok) {
                out.append(renderEnvelope(d.envelope),
                    el('details', { class: 'raw-details' },
                        el('summary', {}, 'Raw envelope string (byte-identical across hosts)'),
                        el('pre', { class: 'code' }, d.raw)));
            } else {
                out.append(el('div', { class: 'error-box', role: 'alert' },
                    el('strong', {}, `${d.error.kind}`),
                    d.error.statusName ? el('span', {}, ` [${d.error.statusName}] `) : el('span', {}, ' '),
                    el('span', {}, d.error.message)));
                if (d.error.envelope) {
                    out.append(el('details', { class: 'raw-details' },
                        el('summary', {}, 'Raw error envelope from the C wrapper'),
                        el('pre', { class: 'code' }, d.error.envelope)));
                }
            }
            card.append(out);

            /* 3. GAME INTERPRETATION ---------------------------------------- */
            const interp = d.interpretation || {};
            card.append(el('div', { class: 'io-block' },
                el('h3', { class: 'io-title io-interpretation' }, 'Game interpretation'),
                el('p', { class: 'io-caption' },
                    'How the simulation used this output — declared by the game module, not by the kernel.'),
                interp.summary
                    ? el('p', { class: 'interp-summary' }, interp.summary)
                    : el('p', { class: 'muted' }, 'No interpretation was recorded for this decision.'),
                interp.applied
                    ? el('p', { class: 'interp-applied' }, el('strong', {}, 'Effect on the simulation: '), interp.applied)
                    : null));

            return card;
        }

        function renderEnvelope(env) {
            const wrap = el('div', { class: 'envelope-view' });
            /* Winner / result line. */
            if (env.primitive === 'extract' && Array.isArray(env.result)) {
                wrap.append(el('div', { class: 'env-result' },
                    el('span', { class: 'env-key' }, 'extracted'),
                    el('ol', { class: 'extract-list' },
                        env.result.map((s) => el('li', {}, String(s))))));
            } else if (typeof env.result === 'boolean') {
                wrap.append(el('div', { class: 'env-result' },
                    el('span', { class: 'env-key' }, 'statement holds'),
                    el('span', { class: env.result ? 'bool-true' : 'bool-false' }, String(env.result))));
            } else if (env.primitive === 'score') {
                wrap.append(el('div', { class: 'env-result' },
                    el('span', { class: 'env-key' }, 'score'),
                    el('span', { class: 'mono' }, String(env.result))));
            } else {
                wrap.append(el('div', { class: 'env-result' },
                    el('span', { class: 'env-key' }, env.primitive === 'rank' ? 'ranking' : 'winner'),
                    el('span', { class: 'env-winner' }, String(env.result))));
            }
            /* Confidence. */
            if (typeof env.confidence === 'number') {
                wrap.append(el('div', { class: 'env-conf' },
                    el('span', { class: 'env-key' }, 'confidence'),
                    el('span', { class: 'mono' }, env.confidence.toFixed(4)),
                    el('div', { class: 'conf-bar', role: 'img', 'aria-label': `confidence ${env.confidence.toFixed(4)}` },
                        el('div', { class: 'conf-fill', style: `width:${(env.confidence * 100).toFixed(1)}%` }))));
            }
            /* Distribution (choice/compare/rank). */
            if (env.distribution && typeof env.distribution === 'object') {
                wrap.append(el('div', { class: 'env-dist' },
                    el('span', { class: 'env-key' }, 'distribution'),
                    el('div', { class: 'dist-rows' },
                        Object.entries(env.distribution)
                            .sort((a, b) => b[1] - a[1])
                            .map(([k, v]) => el('div', { class: 'dist-row' },
                                el('span', { class: 'dist-label' }, k),
                                el('div', { class: 'dist-bar' },
                                    el('div', { class: 'dist-fill', style: `width:${Math.max(2, v * 100).toFixed(1)}%` })),
                                el('span', { class: 'dist-val mono' }, v.toFixed(4)))))));
            }
            /* Extract list and meta block. */
            if (env.meta) {
                wrap.append(el('details', { class: 'meta-details' },
                    el('summary', {}, 'Accounting metadata (meta: true)'),
                    el('pre', { class: 'code' }, JSON.stringify(env.meta, null, 2))));
            }
            return wrap;
        }

        renderList();

        return {
            unmount() { unsubs.forEach((fn) => fn()); clear(host); }
        };
    }
};
