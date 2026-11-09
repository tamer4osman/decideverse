# OmniDecide WASM 1.0.0 — CI Workflow Reference

**Scope:** the nine-job GitHub Actions release pipeline
(`.github/workflows/ci.yml`) — the seven Stage 1 jobs plus the two
additive Stage 2 GameLab jobs (H, I) introduced by Stage 2.1 (coverage
extended by Stage 2.1.1). This
document explains what each job proves, how artifacts flow, how the
fresh-build integrity comparison works, and how to obtain a hosted
green run. The full verification results are documented in
`docs/VERIFICATION-1.0.0.md`; nothing in the pipeline weakens or
removes any acceptance test.

---

## 1. Pipeline overview

| Job | Needs | Proves | Uploads |
|---|---|---|---|
| A `source-verification` | — | Vendored kernel snapshot verified: files, version 2.0.0, pinned commit `561fdb8a…`, recomputed manifest fingerprint `151754b2…`, LICENSE/NOTICE/THIRD_PARTY_NOTICES present; no network download | `job-a-source-verification` (log) |
| B `native` | — | Native build (kernel + wrapper), wrapper smoke (54 checks), native parity runner over the 40-case corpus, ASan+LSan+UBSan pass, full upstream regression incl. `v2-gates` ALL GREEN | `omnidecide-native-parity` (runner + envelopes TSV), `job-b-native-logs` |
| C `wasm` | — | Pinned Emscripten 7.0.0 installed and verified on PATH, WASM build from the pinned source, artifacts verified (presence, size, `\0asm` magic), SHA-256 checksums recorded | `omnidecide-wasm-dist` (2 files, flat), `omnidecide-wasm-checksums` (flat) |
| D `functional-parity` | B, C | Node suite (78 checks), native/WASM parity (40/40 byte-identical, no silent skips), fresh-build byte-identity vs downloaded artifact, artifact integrity JSON, downloaded parity runner reproduces Job B's envelopes | `job-d-functional-parity` (parity-report.json, artifact-integrity.json, native TSV) |
| E `browser` | C | Playwright/Chromium: 29 browser checks, fresh screenshots, correct `application/wasm` MIME, no console/page errors, no JS fallback | `job-e-browser` (screenshots, browser-check.json) |
| F `benchmarks` | C | Fresh standard-scale Node + Chromium benchmark JSON from the actual WASM module; pipeline vs isolated-C timings kept distinct | `job-f-benchmarks` (docs/benchmarks/*.json) |
| G `release-report` | A, B, C, D, E, F, H, I | Consolidated report (run ID, commit, job outcomes, toolchain, kernel, parity, browser, Stage 2 Node summary, Stage 2 browser report, benchmarks, checksums); **fails unless every mandatory job succeeded — including the two Stage 2 jobs — and fails with a distinct message when a required test report is missing although all jobs succeeded** | `job-g-release-report` (ci-verification-report.md) |
| H `gamelab-node-tests` | C | Stage 2 GameLab Node suites against the verified WASM artifact (no rebuild): framework/game (156 checks), adapter lifecycle + handle accounting + **cleanup ownership & recovery** vs the real module (158 checks), action tracking + seed/replay through the real game module (53 checks), Living Village Stage 3A (156 checks: rules R1–R3, output-follows-input, replay, module semantics); machine-readable summary | `job-h-gamelab-node-tests` (per-suite reports incl. village-node-report.json + gamelab-node-summary.json) |
| I `gamelab-browser-tests` | C | Stage 2 GameLab browser E2E on the real WASM distribution (97 checks): the 12-step playable flow plus the 2.1 regressions (adapter lifecycle via the settings UI, shared action history, seed synchronization) and the 2.1.1 cleanup-recovery regression (failing old-instance release reported, handle retained, retry control releases it — kernel never mocked); fresh screenshots; zero console/page errors on the main flow | `job-i-gamelab-browser-tests` (gamelab-browser-check.json, gamelab-*.png) |

Triggers: every push to `main`, every push of a `v*` tag, pull
requests, and manual `workflow_dispatch` runs.

Every upload step uses `if-no-files-found: error`. There is no
`continue-on-error` anywhere and no `|| true` on any setup or
verification step. Top-level `permissions: contents: read` — no job
needs write access.

---

## 2. Pipeline defects corrected before the public release

1. **Outdated Emscripten action.** An earlier workflow referenced
   `mymindstorm/setup-emsdk@v14`. That repository has moved (the old
   path now 301-redirects) to the maintained **`emscripten-core/setup-emsdk`**,
   whose current release is **v16** (2026-04-08).
   *Correction:* all `uses:` entries are pinned to immutable commit
   SHAs with readable version comments; the emsdk action installs the
   same toolchain version as the release host (7.0.0,
   emcc commit `777e6e5d…`) and every job that builds WASM verifies
   `command -v emcc` and `emcc --version` afterwards.

2. **Job D did not initialize Emscripten.** Job D sourced
   `/home/runner/.emsdk/emsdk_env.sh 2>/dev/null || true` — an
   undocumented path, with the failure suppressed, in a runner where
   the SDK from Job C does not exist at all (jobs never share state).
   *Correction:* Job D installs the identical pinned
   `emscripten-core/setup-emsdk@v16` toolchain, verifies
   `emcc --version`, and only then performs the fresh build. An
   environment setup failure fails the job explicitly.

3. **Inconsistent artifact structures.** Job C uploaded
   `dist/omnidecide.wasm`, `dist/omnidecide.js` and
   `build/wasm-checksums.txt` as ONE artifact. `upload-artifact@v4`
   roots such a multi-path artifact at the least common ancestor (the
   workspace root), so downloads produced nested
   `dist/dist/omnidecide.js` paths that do not match where the
   playground and JS loader expect files, and Job G read a
   checksum path that never existed.
   *Correction:* two flat artifacts with exactly one purpose each
   (see the artifact map below); every download lands at the exact
   path its consumer expects; every download is verified with
   `scripts/ci-verify-dist.sh` (presence, non-empty, WASM magic
   bytes `0061736d`).

4. **Ineffective fresh-build checksum comparison.** Job D ran
   `./build.sh` (which writes `dist/`) BEFORE comparing, overwriting
   the downloaded artifact; the subsequent
   `sha256sum -c … || sha256sum dist/…` fell back to merely printing
   new hashes of the already-overwritten files.
   *Correction:* `build.sh` gained `--output-dir <dir>` (default
   behavior unchanged); Job D rebuilds into the isolated
   `build/fresh-wasm/` directory and compares byte-for-byte with
   `cmp` on both `omnidecide.wasm` and `omnidecide.js`, cross-checks
   SHA-256 against the CI checksum report, and records everything in
   the machine-readable `build/artifact-integrity.json`
   (`scripts/ci-integrity.mjs`). A mismatch fails the job; there is
   no fallback that merely prints new checksums.

Related reliability corrections in the same pass:

- Job G now `needs:` **all six** mandatory jobs and fails unless every
  result is `success` (previously a Job A or B failure alone could
  still have produced a green release report).
- Job D no longer rebuilds the native parity runner; it downloads
  Job B's runner artifact, re-executes it over the corpus and `cmp`s
  the regenerated TSV against Job B's uploaded TSV (determinism and
  artifact-chain proof) before running parity.
- Job A's verification is a single `tee`-logged step (the old
  diagnostic re-run with `|| true` is gone); Job B's upstream gates
  are `tee`-logged to `build/verify-logs/` for failure diagnostics.
- The WASM magic-byte check moved into the shared verifier and no
  longer depends on `xxd` output formatting.

---

## 3. Action pins

| Action | Pinned commit SHA | Release |
|---|---|---|
| `actions/checkout` | `11d5960a326750d5838078e36cf38b85af677262` | v4 |
| `actions/setup-node` | `49933ea5288caeca8642d1e84afbd3f7d6820020` | v4 |
| `actions/upload-artifact` | `ea165f8d65b6e75b540449e92b4886f43607fa02` | v4 |
| `actions/download-artifact` | `d3f86a106a0bac45b974a628896c90dbdf5c8093` | v4 |
| `emscripten-core/setup-emsdk` | `4528d102f7230f0e7b276855c01ea1159be0e984` | v16 (2026-04-08) |

SHAs were resolved with `git ls-remote` against the canonical
repositories (the `mymindstorm/setup-emsdk` path redirects to
`emscripten-core/setup-emsdk`; both report identical tag SHAs).
The setup-emsdk v16 `action.yml` was checked: inputs `version` and
`actions-cache-folder` are unchanged from the v14-era syntax, and the
action puts `emcc` on the job's PATH (no filesystem assumptions).

Emscripten version input: `EMSCRIPTEN_VERSION: '7.0.0'` — the same
toolchain the release host verified
(`emcc 7.0.0 (777e6e5db7b4990bcaeb1b9998ed642cc84e6629)`).

---

## 3b. Shell script permissions on fresh checkouts

Every shell script in the repository is stored in the Git index with mode
`100644` — a fresh GitHub checkout therefore contains **no executable
bits** (permissions from a local ZIP or working tree do not survive).
Every job that invokes a shell script directly therefore starts with:

```yaml
- name: Ensure shell scripts have executable permissions
  run: chmod +x build.sh build-native.sh fetch-kernel.sh scripts/*.sh
```

Jobs A–F, H and I invoke scripts directly after this step (Job G carries
the same restoration step for uniformity). The structural linter
(`scripts/ci-workflow-lint.py`, 62 checks) asserts that Jobs H and I
carry this step **before any direct script invocation** — the
`scripts/ci-verify-dist.sh` calls in both Stage 2 jobs depend on it.
The alternative (committing mode `100755`) was rejected to avoid
unnecessary changes to the vendored upstream scripts; the explicit
restoration step is the established, audited practice of this pipeline.
Invocations that need no executable bit (`bash scripts/source-manifest.sh …`)
are called through `bash` explicitly.

---

## 4. Artifact map

```
Job C upload
  omnidecide-wasm-dist        (flat)  omnidecide.js, omnidecide.wasm
  omnidecide-wasm-checksums   (flat)  wasm-checksums.txt
Job B upload
  omnidecide-native-parity    (flat)  odk_parity_runner, native_envelopes.tsv

Job D download
  omnidecide-wasm-dist        -> artifacts/ci-wasm/          (never dist/)
  omnidecide-wasm-checksums   -> artifacts/ci-checksums/
  omnidecide-native-parity    -> artifacts/ci-native/        (+ chmod +x)
  fresh rebuild               -> build/fresh-wasm/           (isolated)
Job E download
  omnidecide-wasm-dist        -> dist/                       (exact loader path)
Job F download
  omnidecide-wasm-dist        -> dist/
Job G download
  all artifacts               -> build/artifacts/<name>/...
```

Post-download verification in D/E/F:
`scripts/ci-verify-dist.sh <dir>` fails on a missing/empty file and on
wrong WASM magic bytes, then prints SHA-256 hashes. Job D additionally
cross-checks the downloaded artifact against Job C's checksum report:

```bash
sed 's# dist/# artifacts/ci-wasm/#' artifacts/ci-checksums/wasm-checksums.txt | sha256sum -c -
```

---

## 5. Fresh-build integrity comparison (Job D)

1. Download the CI artifact into `artifacts/ci-wasm/` — never touched
   afterwards.
2. Fresh build: `./build.sh --output-dir build/fresh-wasm` (vendored
   pinned source, independently initialized emsdk 7.0.0).
3. `cmp` both files; `scripts/ci-integrity.mjs` emits
   `build/artifact-integrity.json` containing: downloaded artifact
   hashes, fresh-build hashes, `cmp` results, SHA-256 comparisons,
   emcc/node versions, kernel version and snapshot commit, recorded +
   recomputed source fingerprint, build exit status.
4. Any difference fails the job. (Emscripten output is byte-determin
   -istic for identical input/flags/version — verified on the release
   host.)

---

## 6. Obtaining a hosted green run

The release pipeline is triggered automatically:

- **push to `main`** — full nine-job verification on every push;
- **push of a `v*` tag** — the release verification run for that tag
  (recommended for `v1.0.0`);
- **pull requests** — verification before merge;
- **Actions → omnidecide-wasm-ci → Run workflow** — manual dispatch.

The exact publishing procedure (git init, remote, push, tag) is in
`RELEASE.md`. A green release requires all nine jobs green; Job G's
`ci-verification-report.md` (artifact `job-g-release-report`) is the
consolidated evidence: run ID, commit, per-job results, integrity
JSON, checksums, parity and browser reports, benchmark listing.

Workflow validation performed offline for this release:

- `actionlint 1.7.7` — no errors (expression, input and shell syntax).
- `python3 scripts/ci-workflow-lint.py` — 46/46 structural checks
  (job graph, pins, artifact name/path consistency, forbidden
  patterns, gate coverage).
