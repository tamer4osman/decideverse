<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# Building OmniDecide WASM

> **Decideverse context:** rebuilding the engine is only needed when
> working on the engine itself — the Decideverse GameLab runs on the
> committed `dist/` artifacts and requires no Emscripten toolchain.

Everything builds from the **vendored pinned kernel source**
(`vendor/omnidecide-c/`). No stage of the build downloads the kernel,
consults the network, or depends on developer-local paths.

## 1. Prerequisites

| Tool | Used for | Minimum | Notes |
|---|---|---|---|
| C compiler (`cc`) | native build | C11 | gcc/clang; `-ffp-contract=off` is always applied |
| CMake | native build | 3.16 | e.g. `pip install cmake` |
| make | upstream regression gates | any | |
| Node.js | tests, benchmarks, JS API | 18 | release verified with v24.22.2 |
| npm | test dependencies | 9 | `tests/` has its own package + lockfile |
| Emscripten SDK | WASM build | — | release verified with emcc 7.0.0 |

Install Emscripten:

```bash
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk && ./emsdk install latest && ./emsdk activate latest
source ./emsdk_env.sh
emcc --version   # build.sh fails with this instruction if emcc is missing
```

Missing tools produce explicit errors (`build.sh: ERROR: Emscripten
compiler ('emcc') not found on PATH...`) — never a silent fallback.

## 2. Kernel source resolution

```bash
./fetch-kernel.sh              # resolve + verify (runs on every build path)
./fetch-kernel.sh --print-path # print the resolved source directory
```

Order: **vendored snapshot** (`vendor/omnidecide-c/`, preferred) then a
developer-provided `$OMNIDECIDE_C` checkout — which must match the
pinned kernel version (2.0.0) and snapshot commit
(`scripts/kernel-pin.env`) or the build refuses to start
(`ODK_ALLOW_FOREIGN_COMMIT=1` records the mismatch but stays
unsupported). The script never downloads the kernel and never updates
it silently. Verification covers required files, header version,
upstream `tests/check_version.sh`, license files and the source
manifest fingerprint (`scripts/source-manifest.sh`).

## 3. Native build

```bash
./build-native.sh              # build + run wrapper smoke + parity runner
./build-native.sh --build-only # build without running the smoke tests
./build-native.sh --asan       # additionally run the smoke test under ASan+LSan+UBSan
./build-native.sh --clean      # remove build/native
```

Outputs (kept out of `vendor/`):

- `build/native/libomnidecide.a` — vendored kernel + wrapper
- `build/native/odk_parity_runner` — envelope runner for parity checks
- `build/native/odk_smoke` — native wrapper smoke test
- `build/native/native_envelopes.tsv` — last corpus run

## 4. WASM build

```bash
./build.sh                     # -> dist/omnidecide.js + dist/omnidecide.wasm
./build.sh --output-dir DIR    # build into DIR instead; dist/ untouched
                               # (CI fresh-build isolation, docs/CI-VERIFICATION.md)
ODK_OPT=-O3 ./build.sh         # override optimization (default -O2)
EMCC=/path/to/emcc ./build.sh  # explicit emcc
```

The build compiles into an intermediate directory first; `dist/` is
only touched after a successful link, so a failing build never leaves a
stale binary behind. Flags are pinned: `-O2 -std=c11 -ffp-contract=off`,
`MODULARIZE`, `EXPORT_NAME=createOmniDecideModule`,
`ENVIRONMENT=web,node`, `ALLOW_MEMORY_GROWTH`. Artifact checksums are
printed at the end of every build and recorded in CI
(`build/wasm-checksums.txt`).

**Never** remove or weaken `-ffp-contract=off`: it is what keeps
results bit-identical across compilers and hosts (native ↔ WASM parity
depends on it, verified by `tests/parity_check.mjs`).

## 5. Test toolchain

```bash
cd tests
npm ci                                 # installs the exact locked Playwright
npx playwright install --with-deps chromium
```

## 6. Clean-checkout verification

From a fresh clone (no generated artifacts, no `OMNIDECIDE_C`, no
`/tmp` state):

```bash
./scripts/verify-release.sh          # full verification (11 stages)
./scripts/verify-release.sh --fast   # reduced mode (skips browser + benchmarks;
                                       # never presented as full verification)
```

## 7. Troubleshooting

- **`emcc not found`** — install/activate the Emscripten SDK and
  `source emsdk_env.sh` (see §1).
- **`vendored kernel source missing`** — restore `vendor/omnidecide-c/`
  from the delivery archive; do not substitute another revision
  (see §2).
- **fingerprint mismatch** — the vendored tree was modified. Restore
  it; WASM-specific changes belong in `src/wrappers/` and the build
  scripts, never in the vendored tree.
- **`cmake not found`** — `pip install cmake` (≥ 3.16).
- **Playwright download fails** — the pinned browser is ~150 MB;
  see `npx playwright install --help` for mirrors.
