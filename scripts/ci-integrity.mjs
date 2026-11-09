/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * ci-integrity.mjs - machine-readable artifact integrity report.
 *
 *   node scripts/ci-integrity.mjs \
 *       --ci    artifacts/ci-wasm \
 *       --fresh build/fresh-wasm \
 *       --out   build/artifact-integrity.json \
 *       [--build-exit 0] [--build-cmd "./build.sh --output-dir build/fresh-wasm"]
 *
 * Compares the downloaded CI artifact (omnidecide.wasm + omnidecide.js)
 * against a freshly rebuilt copy byte-for-byte (cmp) and by SHA-256,
 * records toolchain and kernel provenance, and writes a JSON report.
 *
 * Exit codes: 0 every comparison passed, 1 any mismatch (the report is
 * still written so the failed job leaves evidence behind).  There is no
 * fallback that merely prints new hashes: a mismatch is a hard failure.
 */

import { readFileSync, writeFileSync, statSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import { dirname, join, relative } from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = dirname(fileURLToPath(import.meta.url));
const ROOT = dirname(HERE);

function argValue(flag) {
    const i = process.argv.indexOf(flag);
    if (i < 0) return null;
    const v = process.argv[i + 1];
    if (!v || v.startsWith('--')) {
        console.error(`ci-integrity: ${flag} requires a value`);
        process.exit(2);
    }
    return v;
}

const CI_DIR = argValue('--ci');
const FRESH_DIR = argValue('--fresh');
const OUT = argValue('--out') || join(ROOT, 'build', 'artifact-integrity.json');
const BUILD_EXIT = Number(argValue('--build-exit') ?? '0');
const BUILD_CMD = argValue('--build-cmd') || './build.sh --output-dir <fresh-dir>';

function die(msg) {
    console.error('ci-integrity: FAILED PREREQUISITE: ' + msg);
    process.exit(2);
}

for (const [label, dir] of [['--ci', CI_DIR], ['--fresh', FRESH_DIR]]) {
    if (!dir) die(`missing required argument ${label} <dir>`);
}

const FILES = ['omnidecide.wasm', 'omnidecide.js'];

/* ---- hash + byte-compare each file ---------------------------------- */
function sha256(path) {
    const r = spawnSync('sha256sum', [path], { encoding: 'utf8' });
    if (r.status !== 0) die(`sha256sum failed for ${path}: ${r.stderr}`);
    return r.stdout.trim().split(/\s+/)[0];
}

const ciFiles = [];
const freshFiles = [];
const byteComparison = [];
const sha256Comparison = [];
let mismatches = 0;

for (const f of FILES) {
    const ciPath = join(CI_DIR, f);
    const freshPath = join(FRESH_DIR, f);
    try { statSync(ciPath); } catch { die(`downloaded artifact file missing: ${ciPath}`); }
    try { statSync(freshPath); } catch { die(`freshly built file missing: ${freshPath}`); }

    const ciSha = sha256(ciPath);
    const freshSha = sha256(freshPath);
    const cmp = spawnSync('cmp', ['-s', ciPath, freshPath]);
    const identical = cmp.status === 0;
    if (!identical) mismatches++;
    byteComparison.push({
        file: f,
        ciPath: relative(ROOT, ciPath),
        freshPath: relative(ROOT, freshPath),
        byteIdentical: identical,
        cmpExitStatus: cmp.status
    });
    const shaMatch = ciSha === freshSha;
    if (!shaMatch) mismatches++;
    sha256Comparison.push({ file: f, match: shaMatch });
    ciFiles.push({ file: f, sha256: ciSha, bytes: statSync(ciPath).size });
    freshFiles.push({ file: f, sha256: freshSha, bytes: statSync(freshPath).size });
}

/* ---- toolchain -------------------------------------------------------- */
function firstLine(cmd, args) {
    const r = spawnSync(cmd, args, { encoding: 'utf8' });
    if (r.status !== 0) die(`${cmd} ${args.join(' ')} failed`);
    return r.stdout.split('\n')[0].trim();
}
const emccVersion = firstLine('emcc', ['--version']);

/* ---- kernel provenance ------------------------------------------------ */
const kernelHeader = readFileSync(
    join(ROOT, 'vendor', 'omnidecide-c', 'include', 'omnidecide.h'), 'utf8');
const kernelVersion =
    (kernelHeader.match(/#define ODK_LIBRARY_VERSION "([^"]+)"/) || [])[1] || 'unknown';

const pinEnv = readFileSync(join(ROOT, 'scripts', 'kernel-pin.env'), 'utf8');
const pin = (name) =>
    (pinEnv.match(new RegExp(`^${name}=(\\S+)`, 'm')) || [])[1] || null;

const fingerprintActual = firstLine('bash', [join(ROOT, 'scripts', 'source-manifest.sh'),
    join(ROOT, 'vendor', 'omnidecide-c')]);
const fingerprintRecorded = pin('ODK_VENDOR_FINGERPRINT');
const fingerprintMatch = fingerprintActual === fingerprintRecorded;
if (!fingerprintMatch) mismatches++;

/* ---- report ------------------------------------------------------------ */
const report = {
    generatedAt: new Date().toISOString(),
    build: {
        command: BUILD_CMD,
        exitStatus: BUILD_EXIT,
        outputDir: relative(ROOT, FRESH_DIR),
        note: 'Fresh build from the vendored pinned source with an independently initialized Emscripten toolchain; the downloaded artifact directory was never written to.'
    },
    downloadedArtifact: {
        dir: relative(ROOT, CI_DIR),
        files: ciFiles
    },
    freshBuild: {
        dir: relative(ROOT, FRESH_DIR),
        files: freshFiles
    },
    byteComparison,
    sha256Comparison,
    toolchain: {
        emcc: emccVersion,
        node: process.version,
        emsdkAction: 'emscripten-core/setup-emsdk pinned to v16 (4528d102f7230f0e7b276855c01ea1159be0e984)'
    },
    kernel: {
        version: kernelVersion,
        snapshotCommit: pin('ODK_SNAPSHOT_COMMIT'),
        fingerprintRecorded,
        fingerprintActual,
        fingerprintMatch
    },
    comparisonsPassed: mismatches === 0
};

writeFileSync(OUT, JSON.stringify(report, null, 2) + '\n');
console.log(`ci-integrity: report written to ${OUT}`);

console.log('');
console.log('ci-integrity: downloaded artifact vs fresh build');
for (const c of byteComparison) {
    console.log(`  ${c.file}: cmp exit=${c.cmpExitStatus} ` +
        (c.byteIdentical ? 'BYTE-IDENTICAL' : 'DIFFERS'));
}
for (const f of ciFiles) {
    console.log(`  ${f.file}: sha256 ${f.sha256} (${f.bytes} bytes)`);
}
console.log(`  kernel: ${kernelVersion}, snapshot ${report.kernel.snapshotCommit}`);
console.log(`  fingerprint match: ${fingerprintMatch}`);
console.log(`  emcc: ${emccVersion}`);

if (mismatches > 0) {
    console.error(`ci-integrity: FAILED - ${mismatches} comparison mismatch(es)`);
    process.exit(1);
}
console.log('ci-integrity: OK - downloaded and freshly built artifacts are byte-identical');
