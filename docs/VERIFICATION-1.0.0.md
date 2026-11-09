# OmniDecide WASM 1.0.0 — Consolidated Verification Report

> **Decideverse context:** this report is the preserved evidence for the
> **finished** engine integration at the 1.0.0 tag. Games shipped on
> this engine since (The Last Detective, Living Village) carry their own
> suites and stage reports — platform status:
> `gamelab/docs/ROADMAP.md`.

**Generated:** 2026-11-08 · **Release:** omnidecide-wasm 1.0.0 (first
public release) · **Host:** Linux 5.10.134 x86_64 (Debian 12 userland),
Node v24.22.2, Emscripten 7.0.0
(`777e6e5db7b4990bcaeb1b9998ed642cc84e6629`), gcc 14.2.0,
CMake 4.4.4, Playwright 1.57.0, Chromium 143.0.7499.4.

Every number in this report was produced by a fresh run of the exact
release tree on the release host. Commands are listed verbatim; log
excerpts are truncated to the decisive lines. The machine-readable
counterparts live in `docs/evidence-1.0.0/` (verify summary, benchmark
JSONs, parity and browser reports, artifact integrity report) and
`docs/benchmarks/`.

**Development note (honest record):** before this first public release
the integration went through internal hardening milestones — an
environment recycle forced a documented reconstruction of an earlier
internal tree; browser/UMD and wrapper-allocation defects were found
and fixed by the new verification suites; the vendored pinned kernel
and the seven-job CI pipeline were added; and the pipeline itself was
repaired (Section 10). No earlier version was ever published. Every
claim below was verified against this exact tree as released.

---

## 1. Source pin verification

```
$ ./fetch-kernel.sh
fetch-kernel: OK: all required upstream files present
fetch-kernel: OK: header declares ODK_LIBRARY_VERSION 2.0.0
kernel-verify: OK: upstream version-check passed (header, CMake and CHANGELOG agree)
kernel-verify: OK: LICENSE, NOTICE and THIRD_PARTY_NOTICES.md present and non-empty
kernel-verify: OK: source manifest fingerprint matches the recorded pin
                  (151754b2fcd7b5db8101ca458caced68e11725dc7ca66f8a79c6293b1b392f1a)
fetch-kernel: resolved kernel source: <root>/vendor/omnidecide-c
fetch-kernel: pinned snapshot commit: 561fdb8ae43a4610d3df92cde5ba0a0ac5f63393
fetch-kernel: OK
```

Negative paths verified:

```
$ OMNIDECIDE_C=/tmp/fake-odk ./fetch-kernel.sh        # foreign commit
fetch-kernel: ERROR: external checkout commit 0e171ca4… does not match the
pinned snapshot commit 561fdb8a…. A build must never silently combine files
from different kernel revisions.        → exit 1
$ ./fetch-kernel.sh                                    # vendor absent
fetch-kernel: ERROR: no kernel source available. … does NOT download the
kernel at build time and does NOT fall back to the upstream default branch.
                                        → exit 1
```

Fingerprint sensitivity was verified by a one-byte edit of
`vendor/omnidecide-c/include/omnidecide.h`: the fingerprint changed and
`verify-kernel-source.sh` correctly FAILED until the file was restored.
An earlier internal fingerprint `05d8db72…` and commit `a2e688af…`
could not be reproduced/re-fetched (private repo; algorithm not
preserved) — full discussion in `docs/WASM-STATUS.md` §1.

---

## 2. Native build and upstream regression

```
$ ./build-native.sh --asan
build-native: cmake version 4.4.4
build-native: cc (Debian 14.2.0-19) 14.2.0
build-native: built build/native/odk_parity_runner
build-native: built build/native/odk_smoke
odk_smoke: 54 checks, 0 failures
parity-runner: 32 success envelopes, 8 error envelopes, 0 runner failures
build-native: running wrapper smoke test under ASan+LSan...
odk_smoke: 54 checks, 0 failures
build-native: ASan+LSan pass complete (0 leaks)
build-native: OK
```

```
$ cd vendor/omnidecide-c && make v2-gates
build/smoke_test          → 373 checks, 0 failures
build/spine_test          → spine_test: 210 checks, 0 failures
golden-check              → 2013 cases, all byte-identical
spine-golden-check        → 35 cases, all byte-identical
hostile                   → survived 5443 hostile checks (fixed-LCG, replayable)
stress                    → TOTAL checks=6135 failures=0; soak maxrss growth 0 KB
determinism               → OK (3 runs hash-identical: 71b87365…, d850ed91…, e4ba8f6c…)
sanitize                  → OK (ASan+UBSan: spine_test, golden_spine, synthetic_example)
seam-check / schema-check → OK / OK
check-default             → OK (zero registered customizations in the shipped library)
bench                     → choice @ 2KB: best 0.272 ms, avg 0.317 ms (budget <= 1.0 ms)
v2-gates: ALL GREEN
$ make docs-check    → docs-check: OK (60 relative links resolve)
$ make version-check → version-check: OK (version 2.0.0)
```

