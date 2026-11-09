<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# Compatibility

> **Decideverse context:** engine-layer host compatibility. The
> Decideverse GameLab (`gamelab/`) additionally requires a modern
> browser with WebAssembly; it is verified on Chromium 143, including a
> 390px-wide mobile-layout check.

## Host environments

| Host | Status |
|---|---|
| Node.js (CommonJS) | supported — `require('js/omnidecide.js')` |
| Node.js (ESM import) | supported — default interop |
| Browsers (ES2018+, WebAssembly) | supported — include `dist/omnidecide.js` then `js/omnidecide.js`; globals `OmniDecide` / `OmniDecideError` |
| AMD bundlers | supported (UMD) |
| Workers | supported by the Emscripten profile (`ENVIRONMENT=web,node`) |
| `file://` pages | **not supported** — serve over HTTP(S) with `application/wasm` |

Verified browsers: Chromium 143.0.7499.4 (Playwright-managed, 29/29
checks). Any browser with WebAssembly + `TextDecoder` support should
work; the loader uses streaming instantiation when the MIME type is
correct and falls back to `instantiate` otherwise.

## Engine parity

Native (glibc build of the same wrapper) and WASM produce
**byte-identical decision envelopes** for the identical corpus —
verified on every release (`node tests/parity_check.mjs`, 40/40 at
1.0.0). The envelope contains no wall-clock or environment-dependent
values; no field is excluded from comparison and no tolerance is
applied. `make determinism` (upstream) additionally proves hash-equal
outputs across repeated native runs.

## Upstream profile compliance

The WASM module follows the upstream documented profile (vendor
Makefile target `wasm`): single-threaded, no filesystem, fp-contract
disabled so decisions match native bit-for-bit, sidecar surface
(stdio/HTTP) excluded. The integration layer adds no decision
primitives and does not modify the vendored kernel.

## Text modes and languages

`ascii` (ASCII_COMPAT, byte-limited accounting) and `unicode`
(code-point accounting, code-point-boundary truncation). Language
codes follow upstream `odk_languages.def`: en, zh, hi, es, ar, fr, bn,
pt, id, ur, plus `auto` (deterministic detection) and `und` (baseline
generic behavior). Non-Latin languages are intended for
`mode: 'unicode'` (mirrors the upstream test configuration). Every
language decision in the parity corpus matches native byte-for-byte.

## Version compatibility

- Kernel 2.0.0 (`ODK_LIBRARY_VERSION`), verified per build by
  `fetch-kernel.sh` / `verify-kernel-source.sh`.
- Integration 1.0.0 — first public release; the JS API surface is
  exactly as documented in `docs/API.md` and kept stable from here on
  (additions will be non-breaking: failed evaluations expose
  `OmniDecideError#envelope`, module loading is a shared singleton,
  `Kernel.engineCount()` semantics documented).
- The client evidence stamp is unchanged upstream in 2.0.0, so 1.x-era
  decision bytes replay bit-identically (upstream `docs/UPGRADE-2.0.md`).
