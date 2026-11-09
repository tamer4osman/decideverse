# OmniDecide WASM — Technical Status Report

> **Decideverse context:** the engine documented here is **finished**
> (omnidecide-wasm 1.0.0, verified 2026-11-08). Development continues on
> Decideverse — the decision-simulation platform built on this engine;
> platform status: `gamelab/docs/ROADMAP.md`.

**Integration:** omnidecide-wasm **1.0.0**
**Status date:** 2026-11-08
**Authoritative source-provenance, build-environment, verification and
performance record for this release.** Companion documents:
`docs/VERIFICATION-1.0.0.md` (consolidated verification report
with command evidence) and `docs/TESTING.md` (how to reproduce).

---

## 1. Source provenance

| Item | Value |
|---|---|
| Upstream repository | https://github.com/tamer4osman/omnidecide-c |
| Kernel version | **2.0.0** (`ODK_LIBRARY_VERSION`, verified by upstream `make version-check`) |
| Vendored snapshot commit | `561fdb8ae43a4610d3df92cde5ba0a0ac5f63393` (stamped by GitHub in the delivered source archive comment) |
| Earlier internal pinned commit | `a2e688af1641cd56d884a20e9069d566c54ee3e6` (see note below) |
| Source inclusion strategy | **Vendored snapshot** — the complete upstream source ships inside this repository at `vendor/omnidecide-c/` (118 files, 14,210,369 bytes) |
| Source manifest fingerprint | `151754b2fcd7b5db8101ca458caced68e11725dc7ca66f8a79c6293b1b392f1a` (algorithm pinned in `scripts/source-manifest.sh`) |
| License | Apache-2.0 (upstream `LICENSE`, `NOTICE`, `THIRD_PARTY_NOTICES.md` preserved verbatim in the vendored tree) |

### Fingerprint note (honest record)

An earlier internal review recorded fingerprint
`05d8db7284d7fb5819b089f45f13363f2efc0725084af65e64e1e45540d3830b`.
That value could **not** be reproduced in the release environment for
two reasons, both documented here rather than hidden:

1. The fingerprint algorithm of the earlier internal tooling was not
   preserved in that delivery, so the exact recipe behind the old
   value is unknowable from the delivered artifacts.
2. The upstream repository is not anonymously reachable (HTTP 404 for
   tarball requests of commit `a2e688af…`), so the earlier internal
   commit could not be re-fetched to recompute anything against it.

Resolution: the **user-supplied upstream source archive**
(`omnidecide-c-main.zip`, retrieved 2026-11-04, GitHub commit stamp
`561fdb8ae43a4610d3df92cde5ba0a0ac5f63393`) is authoritative for this
release. It declares kernel version 2.0.0 and passes the complete
upstream acceptance suite (Section 5). The fingerprint algorithm is now
**pinned in-repo** (`scripts/source-manifest.sh`): walk all files under
the vendored tree sorted by path, hash each file, hash the resulting
`sha256sum`-style manifest. The recorded value above is verified on
every build (`./fetch-kernel.sh`) and the check is byte-sensitive (a
one-byte edit flips the fingerprint and fails the build).

### Vendor integrity rules

- The vendored tree is **never modified**; WASM-specific concerns live
  in the integration layer (`src/wrappers/odk_wasm.{c,h}`, build
  scripts).
- `fetch-kernel.sh` prefers the vendored snapshot, verifies it before
  every build, accepts an `OMNIDECIDE_C` local checkout only after
  strict version **and** commit verification, and never downloads from
  the network nor silently falls back to any default branch.

---

## 2. Build environment (verified on the release host)

| Tool | Version |
|---|---|
| OS | Linux 5.10.134 (x86_64, Debian 12 userland) |
| Native compiler | gcc 14.2.0 (Debian 14.2.0-19) |
| CMake | 4.4.4 (pip wheel; minimum required 3.16) |
| Emscripten | **7.0.0** (`777e6e5db7b4990bcaeb1b9998ed642cc84e6629`, emsdk latest) |
| Node.js | v24.22.2 |
| npm | 11.19.1 |
| Playwright | 1.57.0 (pinned exactly in `tests/package.json` + lockfile) |
| Chromium | 143.0.7499.4 (Playwright-managed) |

