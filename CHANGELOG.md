# Changelog

All notable changes to OmniDecide WASM are documented here. The format
is based on Keep a Changelog; versioning is semver.

## [Unreleased] — Stage 3A: Living Village

Second playable Decideverse game on the same kernel and framework.

### Added
- **Living Village** (`gamelab/js/games/living-village/`, registered playable):
  six villagers with private memory and a relationship web stewarded over
  seven deterministic days. Every evening each villager's next goal is a real
  OmniDecide `choice` request built from their day-stamped memory, needs and
  the village stores; disputes are weighed by `compare` against the shared
  chronicle; each day is summarized by an `extract` digest (display-only).
- Declared engine-to-gameplay rules **R1** (the selected goal is pursued; the
  reducer applies its objective effects), **R2** (mediation closes disputes;
  the supported claim gains belonging, "tie" splits it) and **R3**
  (display-only digest) — each tested both ways against the real kernel.
- `tests/gamelab_village.test.mjs` — 156 checks incl. the output-follows-input
  proof (contrasting evidence flips the kernel's choice), byte-identical
  replay of full runs, and module-level action-log/seed/replay semantics.
- Browser E2E extended to 121 checks with the Living Village launch-play-
  inspect-report-reset-replay flow and the 390px workspace guard.

### Fixed
- **Sim views overflowed phones** (`gamelab/styles/gamelab.css`): nowrap feed
  labels carried their full intrinsic width into the workspace feed panel, so
  both playable games' sim views overflowed at 390px (the detective view by
  241px — only the dashboard had ever been checked). Labels now wrap below
  720px; desktop rendering is unchanged.

### CI
- Job H runs the village suite and uploads `village-node-report.json`; Job G's
  missing-report gate blocks the release without it. Workflow lint grew from
  62 to **71** structural checks. No new jobs; Stage 1 jobs A–G untouched.

## [Unreleased] — Decideverse rebrand

The GameLab application is now branded **Decideverse** — a laboratory for a
collection of decision games built on the OmniDecide kernel. Visible product
naming in the UI (page title, header brand, hero, footer, captions, about
page), the primary docs (`README.md`, `gamelab/README.md`, `docs/GAMELAB.md`)
and two test check messages was updated. The kernel identity is unchanged:
`omnidecide-wasm`, the `dist/omnidecide.js/.wasm` artifacts, the C symbols,
the `window.OmniDecide` API and the repository name all keep their names, and
the `gamelab/` directory, internal identifiers and CI job names are
intentionally retained for stability. No gameplay, decision, lifecycle or
test-behavior changes.

## [Unreleased] — Stage 2.1.1 GameLab cleanup recovery & CI finalization

Targeted reliability stage on top of Stage 2.1 (branch
`fix/stage-2.1.1-cleanup-recovery`). Fixes the one remaining reviewed
lifecycle defect, hardens the Stage 2 CI jobs, and keeps every working
behavior.

### Fixed
- **Primary navigation collapsed to ~12px on phones** (`gamelab/styles/gamelab.css`).
  The base `.main-nav { flex: 1 }` (flex-basis 0%) overrode the
  ≤720px `width: 100%`, hiding the nav at 390px; the existing check
  `navigation remains usable at 390px` failed deterministically on
  Chromium 153. The mobile rule now sets `flex: 1 1 100%`.
- **Failed old-instance cleanup could orphan a live WASM handle**
  (`gamelab/js/core/adapter.js`). When the `free()` of the previous
  kernel threw during recreation, the adapter kept the replacement,
  reported the cleanup error — and dropped the only reference to the
  old handle. Adapter shutdown released the replacement but the old
  handle stayed live in the C wrapper's registry forever (reproduced
  against the real module: 2 live handles after recreation, 1
  unreleaseable after shutdown). The adapter now implements
  **explicit handle ownership**: every allocated handle is either the
  active instance or a pending-cleanup entry (reason, attempt count,
  original error, since-timestamp, exposed via
  `pendingCleanupDetail`/`lifecycleStats`); entries leave the registry
  only on release evidence. A non-throwing `free()` is release proof —
  `odkw_kernel_free` clears the registry slot before destroying and is
  a no-op on an already-cleared slot, so a retry can never double-free;
  a completed earlier release is additionally confirmed through the
  wrapper's `closed` flag without any further call.
