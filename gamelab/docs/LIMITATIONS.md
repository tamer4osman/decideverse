<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# Known Limitations

## Engine-level (inherited from the kernel contract)

- **Lexical evidence matching, not understanding.** OmniDecide scores lexical
  support between submitted texts. Paraphrases that share few words can score
  low even when factually equivalent; wording matters. GameLab shows the exact
  submitted text so users can always see *why* the engine answered the way it
  did.
- **The engine cannot see the world.** No images, no audio, no simulation
  state — only the evidence string the game submits. It is not an LLM and has
  no story-level comprehension.
- **Low rank confidence is normal.** Ordering four suspects spreads probability
  thin; the detective's final ranking typically shows confidence ≈ 0.03. The
  UI displays the number honestly and labels the ranking advisory.
- **`score` with a single rubric item is uninformative** (returns 0.5 for both
  innocent and guilty suspects in probing). The adapter exposes the primitive
  — it is part of the kernel contract — but the detective game does not build
  decisions on it; future games should use multi-item rubrics.
- **`compare` can return `"tie"`** when two statements are lexically
  indistinguishable. The UI handles this as a first-class outcome.
- **`file://` pages are unsupported** (Stage 1 constraint) — serve over
  HTTP(S) with the `application/wasm` MIME type.

## Application-level (Stage 2 scope)

- **No persistence.** Sessions, history, telemetry and the replay log live in
  page memory; a reload clears them. A future stage could add
  localStorage/IndexedDB export.
- **One live session per game**, kept only while the page is open.
- **Single managed kernel handle.** Changing engine settings (mode/language)
  re-creates the kernel: the replacement is created and confirmed first, the
  previous instance is released afterwards (Stage 2.1 — verified leak-free).
  If re-creation fails, the previous kernel stays active with its previous
  configuration and the failure is reported; recovery is a retry or
  shutdown + initialize. In-flight invocations are serialized by design.
- **Cleanup failures are retained, not silently resolved (Stage 2.1.1).**
  If releasing a kernel handle fails, the handle stays adapter-owned in the
  pending-cleanup registry: recreation and initialization are deferred until
  it is recovered (`retryCleanup()`, or a repeated `shutdown()` which
  retries every pending release), and incomplete shutdown is reported
  honestly. The engine API exposes no per-handle existence query (only the
  module-wide live count), so while a release keeps failing the adapter
  conservatively reports the handle as unresolved instead of claiming a
  release it cannot evidence; a retry through `odkw_kernel_free` is
  slot-idempotent (never a double-free), and a completed release is
  additionally confirmed by the wrapper's `closed` flag. In the compiled
  WASM module a failed release is not expected in normal operation — the
  paths exist so that a failure can never orphan a live handle.
- **Benchmark numbers are session-local and host-specific.** GameLab never
  mixes them with the Stage 1 release benchmarks (`docs/benchmarks/`), which
  were produced by a different methodology (isolated-C raw loop + pipeline).
- **Scenario content is fixed.** The detective case ships one case with one
  culprit; there is no procedural case generation yet.
- **Replay of a run containing a failed decision aborts at that attempt**
  (deterministically): the run log faithfully re-submits the failed request,
  the engine fails again, and the replay report states the reason. This is
  the documented trade-off between replay fidelity and continuation.
- **Replay requires the same build.** Replays re-invoke the real kernel; if a
  future kernel version changed envelopes, cross-version replay reports would
  legitimately differ.
- **Planned games are scopes, not stubs.** Living Village, Tactical Command,
  The Last Outpost and Rumors & Kingdoms display roadmap information only.
- **CI untouched.** Stage 2 suites are additive and currently run manually;
  wiring them into the existing seven-job pipeline is left to the maintainer
  (see ROADMAP.md) to avoid unrelated changes to the working Stage 2 pipeline
  contract.
