/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * gamelab_browser_check.mjs - Stage 2 browser end-to-end verification.
 *
 *   cd tests && npm ci && npx playwright install --with-deps chromium
 *   node tests/gamelab_browser_check.mjs      (from the project root or tests/)
 *
 * The full required flow:
 *   1.  Open GameLab                       7.  Inspect the submitted request
 *   2.  Wait for the real engine to load   8.  Inspect the returned result
 *   3.  Launch The Last Detective          9.  Continue investigating
 *   4.  Review the case introduction      10.  Complete the case
 *   5.  Discover evidence                 11.  Reset the scenario
 *   6.  Trigger a real OmniDecide         12.  Replay it (byte-identical)
 *       decision
 * Plus: engine readiness/MIME, dashboard and launcher content, all
 * navigation areas, Decision Inspector sections, history, benchmarks,
 * planned-game states, initialization-failure handling (WASM blocked,
 * no silent fallback), a responsive layout check - and the Stage 2.1
 * hardening regressions:
 *   A. adapter lifecycle through the settings UI (recreate -> decision
 *      -> shutdown -> initialize -> decision, no handle accumulation);
 *   B. shared action history (displayed counter == authoritative
 *      session log; rejected actions never count; reset/replay);
 *   C. seed synchronization (selected seed == session seed == workspace
 *      display, retained by reset and replay);
 *   R. Stage 2.1.1 cleanup recovery (a failing old-instance release is
 *      reported, the handle stays adapter-owned, the retry control
 *      releases it - against the real WASM registry; the failure is
 *      injected at the engine-source seam on a separate page, the
 *      kernel itself is never replaced or mocked).
 *
 * Fresh screenshots are written to gamelab/docs/img/ from THIS run.
 * All decisions in this suite are produced by the real WASM kernel.
 */

