/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * about.js - About & Settings.
 *
 * Explains what the engine is (and is not), how GameLab separates
 * simulation rules from decision evaluation, and exposes working
 * settings: engine text mode / language (recreating the real kernel),
 * telemetry reset and history clear. Every control here has a real
 * effect — no decorative buttons.
 */

import { el, clear, fmtMs } from '../core/dom.js';
import { adapter } from '../core/adapter.js';
import { telemetry } from '../core/telemetry-instance.js';
import { history } from '../core/history-instance.js';
import { bus } from '../core/eventbus.js';

const MODES = ['ascii', 'unicode'];
const LANGUAGES = ['und', 'en', 'zh', 'hi', 'es', 'ar', 'fr', 'bn', 'pt', 'id', 'ur', 'auto'];

export const about = {
    id: 'about',
    title: 'About & Settings',
    route: '#/about',
    mount(host) {
        const view = el('div', { class: 'view view-about' });
        host.append(view);
        const unsubs = [bus.on('engine:state', renderEngine), bus.on('engine:ready', renderEngine)];
        let settingsMsg = '';

        function render() {
            clear(view);
            view.append(el('header', { class: 'view-head' },
                el('h1', {}, 'About & settings'),
                el('p', { class: 'lead' }, 'The engine, the architecture, the version — and the settings that actually apply.')));

            const twoCol = el('div', { class: 'about-grid' });
            twoCol.append(whatIsCard(), separationCard(), versionsCard(), limitsCard());
            view.append(twoCol);
            view.append(settingsCard());
        }

        function whatIsCard() {
            return el('section', { class: 'card' },
                el('h2', {}, 'What OmniDecide is'),
                el('p', {},
                    'OmniDecide is a deterministic lexical evidence-matching decision kernel written in C ' +
                    '(kernel 2.0.0) and compiled to WebAssembly. It evaluates textual evidence against textual ' +
                    'alternatives using the six primitives compiled into the module: ',
                    el('span', { class: 'mono' }, 'choice · score · noul · rank · compare · extract'), '.'),
                el('p', {},
                    'It is not an LLM. It has no human-like understanding of stories, images or situations; it ' +
                    'scores lexical support between the evidence you submit and the alternatives you provide. ' +
                    'Identical requests produce byte-identical envelopes on every host — that determinism is ' +
                    'verified by the Stage 1 parity suite (40/40 native↔WASM byte-identical).'),
                el('p', { class: 'muted' },
                    'Decideverse calls the engine exclusively through the project\'s zero-dependency JavaScript API ' +
                    '(omnidecide-wasm 1.0.0). No JavaScript stand-in exists: if the WASM module cannot load, ' +
                    'initialization fails visibly.'));
        }

        function separationCard() {
            return el('section', { class: 'card' },
                el('h2', {}, 'Who decides what'),
                el('ul', { class: 'feature-list' },
                    el('li', {}, el('strong', {}, 'The simulation calculates the world state.'), ' Scenario logic owns time, actors, resources and objective rules.'),
                    el('li', {}, el('strong', {}, 'Game logic determines rules and valid actions.'), ' What is possible is never changed by the engine.'),
                    el('li', {}, el('strong', {}, 'The simulation generates contextual textual evidence.'), ' What gets submitted is what the player actually discovered.'),
                    el('li', {}, el('strong', {}, 'OmniDecide evaluates the submitted alternatives.'), ' Its output is advice computed from lexical support.'),
                    el('li', {}, el('strong', {}, 'Game logic decides how to use the output.'), ' Engine advice is shown transparently and can be followed or ignored — it never silently overrides objective rules.')),
                el('p', { class: 'muted' },
                    'The Decision Inspector always shows the difference between an actual game-world outcome ' +
                    'and the decision engine\'s evaluation.'));
        }

        function versionsCard() {
            return el('section', { class: 'card versions-card' },
                el('h2', {}, 'Versions'),
                el('dl', { class: 'kv' },
                    el('dt', {}, 'Decideverse stage'), el('dd', {}, 'Stage 2 — application + The Last Detective'),
                    el('dt', {}, 'Integration'), el('dd', {}, 'omnidecide-wasm 1.0.0'),
                    el('dt', {}, 'Kernel'), el('dd', {}, `OmniDecide C kernel ${adapter.kernelVersion || '(initializing…)'}`),
                    el('dt', {}, 'Backend'), el('dd', {}, adapter.backend || '—'),
                    el('dt', {}, 'Init duration'), el('dd', {}, fmtMs(adapter.initMs)),
                    el('dt', {}, 'License'), el('dd', {}, 'Apache-2.0 (same as upstream kernel)')));
        }

        function limitsCard() {
            return el('section', { class: 'card' },
                el('h2', {}, 'Known limitations'),
                el('ul', { class: 'feature-list' },
                    el('li', {}, 'Lexical evidence matching, not reasoning: paraphrases that share few words can score low even when factually equivalent.'),
                    el('li', {}, 'No persistence: sessions, history and telemetry live in page memory and reset on reload.'),
                    el('li', {}, 'One kernel at a time in Decideverse; changing settings re-creates the handle.'),
                    el('li', {}, 'The engine cannot see images, audio or the game world — only the evidence text Decideverse submits.'),
                    el('li', {}, 'Benchmark numbers are host-specific snapshots, not guarantees.'),
                    el('li', {}, 'file:// pages are unsupported — serve over HTTP(S) with the application/wasm MIME type.')));
        }

        function settingsCard() {
            const stats = adapter.lifecycleStats;
            const card = el('section', { class: 'card settings-card' },
                el('h2', {}, 'Settings'),
                el('p', { class: 'muted' },
                    'Applying engine settings re-creates the managed kernel with the new text mode / language ' +
                    'through the real adapter: the replacement is created and confirmed first, then the previous ' +
                    'instance is released - no handle is ever abandoned. If re-creation fails, the previous ' +
                    'kernel stays active and the failure is reported here.'),
                el('div', { class: 'settings-row' },
                    el('label', { for: 'set-mode' }, 'Text mode'),
                    el('select', { id: 'set-mode', class: 'select' },
                        MODES.map((m) => el('option', {
                            value: m, selected: adapter.config.mode === m ? '' : null
                        }, m)))),
                el('div', { class: 'settings-row' },
                    el('label', { for: 'set-language' }, 'Language'),
                    el('select', { id: 'set-language', class: 'select' },
                        LANGUAGES.map((l) => el('option', {
                            value: l, selected: adapter.config.language === l ? '' : null
                        }, l)))),
                el('div', { class: 'intro-actions' },
                    el('button', {
                        class: 'btn primary',
                        onclick: async () => {
                            const mode = document.getElementById('set-mode').value;
                            const language = document.getElementById('set-language').value;
                            settingsMsg = `Re-creating engine (${mode} / ${language})…`;
                            settingsNote();
                            const result = await adapter.recreate({ mode, language });
                            if (result.recreated) {
                                settingsMsg = `Engine re-created: kernel ${adapter.kernelVersion}, backend ${adapter.backend}` +
                                    (result.cleanupError
                                        ? ' — WARNING: releasing the previous kernel failed; the handle is retained for cleanup recovery.'
                                        : '.');
                            } else if (result.error && result.error.kind === 'PendingCleanupUnresolved') {
                                settingsMsg = `Engine re-creation deferred: ${result.error.message} ` +
                                    'The current kernel stays active and usable.';
                            } else {
                                settingsMsg = `Engine re-creation failed: ${result.error ? result.error.message : 'unknown error'}. ` +
                                    'The previous kernel remains active with its previous configuration.';
                            }
                            settingsNote();
                            render();
                        }
                    }, 'Apply engine settings (re-create)'),
                    el('button', {
                        class: 'btn ghost', id: 'set-shutdown-engine',
                        onclick: async () => {
                            settingsMsg = 'Releasing the managed kernel…';
                            settingsNote();
                            const result = await adapter.shutdown();
                            if (result.released && result.shutdownComplete) {
                                settingsMsg = 'Engine released. Live managed handle: none.' +
                                    (result.cleanupError ? ' WARNING: the release itself failed.' : '');
                            } else if (result.incompleteShutdown) {
                                settingsMsg = `Engine release incomplete: ${result.pendingCleanup} kernel handle(s) could not be ` +
                                    'released and stay pending for recovery. Use “Retry pending cleanup” or release again.';
                            } else {
                                settingsMsg = 'Engine already released (nothing to release).';
                            }
                            settingsNote();
                            render();
                        }
                    }, 'Release engine (shutdown)'),
                    el('button', {
                        class: 'btn ghost', id: 'set-init-engine',
                        onclick: async () => {
                            settingsMsg = 'Initializing the engine…';
                            settingsNote();
                            const result = await adapter.init(adapter.config);
                            if (result.state === 'ready') {
                                settingsMsg = `Engine ready: kernel ${adapter.kernelVersion}, backend ${adapter.backend}.`;
                            } else if (result.error && result.error.kind === 'PendingCleanupUnresolved') {
                                settingsMsg = `Engine initialization deferred: ${result.error.message}`;
                            } else {
                                settingsMsg = `Engine initialization failed: ${adapter.lastError ? adapter.lastError.message : 'unknown error'}.`;
                            }
                            settingsNote();
                            render();
                        }
                    }, 'Initialize engine'),
                    el('button', {
                        class: 'btn ghost', id: 'set-retry-cleanup',
                        onclick: () => {
                            const result = adapter.retryCleanup();
                            if (result.deferred) {
                                settingsMsg = 'Cleanup retry deferred: a lifecycle transition is running.';
                            } else if (result.remaining > 0) {
                                settingsMsg = `Cleanup retry failed for ${result.remaining} kernel handle(s): the failure persists; ` +
                                    'the handles stay owned and pending (see the lifecycle accounting below).';
                            } else if (result.recovered > 0) {
                                settingsMsg = `Cleanup recovery released ${result.recovered} pending kernel handle(s). Live handles: ` +
                                    `${adapter.engineCount() ?? 0}.`;
                            } else {
                                settingsMsg = 'No pending cleanup: every kernel handle is released or active.';
                            }
                            settingsNote();
                            render();
                        }
                    }, 'Retry pending cleanup'),
                    el('button', { class: 'btn ghost', id: 'set-telemetry-reset', onclick: () => { telemetry.reset(); settingsMsg = 'Telemetry counters reset.'; settingsNote(); } }, 'Reset telemetry'),
                    el('button', { class: 'btn danger-ghost', id: 'set-history-clear', onclick: () => { history.clear(); settingsMsg = 'Session history cleared.'; settingsNote(); } }, 'Clear history')),
                el('p', { class: 'note settings-note', id: 'settings-note' }, settingsMsg || 'Settings apply immediately to every new decision.'),
                el('dl', { class: 'kv lifecycle-stats', id: 'engine-lifecycle-stats' },
                    el('dt', {}, 'Engine state'), el('dd', {}, adapter.state),
                    el('dt', {}, 'Live managed kernels'), el('dd', {}, String(adapter.engineCount() ?? '0')),
                    el('dt', {}, 'Kernels created / released'), el('dd', {}, `${stats.created} / ${stats.released}`),
                    el('dt', {}, 'Recreations / failures'), el('dd', {}, `${stats.recreations} / ${stats.recreateFailures}`),
                    el('dt', {}, 'Cleanup failures / recoveries'), el('dd', {}, `${stats.cleanupFailures} / ${stats.cleanupRecoveries}`),
                    el('dt', {}, 'Pending cleanup handles'), el('dd', {},
                        stats.pendingCleanup === 0
                            ? '0 (all owned handles released)'
                            : `${stats.pendingCleanup} — ${adapter.pendingCleanupDetail.map((e) => `${e.reason} (attempt ${e.attempts})`).join(', ')}`)));
            return card;
        }

        function settingsNote() {
            const note = document.getElementById('settings-note');
            if (note) note.textContent = settingsMsg || 'Settings apply immediately to every new decision.';
        }

        function renderEngine() {
            /* Versions card reflects live engine data once ready. */
            const vc = view.querySelector('.versions-card');
            if (vc) vc.replaceWith(versionsCard());
        }

        render();
        return { unmount() { unsubs.forEach((fn) => fn()); clear(host); } };
    }
};