All versions are pinned or recorded in the CI workflow
(`.github/workflows/ci.yml`): Node via `setup-node`, Emscripten via
`EMSCRIPTEN_VERSION: 7.0.0`, Playwright via `npm ci` from the lockfile.

---

## 3. Build verification

### Exact commands

```bash
./build-native.sh --asan      # native kernel + wrapper + parity runner + leak pass
./build.sh                    # Emscripten WASM build -> dist/
```

Both scripts are working-directory independent and fail with explicit
messages when a toolchain component is missing. `build.sh` compiles
into an intermediate directory first; `dist/` is only touched after a
successful link, so a failed build can never leave a stale binary
behind.

### Artifact hashes (fresh build of this release)

| Artifact | SHA-256 |
|---|---|
| `dist/omnidecide.wasm` | `3c1c8b0acdeb4d3eb8f9c47b115b15ed5017bf5f9951c1d13a351647a1ebbbdb` |
| `dist/omnidecide.js` | `c2e94942cd2c0d2b6fb1e1417c2dc17b15e65a1a3f6e7345be1be1f21e765d6e` |

(wasm 212,763 bytes; recorded again on every CI run as
`build/wasm-checksums.txt`)

### Floating-point contract

`-ffp-contract=off` is applied to **every** compilation (native and
WASM). It is the upstream-mandated condition for bit-identical results
across compilers and hosts and is never touched to fix a build problem.
The kernel scoring implementation is untouched vendored code.

### Clean-checkout reproducibility

`scripts/verify-release.sh` reproduces the complete verification from
a clean checkout: no pre-existing kernel source, no `/tmp` state, no
`OMNIDECIDE_C`, no generated binaries. Stage 3/6 of the script rebuild
native and WASM artifacts from source every run.

---

## 4. Regression test results

All numbers below are from fresh runs of this release on the
release host (exact commands and full logs:
`docs/VERIFICATION-1.0.0.md`).

### Upstream kernel regression (native, exact vendored source)

| Command | Result |
|---|---|
| `make test` | smoke_test: 373 checks, 0 failures; spine_test: 210 checks, 0 failures |
| `make golden-check` | 2013 cases, all byte-identical |
| `make spine-golden-check` | 35 cases, all byte-identical |
| `make hostile` | survived 5443 hostile checks (fixed-LCG, replayable) |
| `make stress` | 6135 checks, 0 failures; soak RSS growth 0 KB |
| `make determinism` | OK — 3 runs hash-identical (spine-golden `71b87365…`, synthetic `d850ed91…`, smoke `e4ba8f6c…`) |
| `make sanitize` | OK — spine_test + golden_spine + synthetic_example under ASan/UBSan |
| `make docs-check` | OK — 60 relative links resolve |
| `make version-check` | OK — version 2.0.0 |
| `make seam-check` / `make schema-check` / `make check-default` / `make bench` | OK / OK / OK / choice@2KB best 0.272 ms |
| `make v2-gates` | **ALL GREEN** |

### Integration tests

| Suite | Result |
|---|---|
| Native wrapper smoke (`tests/odk_smoke.c`) | 54 checks, 0 failures |
| Native wrapper under ASan + LSan + UBSan (`build-native.sh --asan`) | 54 checks, 0 failures, **0 leaks** (all error paths leak-free) |
| Node functional suite (`tests/omnidecide.test.mjs`) | 78 checks, 0 failures |
| Native/WASM parity (`tests/parity_check.mjs`) | **40/40 corpus cases byte-identical** (see Section 5) |
| Browser verification (`tests/browser_check.mjs`) | 29 checks, 0 failures (Section 6) |

Skipped checks: **0 everywhere.** The parity suite aborts with an
explicit failed prerequisite if the native runner or the WASM artifacts
are missing; it never reports a skipped comparison as passed.

---

## 5. Native versus WebAssembly parity

Method: `tests/requests.jsonl` (40 cases) is executed on both hosts
through the **same C interoperability wrapper** (`odk_wasm.c`):

- native: `build/native/odk_parity_runner` (glibc build of the wrapper)
- wasm: `dist/omnidecide.wasm` via `js/omnidecide.js`