- **Jobs H and I invoked `scripts/ci-verify-dist.sh` without restoring
  executable permissions.** The Git index stores every shell script as
  `100644`; the Stage 1 jobs restore the bits after checkout, the two
  Stage 2 jobs did not — a fresh hosted run would have failed with
  "Permission denied". Both jobs now run the same
  `chmod +x build.sh build-native.sh fetch-kernel.sh scripts/*.sh`
  step immediately after checkout; every direct script invocation in
  the workflow was audited against this step.
- **Release gate hardened** (Job G): a dedicated step now fails the
  pipeline with a distinct message when a required test report is
  missing although all nine jobs succeeded (test failure ≠ artifact
  loss, both block).

### Added
- **Safe public retry** `adapter.retryCleanup()` — releases every
  pending handle whose failure has cleared, reports
  `{ deferred, recovered, remaining }` honestly, and defers during
  in-flight transitions instead of racing them.
- **Allocation policy:** `init()`/`recreate()` recover pending entries
  first and are **deferred** (`PendingCleanupUnresolved`) while
  unresolved handles remain — repeated recreation can no longer
  accumulate handles while cleanup is failing.
- **Incomplete-shutdown reporting:** `shutdown()` retries all pending
  entries, aggregates errors, and returns
  `shutdownComplete`/`incompleteShutdown` + `pendingCleanupDetail`
  instead of claiming a full shutdown while handles remain owned.
- Lifecycle diagnostics extended (all measured, none invented):
  `lifecycleStats.activeHandles`, `.pendingCleanup`,
  `.cleanupRecoveries`; `adapter.hasPendingCleanup`; the settings
  panel shows pending-cleanup state and offers the retry control.
- `tests/adapter_lifecycle.test.mjs` extended 76 → **158 checks**:
  20 cleanup-ownership scenarios (retention, safe retry, persistent
  failure, deferred recreation/initialization, shutdown retry,
  incomplete shutdown, partial-release/no-double-free, intermittent
  failures, accounting invariant) plus the critical regression
  sequence; passing paths never rely on manual fixture cleanup and
  every scenario ends with a fixture sweep asserting **zero orphans**.
- Browser E2E coverage (suite now **97 checks**): cleanup failure and
  recovery driven through the real settings UI on a real kernel
  (failure injected at the engine-source seam on a separate page; the
  main flow keeps zero console/page errors).
- `scripts/ci-workflow-lint.py` extended to **62 structural checks**
  (permission restoration in H/I before any direct invocation;
  missing-report gate coverage).

### Documentation
- ADAPTER.md: handle-ownership model, pending-cleanup registry, safe
  retry semantics, updated recreation/shutdown behavior, and the
  documented engine-API limitation (no per-handle existence query).
- TESTING.md, CI-VERIFICATION.md, LIMITATIONS.md updated accordingly.

## [Unreleased] — Stage 2.1 GameLab hardening

Targeted reliability and release-readiness stage for the GameLab
application (branch `fix/stage-2.1-gamelab-hardening`). No interface
redesign, no gameplay changes, no kernel changes.

### Fixed
- **WASM kernel handle leak during recreation** (`gamelab/js/core/adapter.js`).
  `recreate()` previously aliased `init()`, which abandoned the previous
  kernel without releasing it (reproduced against the real module:
  initialize → 1 live handle, recreate → 2, shutdown + initialize → 2).
  The adapter now implements an explicit lifecycle (`uninitialized`,
  `initializing`, `ready`, `recreating`, `shutting-down`, `error`) with
  create-first/release-after replacement, idempotent shutdown, an epoch
  guard that disposes stale in-flight creates, and recorded (never
  swallowed) cleanup failures. The canonical sequence
  initialize → recreate → recreate → decision → shutdown → initialize →
  decision → shutdown now ends with zero live kernel handles.
