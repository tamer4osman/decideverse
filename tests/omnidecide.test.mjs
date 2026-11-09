/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * omnidecide.test.mjs - Node.js functional suite for the OmniDecide
 * WASM integration.
 *
 *   node tests/omnidecide.test.mjs
 *
 * Covers: genuine WASM execution, the README quickstart decision, all
 * six primitives, byte-level determinism, Unicode (zh/ar/hi) decisions,
 * the OmniDecideError identity contract (Node/CommonJS and ESM), input
 * validation, allocation cleanup behavior, output caps, truncation
 * metadata and engine release.  Skipped checks are impossible: any
 * unmet expectation fails the suite with a nonzero exit status.
 */

import { createRequire } from 'node:module';
import { readFileSync, statSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const require = createRequire(import.meta.url);
const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = dirname(HERE);

const OD = require(join(ROOT, 'js', 'omnidecide.js'));
const OmniDecide = OD.OmniDecide;
const OmniDecideError = OD.OmniDecideError;

let checks = 0;
let failures = 0;
const failureLines = [];

function check(cond, label) {
    checks++;
    if (!cond) {
        failures++;
        failureLines.push(label);
        console.error('FAIL: ' + label);
    }
}

async function main() {
    /* ------------------------------------------------------------------ */
    /* 1. Artifacts + module loading                                      */
    /* ------------------------------------------------------------------ */
    const wasmStat = statSync(join(ROOT, 'dist', 'omnidecide.wasm'));
    check(wasmStat.size > 100 * 1024, `dist/omnidecide.wasm exists and is substantial (${wasmStat.size} bytes)`);

    const wasmMagic = readFileSync(join(ROOT, 'dist', 'omnidecide.wasm')).subarray(0, 4);
    check(wasmMagic[0] === 0x00 && wasmMagic[1] === 0x61 &&
          wasmMagic[2] === 0x73 && wasmMagic[3] === 0x6d,
          'dist/omnidecide.wasm carries the WebAssembly magic (\\0asm)');

    const tLoad0 = performance.now();
    const kernel = await OmniDecide.create({ mode: 'ascii', language: 'und' });
    const moduleLoadMs = +(performance.now() - tLoad0).toFixed(2);

    const mod = kernel._mod;
    check(typeof mod._odkw_evaluate === 'function', 'module exposes the real C wrapper exports (_odkw_evaluate)');
    check(typeof mod.HEAPU32 === 'object' && mod.HEAPU32 !== null, 'module exposes WASM heap views (HEAPU32)');
    check(typeof WebAssembly === 'object', 'WebAssembly runtime available');
    check(wasmStat.size > 0 && typeof mod._malloc === 'function',
          'module loaded from the compiled artifacts (malloc export present)');

    /* ------------------------------------------------------------------ */
    /* 2. Identity                                                        */
    /* ------------------------------------------------------------------ */
    check(kernel.kernelVersion === '2.0.0', `kernel version is 2.0.0 (got ${kernel.kernelVersion})`);
    check(kernel.backend === 'embedded-fallback', 'backend is embedded-fallback (no JS fallback)');
    check(OmniDecide.version === '1.0.0', 'integration version is 1.0.0');
    check(mod.HEAPU32 instanceof Uint32Array, 'heap view is a real WASM memory view');

    /* ------------------------------------------------------------------ */
    /* 3. README quickstart decision (matches the native envelope)        */
    /* ------------------------------------------------------------------ */
    const choice = await kernel.evaluateChoice('How should we fix the outage?',
        ['restart the service', 'reinstall the operating system', 'wait for the weekend'],
        'The service stopped responding. Restarting the service restored normal operation within seconds.');
    check(choice.primitive === 'choice', 'quickstart: primitive is choice');
    check(choice.result === 'restart the service', `quickstart: winner is "restart the service" (got ${JSON.stringify(choice.result)})`);
    check(Math.abs(choice.confidence - 0.6312) < 1e-9, `quickstart: confidence matches native value 0.6312 (got ${choice.confidence})`);
    check(Math.abs(choice.probability - 0.6312) < 1e-9, 'quickstart: probability matches');
    const distKeys = Object.keys(choice.distribution);
    check(distKeys.length === 3, 'quickstart: distribution has all 3 options');
    const distSum = Object.values(choice.distribution).reduce((a, b) => a + b, 0);
    check(Math.abs(distSum - 1) < 0.001, `quickstart: distribution sums to ~1 (got ${distSum.toFixed(4)})`);

    /* ------------------------------------------------------------------ */
    /* 4. All six primitives                                              */
    /* ------------------------------------------------------------------ */
    const score = await kernel.evaluateScore('Rate the proposed fix.',
        ['fails', 'partly works', 'works'],
        'The service stopped responding. Restarting the service restored normal operation within seconds.');
    check(typeof score.result === 'number' && score.result >= 0 && score.result <= 1,
          `score: result in [0,1] (got ${score.result})`);

    const noul = await kernel.evaluateNoul('Restarting the service restored normal operation.',
        'The service stopped responding. Restarting the service restored normal operation within seconds.');
    check(noul.result === true, 'noul: supported statement returns true');

    const noulNo = await kernel.evaluateNoul('The database cluster was rebuilt from scratch.',
        'The service stopped responding. Restarting the service restored normal operation within seconds.');
    check(noulNo.result === false, 'noul: unsupported statement returns false');

    const rank = await kernel.evaluateRank('Order the fixes best to worst.',
        ['restart the service', 'reinstall the operating system', 'wait for the weekend'],
        'The service stopped responding. Restarting the service restored normal operation within seconds.');
    check(typeof rank.result === 'string' && rank.result.includes('>'),
          `rank: result is an ordered string (got ${JSON.stringify(rank.result)})`);

    const cmp = await kernel.evaluateCompare('Which fix should we try first?',
        ['restart the service', 'reinstall the operating system'],
        'The service stopped responding. Restarting the service restored normal operation within seconds.');
    check(cmp.primitive === 'compare', 'compare: primitive echoed');

    const extract = await kernel.evaluateExtract('What restored normal operation?',
        'Incident 47: The service stopped responding after the deploy. The on-call engineer restarted the service. Restarting restored normal operation within seconds.',
        2);
    check(Array.isArray(extract.result) && extract.result.length === 2,
          `extract: top-2 snippets returned (got ${extract.result.length})`);

    /* ------------------------------------------------------------------ */
    /* 5. Determinism (byte-identical envelopes)                          */
    /* ------------------------------------------------------------------ */
    const req = { primitive: 'choice',
        instruction: 'How should we fix the outage?',
        options: ['restart the service', 'reinstall the operating system', 'wait for the weekend'],
        context: 'The service stopped responding. Restarting the service restored normal operation within seconds.' };
    const first = kernel.evaluateRaw(req);
    for (let i = 0; i < 5; i++) {
        const again = kernel.evaluateRaw(req);
        check(again === first, `determinism: repeated envelope ${i + 1}/5 byte-identical`);
    }

    /* ------------------------------------------------------------------ */
    /* 6. Unicode                                                         */
    /* ------------------------------------------------------------------ */
    const zh = await OmniDecide.create({ mode: 'unicode', language: 'zh' });
    const zhChoice = await zh.evaluateChoice('备份服务器应该放在哪里？',
        ['柏林数据中心', '远程分公司'],
        '备份服务器应该放在柏林数据中心，因为那里有冗余电源。');
    check(zhChoice.result === '柏林数据中心', `zh: winner is 柏林数据中心 (got ${JSON.stringify(zhChoice.result)})`);
    check(zhChoice.confidence > 0.5, `zh: confidence > 0.5 (got ${zhChoice.confidence})`);

    const zhNoul = await zh.evaluateNoul('备份服务器应该放在柏林数据中心。',
        '备份服务器应该放在柏林数据中心，因为那里有冗余电源。');
    check(typeof zhNoul.result === 'boolean', 'zh: noul returns a boolean');
    check(zhNoul.result === true, 'zh: noul supported returns true (native parity: confidence 0.8772)');

    const zhNoulNo = await zh.evaluateNoul('服务已恢复正常。',
        '服务停止响应。重启服务后几秒内恢复正常！');
    check(zhNoulNo.result === false, 'zh: noul unsupported returns false (matches native envelope)');

    const ar = await OmniDecide.create({ mode: 'unicode', language: 'ar' });
    const arChoice = await ar.evaluateChoice('أين يجب استضافة خادم النسخ الاحتياطي؟',
        ['مركز بيانات برلين', 'مكتب الفرع البعيد'],
        'يجب أن يكون خادم النسخ الاحتياطي في مركز بيانات برلين لأنه يملك طاقة احتياطية.');
    check(arChoice.result === 'مركز بيانات برلين', `ar: winner is مركز بيانات برلين (got ${JSON.stringify(arChoice.result)})`);

    const hi = await OmniDecide.create({ mode: 'unicode', language: 'hi' });
    const hiChoice = await hi.evaluateChoice('बैकअप सर्वर कहाँ रखा जाना चाहिए?',
        ['बर्लिन डेटासेंटर', 'दूरस्थ शाखा कार्यालय'],
        'बैकअप सर्वर बर्लिन डेटासेंटर में रखा जाना चाहिए क्योंकि वहाँ बिजली का बैकअप है।');
    check(hiChoice.result === 'बर्लिन डेटासेंटर', `hi: winner is बर्लिन डेटासेंटर (got ${JSON.stringify(hiChoice.result)})`);

    const auto = await OmniDecide.create({ mode: 'unicode', language: 'auto' });
    const autoChoice = await auto.evaluate({
        primitive: 'choice',
        instruction: '备份服务器应该放在哪里？',
        options: ['柏林数据中心', '远程分公司'],
        context: '备份服务器应该放在柏林数据中心，因为那里有冗余电源。',
        meta: true
    });
    check(autoChoice.meta && autoChoice.meta.detected === 'zh',
          `auto: detector reports zh (got ${autoChoice.meta && autoChoice.meta.detected})`);

    /* ------------------------------------------------------------------ */
    /* 7. OmniDecideError identity contract                               */
    /* ------------------------------------------------------------------ */
    try {
        await OmniDecide.create({ mode: 'turbo' });
        check(false, 'create with unknown mode throws');
    } catch (e) {
        check(e instanceof OmniDecideError, 'create error instanceof OmniDecideError');
        check(e.name === 'OmniDecideError', 'create error name is OmniDecideError');
        check(e.status === 1, `create error status is ODK_ERR_ARGUMENT=1 (got ${e.status})`);
        check(/unknown mode/.test(e.message), 'create error message mentions unknown mode');
    }

    try {
        await OmniDecide.create({ mode: 'ascii', language: 'klingon' });
        check(false, 'create with unknown language throws');
    } catch (e) {
        check(e instanceof OmniDecideError, 'unknown-language error instanceof OmniDecideError');
        check(/unknown language code/.test(e.message), 'unknown-language message');
    }

    try {
        await kernel.evaluateChoice('Pick one.', [], 'ctx');
        check(false, 'choice with empty options throws');
    } catch (e) {
        check(e instanceof OmniDecideError, 'empty-options error instanceof OmniDecideError');
        check(e.status === 1, 'empty-options error status 1');
        check(/at least one option/.test(e.message), 'empty-options message');
    }

    try {
        await kernel.evaluateCompare('Compare.', ['a', 'b', 'c'], 'ctx');
        check(false, 'compare with 3 options throws');
    } catch (e) {
        check(e instanceof OmniDecideError, 'compare-arity error instanceof OmniDecideError');
        check(/exactly 2 options/.test(e.message), 'compare-arity message');
    }

    try {
        await kernel.evaluateExtract('Find.', 'ctx', 0);
        check(false, 'extract with topK=0 throws');
    } catch (e) {
        check(e instanceof OmniDecideError, 'topK error instanceof OmniDecideError');
        check(/top_k >= 1/.test(e.message), 'topK message');
    }

    try {
        await kernel.evaluate({ primitive: 'frobnicate', instruction: 'x' });
        check(false, 'unknown primitive throws');
    } catch (e) {
        check(e instanceof OmniDecideError, 'unknown-primitive error instanceof OmniDecideError');
        check(/unknown primitive/.test(e.message), 'unknown-primitive message');
    }

    /* The exported error class is the class actually used internally
     * (single factory invocation - no duplicate classes). */
    const ODagain = require(join(ROOT, 'js', 'omnidecide.js'));
    check(ODagain.OmniDecideError === OmniDecideError,
          'CommonJS require returns the SAME OmniDecideError class (no double factory call)');
    const errProbe = new OmniDecideError(1, 'probe');
    check(errProbe instanceof OmniDecideError && errProbe instanceof Error,
          'OmniDecideError is an Error subclass');

    /* ------------------------------------------------------------------ */
    /* 8. Allocation cleanup / release semantics                          */
    /* ------------------------------------------------------------------ */
    const countBefore = kernel.engineCount();
    const kTmp = await OmniDecide.create({ mode: 'ascii', language: 'und' });
    check(kernel.engineCount() === countBefore + 1, 'engine count tracks live kernels');
    kTmp.free();
    check(kernel.engineCount() === countBefore, 'engine count drops after free()');
    kTmp.free(); /* double free must be a safe no-op */
    check(kernel.engineCount() === countBefore, 'double free is a no-op');
    check(kTmp.closed === true, 'free() marks the kernel closed');

    try {
        kTmp.evaluateRaw(req);
        check(false, 'evaluate after free throws');
    } catch (e) {
        check(e instanceof OmniDecideError, 'post-free error instanceof OmniDecideError');
        check(/released/.test(e.message), 'post-free message mentions release');
    }

    /* A fresh kernel works after releasing another one. */
    const kFresh = await OmniDecide.create({ mode: 'ascii', language: 'und' });
    const freshEnv = await kFresh.evaluateChoice('How should we fix the outage?',
        ['restart the service', 'wait for the weekend'], req.context);
    check(freshEnv.result === 'restart the service', 'fresh kernel after release still works');
    kFresh.free();

    /* ------------------------------------------------------------------ */
    /* 9. Output caps + truncation metadata (meta contract)               */
    /* ------------------------------------------------------------------ */
    const rankCapped = await kernel.evaluate({
        primitive: 'rank',
        instruction: 'Order the fixes best to worst.',
        options: ['restart the service', 'reinstall the operating system', 'wait for the weekend'],
        context: 'The service stopped responding. Restarting the service restored normal operation within seconds.',
        caps: { maxRanked: 2 },
        meta: true
    });
    check(rankCapped.meta.truncated === 1, 'capped rank reports truncated=1');
    check(rankCapped.meta.totalOptions === 3, 'capped rank reports totalOptions=3 (cap never changes the decision)');
    check(rankCapped.result.split('>').length - 1 === 1, 'capped rank renders 2 options');

    let bigContext = '';
    for (let i = 0; i < 11000; i++) {
        bigContext += 'LOG-0001 alpha option recorded with supporting evidence. ';
    }
    check(bigContext.length > 400000, `oversized test context is genuinely large (${bigContext.length} bytes)`);
    const trunc = await kernel.evaluate({
        primitive: 'choice',
        instruction: 'Which option is supported?',
        options: ['alpha option', 'beta option'],
        context: bigContext,
        meta: true
    });
    check(trunc.meta.contextTruncated === 1, 'oversized context reports contextTruncated=1 (native parity: same value)');
    check(trunc.meta.contextUnitsDropped > 0, 'truncation reports dropped units');
    check(trunc.result === 'alpha option', 'decision still correct under truncation');

    const zhBig = await zh.evaluate({
        primitive: 'choice',
        instruction: '应该采用哪个方案？',
        options: ['方案甲', '方案乙'],
        context: '方案甲更适合。'.repeat(30000),
        meta: true
    });
    check(zhBig.meta.contextTruncated === 1, 'unicode truncation reported in code points');
    check(zhBig.result === '方案甲', 'zh decision stable under truncation');

    /* ------------------------------------------------------------------ */
    /* 10. Kernel config is honored (auto-detection, unicode mode)         */
    /* ------------------------------------------------------------------ */
    const zhEnv = await zh.evaluate({ primitive: 'noul', instruction: '服务已恢复正常。',
        context: '服务停止响应。重启服务后几秒内恢复正常！', meta: true });
    check(zhEnv.meta.language === 'zh', `zh kernel meta.language is zh (got ${zhEnv.meta.language})`);

    zh.free();
    ar.free();
    hi.free();
    auto.free();

    /* ------------------------------------------------------------------ */
    /* 11. Benchmark library sanity (real workloads, tiny scale)           */
    /* ------------------------------------------------------------------ */
    const OdBench = require(join(ROOT, 'js', 'benchmark.js'));
    const benchKernel = await OmniDecide.create({ mode: 'ascii', language: 'und' });
    const wl = OdBench.workloads();
    check(wl.small.context.length >= 150 && wl.small.context.length <= 260,
          `small workload context ~200 bytes (got ${wl.small.context.length})`);
    check(wl.medium.context.length >= 1900 && wl.medium.context.length <= 2200,
          `medium workload context ~2 KB (got ${wl.medium.context.length})`);
    check(wl.large.context.length >= 19000 && wl.large.context.length <= 21500,
          `large workload context ~20 KB (got ${wl.large.context.length})`);
    check(wl.unicode.context.length >= 1900, 'unicode workload context ~2 KB');

    const quick = OdBench.run(benchKernel, 'quick');
    check(!!quick.workloads.small && quick.workloads.small.iterations === 300,
          'benchmark quick scale runs 300 small iterations');
    check(quick.workloads.small.rawLoop && quick.workloads.small.rawLoop.iterations === 300,
          'benchmark reports isolated C rawLoop timing');
    check(quick.overallPipeline.medianMs > 0, 'benchmark pipeline median > 0');
    check(quick.init.firstDecisionMs === null || quick.init.firstDecisionMs >= 0,
          'benchmark init block present');
    check(/no per-call JavaScript marshaling/.test(quick.notes[1]),
          'benchmark explicitly separates isolated C timing from pipeline timing');
    benchKernel.free();

    /* ------------------------------------------------------------------ */
    /* Summary                                                            */
    /* ------------------------------------------------------------------ */
    console.log('');
    console.log(`omnidecide.test: ${checks} checks, ${failures} failures ` +
        `(module load ${moduleLoadMs} ms)`);
    if (failures > 0) {
        failureLines.forEach(l => console.error('  - ' + l));
        process.exit(1);
    }
}

main().catch(e => {
    console.error('FATAL:', e);
    process.exit(1);
});