Exit statuses: 0 for every gate. Skipped checks: 0.

---

## 3. WASM build

```
$ ./build.sh
build.sh: emcc (Emscripten gcc/clang-like replacement + linker emulating GNU ld)
          7.0.0 (777e6e5db7b4990bcaeb1b9998ed642cc84e6629)
build.sh: wrote dist/omnidecide.js + dist/omnidecide.wasm (212763 bytes)
build.sh: artifact checksums:
build.sh:   3c1c8b0acdeb4d3eb8f9c47b115b15ed5017bf5f9951c1d13a351647a1ebbbdb  dist/omnidecide.wasm
build.sh:   c2e94942cd2c0d2b6fb1e1417c2dc17b15e65a1a3f6e7345be1be1f21e765d6e  dist/omnidecide.js
build.sh: OK
```

The shipped `dist/` artifacts hash byte-for-byte identical to freshly
rebuilt ones — the fresh-build check (Section 9, stage 6) is exercised
locally exactly as CI will:

```
ci-verify-dist: OK: dist contains omnidecide.js + omnidecide.wasm (magic 0061736d)
ci-verify-dist: OK: build/fresh-wasm contains omnidecide.js + omnidecide.wasm (magic 0061736d)
fresh-build check: dist/ rebuild is byte-identical (wasm + js)
```

`scripts/ci-integrity.mjs` was exercised with the same inputs CI uses
(positive case exit 0; negative case — corrupted copy — exit 1 with
the JSON report still written for post-mortem). The resulting local
report is preserved as `docs/evidence-1.0.0/artifact-integrity-local.json`:

```json
{
  "byteComparison": [
    { "file": "omnidecide.wasm", "byteIdentical": true, "cmpExitStatus": 0 },
    { "file": "omnidecide.js",   "byteIdentical": true, "cmpExitStatus": 0 }
  ],
  "toolchain": { "emcc": "emcc (…) 7.0.0 (777e6e5db7b4990bcaeb1b9998ed642cc84e6629)" },
  "kernel": { "version": "2.0.0",
              "snapshotCommit": "561fdb8ae43a4610d3df92cde5ba0a0ac5f63393",
              "fingerprintMatch": true },
  "comparisonsPassed": true
}
```

---

## 4. Node functional suite

```
$ node tests/omnidecide.test.mjs
omnidecide.test: 78 checks, 0 failures (module load 3.94 ms)
```

Covers: real WASM execution (`\0asm` magic, 212 KB module, heap views,
wrapper exports), README quickstart decision (winner
`restart the service`, confidence 0.6312 — the native value), all six
primitives, byte-level determinism (5/5 identical envelopes), Unicode
decisions (zh/ar/hi + auto-detection `detected: "zh"`),
`OmniDecideError` identity across CommonJS require and ESM import
(same class — single UMD factory invocation), validation errors with
correct statuses, release semantics (engine count tracking, double-free
no-op, post-release error), output caps (`truncated: 1`,
`totalOptions: 3` under `maxRanked: 2`), 580 KB context truncation
metadata (`contextTruncated: 1`, dropped units > 0), and the benchmark
library contract (sizes ~200 B / 2 KB / 20 KB, isolated-C timing
separation). The suite also asserts the integration version stamp
(`OmniDecide.version === '1.0.0'`).

---

## 5. Native/WASM parity

```
$ node tests/parity_check.mjs
parity: running native parity runner over the corpus...
parity-runner: 32 success envelopes, 8 error envelopes, 0 runner failures
parity: running WASM side over the identical corpus...
parity: corpus cases: 40
parity: byte-identical envelopes: 40
parity: mismatches: 0
parity: missing in WASM: 0
parity: missing in native: 0
parity: OK - native and WASM envelopes are byte-identical on every case
```

Byte-for-byte comparison, no excluded fields, no tolerances (the
envelope contract contains no wall-clock values — see
`docs/WASM-STATUS.md` §5 for corpus coverage). No silent skips: the
suite dies with an explicit failed prerequisite when the native runner
or `dist/` artifacts are missing.

Two real defects were found and fixed by this parity loop during
hardening (documented as evidence the comparison bites):
1. an uninitialized-tail bug in the wrapper's bounded-error excerpt
   helper leaked stack garbage into `unknown mode`/`unknown language`
   envelopes (native showed trailing junk; fixed by explicit
   termination — caught as a 38/40 mismatch before the fix);
2. a corpus-mapping bug in the harness (score rubric not mapped) —
   harness-side, fixed the same hour it appeared.

---

## 6. Browser verification (Playwright + Chromium)

