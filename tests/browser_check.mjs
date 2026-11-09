/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * browser_check.mjs - full browser verification with Playwright.
 *
 *   cd tests && npm ci && npx playwright install --with-deps chromium
 *   node browser_check.mjs           (from the project root or tests/)
 *
 * Serves the project root over local HTTP (never file://), launches
 * Chromium and verifies the browser acceptance list: real WASM
 * loading, engine initialization, README quickstart, confidence,
 * probability distribution, invalid input handling, the Chinese
 * Unicode preset, the determinism check, the benchmark, engine status,
 * initialization-failure handling, engine release, console/page error
 * capture, failed-request capture and the correct WASM MIME type.
 *
 * Fresh screenshots are written to docs/img/ from THIS run.
 */

import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';
import { createServer } from 'node:http';
import { readFileSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import { extname, resolve } from 'node:path';

const require = createRequire(import.meta.url);
const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = dirname(HERE);

const { chromium } = require(join(ROOT, 'tests', 'node_modules', 'playwright'));

/* ------------------------------------------------------------------ */
/* Static HTTP server with correct MIME types (application/wasm!)      */
/* ------------------------------------------------------------------ */
const MIME = {
    '.html': 'text/html; charset=utf-8',
    '.js': 'text/javascript; charset=utf-8',
    '.mjs': 'text/javascript; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.json': 'application/json; charset=utf-8',
    '.jsonl': 'application/jsonl; charset=utf-8',
    '.wasm': 'application/wasm',
    '.png': 'image/png',
    '.md': 'text/markdown; charset=utf-8'
};

let server;
let serverPort;

function startServer() {
    return new Promise((res, rej) => {
        server = createServer((req, resp) => {
            const urlPath = decodeURIComponent(req.url.split('?')[0]);
            const filePath = resolve(join(ROOT, urlPath === '/' ? 'web/index.html' : urlPath.slice(1)));
            if (!filePath.startsWith(resolve(ROOT))) {
                resp.writeHead(403); resp.end('forbidden'); return;
            }
            try {
                const body = readFileSync(filePath);
                resp.writeHead(200, {
                    'Content-Type': MIME[extname(filePath)] || 'application/octet-stream',
                    'Content-Length': body.length,
                    'Cache-Control': 'no-store'
                });
                resp.end(body);
            } catch {
                resp.writeHead(404, { 'Content-Type': 'text/plain' });
                resp.end('not found');
            }
        });
        server.listen(0, '127.0.0.1', () => {
            serverPort = server.address().port;
            res(serverPort);
        });
        server.on('error', rej);
    });
}

/* ------------------------------------------------------------------ */
/* Checks                                                              */
/* ------------------------------------------------------------------ */
let checks = 0;
let failures = 0;
const failureLines = [];
function check(cond, label) {
    checks++;
    if (!cond) { failures++; failureLines.push(label); console.error('FAIL: ' + label); }
}

async function main() {
    if (!existsSync(join(ROOT, 'dist', 'omnidecide.wasm'))) {
        console.error('browser_check: dist/omnidecide.wasm missing - run ./build.sh first');
        process.exit(2);
    }
    await startServer();
    const base = `http://127.0.0.1:${serverPort}`;
    console.log(`browser_check: serving ${base} over HTTP (not file://)`);

    const browser = await chromium.launch({ headless: true });
    const context = await browser.newContext({ viewport: { width: 1280, height: 900 } });
    const page = await context.newPage();

    const consoleErrors = [];
    const pageErrors = [];
    const failedRequests = [];
    let wasmResponse = null;

    page.on('console', m => { if (m.type() === 'error') consoleErrors.push(m.text()); });
    page.on('pageerror', e => pageErrors.push(String(e)));
    page.on('response', r => {
        if (r.status() >= 400) failedRequests.push(`${r.status()} ${r.url()}`);
        if (r.url().endsWith('.wasm')) {
            wasmResponse = { status: r.status(), contentType: r.headers()['content-type'] || '' };
        }
    });

    /* --- load --------------------------------------------------------- */
    await page.goto(`${base}/web/index.html`, { waitUntil: 'networkidle' });
    await page.waitForFunction(() => window.__OD_APP__ !== undefined, null, { timeout: 15000 });
    await page.waitForFunction(() =>
        window.__OD_APP__.status() === 'ready' || window.__OD_APP__.status() === 'error',
        null, { timeout: 30000 });

    /* 1-2. real WASM + MIME */
    check(wasmResponse !== null, 'the compiled WASM module was fetched by the browser');
    check(wasmResponse && wasmResponse.status === 200, `WASM fetched with HTTP 200 (got ${wasmResponse && wasmResponse.status})`);
    check(wasmResponse && /application\/wasm/.test(wasmResponse.contentType),
          `WASM served with application/wasm MIME (got ${wasmResponse && wasmResponse.contentType})`);
    const usingWasm = await page.evaluate(() => window.__OD_APP__.state.usingWasm);
    check(usingWasm === true, 'engine reports genuine WebAssembly execution (no JS fallback)');

    /* 3. status */
    check(await page.evaluate(() => window.__OD_APP__.status()) === 'ready',
          'engine status shows ready');
    const versionText = await page.textContent('#engine-version');
    check(/kernel 2\.0\.0/.test(versionText || ''), `version display shows kernel 2.0.0 (got "${versionText}")`);
    const backendText = await page.textContent('#engine-backend');
    check(/embedded-fallback/.test(backendText || ''), `backend display shows embedded-fallback (got "${backendText}")`);
    const countText = await page.textContent('#engine-count');
    check(/live kernels: 1/.test(countText || ''), `engine count shows 1 live kernel (got "${countText}")`);

    /* 4-6. README quickstart */
    await page.click('#btn-quickstart');
    await page.waitForSelector('#decision:not(.hidden)', { timeout: 10000 });
    const winner = await page.textContent('#decision-winner');
    check(winner === 'restart the service', `README quickstart winner displayed (got "${winner}")`);
    const confidence = await page.textContent('#decision-confidence');
    check(/confidence 0\.6312/.test(confidence || ''), `confidence displayed (got "${confidence}")`);
    const barCount = await page.locator('#distribution .dist-row').count();
    check(barCount === 3, `probability distribution rendered with 3 bars (got ${barCount})`);
    const distText = await page.textContent('#distribution');
    check(/restart the service/.test(distText) && /0\.6312/.test(distText),
          'distribution shows option label and probability value');
    await page.screenshot({ path: join(ROOT, 'docs', 'img', 'browser-quickstart.png'), fullPage: true });
    await page.click('#envelope-details summary');
    await page.screenshot({ path: join(ROOT, 'docs', 'img', 'browser-distribution.png'), fullPage: true });

    /* 7. invalid input */
    await page.click('#btn-invalid');
    await page.waitForSelector('#error-box:not(.hidden)', { timeout: 10000 });
    const errText = await page.textContent('#error-box');
    check(/OmniDecideError \[status 1\]/.test(errText || ''), `invalid input shows OmniDecideError with status 1 (got "${(errText || '').slice(0, 80)}")`);
    check(await page.evaluate(() => window.__OD_APP__.status()) === 'ready',
          'engine stays ready after invalid input');
    check(await page.evaluate(() => window.__OD_APP__.state.invalidRan) === true,
          'invalid input case recorded');

    /* 8. unicode preset */
    await page.click('#btn-unicode');
    await page.waitForFunction(() =>
        document.querySelector('#decision-winner') &&
        document.querySelector('#decision-winner').textContent.includes('柏林数据中心'),
        null, { timeout: 10000 });
    const zhWinner = await page.textContent('#decision-winner');
    check(zhWinner === '柏林数据中心', `unicode preset winner displayed (got "${zhWinner}")`);
    await page.screenshot({ path: join(ROOT, 'docs', 'img', 'browser-unicode.png'), fullPage: true });

    /* 9. determinism */
    await page.click('#btn-determinism');
    await page.waitForFunction(() =>
        document.querySelector('#determinism-result') &&
        !document.querySelector('#determinism-result').classList.contains('hidden'),
        null, { timeout: 10000 });
    const detText = await page.textContent('#determinism-result');
    check(/5\/5 identical/.test(detText || ''), `determinism check shows 5/5 identical (got "${detText}")`);

    /* replay */
    await page.click('#btn-replay');
    await page.waitForFunction(() =>
        /replay:/.test(document.querySelector('#determinism-result').textContent),
        null, { timeout: 10000 });
    const replayText = await page.textContent('#determinism-result');
    check(/byte-identical/.test(replayText || ''), `replay is byte-identical (got "${replayText}")`);

    /* 10. benchmark */
    await page.click('#btn-benchmark');
    await page.waitForSelector('#benchmark-result:not(.hidden)', { timeout: 60000 });
    const benchText = await page.textContent('#benchmark-result');
    check(/decisions\/s/.test(benchText || ''), `benchmark executed and reported throughput (got "${(benchText || '').split('\n')[1] || ''}")`);
    check(/isolated C raw loop avg/.test(benchText || ''), 'benchmark separates isolated C timing');
    await page.screenshot({ path: join(ROOT, 'docs', 'img', 'browser-benchmark.png'), fullPage: true });

    /* 11. engine release + re-init */
    await page.click('#btn-release');
    await page.waitForFunction(() => window.__OD_APP__.status().indexOf('released') >= 0,
        null, { timeout: 10000 });
    check(true, 'engine release reported in status');
    const countAfterRelease = await page.textContent('#engine-count');
    check(/live kernels: 0/.test(countAfterRelease || ''), `engine count is 0 after release (got "${countAfterRelease}")`);
    await page.click('#btn-quickstart');
    await page.waitForFunction(() => window.__OD_APP__.status() === 'ready', null, { timeout: 15000 });
    await page.waitForSelector('#decision:not(.hidden)', { timeout: 10000 });
    const winner2 = await page.textContent('#decision-winner');
    check(winner2 === 'restart the service', 'engine re-initialized after release and re-decided correctly');

    /* 12. errors and failed requests */
    check(consoleErrors.length === 0, `no browser console errors (got ${consoleErrors.length}: ${consoleErrors[0] || ''})`);
    check(pageErrors.length === 0, `no uncaught page errors (got ${pageErrors.length}: ${pageErrors[0] || ''})`);
    check(failedRequests.length === 0, `no failed network requests (got ${failedRequests.length}: ${failedRequests[0] || ''})`);

    /* 13. initialization failure handling (fresh page, WASM blocked) */
    const page2 = await context.newPage();
    const page2Errors = [];
    page2.on('pageerror', e => page2Errors.push(String(e)));
    await page2.route('**/*.wasm', route => route.abort());
    await page2.goto(`${base}/web/index.html`, { waitUntil: 'domcontentloaded' });
    await page2.waitForFunction(() =>
        window.__OD_APP__ && (window.__OD_APP__.status() === 'error' || window.__OD_APP__.state.initFailureHandled),
        null, { timeout: 20000 });
    const status2 = await page2.evaluate(() => window.__OD_APP__.status());
    check(status2 === 'error', `initialization failure surfaces as an error state (got "${status2}")`);
    const err2 = await page2.textContent('#error-box');
    check((err2 || '').length > 0, 'initialization failure shows a diagnostic message');
    const page2WinnerVisible = await page2.locator('#decision:not(.hidden)').count();
    check(page2WinnerVisible === 0, 'no decision is rendered after failed initialization (no silent fallback)');
    await page2.close();

    await browser.close();
    server.close();

    /* --- summary ------------------------------------------------------- */
    console.log('');
    console.log(`browser_check: ${checks} checks, ${failures} failures`);

    const report = {
        timestamp: new Date().toISOString(),
        browser: 'chromium (playwright)',
        url: `${base}/web/index.html`,
        checks,
        failures,
        consoleErrors,
        pageErrors,
        failedRequests,
        wasmResponse,
        screenshots: [
            'docs/img/browser-quickstart.png',
            'docs/img/browser-distribution.png',
            'docs/img/browser-unicode.png',
            'docs/img/browser-benchmark.png'
        ]
    };
    mkdirSync(join(ROOT, 'build'), { recursive: true });
    writeFileSync(join(ROOT, 'build', 'browser-check.json'), JSON.stringify(report, null, 2) + '\n');

    if (failures > 0) {
        failureLines.forEach(l => console.error('  - ' + l));
        process.exit(1);
    }
    console.log('browser_check: OK');
}

main().catch(e => {
    console.error('browser_check: FATAL', e);
    process.exit(1);
});
