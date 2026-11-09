<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# Decideverse (Stage 2)

Stage 2 turns the verified WASM foundation (`omnidecide-wasm-1.0.0`) into
**Decideverse** — a browser-based simulation and decision laboratory
with The Last Detective as the first complete playable game. Decideverse
(previously branded "OmniDecide GameLab") is designed as a collection of
games on one kernel: the `gamelab/` directory and internal identifiers keep
the historical name for stability.

- Application: [`gamelab/`](../gamelab/) — vanilla ES modules + CSS, no build
  step, served statically (e.g. `python3 -m http.server 8000`, then open
  `/gamelab/index.html`). Full details: [`gamelab/README.md`](../gamelab/README.md).
- Architecture: [`gamelab/docs/ARCHITECTURE.md`](../gamelab/docs/ARCHITECTURE.md)
- Engine adapter: [`gamelab/docs/ADAPTER.md`](../gamelab/docs/ADAPTER.md)
- Simulation framework: [`gamelab/docs/SIMULATION-FRAMEWORK.md`](../gamelab/docs/SIMULATION-FRAMEWORK.md)
- Game module guide: [`gamelab/docs/GAME-MODULES.md`](../gamelab/docs/GAME-MODULES.md)
- The Last Detective: [`gamelab/docs/LAST-DETECTIVE.md`](../gamelab/docs/LAST-DETECTIVE.md)
- Testing (all four suites green: 78 + 29 Stage 1, 156 + 57 Stage 2):
  [`gamelab/docs/TESTING.md`](../gamelab/docs/TESTING.md)
- Limitations: [`gamelab/docs/LIMITATIONS.md`](../gamelab/docs/LIMITATIONS.md)
- Roadmap for the remaining four games:
  [`gamelab/docs/ROADMAP.md`](../gamelab/docs/ROADMAP.md)
- Stage 2 completion report: [`GAMELAB-STAGE2-REPORT.md`](GAMELAB-STAGE2-REPORT.md)

Stage 1 files, suites and the CI pipeline are unchanged; all Stage 2 code and
tests are additive.
