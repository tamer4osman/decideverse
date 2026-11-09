<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# Releasing OmniDecide WASM (the Decideverse engine)

> **Decideverse context:** this walkthrough covers **engine releases**
> (`omnidecide-wasm`, finished at 1.0.0). The Decideverse platform
> itself — the games in `gamelab/` — ships through the same nine-job
> pipeline: Jobs H and I verify the games on every push, and Job G
> blocks the report unless they are green. Platform status:
> `gamelab/docs/ROADMAP.md`.

This repository verifies itself on GitHub Actions. Pushing the
repository and tagging a version is enough — the nine-job pipeline
(`.github/workflows/ci.yml`) builds the kernel natively and as WASM,
runs every acceptance suite (including the Decideverse game suites),
byte-compares the downloaded WASM artifact against an isolated fresh
rebuild, verifies the browser behavior and benchmarks, and only then
emits the consolidated release report.

## 1. Publish and tag v1.0.0

```bash
cd decideverse

# one-time: create the GitHub repository and connect it
git init -b main
git add -A
git commit -m "OmniDecide WASM 1.0.0 — first public release"
git remote add origin https://github.com/tamer4osman/decideverse.git
git push -u origin main

# tag the release; the v* tag push triggers the full pipeline
git tag -a v1.0.0 -m "OmniDecide WASM 1.0.0"
git push origin v1.0.0
```

(HTTPS remote works too: `https://github.com/<your-account>/omnidecide-wasm.git`.)

## 2. Watch the hosted run

- **Actions → omnidecide-wasm-ci** — the tag push starts the run; a
  push to `main` starts one as well. Pull requests and
  *Run workflow* (manual dispatch) also trigger it.
- Typical duration: roughly 25–40 minutes (Jobs A–F run mostly in
  parallel; Job G waits for all six).
- A green release requires **all seven jobs green**. Nothing in the
  pipeline swallows failures: no `continue-on-error`, no `|| true`,
  and Job G fails unless every mandatory job reports `success`.

## 3. Read the evidence

| Artifact | Content |
|---|---|
| `job-g-release-report/ci-verification-report.md` | consolidated report: run ID, commit, per-job results, toolchain/kernel info, WASM checksums, parity report, browser report, benchmark inventory |
| `job-d-functional-parity` | `parity-report.json` (40/40 byte-identical), `artifact-integrity.json` (downloaded artifact == fresh rebuild), native envelopes TSV |
| `job-e-browser` | `browser-check.json` + fresh Chromium screenshots |
| `job-f-benchmarks` | fresh Node + Chromium benchmark JSON |
| `omnidecide-wasm-dist` / `omnidecide-wasm-checksums` | verified WASM artifacts + SHA-256 report |

For an actual GitHub Release, attach the two files from
`omnidecide-wasm-dist` plus `wasm-checksums.txt` to the release page
for tag `v1.0.0`.

## 4. Local pre-flight (optional but recommended)

Before pushing, the same verification can be reproduced locally:

```bash
./scripts/verify-release.sh         # full run (needs emsdk, cmake, Chromium deps)
./scripts/verify-release.sh --fast  # reduced mode — never present as full verification
python3 scripts/ci-workflow-lint.py # 39/39 structural checks of ci.yml
```

## 5. Versioning rules

- Versions are semver (`vMAJOR.MINOR.PATCH` tags).
- The integration version lives in three kept-in-sync places:
  `js/omnidecide.js` (`INTEGRATION_VERSION`), `tests/package.json`
  (+ lockfile), and the Node suite's version assertion in
  `tests/omnidecide.test.mjs`. `CMakeLists.txt` carries the same
  version. Update all of them together.
- The kernel version (2.0.0) is independent and pinned in
  `scripts/kernel-pin.env`; it changes only with a new vendored
  snapshot.
