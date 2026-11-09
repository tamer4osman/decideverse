<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# OmniDecide C

A small, dependency-free **C11 library that makes deterministic, explainable
decisions from text**: pick the best-supported option, score against a
rubric, check whether a statement is supported, rank, compare, or pull out
the evidence behind an answer.

- **Deterministic.** The same input gives the same output, bit for bit, on a
  given platform: no randomness, no locale or environment dependence, no
  network, no model weights. (The one exception is the client pipeline's
  `execution_time_ms` metadata field, which reports wall-clock time.)
- **Explainable.** Every decision comes with a full probability distribution
  over the options and a confidence value.
- **Offline and small.** Needs only libc and libm. The static library is about
  0.5 MB.
- **Multilingual.** English, Chinese, Hindi, Spanish, Arabic, French, Bengali,
  Portuguese, Indonesian and Urdu, plus a generic mode for any Unicode text.
- **Optionally adaptive.** An opt-in "experience brain" learns from outcomes
  you feed back to it. Without it, the kernel is stateless.

License: [Apache-2.0](LICENSE).

## v2.0.0: from decision library to decision platform

Version 2.0.0 keeps the deterministic kernel exactly as it was (same
decisions, byte for byte) and wraps it in an application-agnostic
integration spine:

- **`odk_classify`** — multi-scheme classification from declarative,
  versioned scheme data; two labeled tiers (A: deterministic lexical
  rules; B: brain-assisted paraphrase resilience, disableable).
- **Temporal validity** — reclassification appends; as-of queries answer
  from stored intervals.
- **`odk_decompose`** — parameterized, versioned decomposition templates.
- **Calibration reports** — predicted confidence vs observed outcomes,
  binned, byte-identical for the same log.
- **Schema-first types** — `schema/omnidecide.schema` is the single
  source of truth; C types and JSON (de)serialization are generated;
  pins record the schema fingerprint.
- **Integration surface** — a sidecar speaking JSONL over stdio or HTTP,
  decision pinning with `exact` / `semantic` / `foreign` replay classes,
  and the `worldState(variables, scope, t)` context contract.
- **Extension API** — schemes, templates, vocabularies, corpora and
  brain snapshots are registered as versioned data; the shipped build
  contains zero registered customizations.
- **E2E tested** — a Playwright + Chromium suite (`e2e/`) drives the
  sidecar's HTTP surface and a browser harness page end to end
  (`make e2e`).

Docs: [SPINE-API.md](docs/SPINE-API.md) (spine API + sidecar),
[EXTENSION.md](docs/EXTENSION.md) (the customization seam),
[REPLAY.md](docs/REPLAY.md) (replay semantics),
[UPGRADE-2.0.md](docs/UPGRADE-2.0.md) (1.x → 2.0, all breaking changes
itemized), [golden/spine/README.md](golden/spine/README.md) (corpus
discipline). Run the proof: `./build/synthetic_example`, then
`make v2-gates`.

## Read this first: what it is, and what it is not

OmniDecide is an **evidence-matching engine, not a language model**. It scores
how well each option is supported by the context you give it, using lexical
signals (word overlap, character trigrams, context chunks). It does not
understand meaning. That makes it fast, auditable and reproducible, and it also
means it can be fooled by wording.

It works well when options and statements reuse the words of the context. In
our own tests it went wrong in these situations:

| Situation | Example | Result |
|---|---|---|
| A wrong option shares a word with the question | question "How should we respond to the alert?", option "ignore the alert" | the distractor can win |
| Both options are mentioned in the context | "The customer chose the annual plan after comparing it with the monthly plan." | can pick the monthly plan |
| Paraphrase | statement "The service was restarted." against context "Restarting the service restored normal operation" | judged "not supported" |

