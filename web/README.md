# OmniDecide WASM Playground

A dependency-free browser demo driving the real WebAssembly engine.

## Run

```bash
python3 -m http.server 8000        # from the repository root
# open http://127.0.0.1:8000/web/index.html
```

Serve the **repository root** over HTTP — never open `index.html` via
`file://` (the WASM loader needs HTTP and `application/wasm`).

## What it demonstrates

- **Engine status bar** — initialization lifecycle (loading → ready /
  error / released), kernel version 2.0.0, backend
  `embedded-fallback`, live kernel count.
- **README quickstart** — the canonical choice decision
  (`restart the service`, confidence 0.6312) with the full probability
  distribution rendered as bars.
- **中文 preset** — deliberate multilingual capability demo: a
  Unicode-mode decision in Chinese (`柏林数据中心`, "Berlin data
  center"), byte-identical to the native build.
- **Invalid input** — an empty-options request showing the
  deterministic `OmniDecideError [status 1]` while the engine stays
  ready.
- **Determinism check** — five repeated envelopes, byte-compared.
- **Replay** — re-runs the last request and compares bytes.
- **Benchmark** — the shared `js/benchmark.js` workloads in-page,
  separating the full JS→WASM pipeline from the isolated C timing.
- **Release engine** — frees the kernel (live count → 0);
  any preset re-initializes.

Each decision can be expanded to show the raw envelope — the same
bytes the native build produces (verified by `tests/parity_check.mjs`).

## Files

- `index.html` — structure
- `app.js` — controller; exposes `window.__OD_APP__` used by
  `tests/browser_check.mjs`
- `styles.css` — dark theme styling
