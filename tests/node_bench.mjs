/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * node_bench.mjs - Node.js decision benchmark over the real WASM module.
 *
 *   node tests/node_bench.mjs --scale standard   (default)
 *   node tests/node_bench.mjs --scale quick
 *   node tests/node_bench.mjs --scale standard --json <path>
 *
 * Every measurement exercises the actual OmniDecide WebAssembly binary
 * through js/omnidecide.js.  Results are written as fresh machine-
 * readable JSON (docs/benchmarks/node-bench.json plus a timestamped
 * copy); historical numbers are never reused or invented.
 */

import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { writeFileSync, mkdirSync } from 'node:fs';
import os from 'node:os';

const require = createRequire(import.meta.url);
const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = dirname(HERE);

const OD = require(join(ROOT, 'js', 'omnidecide.js'));
const OdBench = require(join(ROOT, 'js', 'benchmark.js'));

function argValue(flag, fallback) {
    const i = process.argv.indexOf(flag);
    return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : fallback;
}

const scale = process.argv.includes('--scale', 2) === false && process.argv.includes('--scale') ?
    argValue('--scale', 'standard') : argValue('--scale', 'standard');
const jsonOut = argValue('--json', join(ROOT, 'docs', 'benchmarks', 'node-bench.json'));

async function main() {
    const env = {
        timestamp: new Date().toISOString(),
        host: {
            platform: os.platform(),
            release: os.release(),
            arch: os.arch(),
            cpuModel: os.cpus()[0] ? os.cpus()[0].model.trim() : 'unknown',
            cpuCount: os.cpus().length,
            memoryMB: Math.round(os.totalmem() / (1024 * 1024))
        },
        runtime: {
            nodeVersion: process.version,
            integrationVersion: OD.OmniDecide.version,
            kernel: 'omnidecide-c 2.0.0 (pinned vendored source)',
            emscripten: '7.0.0',
            wasmBinary: 'dist/omnidecide.wasm'
        },
        scale
    };

    /* --- initialization timings ------------------------------------- */
    const t0 = performance.now();
    const kernel = await OD.OmniDecide.create({ mode: 'ascii', language: 'und' });
    const moduleLoadMs = +(performance.now() - t0).toFixed(2);

    const tCreate = performance.now();
    const k2 = await OD.OmniDecide.create({ mode: 'unicode', language: 'zh' });
    const kernelCreateMs = +(performance.now() - tCreate).toFixed(2);

    const tFirst = performance.now();
    await kernel.evaluateChoice(OdBench.quickstart.options ? 'How should we fix the outage?' : '',
        OdBench.quickstart.options, OdBench.quickstart.context);
    const firstDecisionMs = +(performance.now() - tFirst).toFixed(2);

    console.log(`node_bench: scale=${scale}`);
    console.log(`node_bench: module load ${moduleLoadMs} ms, kernel create ${kernelCreateMs} ms, first decision ${firstDecisionMs} ms`);

    /* --- workloads --------------------------------------------------- */
    const report = OdBench.run(kernel, scale, {
        moduleLoadMs,
        kernelCreateMs,
        firstDecisionMs
    });

    console.log('');
    console.log('workload                    iter   median    avg     p95     p99   ops/s   ctx');
    Object.keys(report.workloads).forEach(id => {
        const w = report.workloads[id];
        const pad = (s, n) => String(s).padStart(n);
        console.log(
            pad(w.name, 26).slice(0, 26) +
            pad(w.iterations, 7) + pad(w.medianMs.toFixed(3), 8) + 'ms' +
            pad(w.avgMs.toFixed(3), 8) + pad(w.p95Ms.toFixed(3), 8) +
            pad(w.p99Ms.toFixed(3), 8) + pad(w.opsPerSec.toFixed(0), 8) +
            pad(w.contextBytes + 'B', 7));
    });
    console.log('');
    console.log('pipeline = full JavaScript-to-WASM decision (marshaling + wrapper + envelope parse)');
    console.log('rawLoop  = isolated C timing (pre-marshaled, no per-call JS marshaling) - NOT the pipeline number');

    const out = {
        ...env,
        init: report.init,
        workloads: report.workloads,
        overallPipeline: report.overallPipeline,
        notes: report.notes,
        method: 'fresh execution over dist/omnidecide.wasm; no historical data reused'
    };
    mkdirSync(dirname(jsonOut), { recursive: true });
    writeFileSync(jsonOut, JSON.stringify(out, null, 2) + '\n');
    const stamped = jsonOut.replace(/\.json$/, '') + '-' +
        new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19) + '.json';
    writeFileSync(stamped, JSON.stringify(out, null, 2) + '\n');
    console.log(`node_bench: JSON written to ${jsonOut} (and ${stamped})`);
    console.log('node_bench: OK');

    kernel.free();
    k2.free();
}

main().catch(e => { console.error('node_bench: FATAL', e); process.exit(1); });
