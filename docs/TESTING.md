<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# Testing OmniDecide WASM

> **Decideverse context:** this page covers the **engine** suites. The
> Decideverse game suites (gamelab, adapter lifecycle, state, Living
> Village, browser E2E) are documented in `gamelab/docs/TESTING.md`;
> `CONTRIBUTING.md` lists exactly what the nine-job CI pipeline runs.

Prerequisites once: `cd tests && npm ci && npx playwright install
--with-deps chromium`. Build once: `./build.sh` (WASM) and
`./build-native.sh` (native).

One-command full verification: `./scripts/verify-release.sh`.

## Suites

| Command | What it does | Expected |
|---|---|---|
| `build/native/odk_smoke` | native wrapper smoke: lifecycle, six primitives, error envelopes, handle bookkeeping | 54 checks, 0 failures |
| `./build-native.sh --asan` | same suite + leak pass under ASan + LSan + UBSan | 0 failures, 0 leaks |
| `node tests/omnidecide.test.mjs` | Node functional suite: real WASM execution, README quickstart, six primitives, determinism, Unicode (zh/ar/hi/auto), error-class identity (CJS + ESM), validation, release semantics, caps, truncation metadata | 78 checks, 0 failures |
| `node tests/parity_check.mjs` | native vs WASM byte-identical envelopes over the corpus — **hard prerequisite failure if the native runner or dist/ is missing; skipped is never passed** | 40/40 byte-identical |
| `node tests/browser_check.mjs` | Playwright + Chromium over local HTTP (never `file://`): WASM MIME, engine lifecycle, quickstart, distribution, invalid input, 中文 preset, determinism, replay, benchmark, release + re-init, init-failure handling, console/page-error and failed-request capture; writes fresh screenshots to `docs/img/` | 29 checks, 0 failures |

## Parity corpus

`tests/requests.jsonl` — one request per line:

```json
{"id":"zh-choice-berlin",
 "kernel":{"mode":"unicode","language":"zh"},
 "request":{"primitive":"choice",
            "instruction":"备份服务器应该放在哪里？",
            "options":["柏林数据中心","远程分公司"],
            "context":"备份服务器应该放在柏林数据中心，因为那里有冗余电源。",
            "meta":true}}
```

- `kernel` — `mode` (`ascii`|`unicode`), `language` (code, `auto`,
  `und`); kernels are cached per (mode, language) on both hosts.
- `request` — `primitive`, `instruction` (noul: statement),
  `options` (choice/rank/compare), `rubric` (score), `context`,
  `topK` (extract), `caps.{maxRanked,maxSnippetChars,maxTotalChars}`,
  `meta` (append the accounting block).

The native runner (`tests/native_parity.c`, no external dependencies)
and the JS harness both emit `id<TAB>status<TAB>envelope`; envelopes
are authored by the same C wrapper on both hosts and contain no
wall-clock values, so the comparison is byte-for-byte with **no
excluded fields and no tolerances**. Error behavior (kernel
`odk_status` + deterministic diagnostic) is part of the comparison.
Corpus additions must run on both hosts — a case that cannot run is a
corpus bug, not a skip.

## Upstream regression gates

Run against the exact vendored source:

```bash
cd vendor/omnidecide-c
make test            # smoke 373 + spine 210 checks
make golden-check    # 2013 cases, byte-identical
make spine-golden-check
make hostile
make stress
make determinism
make sanitize
make docs-check
make version-check
make v2-gates        # acceptance aggregate — ALL GREEN
```

## Rules observed by this repository

- Skipped is never passed: missing prerequisites abort with an
  explicit error and a nonzero exit status.
- Native executable missing → parity aborts (failed prerequisite),
  never a silent native-less run.
- Existing tests are never weakened to obtain a green result; the two
  defects parity uncovered during hardening were fixed in the
  integration layer (see `docs/VERIFICATION-1.0.0.md` §6).
- Fresh artifacts only: browser screenshots and benchmark JSONs are
  regenerated on every run; stale outputs are never presented as new
  evidence.