import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { dirname, join, extname, resolve } from 'node:path';
import { createServer } from 'node:http';
import { readFileSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';

const require = createRequire(import.meta.url);
const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = dirname(HERE);

const { chromium } = require(join(ROOT, 'tests', 'node_modules', 'playwright'));

/* ------------------------------------------------------------------ */
/* Static HTTP server with the correct application/wasm MIME type      */
/* ------------------------------------------------------------------ */
const MIME = {
    '.html': 'text/html; charset=utf-8',
    '.js': 'text/javascript; charset=utf-8',
    '.mjs': 'text/javascript; charset=utf-8',
    '.css': 'text/css; charset=utf-8',
    '.json': 'application/json; charset=utf-8',
    '.wasm': 'application/wasm',
    '.png': 'image/png',
    '.md': 'text/markdown; charset=utf-8',
    '.svg': 'image/svg+xml'
};

let server;
let serverPort;

function startServer() {
    return new Promise((res, rej) => {
        server = createServer((req, resp) => {
            const urlPath = decodeURIComponent(req.url.split('?')[0]);
            const filePath = resolve(join(ROOT, urlPath === '/' ? 'gamelab/index.html' : urlPath.slice(1)));
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

const IMG = join(ROOT, 'gamelab', 'docs', 'img');
function shot(page, name) {
    return page.screenshot({ path: join(IMG, name), fullPage: true });
}

async function main() {
    if (!existsSync(join(ROOT, 'dist', 'omnidecide.wasm'))) {
        console.error('gamelab_browser_check: dist/omnidecide.wasm missing');
        process.exit(2);
    }
    mkdirSync(IMG, { recursive: true });
    await startServer();
    const base = `http://127.0.0.1:${serverPort}`;
    console.log(`gamelab_browser_check: serving ${base} over HTTP (not file://)`);

    const browser = await chromium.launch({ headless: true });
    const context = await browser.newContext({ viewport: { width: 1360, height: 900 } });
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

    const GL = () => 'window.__GAMELAB__';
    async function engineReady() {
        await page.waitForFunction(`${GL()} !== undefined`, null, { timeout: 15000 });
        await page.waitForFunction(`${GL()}.adapter.state === 'ready'`, null, { timeout: 30000 });
    }
    /* Stage 2.1 helpers ------------------------------------------------ */
    const CUSTOM_SEED = 4711;
    async function actionsLogged() {
        return page.evaluate(`${GL()}.liveSession('detective').actionLog.length`);
    }
    async function actionsShown() {
        return page.evaluate(`document.querySelector('[data-status-key="Actions logged"]').textContent`);
    }
    async function seedShown() {
        return page.evaluate(`document.querySelector('[data-status-key="Seed"]').textContent`);
    }

    /* ================================================================== */
    /* STEP 1-2: open GameLab, wait for the REAL engine                    */
    /* ================================================================== */
    await page.goto(`${base}/gamelab/index.html`, { waitUntil: 'networkidle' });
    await engineReady();
    check(wasmResponse !== null, 'the compiled WASM module was fetched by the browser');
    check(wasmResponse && wasmResponse.status === 200, `WASM fetched with HTTP 200 (got ${wasmResponse && wasmResponse.status})`);
    check(wasmResponse && /application\/wasm/.test(wasmResponse.contentType),
        `WASM served with application/wasm MIME (got ${wasmResponse && wasmResponse.contentType})`);
    check(await page.evaluate(`${GL()}.adapter.kernelVersion`) === '2.0.0', 'kernel version is 2.0.0');
    check(await page.evaluate(`${GL()}.adapter.backend`) === 'embedded-fallback', 'backend is embedded-fallback');
    check((await page.textContent('#engine-pill')).includes('engine: ready'), 'top bar shows engine: ready');

    /* ================================================================== */
    /* Dashboard                                                           */
    /* ================================================================== */
    check(await page.locator('.hero').count() === 1, 'dashboard shows the Decideverse identity');
    const engineCardText = await page.textContent('.engine-card');
    check(/kernel 2\.0\.0/.test(engineCardText), 'dashboard engine card shows kernel 2.0.0');
    check(await page.locator('.stats-row .stat').count() >= 5, 'dashboard shows live statistics');
    await shot(page, 'gamelab-dashboard.png');

    /* ================================================================== */
    /* Launcher                                                            */
    /* ================================================================== */
    await page.click('a[data-nav="launcher"]');
    await page.waitForSelector('.launcher-grid .game-card', { timeout: 5000 });
    const cardCount = await page.locator('.game-card').count();
    check(cardCount === 5, `launcher presents all five planned games (got ${cardCount})`);
    check(await page.locator('.game-card.playable').count() === 2,
        'exactly two playable modules (The Last Detective, Living Village)');
    check(await page.locator('.game-card.planned').count() === 3,
        'three games shown as planned (no fake playables)');
    const detectiveCard = await page.textContent('.game-card.playable');
    check(/The Last Detective/.test(detectiveCard), 'playable card is The Last Detective');
    await shot(page, 'gamelab-launcher.png');

    /* ================================================================== */
    /* STEP 3-4: launch The Last Detective, review the case introduction   */
    /* ================================================================== */
    await page.click('.game-card.playable a[href="#/sim/detective"]');
    await page.waitForSelector('#dt-start-simulation', { timeout: 5000 });
    await page.click('#dt-start-simulation');
    await page.waitForSelector('#dt-begin-investigation', { timeout: 5000 });
    const briefing = await page.textContent('.dt-briefing');
    check(/Meridian Diamond/.test(briefing) && /Aldermoor/.test(briefing),
        'case introduction describes the missing Meridian Diamond at the Aldermoor Museum');
    check(await page.locator('.suspect-chip').count() === 4, 'case introduction lists the four suspects');
    await shot(page, 'gamelab-briefing.png');

    /* Stage 2.1 C: select a CUSTOM seed in the briefing. */
    await page.fill('#dt-seed', String(CUSTOM_SEED));

    /* ================================================================== */
    /* STEP 5: begin investigation, discover evidence                      */
    /* ================================================================== */
    await page.click('#dt-begin-investigation');
    await page.waitForSelector('.dt-board', { timeout: 5000 });
    check((await page.textContent('#dt-hours .meter-value')) === '9', 'nine investigation hours allocated');

    /* Stage 2.1 C: the selected seed is authoritative everywhere. */
    check(await page.evaluate(`${GL()}.liveSession('detective').seed`) === CUSTOM_SEED,
        `C: the session carries the selected seed ${CUSTOM_SEED}`);
    check(await seedShown() === String(CUSTOM_SEED),
        'C: the workspace displays the selected seed, not the default');

    await page.click('[data-action-id="hall-guests"]');
    await page.waitForFunction(`document.querySelector('#dt-evidence-count .meter-value').textContent === '2'`,
        null, { timeout: 5000 });
    await page.click('[data-action-id="cur-interview"]');
    await page.waitForFunction(`document.querySelector('#dt-evidence-count .meter-value').textContent === '3'`,
        null, { timeout: 5000 });
    check(await page.locator('.dt-evidence-item').count() === 3, 'evidence ledger lists discovered items');
    check((await page.textContent('.ws-feed')).includes('evidence:discovered'), 'event feed shows evidence discovery events');

    /* Stage 2.1 B: the displayed counter matches the authoritative log.
     * begin + 2 investigations = 3 successful player actions. */
    check(await actionsShown() === '3', `B: workspace shows 3 actions after begin + 2 investigations (got ${await actionsShown()})`);
    check(await actionsLogged() === 3, 'B: the displayed counter equals session.actionLog.length');
    await shot(page, 'gamelab-investigation.png');

    /* ================================================================== */
    /* STEP 6: trigger an actual OmniDecide decision (choice)              */
    /* ================================================================== */
    await page.click('[data-review-id="consult-leads"]');
    await page.waitForSelector('#dt-recommendation', { timeout: 15000 });
    const recommendation = await page.textContent('#dt-recommendation .meter-value');
    check(recommendation.length > 0, `engine recommendation surfaced in the game (got "${recommendation}")`);
    const decisionsInHistory = await page.evaluate(`${GL()}.adapter && document.querySelectorAll('.feed-item').length`);
    check(decisionsInHistory > 0, 'decision events appear in the feed');

    /* Stage 2.1 B: a successful decision is a player action (4 total). */
    check(await actionsShown() === '4', `B: consultation counted as one action (got ${await actionsShown()})`);

    /* ================================================================== */
    /* STEP 9 (part 1): continue investigating + more real decisions       */
    /* ================================================================== */
    /* Stage 2.1 B: an INVALID action (double-click: the second click
     * reaches the real handler with the action already taken) must not
     * move the successful-action counter. */
    const secBtn = await page.$('[data-action-id="sec-camera"]');
    await secBtn.click();
    await page.waitForFunction(`document.querySelector('#dt-evidence-count .meter-value').textContent === '4'`,
        null, { timeout: 5000 });
    await page.evaluate(el => el.click(), secBtn); /* stale node -> reducer rejects */
    await page.waitForTimeout(200);
    check((await page.textContent('.ws-feed')).includes('Action rejected'),
        'B: the invalid (double-submitted) action surfaces a rejection event');
    check(await actionsShown() === '5', `B: the rejected action did not increment the counter (got ${await actionsShown()})`);

    await page.click('[data-action-id="sec-board"]');
    await page.waitForTimeout(150);
    await page.click('[data-review-id="weigh-statements"]');
    await page.waitForSelector('.dt-advisory', { timeout: 15000 });
    const advisory = await page.textContent('.dt-advisory');
    check(/compare/.test(advisory), 'compare advisory displayed after statement weighing');
    await page.click('[data-action-id="dock-log"]');
    await page.waitForTimeout(150);
    await page.click('[data-review-id="claim-check"]');
    await page.waitForTimeout(600);
    check(/noul/.test(await page.textContent('.dt-advisory')), 'noul advisory displayed after claim check');
    await page.click('[data-action-id="hall-case"]');
    await page.waitForTimeout(150);
    await page.click('[data-action-id="cur-stage"]');
    await page.waitForTimeout(150);
    await page.click('[data-action-id="dock-staff"]');
    await page.waitForTimeout(150);
    await page.click('[data-review-id="case-brief"]');
    await page.waitForSelector('.dt-brief', { timeout: 15000 });
    check(await page.locator('.dt-brief .extract-list li').count() === 3, 'case brief shows the extracted top-3 evidence');
    await page.click('[data-review-id="final-ranking"]');
    await page.waitForTimeout(600);
    const advisoryFinal = await page.textContent('.dt-advisory');
    check(/rank/.test(advisoryFinal) && /Adrian Cole/.test(advisoryFinal),
        'rank advisory displayed with the full-evidence ranking');
    await shot(page, 'gamelab-workspace.png');

    /* ================================================================== */
    /* STEP 7-8: inspect the submitted request and the returned result     */
    /* ================================================================== */
    await page.click('.ws-controls a[href="#/inspector"]');
    await page.waitForSelector('.inspector-detail', { timeout: 5000 });
    const rows = await page.locator('.inspector-list-card tbody tr').count();
    check(rows === 5, `Decision Inspector lists all five decisions (got ${rows})`);
    check(await page.locator('.io-input').count() === 1, 'inspector shows the SUBMITTED INPUT section');
    check(await page.locator('.io-output').count() === 1, 'inspector shows the ENGINE OUTPUT section');
    check(await page.locator('.io-interpretation').count() === 1, 'inspector shows the GAME INTERPRETATION section');
    const submitted = await page.textContent('.io-input ~ pre.code, .inspector-detail .io-block:first-of-type pre');
    check(/"primitive"\s*:\s*"rank"/.test(submitted), 'submitted input shows the exact kernel request JSON');
    check(/"context"\s*:\s*"E-00:/.test(submitted), 'submitted context is the actual evidence ledger');
    const detailText = await page.textContent('.inspector-detail');
    check(/confidence/.test(detailText), 'engine output shows confidence');
    check(/Adrian Cole/.test(detailText), 'engine output shows the actual kernel result');
    await page.click('.raw-details summary');
    const rawShown = await page.textContent('.raw-details pre');
    check(rawShown.trim().startsWith('{') && /"primitive"\s*:\s*"rank"/.test(rawShown),
        'raw envelope string from the C wrapper is inspectable');
    await shot(page, 'gamelab-inspector.png');

    /* Decision detail switching works. */
    await page.click('.inspector-list-card tbody tr:first-child a');
    await page.waitForTimeout(300);
    check(await page.locator('.inspector-detail').count() === 1, 'inspector can switch between decision records');

    /* ================================================================== */
    /* History                                                             */
    /* ================================================================== */
    await page.click('a[data-nav="history"]');
    await page.waitForSelector('.history-list', { timeout: 5000 });
    const eventCount = await page.locator('.history-list .history-item').count();
    check(eventCount > 10, `activity history lists the session record (got ${eventCount})`);
    check((await page.textContent('.toolbar')).includes('Simulation events'), 'history separates events from decisions');
    await page.click('.tab-row .tab:nth-child(2)');
    await page.waitForTimeout(250);
    check(await page.locator('.history-list .history-item').count() === 5, 'decisions tab lists the five decisions');
    await shot(page, 'gamelab-history.png');

    /* ================================================================== */
    /* Benchmarks                                                          */
    /* ================================================================== */
    await page.click('a[data-nav="benchmarks"]');
    await page.waitForSelector('.bench-grid', { timeout: 5000 });
    const benchText = await page.textContent('.bench-grid');
    check(/5/.test(await page.textContent('.bench-card:first-child')), 'benchmark overview shows the real invocation count');
    check(/ms/.test(benchText), 'benchmark overview shows real measured times');
    check(await page.locator('.bench-bar').count() === 5, 'recent duration bars reflect real decisions');
    await shot(page, 'gamelab-benchmarks.png');

    /* ================================================================== */
    /* STEP 10: complete the case (accuse with the evidence on file)       */
    /* ================================================================== */
    await page.click('a[data-nav="launcher"]');
    await page.waitForTimeout(150);
    await page.goto(`${base}/gamelab/index.html#/sim/detective`);
    await page.waitForSelector('[data-suspect-id="adrian-cole"]', { timeout: 5000 });
    check(await page.locator('.dt-accuse-row .btn.accuse').count() === 4,
        'accusation panel offers all four suspects');
    await page.click('[data-suspect-id="adrian-cole"]');
    await page.waitForSelector('#dt-ending', { timeout: 5000 });
    const ending = await page.textContent('#dt-ending');
    check(/Case closed/.test(ending) && /Meridian Diamond is recovered/.test(ending),
        'accusing Adrian Cole with full evidence resolves the case correctly');
    /* Stage 2.1 B: the full canonical run = begin + 8 actions + 5
     * decisions + accusation = 15 successful player actions. */
    check(await actionsShown() === '15', `B: completion counter is 15 (got ${await actionsShown()})`);
    check(await actionsLogged() === 15, 'B: the authoritative log agrees at completion');
    await shot(page, 'gamelab-ending.png');

    /* ================================================================== */
    /* STEP 11-12: reset the scenario, then replay it                      */
    /* ================================================================== */
    await page.click('#dt-reset-ending');
    await page.waitForSelector('#dt-begin-investigation', { timeout: 5000 });
    check(true, 'scenario resets back to the case introduction');
    /* Stage 2.1 B: reset starts a new run at zero. */
    check(await actionsShown() === '0', `B: reset clears the action counter for the new run (got ${await actionsShown()})`);
    /* Stage 2.1 C: reset retains the documented effective seed. */
    check(await seedShown() === String(CUSTOM_SEED),
        'C: reset retains the selected seed in the workspace');
    check(await page.evaluate(`${GL()}.liveSession('detective').seed`) === CUSTOM_SEED,
        'C: reset retains the selected seed in the session');
    await page.click('#dt-replay');
    await page.waitForSelector('#dt-replay-report', { timeout: 30000 });
    const replayReportText = await page.textContent('#dt-replay-report');
    check(/5 \/ 5/.test(replayReportText), `replay reports byte-identical envelopes (got "${(replayReportText.match(/\d+ \/ \d+/) || ['?'])[0]}")`);
    check(/Ending matches original\s*yes/.test(replayReportText.replace(/\s+/g, ' ')), 'replay reproduces the same ending');
    check(/15/.test(replayReportText), 'replay re-executed the full 15-step sequence (briefing + 8 actions + 5 decisions + accusation)');
    /* Stage 2.1 B/C: replay is inspection - no action entries, seed untouched. */
    check(await actionsShown() === '0', `B: replay never appends to the action counter (got ${await actionsShown()})`);
    check(await seedShown() === String(CUSTOM_SEED), 'C: replay leaves the effective seed untouched');
    await shot(page, 'gamelab-replay.png');

    /* About & settings apply for real. */
    await page.click('a[data-nav="about"]');
    await page.waitForSelector('.settings-card', { timeout: 5000 });
    check((await page.textContent('.view-about')).includes('deterministic lexical evidence-matching'),
        'about page explains the engine honestly');
    await page.click('#set-telemetry-reset');
    await page.waitForTimeout(200);
    check((await page.textContent('#settings-note')).includes('Telemetry counters reset'),
        'settings: telemetry reset works and is confirmed');
    await shot(page, 'gamelab-about.png');

    /* ================================================================== */
    /* Stage 2.1 A: adapter lifecycle through the settings UI              */
    /* ================================================================== */
    check(await page.evaluate(`${GL()}.adapter.engineCount()`) === 1,
        'A: exactly one managed kernel handle while ready');

    /* Recreate through the supported UI. */
    await page.selectOption('#set-mode', 'unicode');
    await page.selectOption('#set-language', 'en');
    await page.click('.settings-card .intro-actions button:has-text("Apply engine settings")');
    await page.waitForFunction(`${GL()}.adapter.state === 'ready' && ${GL()}.adapter.lifecycleStats.recreations >= 1`,
        null, { timeout: 15000 });
    check((await page.textContent('#settings-note')).includes('Engine re-created'),
        'A: settings confirm the re-creation with the real kernel');
    check(await page.evaluate(`${GL()}.adapter.config.mode`) === 'unicode',
        'A: the re-created kernel runs the new configuration');
    check(await page.evaluate(`${GL()}.adapter.engineCount()`) === 1,
        'A: recreation released the previous handle - still exactly one');
    check(await page.evaluate(`${GL()}.adapter.lifecycleStats.released`) >= 1,
        'A: lifecycle accounting recorded the release');

    /* A real decision still works after recreation (real WASM kernel). */
    const postRecreate = await page.evaluate(`${GL()}.adapter.invoke({
        primitive: 'choice',
        instruction: 'Which investigation lead is best supported by the evidence currently collected?',
        options: ['Investigate the security office', 'Interview the museum curator'],
        context: 'The camera feed has a gap. The curator saw the appraiser.'
    }).ok`);
    check(postRecreate === true, 'A: a real decision executes after recreation');

    /* Shutdown through the supported UI. */
    await page.click('#set-shutdown-engine');
    await page.waitForFunction(`${GL()}.adapter.state === 'uninitialized'`, null, { timeout: 10000 });
    check((await page.textContent('#settings-note')).includes('Engine released'),
        'A: shutdown through the UI releases the managed handle');
    check(await page.evaluate(`${GL()}.adapter.engineCount()`) === null,
        'A: no managed handle remains after shutdown');
    const afterShutdownDecision = await page.evaluate(
        `${GL()}.adapter.invoke({primitive:'choice',instruction:'x',options:['a'],context:'c'}).ok`);
    check(afterShutdownDecision === false,
        'A: decisions after shutdown are cleanly rejected (no released-handle use)');

    /* Initialize again through the supported UI; app remains usable. */
    await page.click('#set-init-engine');
    await page.waitForFunction(`${GL()}.adapter.state === 'ready'`, null, { timeout: 15000 });
    check((await page.textContent('#engine-pill')).includes('engine: ready'),
        'A: the engine pill shows ready after re-initialization');
    const afterReinitDecision = await page.evaluate(`${GL()}.adapter.invoke({
        primitive: 'noul',
        instruction: 'Restarting the service restored normal operation.',
        context: 'The service stopped responding. Restarting the service restored normal operation within seconds.'
    }).ok`);
    check(afterReinitDecision === true, 'A: the application remains usable (real decision after re-init)');
    check(await page.evaluate(`${GL()}.adapter.engineCount()`) === 1,
        'A: re-initialization holds exactly one managed handle (no accumulation across the whole cycle)');
    const life = await page.evaluate(`${GL()}.adapter.lifecycleStats`);
    check(life.created === life.released + 1,
        `A: accounting balances at the end (created ${life.created}, released ${life.released}, one managed)`);
    check(life.pendingCleanup === 0 && life.activeHandles === 1,
        'A: healthy lifecycle leaves zero pending-cleanup handles and one active handle');
    const statsPanel = await page.textContent('#engine-lifecycle-stats');
    check(statsPanel.includes('Pending cleanup handles') && statsPanel.includes('Cleanup failures / recoveries'),
        'A: the settings panel reports the real pending-cleanup and recovery accounting');

    /* ================================================================== */
    /* LIVING VILLAGE (Stage 3A): launch, steward, arbitrate, report       */
    /* ================================================================== */
    await page.goto(`${base}/gamelab/index.html#/launcher`, { waitUntil: 'networkidle' });
    await page.waitForFunction(`${GL()} !== undefined && ${GL()}.adapter.state === 'ready'`,
        null, { timeout: 30000 });
    check(await page.locator('a[href="#/sim/living-village"]').count() === 1,
        'V: the launcher offers the playable Living Village card');
    const villagePill = await page.textContent('.game-card.playable:has(a[href="#/sim/living-village"]) .status-pill');
    check(/Stage 3/.test(villagePill), `V: the village card is badged Stage 3 (got "${villagePill.trim()}")`);
    await page.click('a[href="#/sim/living-village"]');
    await page.waitForSelector('#dt-start-simulation', { timeout: 5000 });
    await page.click('#dt-start-simulation');
    await page.waitForSelector('#lv-begin', { timeout: 5000 });
    const villageBriefing = await page.textContent('.lv-briefing');
    check(/Thornbury/.test(villageBriefing), 'V: the briefing introduces the village of Thornbury');
    check(/lexical evidence matcher/.test(villageBriefing),
        'V: the briefing describes the engine honestly (lexical evidence matcher)');
    await shot(page, 'gamelab-village-briefing.png');

    /* Briefing seed: validated, synced to the session, displayed. */
    await page.fill('#lv-seed', '20261109');
    await page.click('#lv-begin');
    await page.waitForSelector('.lv-day', { timeout: 5000 });
    check(await page.evaluate(`${GL()}.liveSession('living-village').seed`) === 20261109,
        'V: the session carries the selected village seed');
    check((await page.textContent('.controls-note')).includes('20261109'),
        'V: the workspace displays the village seed');
    check(await page.locator('.lv-villager').count() === 6, 'V: six villager cards render');

    /* Steward day 1: assign the baker, host a gathering, end the day. */
    await page.click('[data-assign="tamsin"]');
    await page.click('[data-steward-action="host-gathering"]');
    check(await actionsShown() === '3', 'V: begin + 2 steward actions = 3 successful player actions');
    await page.click('#lv-endday');
    await page.waitForSelector('h2:has-text("Day 2 in Thornbury")', { timeout: 20000 });
    check(await actionsShown() === '4', 'V: end-day counts as exactly one further action');
    check(await page.evaluate(`${GL()}.liveSession('living-village').actionLog.filter(
        (a) => a.action === 'end-day').length`) === 1, 'V: the end-day is logged once');
    const villageDigest = await page.textContent('#lv-digest');
    check(/digest/i.test(villageDigest), 'V: the kernel-extracted chronicle digest is displayed');

    /* 390px: the village day view adds no horizontal overflow and the
     * primary nav stays usable (Stage 2.1.1 regression guard). */
    await page.setViewportSize({ width: 390, height: 844 });
    const vOverflow = await page.evaluate(
        'document.documentElement.scrollWidth - document.documentElement.clientWidth');
    check(vOverflow <= 2, `V: no horizontal overflow at 390px in the village (got ${vOverflow}px)`);
    const vNavWidth = await page.evaluate(
        'document.querySelector(".main-nav").getBoundingClientRect().width');
    check(vNavWidth > 100, `V: the primary nav stays usable at 390px in the village (got ${Math.round(vNavWidth)}px)`);
    await page.setViewportSize({ width: 1360, height: 900 });
    await shot(page, 'gamelab-village.png');

    /* Decision Inspector shows a REAL village arbitration. */
    await page.click('.ws-controls a[href="#/inspector"]');
    await page.waitForSelector('.io-input', { timeout: 5000 });
    const vDetail = await page.textContent('#decision-detail');
    check(/today in the village/.test(vDetail),
        'V: the inspector shows a real village arbitration (the day-1 digest request)');
    check(/"primitive"\s*:\s*"extract"/.test(vDetail),
        'V: the inspector shows the exact submitted request (extract envelope JSON)');
    check(/R3/.test(vDetail), 'V: the inspector shows the declared rule R3 interpretation');

    /* Play the remaining days out to the report (the live session
     * survives navigation and auto-resumes). */
    await page.evaluate("location.hash = '#/sim/living-village'");
    await page.waitForSelector('#lv-endday', { timeout: 5000 });
    for (let d = 2; d <= 7; d++) {
        await page.click('#lv-endday');
        await page.waitForSelector(`h2:has-text("Day ${d + 1} in Thornbury")`, { timeout: 20000 })
            .catch(() => {}); /* day 8 renders the report, not a day header */
        if (d === 7) await page.waitForSelector('#lv-report', { timeout: 20000 });
    }
    await page.waitForSelector('#lv-report', { timeout: 5000 });
    const reportText = await page.textContent('#lv-report');
    check(/Village report/.test(reportText), 'V: the run reaches the village report');
    check(/thriving|settled|struggling/.test(reportText), `V: the report names its ending kind`);
    check(/Disputes opened \/ resolved/.test(reportText), 'V: the report summarizes disputes');
    await shot(page, 'gamelab-village-report.png');

    /* Reset from the report: back to the briefing, counter zeroed. */
    await page.click('#lv-reset-report');
    await page.waitForSelector('#lv-begin', { timeout: 5000 });
    check(await page.evaluate(`${GL()}.liveSession('living-village').actionLog.length`) === 0,
        'V: reset clears the shared action log');
    check(await page.evaluate(`${GL()}.liveSession('living-village').seed`) === 20261109,
        'V: reset retains the effective seed');

    /* Replay the recorded run: real re-invocations, byte-identical. */
    await page.click('#lv-begin');
    await page.waitForSelector('#lv-replay', { timeout: 5000 });
    await page.click('#lv-replay');
    await page.waitForSelector('#lv-replay-report', { timeout: 30000 });
    const vReplay = await page.textContent('#lv-replay-report');
    check(/byte-identical envelopes/.test(vReplay), 'V: the replay report compares envelopes');
    const identicalMatch = vReplay.match(/Byte-identical envelopes(\d+) \/ (\d+)/);
    check(identicalMatch !== null && identicalMatch[1] === identicalMatch[2] && Number(identicalMatch[1]) >= 49,
        `V: every recorded envelope replayed byte-identical (got ` +
        `${identicalMatch ? identicalMatch[1] + ' / ' + identicalMatch[2] : 'no match'})`);
    check(/Failuresnone/.test(vReplay.replace(/\s+/g, '')), 'V: the replay aborted for no reason');

    /* ================================================================== */
    /* Responsive layout check (390x844, phone). */
    const mobile = await context.newPage();
    await mobile.setViewportSize({ width: 390, height: 844 });
    await mobile.goto(`${base}/gamelab/index.html`, { waitUntil: 'networkidle' });
    await mobile.waitForFunction(`${GL()} !== undefined`, null, { timeout: 15000 });
    await mobile.waitForFunction(`${GL()}.adapter.state === 'ready'`, null, { timeout: 30000 });
    const overflow = await mobile.evaluate(
        'document.documentElement.scrollWidth - document.documentElement.clientWidth');
    check(overflow <= 2, `no horizontal overflow at 390px (got ${overflow}px)`);
    const navVisible = await mobile.evaluate(
        'document.querySelector(".main-nav").getBoundingClientRect().width > 100');
    check(navVisible, 'navigation remains usable at 390px');
    await shot(mobile, 'gamelab-responsive-390.png');
    await mobile.close();

    /* ================================================================== */
    /* Initialization failure handling (fresh page, WASM blocked)          */
    /* ================================================================== */
    const page2 = await context.newPage();
    const page2Errors = [];
    page2.on('pageerror', e => page2Errors.push(String(e)));
    await page2.route('**/*.wasm', route => route.abort());
    await page2.goto(`${base}/gamelab/index.html`, { waitUntil: 'domcontentloaded' });
    await page2.waitForFunction(
        `${GL()} && (${GL()}.adapter.state === 'error' || ${GL()}.adapter.initFailureHandled === true)`,
        null, { timeout: 20000 });
    const errState = await page2.evaluate(`${GL()}.adapter.state`);
    check(errState === 'error', `initialization failure surfaces as an error state (got "${errState}")`);
    const errCard = await page2.textContent('.engine-card');
    check(/initialization failed/i.test(errCard), 'dashboard shows a visible initialization failure diagnostic');
    const decisionsMade = await page2.evaluate(`${GL()}.adapter ? true : false`);
    check(decisionsMade, 'adapter object still exists for diagnostics');
    const noDecisions = await page2.evaluate(
        `${GL()}.adapter.invoke({primitive:'choice',instruction:'x',options:['a'],context:'c'}).ok`);
    check(noDecisions === false, 'no decision can be produced after failed initialization (no silent fallback)');
    await page2.close();

    /* ================================================================== */
    /* Stage 2.1.1: cleanup failure and recovery through the settings UI   */
    /* (fresh page; the failure is injected at the engine-source seam -    */
    /* the kernel underneath stays REAL; nothing is mocked)                */
    /* ================================================================== */
    const page3 = await context.newPage();
    const page3Errors = [];
    const page3Console = [];
    page3.on('pageerror', e => page3Errors.push(String(e)));
    page3.on('console', m => { if (m.type() === 'error') page3Console.push(m.text()); });
    /* Install the trap BEFORE any page script runs: when the engine
     * UMD assigns window.OmniDecide, it is replaced by a wrapper whose
     * create() arms the FIRST kernel (the boot kernel). That kernel's
     * first free() throws BEFORE the real release - so the recreation
     * path (which releases this previous instance) hits the failure.
     * The kernel underneath stays REAL; nothing is replaced or mocked. */
    await page3.addInitScript(`(() => {
        let armed = true;
        Object.defineProperty(window, 'OmniDecide', {
            configurable: true,
            set(v) {
                const wrappedCreate = async (config) => {
                    const kernel = await v.create(config);
                    if (!armed) return kernel;
                    armed = false;
                    const realFree = kernel.free.bind(kernel);
                    kernel.free = () => {
                        if (!realFree.__done) {
                            realFree.__done = true;
                            throw new Error('simulated old-instance release failure (browser)');
                        }
                        return realFree();
                    };
                    return kernel;
                };
                const wrapped = Object.assign(Object.create(v), { create: wrappedCreate });
                Object.defineProperty(window, 'OmniDecide', { value: wrapped, writable: true, configurable: true });
            },
            get() { return undefined; }
        });
    })()`);
    await page3.goto(`${base}/gamelab/index.html`, { waitUntil: 'networkidle' });
    await page3.waitForFunction(`${GL()} !== undefined`, null, { timeout: 15000 });
    await page3.waitForFunction(`${GL()}.adapter.state === 'ready'`, null, { timeout: 30000 });
    await page3.click('a[data-nav="about"]');
    await page3.waitForSelector('.settings-card', { timeout: 5000 });

    /* Recreate through the settings UI: succeeds, cleanup failure reported. */
    await page3.selectOption('#set-mode', 'unicode');
    await page3.click('.settings-card .intro-actions button:has-text("Apply engine settings")');
    await page3.waitForFunction(`${GL()}.adapter.lifecycleStats.pendingCleanup === 1`, null, { timeout: 15000 });
    check((await page3.textContent('#settings-note')).includes('WARNING'),
        'R: the settings note reports the failed old-instance release honestly');
    const pendingState = await page3.evaluate(`({
        pending: ${GL()}.adapter.lifecycleStats.pendingCleanup,
        detail: ${GL()}.adapter.pendingCleanupDetail,
        live: ${GL()}.adapter.engineCount(),
        state: ${GL()}.adapter.state
    })`);
    check(pendingState.pending === 1 && pendingState.detail.length === 1 &&
          pendingState.detail[0].reason === 'recreate',
        'R: the failed handle stays adapter-owned and visible in the pending-cleanup registry');
    check(pendingState.live === 2,
        `R: the real WASM registry holds both handles while cleanup is pending (got ${pendingState.live})`);
    check(pendingState.state === 'ready', 'R: the replacement kernel remains correctly managed (ready)');
    const rDecision = await page3.evaluate(
        `${GL()}.adapter.invoke({primitive:'choice',instruction:'Which lead is best supported?',options:['a','b'],context:'evidence'}).ok`);
    check(rDecision === true, 'R: the replacement still executes real decisions');

    /* Recovery through the dedicated settings control. */
    await page3.click('#set-retry-cleanup');
    await page3.waitForFunction(`${GL()}.adapter.lifecycleStats.pendingCleanup === 0`, null, { timeout: 10000 });
    check((await page3.textContent('#settings-note')).includes('Cleanup recovery released 1'),
        'R: the retry control reports the recovered handle');
    const afterRecovery = await page3.evaluate(`({
        pending: ${GL()}.adapter.lifecycleStats.pendingCleanup,
        live: ${GL()}.adapter.engineCount(),
        recoveries: ${GL()}.adapter.lifecycleStats.cleanupRecoveries
    })`);
    check(afterRecovery.live === 1,
        `R: the real WASM registry is back to exactly the active handle (got ${afterRecovery.live})`);
    check(afterRecovery.recoveries === 1, 'R: the recovery is visible in the lifecycle accounting');
    const panelAfter = await page3.textContent('#engine-lifecycle-stats');
    check(panelAfter.includes('0 (all owned handles released)'),
        'R: the panel shows the honest zero-pending state after recovery');

    /* The adapter logged the cleanup failure (expected diagnostic), and
     * no uncaught page error occurred. */
    check(page3Console.some((t) => t.includes('kernel release failed')),
        'R: the cleanup failure was logged as an explicit diagnostic (never silent)');
    check(page3Errors.length === 0, `R: no uncaught page errors during the failure/recovery cycle (got ${page3Errors[0] || ''})`);
    await page3.close();

    /* ================================================================== */
    /* Console / network hygiene                                           */
    /* ================================================================== */
    check(consoleErrors.length === 0, `no browser console errors (got ${consoleErrors.length}: ${consoleErrors[0] || ''})`);
    check(pageErrors.length === 0, `no uncaught page errors (got ${pageErrors.length}: ${pageErrors[0] || ''})`);
    check(failedRequests.length === 0, `no failed network requests (got ${failedRequests.length}: ${failedRequests[0] || ''})`);

    await browser.close();
    server.close();

    /* --- summary ------------------------------------------------------- */
    console.log('');
    console.log(`gamelab_browser_check: ${checks} checks, ${failures} failures`);

    const report = {
        timestamp: new Date().toISOString(),
        browser: 'chromium (playwright)',
        url: `${base}/gamelab/index.html`,
        checks,
        failures,
        consoleErrors,
        pageErrors,
        failedRequests,
        wasmResponse,
        screenshots: [
            'gamelab/docs/img/gamelab-dashboard.png',
            'gamelab/docs/img/gamelab-launcher.png',
            'gamelab/docs/img/gamelab-briefing.png',
            'gamelab/docs/img/gamelab-investigation.png',
            'gamelab/docs/img/gamelab-workspace.png',
            'gamelab/docs/img/gamelab-inspector.png',
            'gamelab/docs/img/gamelab-history.png',
            'gamelab/docs/img/gamelab-benchmarks.png',
            'gamelab/docs/img/gamelab-ending.png',
            'gamelab/docs/img/gamelab-replay.png',
            'gamelab/docs/img/gamelab-village-briefing.png',
            'gamelab/docs/img/gamelab-village.png',
            'gamelab/docs/img/gamelab-village-report.png',
            'gamelab/docs/img/gamelab-about.png',
            'gamelab/docs/img/gamelab-responsive-390.png'
        ]
    };
    mkdirSync(join(ROOT, 'build'), { recursive: true });
    writeFileSync(join(ROOT, 'build', 'gamelab-browser-check.json'), JSON.stringify(report, null, 2) + '\n');

    if (failures > 0) {
        failureLines.forEach(l => console.error('  - ' + l));
        process.exit(1);
    }
    console.log('gamelab_browser_check: OK');
}

main().catch(e => {
    console.error('gamelab_browser_check: FATAL', e);
    process.exit(1);
});