- **Detective action tracking disconnected from the shared session** —
  the workspace counter read `session.actionLog` while the game only
  wrote its own `runLog`. Successful player actions (begin, investigate,
  accepted decide, accuse, close-unsolved) now flow into the shared
  session log exactly once; rejected commands and failed engine
  invocations never count; reset/replay semantics are defined and tested.
- **Selected scenario seed not synchronized** — the briefing seed now
  goes through `session.setSeed()` (the authoritative owner), so the
  workspace display, scenario state, reset and replay always agree.
  Documented normalization: non-numeric input → default seed, numeric →
  unsigned 32-bit.

### Added
- `tests/adapter_lifecycle.test.mjs` — 76 checks against the REAL WASM
  module covering init/recreate/shutdown/error paths, the shutdown-during-
  initialization race, and probe-based module handle accounting.
- `tests/gamelab_state.test.mjs` — 53 checks driving the REAL game module
  (DOM shim + real session + shared adapter singleton + real engine) for
  action-tracking and seed/replay semantics.
- Browser E2E coverage for all three findings (suite now 84 checks):
  adapter lifecycle through the settings UI, action-counter consistency
  including an invalid double-submitted action, and seed synchronization
  across start/reset/replay. The previous 57 checks are preserved.
- Settings UI: engine lifecycle controls (Release/Initialize) plus real
  lifecycle accounting (created/released/recreations/cleanup failures).
- GitHub Actions: additive Stage 2 jobs **H** (`gamelab-node-tests`) and
  **I** (`gamelab-browser-tests`); the release report (Job G) now gates on
  all nine jobs and embeds the Stage 2 reports. Machine-readable reports
  are produced by every GameLab Node suite.
- `scripts/ci-workflow-lint.py` extended to the nine-job structure
  (46/46 structural checks).

### Documentation
- ADAPTER.md: full lifecycle specification (states, transitions, failure
  semantics, handle accounting); SIMULATION-FRAMEWORK.md: action-history
  and seed-ownership semantics; LAST-DETECTIVE.md: runLog vs actionLog
  purposes, reset/replay behavior; TESTING.md: new suites; CI-VERIFICATION.md:
  nine-job pipeline; LIMITATIONS.md: recreate-failure and replay-abort notes.

## [1.0.0] — 2026-11-08

First public release of OmniDecide WASM: the OmniDecide C kernel
(2.0.0, exact pinned snapshot vendored at `vendor/omnidecide-c/`)
compiled to WebAssembly behind a small C interop layer and a
dependency-free JavaScript API, with a seven-job GitHub Actions
release pipeline. This is a generic WebAssembly binding — any host
application (Node service, browser app, bundler-based project) can
embed it; no downstream-specific product logic is included.

### Added
- **Vendored pinned kernel source** at `vendor/omnidecide-c/`
  (snapshot commit `561fdb8ae43a4610d3df92cde5ba0a0ac5f63393`,
  kernel 2.0.0, 118 files) — the project is self-contained: clean
  checkouts build without any network access.
- `scripts/source-manifest.sh` — pinned, content-sensitive source
  manifest fingerprint algorithm.
- `scripts/verify-kernel-source.sh` + `scripts/kernel-pin.env` —
  per-build verification of files, version (2.0.0), licenses and
  fingerprint.
- `fetch-kernel.sh`: vendored-first resolution, strict `OMNIDECIDE_C`
  verification (version AND commit; explicit errors, no silent kernel
  updates, no default-branch fallback).
- C interop wrapper (`src/wrappers/odk_wasm.{c,h}`) exposing exactly
  the six decision primitives (choice, score, noul, rank, compare,
  extract) plus output caps, `odkw_last_error_envelope()` for
  host-composed error envelopes, engine bookkeeping and `free()`.
- Dependency-free JavaScript API (`js/omnidecide.js`, UMD: Node
  CJS/ESM, AMD, browser globals) with `OmniDecideError#envelope`,
  shared-module singleton loading and `Kernel.engineCount()`.
- Browser playground (`web/`), Node/Playwright test suites, native
  parity runner, Node + browser benchmark tooling, prebuilt `dist/`
  artifacts.
- `tests/parity_check.mjs` — native vs WASM byte-identical envelope
  parity over `tests/requests.jsonl` (40 cases) with explicit failed
  prerequisites instead of silent skips.
