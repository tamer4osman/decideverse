/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * browser_bench.mjs - Chromium decision benchmark via Playwright.
 *
 *   node tests/browser_bench.mjs --scale standard
 *
 * Serves the project root over local HTTP, loads the playground in
 * Chromium and runs the SAME shared benchmark library (js/benchmark.js)
 * inside the page against the real WebAssembly engine.  Results are
 * written as fresh JSON to docs/benchmarks/browser-bench.json.
 */

import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { createServer } from 'node:http';
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { extname, resolve } from 'node:path';

const require = createRequire(import.meta.url);
const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = dirname(HERE);
const { chromium } = require(join(ROOT, 'tests', 'node_modules', 'playwright'));

function argValue(flag, fallback) {
    const i = process.argv.indexOf(flag);
    return i >= 0 && i + 1 < process.argv.length ? process.argv[i + 1] : fallback;
}
const scale = argValue('--scale', 'standard');
const jsonOut = argValue('--json', join(ROOT, 'docs', 'benchmarks', 'browser-bench.json'));

const MIME = {
    '.html': 'text/html; charset=utf-8',
    '.js': 'text/javascript; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.wasm': 'application/wasm'
};

async function main() {
    const server = createServer((req, resp) => {
        const urlPath = decodeURIComponent(req.url.split('?')[0]);
        const filePath = resolve(join(ROOT, urlPath === '/' ? 'web/index.html' : urlPath.slice(1)));
        if (!filePath.startsWith(resolve(ROOT))) { resp.writeHead(403); resp.end(); return; }
        try {
            const body = readFileSync(filePath);
            resp.writeHead(200, { 'Content-Type': MIME[extname(filePath)] || 'application/octet-stream' });
            resp.end(body);
        } catch {
            resp.writeHead(404); resp.end();
        }
    });
    await new Promise(res => server.listen(0, '127.0.0.1', res));
    const base = `http://127.0.0.1:${server.address().port}`;

    const browser = await chromium.launch({ headless: true });
    const page = await browser.newPage();
    let wasmResponse = null;
    page.on('response', r => {
        if (r.url().endsWith('.wasm')) {
            wasmResponse = { status: r.status(), contentType: r.headers()['content-type'] || '' };
        }
    });

    console.log(`browser_bench: serving ${base}, scale=${scale}`);
    const tNav0 = Date.now();
    await page.goto(`${base}/web/index.html`, { waitUntil: 'networkidle' });
    const pageLoadMs = Date.now() - tNav0;

    await page.waitForFunction(() => window.__OD_APP__ !== undefined &&
        window.OdBenchmark !== undefined, null, { timeout: 20000 });

    /* Release the auto-created kernel so the benchmark measures a clean
     * initialization sequence of its own. */
    await page.evaluate(() => window.__OD_APP__.release());

    const report = await page.evaluate(async (scale) => {
        const OD = window.OmniDecide;
        const t0 = performance.now();
        const kernel = await OD.create({ mode: 'ascii', language: 'und' });
        const moduleLoadMs = +(performance.now() - t0).toFixed(2);
        const tFirst = performance.now();
        await kernel.evaluateChoice('How should we fix the outage?',
            window.OdBenchmark.quickstart.options, window.OdBenchmark.quickstart.context);
        const firstDecisionMs = +(performance.now() - tFirst).toFixed(2);
        const rep = window.OdBenchmark.run(kernel, scale, {
            moduleLoadMs,
            kernelCreateMs: null,
            firstDecisionMs
        });
        kernel.free();
        return rep;
    }, scale);

    await browser.close();
    server.close();

    console.log(`browser_bench: page load ${pageLoadMs} ms, module load ${report.init.moduleLoadMs} ms, first decision ${report.init.firstDecisionMs} ms`);
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

    const env = {
        timestamp: new Date().toISOString(),
        host: { platform: process.platform, arch: process.arch },
        runtime: {
            browser: 'chromium (playwright-managed)',
            nodeVersion: process.version,
            integrationVersion: '1.0.0',
            kernel: 'omnidecide-c 2.0.0 (pinned vendored source)',
            wasmBinary: 'dist/omnidecide.wasm',
            wasmResponse
        },
        scale,
        pageLoadMs,
        init: report.init,
        workloads: report.workloads,
        overallPipeline: report.overallPipeline,
        notes: report.notes,
        method: 'fresh in-page execution of js/benchmark.js against dist/omnidecide.wasm; no historical data reused'
    };
    mkdirSync(dirname(jsonOut), { recursive: true });
    writeFileSync(jsonOut, JSON.stringify(env, null, 2) + '\n');
    const stamped = jsonOut.replace(/\.json$/, '') + '-' +
        new Date().toISOString().replace(/[:.]/g, '-').slice(0, 19) + '.json';
    writeFileSync(stamped, JSON.stringify(env, null, 2) + '\n');
    console.log(`browser_bench: JSON written to ${jsonOut} (and ${stamped})`);
    console.log('browser_bench: OK');
}

main().catch(e => { console.error('browser_bench: FATAL', e); process.exit(1); });
