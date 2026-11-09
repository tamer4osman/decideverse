/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * app.js - OmniDecide WASM playground controller.
 *
 * Drives the real WebAssembly engine through the OmniDecide JS API and
 * exposes a small window.__OD_APP__ surface used by the Playwright
 * browser verification suite (tests/browser_check.mjs).
 */

(function () {
    'use strict';

    var els = {
        status: document.getElementById('status'),
        version: document.getElementById('engine-version'),
        backend: document.getElementById('engine-backend'),
        count: document.getElementById('engine-count'),
        errorBox: document.getElementById('error-box'),
        decision: document.getElementById('decision'),
        primitive: document.getElementById('decision-primitive'),
        winner: document.getElementById('decision-winner'),
        confidence: document.getElementById('decision-confidence'),
        distribution: document.getElementById('distribution'),
        metaDetails: document.getElementById('meta-details'),
        metaPre: document.getElementById('meta-pre'),
        envelopeDetails: document.getElementById('envelope-details'),
        envelopePre: document.getElementById('envelope-pre'),
        determinism: document.getElementById('determinism-result'),
        benchmark: document.getElementById('benchmark-result'),
        footer: document.getElementById('footer-note')
    };

    var state = {
        kernel: null,
        usingWasm: false,
        lastRequest: null,
        lastEnvelope: null,
        initError: null,
        quickstartRan: false,
        unicodeRan: false,
        invalidRan: false,
        determinismRan: false,
        benchmarkRan: false,
        releaseRan: false,
        initFailureHandled: false
    };

    function setStatus(kind, text) {
        els.status.textContent = text;
        els.status.className = 'status-pill ' + kind;
    }

    function showError(err) {
        els.errorBox.classList.remove('hidden');
        els.errorBox.textContent = 'OmniDecideError [status ' + err.status + ']: ' + err.message;
    }

    function hideError() {
        els.errorBox.classList.add('hidden');
    }

    var PRESETS = {
        quickstart: {
            primitive: 'choice',
            instruction: 'How should we fix the outage?',
            options: ['restart the service', 'reinstall the operating system',
                      'wait for the weekend'],
            context: 'The service stopped responding. Restarting the service ' +
                     'restored normal operation within seconds.',
            meta: true
        },
        unicode: {
            primitive: 'choice',
            instruction: '备份服务器应该放在哪里？',
            options: ['柏林数据中心', '远程分公司'],
            context: '备份服务器应该放在柏林数据中心，因为那里有冗余电源。',
            meta: true
        },
        invalid: {
            primitive: 'choice',
            instruction: 'Pick one option.',
            options: [],
            context: 'The service stopped responding.',
            meta: true
        }
    };

    function renderEnvelope(envelope) {
        els.decision.classList.remove('hidden');
        els.primitive.textContent = envelope.primitive;
        var resultText;
        if (typeof envelope.result === 'boolean') {
            resultText = envelope.result ? 'supported' : 'not supported';
        } else if (Array.isArray(envelope.result)) {
            resultText = envelope.result.join(' | ');
        } else {
            resultText = String(envelope.result);
        }
        els.winner.textContent = resultText;
        els.confidence.textContent =
            'confidence ' + envelope.confidence.toFixed(4) +
            ' / probability ' + envelope.probability.toFixed(4);

        els.distribution.innerHTML = '';
        var entries = Object.keys(envelope.distribution).map(function (k) {
            return { label: k, value: envelope.distribution[k] };
        });
        var max = 0;
        entries.forEach(function (e) { if (e.value > max) max = e.value; });
        entries.forEach(function (e) {
            var row = document.createElement('div');
            row.className = 'dist-row';
            var label = document.createElement('span');
            label.className = 'dist-label';
            label.textContent = e.label;
            var barWrap = document.createElement('span');
            barWrap.className = 'dist-bar-wrap';
            var bar = document.createElement('span');
            bar.className = 'dist-bar';
            bar.style.width = Math.max(2, Math.round(100 * e.value / (max || 1))) + '%';
            var value = document.createElement('span');
            value.className = 'dist-value';
            value.textContent = e.value.toFixed(4);
            barWrap.appendChild(bar);
            row.appendChild(label);
            row.appendChild(barWrap);
            row.appendChild(value);
            els.distribution.appendChild(row);
        });

        if (envelope.meta) {
            els.metaDetails.classList.remove('hidden');
            els.metaPre.textContent = JSON.stringify(envelope.meta, null, 2);
        } else {
            els.metaDetails.classList.add('hidden');
        }
        els.envelopeDetails.classList.remove('hidden');
        els.envelopePre.textContent = state.lastEnvelope || JSON.stringify(envelope);
    }

    function runRequest(request) {
        hideError();
        state.lastRequest = request;
        var raw = state.kernel.evaluateRaw(request);
        state.lastEnvelope = raw;
        renderEnvelope(JSON.parse(raw));
        return raw;
    }

    function runPreset(name) {
        var request = PRESETS[name];
        if (name === 'quickstart') state.quickstartRan = true;
        if (name === 'unicode') state.unicodeRan = true;
        if (name === 'invalid') state.invalidRan = true;
        var raw = runRequest(request);
        return raw;
    }

    function runDeterminism() {
        hideError();
        var request = state.lastRequest || PRESETS.quickstart;
        var envelopes = [];
        for (var i = 0; i < 5; i++) {
            envelopes.push(state.kernel.evaluateRaw(request));
        }
        var allEqual = envelopes.every(function (e) { return e === envelopes[0]; });
        els.determinism.classList.remove('hidden');
        els.determinism.textContent = allEqual ?
            'determinism: 5/5 identical envelopes' :
            'determinism: MISMATCH DETECTED';
        els.determinism.className = allEqual ? 'ok-banner' : 'fail-banner';
        state.determinismRan = true;
        return allEqual;
    }

    function runReplay() {
        hideError();
        if (!state.lastRequest) {
            els.determinism.classList.remove('hidden');
            els.determinism.className = 'fail-banner';
            els.determinism.textContent = 'replay: no previous request';
            return false;
        }
        var before = state.lastEnvelope;
        var after = state.kernel.evaluateRaw(state.lastRequest);
        els.determinism.classList.remove('hidden');
        var same = before === after;
        els.determinism.textContent = same ?
            'replay: byte-identical to previous run' :
            'replay: MISMATCH vs previous run';
        els.determinism.className = same ? 'ok-banner' : 'fail-banner';
        return same;
    }

    function runBenchmark(scale) {
        hideError();
        var report = window.OdBenchmark.run(state.kernel, scale || 'quick');
        els.benchmark.classList.remove('hidden');
        var lines = ['benchmark scale: ' + report.scale];
        Object.keys(report.workloads).forEach(function (id) {
            var w = report.workloads[id];
            lines.push(w.name + ': pipeline median ' + w.medianMs + ' ms, ' +
                       w.opsPerSec + ' decisions/s (' + w.iterations +
                       ' iterations); isolated C raw loop avg ' +
                       w.rawLoop.avgMs + ' ms');
        });
        els.benchmark.textContent = lines.join('\n');
        state.benchmarkRan = true;
        state.lastBenchmark = report;
        return report;
    }

    function releaseEngine() {
        if (state.kernel) {
            state.kernel.free();
            state.kernel = null;
        }
        state.releaseRan = true;
        updateCounters();
        setStatus('released', 'engine released');
        els.footer.textContent = 'Engine released. Click any preset to re-initialize.';
    }

    function updateCounters() {
        if (state.kernel) {
            els.count.textContent = 'live kernels: ' + state.kernel.engineCount();
        } else {
            els.count.textContent = 'live kernels: 0';
        }
    }

    function ensureKernel() {
        if (state.kernel) {
            return Promise.resolve(state.kernel);
        }
        return window.OmniDecide.create({ mode: 'ascii', language: 'und' })
            .then(function (kernel) {
                state.kernel = kernel;
                state.usingWasm = !!(kernel._mod &&
                    typeof kernel._mod._odkw_evaluate === 'function' &&
                    kernel._mod.HEAPU32 instanceof Uint32Array);
                setStatus('ready', 'ready');
                els.version.textContent = 'kernel ' + kernel.kernelVersion;
                els.backend.textContent = 'backend ' + kernel.backend;
                updateCounters();
                els.footer.textContent = 'OmniDecide integration v' +
                    window.OmniDecide.version + ' - deterministic decisions in the browser';
                return kernel;
            })
            .catch(function (err) {
                state.initError = err;
                state.initFailureHandled = true;
                setStatus('error', 'error');
                showError(err);
                throw err;
            });
    }

    function guard(fn) {
        return function () {
            ensureKernel().then(fn).catch(function (err) {
                if (state.kernel) {
                    /* runtime error (e.g. invalid input): show, stay ready */
                    showError(err);
                }
                /* initialization errors are shown by ensureKernel */
            });
        };
    }

    document.getElementById('btn-quickstart').addEventListener('click',
        guard(function () { runPreset('quickstart'); }));
    document.getElementById('btn-unicode').addEventListener('click',
        guard(function () { runPreset('unicode'); }));
    document.getElementById('btn-invalid').addEventListener('click',
        guard(function () { runPreset('invalid'); }));
    document.getElementById('btn-determinism').addEventListener('click',
        guard(function () { runDeterminism(); }));
    document.getElementById('btn-replay').addEventListener('click',
        guard(function () { runReplay(); }));
    document.getElementById('btn-benchmark').addEventListener('click',
        guard(function () { runBenchmark('quick'); }));
    document.getElementById('btn-release').addEventListener('click',
        guard(function () { releaseEngine(); }));

    /* Playwright hook: initialization failure injection support. */
    window.__OD_APP__ = {
        init: ensureKernel,
        ensureKernel: ensureKernel,
        runPreset: function (name) { return ensureKernel().then(function () { return runPreset(name); }); },
        runDeterminism: function () { return ensureKernel().then(runDeterminism); },
        runReplay: function () { return ensureKernel().then(runReplay); },
        runBenchmark: function (scale) { return ensureKernel().then(function () { return runBenchmark(scale); }); },
        release: releaseEngine,
        status: function () { return els.status.textContent; },
        state: state
    };

    /* Auto-initialize on load; a failure must surface as an error state,
     * never as a silent fallback. */
    ensureKernel().catch(function () { /* surfaced in UI + state */ });
})();