- `scripts/verify-release.sh` — single entry point reproducing the
  full release verification (11 stages + reduced `--fast` mode that is
  never presented as full verification).
- `.github/workflows/ci.yml` — seven-job release pipeline
  (source verification / native / wasm / functional+parity / browser /
  benchmarks / release report) with pinned toolchains and actions,
  two flat WASM artifacts, a genuine isolated fresh-build
  byte-identity comparison (`build.sh --output-dir` +
  `scripts/ci-integrity.mjs`), magic-byte and checksum verification of
  every downloaded artifact, `permissions: contents: read`, and a
  release-report job that fails unless every mandatory job succeeded.
  Triggers: push to `main`, `v*` tags, pull requests, manual dispatch.
- Documentation: `README.md`, `docs/WASM-STATUS.md` (authoritative
  status), `docs/VERIFICATION-1.0.0.md` (consolidated verification
  report), `docs/CI-VERIFICATION.md` (pipeline reference),
  `docs/API.md`, `docs/BUILDING.md`, `docs/TESTING.md`,
  `docs/BENCHMARKS.md`, `docs/COMPATIBILITY.md`, `RELEASE.md`.

### Fixed (found and fixed by the verification suites during
pre-release hardening)
- UMD factory closure bug in `js/omnidecide.js`: the browser factory
  could not resolve the Emscripten loader (the wrapper's `root`
  parameter was out of scope in the factory body); found by the
  browser verification suite, now covered by regression checks.
- `append_excerpt()` in the C wrapper left the message buffer
  unterminated when an excerpt filled the buffer exactly, leaking
  stack garbage into `unknown mode`/`unknown language` envelopes —
  caught by the native↔WASM byte-parity comparison.
- `create()` now loads the Emscripten module exactly once (shared
  singleton) instead of instantiating a module per kernel — kernel
  bookkeeping (live counts) and benchmarks are now per-process
  consistent.
- Failed evaluations attach the raw C-authored envelope to
  `OmniDecideError#envelope` for tooling and parity verification.
- CI pipeline: Emscripten setup action re-pinned to the maintained
  `emscripten-core/setup-emsdk` (the `mymindstorm/` path redirects
  there) at the immutable v16 commit SHA; Job D initializes and
  verifies its own pinned toolchain instead of sourcing a suppressed,
  undocumented runner path; artifact uploads/downloads restructured
  into flat, purpose-specific artifacts verified after every download;
  the fresh-build comparison made genuine (isolated rebuild,
  byte-for-byte `cmp`, no checksum-printing fallbacks). Details:
  `docs/CI-VERIFICATION.md` §2.

### Verified (fresh runs, this release)
- Upstream gates on the exact vendored source: `make test` (373+210
  checks), golden-check (2013), spine-golden-check (35), hostile
  (5443), stress (6135, 0 growth), determinism, sanitize, docs-check,
  version-check, seam/schema/check-default, bench — **v2-gates: ALL
  GREEN**.
- Integration: native smoke 54/54 (plus ASan+LSan+UBSan clean), Node
  suite 78/78, parity 40/40 byte-identical, browser 29/29 with fresh
  screenshots, fresh standard-scale benchmarks (Node + Chromium) as
  JSON, WASM artifacts byte-identical to an isolated fresh rebuild.
- Offline workflow validation: `actionlint 1.7.7` clean,
  `scripts/ci-workflow-lint.py` 39/39 structural checks.
- Hosted GitHub Actions execution: **NOT VERIFIED at delivery time** —
  dispatched by the repository owner via push + `v1.0.0` tag (see
  `RELEASE.md`; honest status recorded in
  `docs/VERIFICATION-1.0.0.md` §12).

### Internal development history (pre-public)
Before this release, the integration was developed and hardened
through internal milestones that were never published: the initial
kernel-to-WASM integration; browser UMD export and allocation-cleanup
fixes with kernel-source pinning documentation; a documented
reconstruction after an environment recycle, adding the vendored
pinned kernel, native/WASM parity and browser verification; and the
CI pipeline repair described above. Their outcomes are folded into
the Added/Fixed/Verified entries; no earlier version number was
published.
