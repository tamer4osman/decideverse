<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# JavaScript API

Module: `js/omnidecide.js` (UMD — CommonJS, AMD, browser globals).
Dependencies: none. Engine: the vendored OmniDecide C kernel 2.0.0
compiled to WebAssembly (`dist/omnidecide.wasm`).

```js
// CommonJS
const OmniDecide = require('js/omnidecide.js');
const OmniDecideError = require('js/omnidecide.js').OmniDecideError;
// ESM (default interop)
import OmniDecide from 'js/omnidecide.js';
// Browser (include dist/omnidecide.js FIRST, then js/omnidecide.js)
window.OmniDecide; window.OmniDecideError;
```

## OmniDecide.create(options) → Promise&lt;Kernel&gt;

```js
const kernel = await OmniDecide.create({
    mode: 'ascii',     // 'ascii' (default) | 'unicode'
    language: 'und',   // 'en','zh','hi','es','ar','fr','bn','pt','id','ur',
                       // 'auto' (deterministic detection), 'und' (default)
});
```

Rejects with `OmniDecideError` for unknown mode/language or loader
problems. The Emscripten module is loaded once per page/process and
shared by all kernels. Use `mode: 'unicode'` for non-Latin languages
(mirrors the upstream test configuration).

## Kernel

| Member | Description |
|---|---|
| `kernelVersion` | `"2.0.0"` |
| `backend` | `"embedded-fallback"` (deterministic engine; no JS fallback exists) |
| `closed` | `true` after `free()` |
| `evaluate(request)` | parsed envelope (see below) |
| `evaluateRaw(request)` | the raw envelope **string** (byte-identical across hosts) |
| `free()` | release the kernel (idempotent; calls afterwards throw `OmniDecideError` status 1) |
| `engineCount()` | live kernel handles in the module |

### Request object

```js
{
  primitive: 'choice',        // choice | score | noul | rank | compare | extract
  instruction: '...',         // noul: the statement under test
  options: ['a', 'b'],        // choice/rank/compare; rubric items for score
  context: '...',             // evidence text (default '')
  topK: 2,                    // extract only (>= 1)
  caps: { maxRanked: 2, maxSnippetChars: 40, maxTotalChars: 1000 }, // 0/omitted = kernel default
  meta: true                  // append the accounting block to the envelope
}
```

Caps never change a numeric decision (upstream guarantee).

### Envelope (upstream five-key contract)

```json
{"primitive": "choice",
 "result": "restart the service",
 "confidence": 0.6312,
 "probability": 0.6312,
 "distribution": {"restart the service":0.6312, "...":0.0},
 "meta": {"totalOptions":0,"truncated":0,"emitted":0,"dropped":0,
          "chunksKept":0,"chunksDropped":0,"workUsed":392,
          "workBudget":260000,"workExceeded":0,"language":"und",
          "detected":"und","detectedConfidence":0.0000,
          "contextTruncated":0,"contextUnitsDropped":0}}
```

`meta` (present when `request.meta`): output-cap accounting, effective
language + detector verdict (auto mode), work budget and context
truncation (units = bytes in ASCII mode, code points in Unicode mode).

### Convenience wrappers

`evaluateChoice(instruction, options, context, opts?)`,
`evaluateScore(instruction, rubric, context, opts?)`,
`evaluateNoul(statement, context, opts?)`,
`evaluateRank(instruction, options, context, opts?)`,
`evaluateCompare(instruction, options, context, opts?)` (exactly two
options), `evaluateExtract(instruction, context, topK, opts?)` —
`opts` carries `caps`/`meta`.

## OmniDecideError

```js
try { await kernel.evaluateChoice('Pick one.', [], 'ctx'); }
catch (e) {
    e instanceof OmniDecideError; // true in Node AND browsers
    e.status;   // 1 = ODK_ERR_ARGUMENT, 2 = INFERENCE, 3 = MEMORY, 4 = INTERNAL
    e.message;  // deterministic diagnostic from the C wrapper
    e.envelope; // raw C-authored error envelope (failed evaluations)
}
```

The class is exported from the same single factory invocation as
`OmniDecide`, so `instanceof` is stable across Node (CommonJS/ESM) and
browsers. Unknown primitives are rejected wrapper-side; only the six
primitives exist.

## Exposed vs upstream

**Not exposed** (by design): image evidence channel, explain API,
query/client pipeline, spine pin/registry/calibration, per-call
language overrides, model/lexicon loading, minifier/token estimation,
upstream HTTP sidecar. See `docs/WASM-STATUS.md` §8.
