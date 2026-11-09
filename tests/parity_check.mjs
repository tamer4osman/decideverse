/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * parity_check.mjs - native versus WebAssembly envelope parity.
 *
 *   node tests/parity_check.mjs
 *
 * Runs every request in tests/requests.jsonl through BOTH hosts:
 *   1. native:  build/native/odk_parity_runner (same C wrapper, glibc)
 *   2. wasm:    dist/omnidecide.wasm via js/omnidecide.js
 * and compares the "id<TAB>status<TAB>envelope" lines byte-for-byte.
 *
 * There is NO silent skip path: a missing native runner is a hard
 * failure (build it with ./build-native.sh first), a missing WASM
 * artifact is a hard failure, and any mismatch or missing id is a hard
 * failure.  Envelopes contain no wall-clock or environment-dependent
 * values, so no field is excluded and no tolerance is applied.
 */

import { createRequire } from 'node:module';
import { readFileSync, writeFileSync, existsSync } from 'node:fs';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

const require = createRequire(import.meta.url);
const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = dirname(HERE);

const RUNNER = process.env.ODK_PARITY_RUNNER ||
    join(ROOT, 'build', 'native', 'odk_parity_runner');
const REQUESTS = join(ROOT, 'tests', 'requests.jsonl');
const NATIVE_TSV = process.env.ODK_NATIVE_TSV ||
    join(ROOT, 'build', 'native', 'native_envelopes.tsv');

function die(msg) {
    console.error('parity: FAILED PREREQUISITE: ' + msg);
    console.error('parity: no silent skips - fix the prerequisite and rerun.');
    process.exit(2);
}

if (!existsSync(RUNNER)) {
    die(`native parity runner not found at ${RUNNER}. Run ./build-native.sh first.`);
}
if (!existsSync(REQUESTS)) {
    die(`request corpus missing: ${REQUESTS}`);
}

/* 1. Regenerate the native envelopes from the exact corpus. */
console.log('parity: running native parity runner over the corpus...');
execFileSync(RUNNER, [REQUESTS, NATIVE_TSV], { stdio: ['ignore', 'ignore', 'inherit'] });

/* 2. Load native lines. */
const nativeLines = readFileSync(NATIVE_TSV, 'utf8').split('\n').filter(l => l.length > 0);
const native = new Map();
for (const line of nativeLines) {
    const tab1 = line.indexOf('\t');
    const tab2 = line.indexOf('\t', tab1 + 1);
    if (tab1 < 0 || tab2 < 0) die(`malformed native line: ${line.slice(0, 80)}`);
    const id = line.slice(0, tab1);
    const status = line.slice(tab1 + 1, tab2);
    const envelope = line.slice(tab2 + 1);
    native.set(id, { status, envelope });
}

/* 3. Run the WASM side over the identical corpus. */
const OD = require(join(ROOT, 'js', 'omnidecide.js'));
const wasmLines = [];
const kernelCache = new Map();

async function wasmRun() {
    const requests = readFileSync(REQUESTS, 'utf8').split('\n')
        .map(l => l.trim()).filter(l => l.length > 0);

    for (const line of requests) {
        const spec = JSON.parse(line);
        const kernelCfg = spec.kernel || {};
        const mode = kernelCfg.mode || 'ascii';
        const language = kernelCfg.language || 'und';
        const key = mode + '\u0000' + language;

        let envelopeRaw = null;
        let status = null;

        if (!kernelCache.has(key)) {
            try {
                kernelCache.set(key, await OD.OmniDecide.create({ mode, language }));
            } catch (e) {
                /* Kernel creation failure: compose the identical error
                 * envelope the native runner emits (wrapper-authored
                 * message, no control characters, same escaping). */
                kernelCache.set(key, { __error: e });
            }
        }
        const entry = kernelCache.get(key);

        if (entry.__error) {
            const err = entry.__error;
            status = String(err.status);
            envelopeRaw = JSON.stringify({ error: { status: err.status, message: err.message } });
        } else {
            const kernel = entry;
            const req = spec.request || {};
            const request = {
                primitive: req.primitive,
                instruction: req.instruction || '',
                options: Array.isArray(req.options) ? req.options
                    : (Array.isArray(req.rubric) ? req.rubric : undefined),
                context: req.context || '',
                topK: typeof req.topK === 'number' ? req.topK : 1,
                caps: req.caps || undefined,
                meta: !!req.meta
            };
            let raw;
            try {
                raw = kernel.evaluateRaw(request);
            } catch (e) {
                if (e instanceof OD.OmniDecideError && typeof e.envelope === 'string') {
                    raw = e.envelope; /* error envelope: use the exact C bytes */
                } else {
                    throw e;
                }
            }
            envelopeRaw = raw;
            const parsed = JSON.parse(raw);
            status = parsed && parsed.error ? String(parsed.error.status) : '0';
        }
        wasmLines.push({ id: spec.id, status, envelope: envelopeRaw });
    }
}

console.log('parity: running WASM side over the identical corpus...');
await wasmRun();

/* 4. Byte-for-byte comparison. */
let identical = 0;
const mismatches = [];
const missingInWasm = [];
const missingInNative = [];

for (const [id, n] of native) {
    const w = wasmLines.find(l => l.id === id);
    if (!w) { missingInWasm.push(id); continue; }
    if (n.status === w.status && n.envelope === w.envelope) {
        identical++;
    } else {
        mismatches.push({ id, native: n, wasm: w });
    }
}
for (const l of wasmLines) {
    if (!native.has(l.id)) missingInNative.push(l.id);
}

const total = native.size;
console.log('');
console.log(`parity: corpus cases: ${total}`);
console.log(`parity: byte-identical envelopes: ${identical}`);
console.log(`parity: mismatches: ${mismatches.length}`);
console.log(`parity: missing in WASM: ${missingInWasm.length}`);
console.log(`parity: missing in native: ${missingInNative.length}`);

if (mismatches.length > 0) {
    const first = mismatches[0];
    console.error(`parity: FIRST MISMATCH at id=${first.id}`);
    console.error('parity: native: ' + first.native.envelope.slice(0, 300));
    console.error('parity: wasm:   ' + first.wasm.envelope.slice(0, 300));
}
if (mismatches.length > 0 || missingInWasm.length > 0 || missingInNative.length > 0) {
    console.error('parity: FAILED');
    process.exit(1);
}

const report = {
    timestamp: new Date().toISOString(),
    corpus: total,
    byteIdentical: identical,
    mismatches: 0,
    missingInWasm: 0,
    missingInNative: 0,
    note: 'Envelopes are the wrapper-authored decision payloads; no field is excluded and no tolerance is applied.'
};
writeFileSync(join(ROOT, 'build', 'parity-report.json'), JSON.stringify(report, null, 2) + '\n');
console.log('parity: OK - native and WASM envelopes are byte-identical on every case');
