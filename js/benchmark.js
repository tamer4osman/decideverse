/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * benchmark.js - shared decision benchmark workloads.
 *
 * Used by tests/node_bench.mjs (Node) and by the browser playground
 * (tests/browser_bench.mjs drives the same code inside Chromium).
 *
 * Every workload calls the REAL OmniDecide WebAssembly functions through
 * the js/omnidecide.js API - there are no synthetic JavaScript
 * workloads and no mocked decisions.
 *
 * Two timings are reported per workload and MUST NOT be conflated:
 *   pipelineMs  - full JavaScript-to-WASM decision pipeline: string
 *                 marshaling into WASM heap + wrapper call + JSON
 *                 envelope parse.  This is what an application pays.
 *   rawLoopMs   - isolated C timing: the same request pre-marshaled
 *                 once, then the C evaluation looped without any
 *                 per-call JavaScript marshaling.  This isolates the
 *                 wrapper + kernel cost.
 */

(function (root, factory) {
    if (typeof module === 'object' && typeof module.exports === 'object') {
        module.exports = factory();
    } else {
        root.OdBenchmark = factory();
    }
})(typeof self !== 'undefined' ? self : this, function () {
    'use strict';

    function now() {
        return (typeof performance !== 'undefined' &&
                typeof performance.now === 'function') ?
            performance.now() : Date.now();
    }

    var QUICKSTART_CONTEXT = 'The service stopped responding. ' +
        'Restarting the service restored normal operation within seconds.';
    var QUICKSTART_OPTIONS = ['restart the service',
                              'reinstall the operating system',
                              'wait for the weekend'];

    function zhSentence(repeats) {
        var s = '会议纪要：团队讨论了方案甲和方案乙。方案甲成本低、风险小，本周即可上线。' +
                '方案乙需要外部供应商支持，周期四周。过去三次类似项目中，方案甲都按时交付。';
        var out = '';
        for (var i = 0; i < repeats; i++) {
            out += s;
        }
        return out;
    }

    function logLine(prefix, bytes) {
        var out = '';
        var n = Math.ceil(bytes / (prefix.length + 1));
        for (var i = 0; i < n; i++) {
            out += prefix + (i + 1) + ' ';
        }
        return out.slice(0, Math.max(bytes, prefix.length + 1)).trim() + '.';
    }

    /* Workload definitions.  Contexts target the documented sizes. */
    function workloads() {
        return {
            small: {
                name: 'small (200 B context)',
                contextBytes: 200,
                context: logLine('The review meeting discussed scaling options and capacity planning for the team.', 200),
                primitive: 'choice',
                instruction: 'Which option should the team pick?',
                options: ['scale the service horizontally', 'scale the service vertically']
            },
            medium: {
                name: 'medium (2 KB context)',
                contextBytes: 2048,
                context: logLine('OPSLOG reboot the node cleared the alerts and the disk replacement had no follow-up evidence in the incident log.', 2048),
                primitive: 'choice',
                instruction: 'Which action does the log support?',
                options: ['reboot the node', 'replace the disk']
            },
            large: {
                name: 'large (20 KB context)',
                contextBytes: 20480,
                context: logLine('LOG-ENTRY alpha option recorded by the reviewer with supporting evidence and cross references.', 20480),
                primitive: 'choice',
                instruction: 'Which option is supported?',
                options: ['alpha option', 'beta option']
            },
            unicode: {
                name: 'unicode (中文 ~2 KB context)',
                contextBytes: 2048,
                context: zhSentence(30),
                primitive: 'choice',
                instruction: '应该采用哪个方案？',
                options: ['方案甲', '方案乙']
            }
        };
    }

    function iterationDefaults(scale) {
        if (scale === 'quick') {
            return { small: 300, medium: 150, large: 40, unicode: 150 };
        }
        /* standard */
        return { small: 2000, medium: 800, large: 150, unicode: 800 };
    }

    function stats(samples) {
        var sorted = samples.slice().sort(function (a, b) { return a - b; });
        var n = sorted.length;
        var sum = 0, i;
        for (i = 0; i < n; i++) sum += sorted[i];
        function pct(p) {
            var idx = Math.min(n - 1, Math.ceil((p / 100) * n) - 1);
            return idx < 0 ? sorted[0] : sorted[idx];
        }
        return {
            iterations: n,
            avgMs: +(sum / n).toFixed(4),
            medianMs: +((n % 2) ? sorted[(n - 1) / 2]
                        : (sorted[n / 2 - 1] + sorted[n / 2]) / 2).toFixed(4),
            minMs: +sorted[0].toFixed(4),
            maxMs: +sorted[n - 1].toFixed(4),
            p95Ms: +pct(95).toFixed(4),
            p99Ms: +pct(99).toFixed(4),
            opsPerSec: +(1000 / (sum / n)).toFixed(1)
        };
    }

    /**
     * Run the full benchmark against a Kernel (js/omnidecide.js).
     * Returns a JSON-serializable result object.
     */
    function run(kernel, scale, options) {
        options = options || {};
        var iters = iterationDefaults(scale || 'standard');
        var ids = options.workloads ||
            ['small', 'medium', 'large', 'unicode'];
        var tInit0 = now();
        var wls = workloads();
        var moduleLoadMs = options.moduleLoadMs || null;

        var results = {};
        var totalPipelineSamples = [];

        ids.forEach(function (id) {
            var w = wls[id];
            if (!w) return;
            var n = iters[id];
            var req = { primitive: w.primitive, instruction: w.instruction,
                        options: w.options, context: w.context };

            /* Warm-up (5 pipeline calls, not counted). */
            for (var i = 0; i < 5; i++) kernel.evaluate(req);

            /* Full JS -> WASM pipeline. */
            var pipeline = [];
            var t0 = now();
            for (var j = 0; j < n; j++) {
                var c0 = now();
                kernel.evaluate(req);
                pipeline.push(now() - c0);
            }
            var totalMs = now() - t0;

            /* Isolated C timing (pre-marshaled, no per-call marshaling). */
            kernel.evaluate(req); /* warm */
            var rawMs = kernel.rawLoopMs(req, n);

            var s = stats(pipeline);
            s.name = w.name;
            s.totalMs = +totalMs.toFixed(2);
            s.contextBytes = w.contextBytes;
            s.rawLoop = {
                iterations: n,
                totalMs: +rawMs.toFixed(2),
                avgMs: +(rawMs / n).toFixed(4),
                opsPerSec: +(1000 / (rawMs / n)).toFixed(1)
            };
            results[id] = s;
            totalPipelineSamples = totalPipelineSamples.concat(pipeline);
        });

        var overall = stats(totalPipelineSamples);
        var initMs = +(now() - tInit0).toFixed(2);

        return {
            scale: scale || 'standard',
            workloads: results,
            overallPipeline: overall,
            init: {
                moduleLoadMs: moduleLoadMs,
                kernelCreateMs: options.kernelCreateMs || null,
                firstDecisionMs: options.firstDecisionMs || null
            },
            notes: [
                'pipelineMs = full JavaScript-to-WASM decision pipeline ' +
                '(string marshaling + wrapper call + envelope parse)',
                'rawLoopMs = isolated C timing: pre-marshaled request, ' +
                'no per-call JavaScript marshaling',
                'isolated C timing is NOT the full pipeline and must not ' +
                'be reported as such'
            ]
        };
    }

    return {
        workloads: workloads,
        iterationDefaults: iterationDefaults,
        stats: stats,
        run: run,
        quickstart: {
            context: QUICKSTART_CONTEXT,
            options: QUICKSTART_OPTIONS
        }
    };
});