```
$ node tests/browser_check.mjs
browser_check: serving http://127.0.0.1:35083 over HTTP (not file://)
browser_check: 29 checks, 0 failures
browser_check: OK
```

Machine-readable: `build/browser-check.json` (console errors: [],
page errors: [], failed requests: [], WASM response: 200,
`application/wasm`). Fresh screenshots written to `docs/img/` from this
run: `browser-quickstart.png`, `browser-distribution.png`,
`browser-unicode.png`, `browser-benchmark.png` (1280×900 each).

A real defect was found and fixed by browser verification: the UMD
factory closed over the wrapper's `root` parameter inside its factory
body (out of scope in browsers), so the Emscripten loader was never
found and initialization failed — surfaced by the suite as an error
state, fixed, and now covered by the passing checks (including the
`instanceof OmniDecideError` identity and init-failure handling).

---

## 7. Benchmarks (fresh, standard scale)

```
$ node tests/node_bench.mjs --scale standard
node_bench: module load 3.22 ms, kernel create 0.02 ms, first decision 3.02 ms
small (200 B context)   2000  median 0.053ms  p95 0.091  p99 0.136  14,594 ops/s
medium (2 KB context)    800  median 0.219ms  p95 0.230  p99 0.237   4,515 ops/s
large (20 KB context)    150  median 1.802ms  p95 1.830  p99 1.843     554 ops/s
unicode (~2 KB context)  800  median 0.090ms  p95 0.099  p99 0.109  10,844 ops/s
node_bench: JSON written to docs/benchmarks/node-bench.json (+ timestamped copy)

$ node tests/browser_bench.mjs --scale standard
browser_bench: page load 559 ms, module load 0.1 ms, first decision 2.8 ms
small (200 B context)   2000  median 0.100ms  p95 0.200  p99 0.200  14,327 ops/s
medium (2 KB context)    800  median 0.200ms  p95 0.300  p99 0.300   4,283 ops/s
large (20 KB context)    150  median 1.900ms  p95 2.000  p99 2.000     527 ops/s
unicode (~2 KB context)  800  median 0.100ms  p95 0.200  p99 0.200  10,101 ops/s
browser_bench: JSON written to docs/benchmarks/browser-bench.json (+ timestamped copy)
```

Both tools report the full JS→WASM pipeline separately from the
isolated C `rawLoop` timing and label the distinction explicitly; JSON
outputs embed environment metadata and the statement that no historical
data was reused.

---

## 8. Unified verification script

```
$ ./scripts/verify-release.sh --help    (stages documented)
$ ./scripts/verify-release.sh           (full run; see build/verify-logs/)
  01 prerequisites
  02 kernel source pin and manifest verification
  03 native build (kernel + integration wrapper)
  04 native wrapper smoke + ASan/LSan leak pass
  05 upstream kernel regression gates (incl. v2-gates aggregate)
  06 WASM build + fresh-build byte-identity check
  07 Node functional suite
  08 native/WASM envelope parity
  09 browser Playwright verification
  10 standard benchmarks (Node + browser)
  11 verification summary
```

Full-run summary (copied to `docs/evidence-1.0.0/verify-summary-1.0.0.txt`):

```
prerequisites                                               PASS
kernel source pin and manifest verification                 PASS
native build (kernel + integration wrapper)                 PASS
native wrapper smoke + ASan/LSan leak pass                  PASS
upstream kernel regression gates (incl. v2-gates aggregate) PASS
WASM build + fresh-build byte-identity check                PASS
Node functional suite                                       PASS
native/WASM envelope parity                                 PASS
browser Playwright verification                             PASS
standard benchmarks (Node + browser)                        PASS
verify-release: ALL STAGES PASSED                           (exit 0)
```

`--fast` runs a clearly-labeled REDUCED mode (browser + benchmarks
skipped; counted as not-fully-green). Exit code is nonzero when any
mandatory stage fails.

---

## 9. CI pipeline

`.github/workflows/ci.yml` — seven jobs: A source-verification,
B native (build + upstream gates + ASan), C wasm (pinned emsdk 7.0.0 +
checksums), D functional-parity (needs B,C; byte-verifies downloaded
artifacts against a fresh rebuild), E browser (needs C), F benchmarks
(needs C), G release-report (needs A–F; fails if any mandatory job
failed). Triggers: push to `main`, `v*` tags, pull requests, manual
dispatch. YAML validated (`yaml.safe_load` OK). Every stage of A–F is
executed locally by `scripts/verify-release.sh` on the release host;
the workflow file itself requires a GitHub runner to execute and is
delivered validated but not remotely executed — see Section 11 for
the honest hosted-run status.

---

## 10. Pipeline corrections applied before the public release