Each case yields one envelope line `id<TAB>status<TAB>envelope`; the
two host outputs are compared **byte-for-byte**. Envelopes contain no
wall-clock or environment-dependent values, so **no field is excluded
and no tolerance is applied** — the spec's nondeterministic-field
exclusion allowance is not needed.

Coverage of the corpus: all six primitives (choice, score, noul, rank,
compare, extract); output caps (`maxRanked`, `maxSnippetChars`) and the
full meta accounting block (truncation, language detection, work
budget); five languages beyond English (zh, ar, hi, es, fr) plus auto
detection; emoji and CJK punctuation; 25 KB, 580 KB and 8 KB
instruction contexts; control characters in JSON strings; duplicate
options; and eight deterministic error envelopes (unknown primitive,
empty options/rubric, compare arity, `topK=0`, empty noul statement,
unknown mode, unknown language).

Result: **40/40 byte-identical** (parity report:
`build/parity-report.json`). The first comparison failure would be
reported with both envelopes; none occurred.

---

## 6. Browser verification

Playwright 1.57.0 + Chromium 143.0.7499.4 against the playground served
over local HTTP (never `file://`), WASM served as `application/wasm`
(streaming instantiation active).

**29 checks, 0 failures, 0 console errors, 0 uncaught page errors, 0
failed requests.** The suite verifies: the real compiled WASM module is
fetched (HTTP 200, correct MIME); genuine WASM execution (no JavaScript
fallback exists in the code path); engine status lifecycle
(loading → ready, version 2.0.0, backend embedded-fallback, live kernel
count); README quickstart winner `restart the service` with confidence
`0.6312` displayed; probability distribution rendered (3 bars,
values shown); invalid input surfaces `OmniDecideError [status 1]`
while the engine stays ready; the Chinese preset decides `柏林数据中心`;
the determinism check reports 5/5 identical envelopes; replay is
byte-identical; the in-page benchmark reports throughput and separates
isolated-C timing; the engine releases (live kernels → 0) and
re-initializes correctly; and initialization failure (WASM fetch
blocked) surfaces as an explicit error state with a diagnostic and no
rendered decision.

Fresh screenshots from the verification run are in `docs/img/`:
`browser-quickstart.png`, `browser-distribution.png`,
`browser-unicode.png`, `browser-benchmark.png`. They are regenerated on
every `browser_check.mjs` run — screenshots from earlier verification
runs are never reused.

---

## 7. Performance results (fresh, standard scale)

Machine-readable output: `docs/benchmarks/node-bench.json` and
`docs/benchmarks/browser-bench.json` (plus timestamped copies). Full
methodology: `docs/BENCHMARKS.md`.

Environment: Linux 5.10.134 x86_64, Node v24.22.2, Emscripten 7.0.0;
Chromium 143.0.7499.4 for the browser run.

### Node (pipeline = full JavaScript→WASM decision)

| Workload | Iter | Median | Avg | p95 | p99 | ops/s | Ctx |
|---|---:|---:|---:|---:|---:|---:|---:|
| small (200 B) | 2000 | 0.053 ms | 0.069 ms | 0.091 ms | 0.136 ms | 14,594 | 200 B |
| medium (2 KB) | 800 | 0.219 ms | 0.222 ms | 0.230 ms | 0.237 ms | 4,515 | 2048 B |
| large (20 KB) | 150 | 1.802 ms | 1.806 ms | 1.830 ms | 1.843 ms | 554 | 20480 B |
| unicode 中文 (~2 KB) | 800 | 0.090 ms | 0.092 ms | 0.099 ms | 0.109 ms | 10,844 | 2048 B |

Init: module load 3.2 ms, kernel create 0.02 ms, first decision 3.0 ms.

### Browser (Chromium)

| Workload | Iter | Median | p95 | p99 | ops/s | Ctx |
|---|---:|---:|---:|---:|---:|---:|
| small (200 B) | 2000 | 0.100 ms | 0.200 ms | 0.200 ms | 14,327 | 200 B |
| medium (2 KB) | 800 | 0.200 ms | 0.300 ms | 0.300 ms | 4,283 | 2048 B |
| large (20 KB) | 150 | 1.900 ms | 2.000 ms | 2.000 ms | 527 | 20480 B |
| unicode 中文 (~2 KB) | 800 | 0.100 ms | 0.200 ms | 0.200 ms | 10,101 | 2048 B |