**Long inputs are cut by default - and the cut is reported.** The default
limits (ASCII mode: 20,000 bytes of context) discard the rest of a longer
context from the evidence view. A decisive sentence at the end of a 90 KB
context was simply never seen in our test. The `_opts` evaluators now report
the cut in `odk_result_meta` (`context_truncated`, `context_units_dropped` - a
pure-accounting flag that never changes the decision). Raise the limits, use
`ODK_OVERLIMIT_HEAD_TAIL`, or use Unicode mode, and see
[docs/TEXT-MODES.md](docs/TEXT-MODES.md#limits-and-what-happens-past-them).

Use it as a fast, deterministic, explainable first pass, as a fallback when no
model is available, or as an auditable component beside other systems. Do not
use it as the only judge where a wrong answer is costly, and test it on your own
data first.

## Quick start

```c
#include <stdio.h>
#include "omnidecide.h"

int main(void)
{
    const char *context =
        "The service stopped responding. "
        "Restarting the service restored normal operation within seconds.";
    const char *options[] = {"restart the service",
                             "reinstall the operating system",
                             "wait for the weekend"};
    odk_kernel *kernel = odk_kernel_create();
    odk_result result;
    odk_error err;

    if (odk_evaluate_choice(kernel, "How should we fix the outage?",
                            options, 3, context, NULL, &result,
                            &err) == ODK_OK) {
        printf("%s (confidence %.2f)\n", result.string, result.confidence);
        odk_result_release(&result);
    }
    odk_kernel_free(kernel);
    return 0;
}
```

Output: `restart the service (confidence 0.63)`. The same result as JSON:

```json
{"primitive": "choice", "result": "restart the service", "confidence": 0.6312,
 "probability": 0.6312, "distribution": {"restart the service": 0.6312,
 "reinstall the operating system": 0.2035, "wait for the weekend": 0.1653}}
```

Build and run the bundled examples:

```sh
make examples
./build/quickstart     # primitives, the JSON contract, the client pipeline
./build/languages      # Spanish, Arabic and Chinese with the UNICODE text mode
./build/learning       # the experience brain flips a decision after outcomes
```

## The six decision primitives

| Primitive | Question it answers | Result |
|---|---|---|
| `choice` | Which option is best supported by the context? | the winning option |
| `score` | How well does the context fit a rubric, for example "fails / partly works / works"? | a number from 0 to 1 |
| `noul` | Is this statement supported by the context? | true or false |
| `rank` | In what order do the options rank? | `"a > b > c"` (the full array via a separate call) |
| `compare` | Which of two options is better supported? | the winner |
| `extract` | Which sentences of the context best support a query? | a list of snippets |

Each returns a confidence and a probability distribution. See
[docs/API.md](docs/API.md) for signatures and result shapes.

## Performance

Measured with `gcc -O2 -ffp-contract=off` on a **single shared vCPU** (Intel
Xeon @ 2.1 GHz virtual machine). Medians of three runs, one thread, kernel
reused, three options. Treat these as indicative: a real server or laptop will
differ, and run-to-run spread here was 2% to 20%.

| Workload | Time per call | Calls per second |
|---|---|---|
| `choice`, 200 B context | 67 µs | ~15,000 |
| `choice`, 2 KB context | 0.54 ms | ~1,900 |
| `choice`, 20 KB context | 4.8 ms | ~210 |
| `choice`, 100 KB context (Unicode mode) | 24 ms | ~42 |
| `score` / `noul` / `rank` / `compare`, 2 KB | 0.47 to 0.52 ms | ~1,900 to 2,100 |
| `extract`, 2 KB | 0.29 ms | ~3,400 |
| short question in another language (UND, es, ar, zh), one sentence | 30 to 57 µs | ~17,000 to 33,000 |
| language auto-detection (`ODK_LANG_AUTO`) adds | ~35 µs | |
| client pipeline, 2 KB text, one query | 0.68 ms | ~1,500 |
| brain `evaluate_choice`, 2 KB | 0.51 ms | ~1,950 |
| brain `record_outcome` | 1.0 µs | ~1,000,000 |
| `odk_kernel_create` + free | ~30 ns | |

Cost grows roughly linearly with context length (about 4 MB of context per
second per core), and the Unicode and ASCII modes cost about the same. There is
no model to load: the library is a 0.5 MB static archive and a kernel is ready
in nanoseconds. One kernel can be shared by threads without data races (see
Threads below), so you can spread work across cores; this machine has a single
vCPU, so multi-core scaling was not measured. The default ASCII limits (20 KB
of context, 40 KB of chunk source) start to apply to larger contexts, so
measure and configure those yourself (see the warning above).

Spine baseline at 2.0.0 (`make bench`, same measurement rules): a Tier A
classification decision costs 0.28 ms at a 2 KB context (budget ≤ 1 ms),
classification throughput is ≈ 22,000 classifications/s at 2 KB, and one
pin + replay round-trip is ≈ 0.7 ms. Regressions beyond 10 % against this
baseline require written justification (see
[docs/SPINE-API.md](docs/SPINE-API.md#benchmarks)).

## Install and integrate

**Make** (Linux, macOS, BSD; a POSIX.1-2008 environment is required):

```sh
make                  # build/libomnidecide.a
make test             # build and run the kernel and spine tests
sudo make install     # /usr/local/lib/libomnidecide.a and both public headers
cc -std=c11 myapp.c -lomnidecide -lm -o myapp
```

**CMake**, as a subdirectory:

```cmake
add_subdirectory(omnidecide-c)
target_link_libraries(my_app PRIVATE omnidecide::omnidecide)
```

**Drop-in sources:** add every `odk-core/*.c`, `odk-brain/*.c` and
`odk-spine/*.c` to your build with `-Iinclude -Iodk-core -Iodk-brain -Iodk-spine`.

**Keep `-ffp-contract=off`** when you compile the library. It stops the
compiler from fusing floating-point operations, which keeps results
bit-identical across compilers and optimization levels. The bundled Makefile
and CMakeLists apply it for you.

**Threads:** we ran a ThreadSanitizer test with separate kernels, brains and
clients in 8 threads, and with one `odk_kernel` shared by 8 threads running
evaluations. Neither reported a data race. We did not test sharing a brain or a
client across threads, so protect those with your own lock.

## Text modes and languages (important for non-English text)

The **default text mode is ASCII-only**: it tokenizes `[a-z0-9]` words and cannot
see words in other scripts. For anything else, switch on the Unicode mode:

```c
odk_kernel_config cfg = {0};          /* zero = the documented defaults */
cfg.text_mode = ODK_TEXT_UNICODE;     /* Unicode normalization + segmentation */
cfg.language  = ODK_LANG_AUTO;        /* or ODK_LANG_ES, ODK_LANG_AR, ... */
odk_kernel *kernel = odk_kernel_create_ex(&cfg);
```

`ODK_LANG_UND`, the default language, is the generic mode and works for any
script. Language packs add stopwords and script-specific rules. **Treat packs
as optional.** The non-English packs have not been reviewed by native
speakers. In our own measurements their gain over
the generic mode is small, and for five of the ten languages it is inconsistent
across test corpora. See [docs/LANGUAGES.md](docs/LANGUAGES.md) and
[docs/TEXT-MODES.md](docs/TEXT-MODES.md).

## Input formats

The library reads **text in memory (UTF-8)**; it never opens your input files.
Your application extracts the text, then picks an input profile that controls
how it is chunked: plain, markdown, chat, log, JSON, JSON Lines, CSV, TSV, source
code, HTML text (tags already stripped), extracted PDF text, or auto-detect. It
does not parse PDF, Word, Excel or HTML files.

An optional **image channel** takes raw RGB pixels (not PNG or JPEG files) and
adds a short brightness, colour and aspect-ratio fingerprint to the evidence.
It is not image recognition and cannot read text in images.

## Learning from outcomes (optional)

```c
odk_brain *brain = odk_brain_create();
odk_brain_record_outcome(brain, "choice", question, "power cycle the machine",
                         1.0, &err);                   /* reward in [-1, 1] */
odk_brain_evaluate_choice(brain, kernel, question, options, 3, context, NULL,
                          1.0 /* weight */, &result, &err);
odk_brain_save_file(brain, "brain.json", &err);        /* plain JSON snapshot */
```

With weight `0.0`, or with no brain, you get exactly the plain kernel result. See
[docs/LEARNING.md](docs/LEARNING.md).

## Explaining a decision

```c
odk_explain ex;
odk_explain_choice(kernel, question, options, 3, context, NULL, &ex, &err);
char *json = odk_explain_to_json(&ex);   /* every intermediate, per option */
odk_explain_release(&ex);
```

The explain API returns, for each option, every number behind the decision
(the three similarity channels, the context and question scores, the fused
score and the final probability). The values are captured from the real scoring
path, so they equal the internal numbers exactly. See
[docs/EXPLAIN.md](docs/EXPLAIN.md) and `examples/explain.c`.

## Scoring models (experimental)

The nine hand-set scoring constants can be replaced by a small **model file**
(`odk_kernel_load_model`). With no model loaded, output is byte-identical to
the default behavior; the golden corpus enforces that. A model file is trusted
configuration (a hash detects corruption, not tampering), and no trained model
ships with the library: validate any model on held-out data before using it.
See [docs/MODEL.md](docs/MODEL.md) and the experimental extra features in
[docs/FEATURES.md](docs/FEATURES.md).

## Documentation

| Document | Contents |
|---|---|
| [docs/API.md](docs/API.md) | every function family, results, errors, ownership, limits |
| [docs/TEXT-MODES.md](docs/TEXT-MODES.md) | ASCII and Unicode modes, normalization, tokenization, input profiles |
| [docs/LANGUAGES.md](docs/LANGUAGES.md) | the ten languages, how to choose, measured results and limits |
| [docs/LEARNING.md](docs/LEARNING.md) | the experience brain, word links, snapshots |
| [docs/EXPLAIN.md](docs/EXPLAIN.md) | the explain API: every intermediate behind a decision |
| [docs/MODEL.md](docs/MODEL.md) | scoring model files, loader rules, trust |
| [docs/FEATURES.md](docs/FEATURES.md) | the experimental extra features of feature set 1 |
| [docs/GOLDEN.md](docs/GOLDEN.md) | the golden regression corpus |
| [docs/SPINE-API.md](docs/SPINE-API.md) | the v2 spine: classify, temporal validity, decompose, calibration, sidecar |
| [docs/EXTENSION.md](docs/EXTENSION.md) | the customization seam: registering versioned data (F7) |
| [docs/REPLAY.md](docs/REPLAY.md) | decision pinning and replay semantics |
| [docs/UPGRADE-2.0.md](docs/UPGRADE-2.0.md) | upgrading 1.x → 2.0, every breaking change itemized |
| [CHANGELOG.md](CHANGELOG.md) | release notes |
| [CONTRIBUTING.md](CONTRIBUTING.md), [SECURITY.md](SECURITY.md) | contributing and vulnerability reporting |

## Status and verification

Current version: **2.0.0**, the platform release. Kernel decisions are
unchanged from 1.0.0: the full v1 golden corpus still replays byte for byte.

- Builds without warnings under `-Wall -Wextra -Werror -pedantic` with gcc
  (verified on the 2.0.0 tree with gcc 14); the 1.0.0 release was also
  verified with clang 18 and musl-gcc. macOS is covered by the CI workflow
  but has not been run yet. Windows and MSVC are not supported: the library
  uses POSIX `clock_gettime` and `strdup`.
- `make test` runs the 373-check kernel smoke test (every primitive, all ten
  languages, bad input, truncation reporting, the explain API, model files,
  the JSON contract, the image channel, the brain) and the 210-check spine
  test suite.
- `make golden-check` replays the 2,013-case v1 corpus and
  `make spine-golden-check` the 35-case v2 spine corpus, both requiring
  byte-identical output; the v1 corpus passed on gcc and clang at -O0 to -O3
  and on musl at 1.0.0, and replays byte-identically on the 2.0.0 tree.
- `make hostile` runs 5,443 hostile-input checks and `make thread-probe`
  shares one kernel (with and without a loaded model) across 8 threads.
  `make stress` drives a 6,135-check stress suite: contract-boundary abuse,
  a seeded fuzz marathon with pairwise byte-identical transcripts across
  four kernels, truncation/caps/work-budget sweeps, an 8-thread concurrency
  storm over all six primitives, spine lineage/as-of/pin/replay/calibration
  endurance, and a lifecycle churn soak with an RSS watch. `make stress-asan`
  reruns it under ASan + UBSan, and the suite is race-clean under TSan.
- `make e2e` drives the sidecar's HTTP surface and the browser harness page
  from Playwright + Chromium (13 tests). `make docs-check` verifies the
  documentation links, `make version-check` that the header, CMake and
  CHANGELOG agree on the version, and `make v2-gates` runs the full
  verification aggregate.
- The code is clean under AddressSanitizer, UndefinedBehaviorSanitizer,
  LeakSanitizer and ThreadSanitizer (valgrind-clean at 1.0.0).
- No independent third-party audit has been done.

## Versioning

OmniDecide follows [semantic versioning](https://semver.org). The version is
`ODK_LIBRARY_VERSION` in `include/omnidecide.h`.

- **Patch releases (x.y.z)** fix bugs only. Decisions, scores and JSON output
  for existing inputs do not change; `make golden-check` must stay
  byte-identical.
- **Minor releases (x.y.0)** add features. The public API stays
  source-compatible and existing results do not change, except for new fields
  appended to output.
- **Major releases (x.0.0)** may change decisions, scores or the API.
- **No ABI guarantee.** Public structs may grow in minor releases, so link
  statically or rebuild your application when you upgrade.
- Snapshot, model-file and schema formats carry their own format numbers or
  fingerprints, so upgrading the library never forces a data migration:
  1.x-era brain snapshots still load, and 1.x-era client decisions still
  replay on the 2.x kernel.

## Provenance

Author, owner and copyright holder: Tamer Osman
([GitHub](https://github.com/tamer4osman),
[LinkedIn](https://www.linkedin.com/in/tamer4osman)). The
Unicode tables in `odk-core/odk_unicode_data.c` are derived from the Unicode
Character Database 15.1.0 under the Unicode License; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and [NOTICE](NOTICE). Keep
`LICENSE`, `NOTICE` and `THIRD_PARTY_NOTICES.md` with any redistribution.