| # | Pre-release defect | Correction shipped in 1.0.0 |
|---|---|---|
| 1 | `mymindstorm/setup-emsdk@v14` — repo moved upstream; mutable tag reference | `emscripten-core/setup-emsdk@4528d102f7230f0e7b276855c01ea1159be0e984` (v16, 2026-04-08, current release verified via `git ls-remote` + release page); emsdk 7.0.0 preserved; `emcc --version` verified after install |
| 2 | Job D "sourced" `/home/runner/.emsdk/emsdk_env.sh … \|\| true` — SDK never installed on that runner, failure suppressed | Job D installs the identical pinned emsdk v16 / 7.0.0 toolchain, verifies `command -v emcc` + version, then builds. No `\|\| true` anywhere in the workflow |
| 3 | One mixed artifact (`dist/*` + `build/wasm-checksums.txt`) produced nested `dist/dist/…` on download; Job G read a checksum path that never existed | Two flat artifacts (`omnidecide-wasm-dist`, `omnidecide-wasm-checksums`); downloads to `dist/` (E, F), `artifacts/ci-wasm/` + `artifacts/ci-checksums/` (D); `scripts/ci-verify-dist.sh` verifies presence/size/`\0asm` magic after every download |
| 4 | Job D ran `./build.sh` (writes `dist/`) before comparing → overwrote the downloaded artifact; `sha256sum -c … \|\| sha256sum …` fallback merely printed new hashes | `build.sh --output-dir build/fresh-wasm` rebuilds in isolation; `cmp` on both files; SHA-256 cross-check against Job C's checksum report; `scripts/ci-integrity.mjs` emits `artifact-integrity.json`; mismatch = job failure |

Related corrections: Job G gates on **all six** mandatory jobs; Job D
downloads Job B's parity runner and re-executes it, `cmp`-checking the
regenerated TSV against Job B's uploaded TSV; Job A verification is a
single `tee`-logged step (no diagnostic `|| true` re-run);
`permissions: contents: read` declared top-level; all five actions
pinned to immutable commit SHAs with version comments.

---

## 11. Workflow validation (offline)

```
$ actionlint .github/workflows/ci.yml        # actionlint 1.7.7
(no output - no errors)

$ python3 scripts/ci-workflow-lint.py
39/39 structural checks passed
```

The structural lint asserts: the seven-job graph and `needs` edges
(including Job G needing all six), SHA pinning + version comments on
every `uses:`, absence of `|| true` / `continue-on-error` /
`/home/runner` / the outdated action reference, emsdk `version`
input syntax, artifact upload/download name+path consistency
(see the map in `docs/CI-VERIFICATION.md` §4), presence of the
isolated fresh-build comparison, and gate coverage for every
mandatory job.

---

## 12. Hosted GitHub Actions run — NOT VERIFIED (pending)

**Honest status: the hosted run has not been executed at delivery
time.** The release host has no GitHub push credentials, so the hosted
acceptance criterion cannot be claimed. No hosted result is fabricated.

Everything required to obtain the hosted green run is prepared:

- Push the repository to GitHub and tag `v1.0.0`
  (exact commands in `RELEASE.md`).
- The tag push triggers `omnidecide-wasm-ci`; a push to `main` also
  triggers it, as do pull requests and manual dispatch.
- Expected evidence of success: all seven jobs green; artifact
  `job-g-release-report/ci-verification-report.md` lists run ID,
  commit, per-job outcomes, the artifact-integrity JSON (byte-identical
  downloaded vs fresh WASM), parity 40/40, browser 29/29 and fresh
  benchmark references.

Until that run completes, the hosted-CI row in the acceptance table
must be read as **NOT VERIFIED**, exactly as required for a test that
cannot execute.

---

## 13. Acceptance criteria scorecard

| # | Criterion | Status |
|---|---|---|
| 1 | Kernel source acquisition reproducible (vendored pinned snapshot + strict overrides) | ✅ §1 |
| 2 | Genuinely clean build process (no stale state, cwd-independent, explicit toolchain errors) | ✅ §2–3, `verify-release.sh` |
| 3 | Native vs WASM parity independently reproduced, byte-for-byte, no silent skips | ✅ §5 (40/40) |
| 4 | Full native regression on exact pinned source (incl. v2-gates ALL GREEN) | ✅ §2 |
| 5 | Browser verification independently reproduced with fresh screenshots | ✅ §6 (29/29) |
| 6 | Benchmarks freshly reproduced with JSON output, pipeline vs isolated C separated | ✅ §7 |
| 7 | CI strengthened to the seven-job release pipeline with genuine fresh-build verification | ✅ §9–11 (validated; local equivalent executed) |
| 8 | Unified verification entry point | ✅ §8 |
| 9 | Documentation and release evidence consolidated | ✅ `docs/WASM-STATUS.md`, this report |
| 10 | Hosted GitHub Actions run green | ⏳ NOT VERIFIED — pending repository owner dispatch (§12) |