Init: page load 559 ms, module load 0.1 ms, first decision 2.8 ms.

### Pipeline versus isolated C

Both benchmarks report **two** timings per workload and never conflate
them: the full JS→WASM pipeline (string marshaling + wrapper call +
envelope parse) and the isolated C timing (`rawLoop`: the request
pre-marshaled once, then the C evaluation looped without per-call
JavaScript marshaling). The isolated C number is **not** described as
the pipeline cost anywhere in the tooling or docs. No performance
thresholds are imposed in this milestone; numbers are for accurate
measurement and reproducibility.

---

## 8. Features exposed by the JS wrapper

**Exposed** (js/omnidecide.js):
- `OmniDecide.create({mode, language})` → `Promise<Kernel>`; modes
  `ascii` / `unicode`; language codes accepted by
  `odk_lang_from_code` plus `auto` / `und`
- `Kernel.evaluateChoice / evaluateScore / evaluateNoul / evaluateRank
  / evaluateCompare / evaluateExtract` — the six upstream primitives,
  nothing more
- `Kernel.evaluate(request)` / `Kernel.evaluateRaw(request)` — raw
  request object → five-key envelope (+ `meta` accounting when
  `request.meta`), raw envelope string for tooling
- output caps: `request.caps.{maxRanked, maxSnippetChars,
  maxTotalChars}` (0 = kernel default; caps never change numeric
  decisions)
- `Kernel.free()` (idempotent), `Kernel.engineCount()`,
  `Kernel.kernelVersion`, `Kernel.backend`
- `OmniDecideError(status, message)` — `instanceof`-stable in Node
  (CommonJS + ESM) and browsers (single factory invocation); failed
  evaluations attach the raw C envelope as `err.envelope`
- `OmniDecide.version` (integration version), `OmniDecide.STATUS`

**Not exposed** (deliberately, to keep the surface at the six
primitives): image evidence channel (`odk_image`), explain API,
query/client pipeline (`odk_query`, `odk_client`), spine
pin/registry/calibration APIs, per-call language overrides (`_call`
variants), model loading (`odk_kernel_load_model`), stopword/profile
kernel configuration beyond `mode`+`language`, minifier/token
estimation, and the upstream HTTP sidecar (host-side only per the
upstream WASM profile).

---

## 9. Known limitations

1. Single-threaded WASM (upstream profile; no pthreads).
2. No filesystem in the module — brain snapshots move through memory
   exports; the model/lexicon loading APIs are not exposed.
3. Upstream repo not anonymously reachable: the earlier internal pin
   `a2e688af…` could not be re-fetched; provenance relies on the
   user-supplied archive (Section 1) and the pinned fingerprint.
4. The fingerprint algorithm of the earlier internal review is
   unknown (see Section 1); the recorded old value is superseded by
   the pinned in-repo algorithm.
5. Benchmark numbers are host-dependent; no thresholds are asserted.
6. `emcc` is not shipped; developers must install the Emscripten SDK
   (`docs/BUILDING.md`) to rebuild `dist/` — the pinned snapshot makes
   the build self-contained apart from the toolchain itself.
7. CI runs on GitHub-hosted ubuntu-latest runners; local reproduction
   uses `scripts/verify-release.sh`.

---

## 10. Remaining tasks / roadmap

1. **Hosted CI verification:** the release pipeline is complete and
   validated offline (`actionlint`, structural lint, full local
   verification — see `docs/CI-VERIFICATION.md` and
   `docs/VERIFICATION-1.0.0.md`); the actual GitHub Actions run is
   dispatched by the repository owner by pushing the repository and
   tagging `v1.0.0` (procedure in `RELEASE.md`).
2. Optional: cross-check against upstream commit `a2e688af…` when the
   repository becomes reachable, and reconcile fingerprints.
3. Optional: `make thread-probe` harness for a future multi-threaded
   profile (out of scope for 1.0.0 by design).
4. Optional: publish `docs/benchmarks/` JSON schema evolution notes if
   new workloads are added later.
5. Downstream applications (dashboards, decision games or other host
   integrations) build on this generic WASM binding and are outside
   the scope of this repository.
