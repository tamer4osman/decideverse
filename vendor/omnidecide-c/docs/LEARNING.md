<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# Learning from outcomes

The kernel on its own is **stateless**: the same inputs always give the same
answer. The optional **brain** (`odk_brain`) is a small, persistent, pure-data
memory of which options worked before. It can shift the kernel's scores toward
what has been rewarded. It is opt-in, deterministic (no randomness, no clock)
and auditable.

With no brain, or with `weight = 0.0`, results are bit-identical to the plain
kernel.

## The basic loop

```c
odk_kernel *kernel = odk_kernel_create();
odk_brain  *brain  = odk_brain_create();

/* 1. decide */
odk_brain_evaluate_choice(brain, kernel, question, options, 3, context, NULL,
                          1.0 /* weight */, &result, &err);

/* 2. after you learn how it went, record the outcome: reward in [-1, 1] */
odk_brain_record_outcome(brain, "choice", question, chosen_option, +1.0, &err);

/* 3. persist */
odk_brain_save_file(brain, "brain.json", &err);
```

`examples/learning.c` runs a complete version. Its real output:

```text
kernel only            -> open the paper tray
    open the paper tray      0.7369
    power cycle the machine  0.1834
    replace the toner        0.0797
kernel + experience    -> power cycle the machine
    open the paper tray      0.0030
    power cycle the machine  0.9853
    replace the toner        0.0117
restored bonuses: -0.600 0.600 0.000
```

The example records six rewards for one option and six penalties for another;
with weight 1.0 that flips a 74% decision to a 98.5% decision for the other
option. **Experience can override the kernel's evidence by design.** Use a
smaller weight (for example 0.2 to 0.5) if you want it to nudge rather than
decide, and only feed it outcomes you trust.

## How it works

- **Memory entries.** One entry per `(primitive, instruction, option)` key,
  holding wins, losses, draws, visits and a moving average of rewards. A reward
  of at least +0.05 counts as a win, at most -0.05 as a loss, anything between
  as a draw. Rewards outside [-1, 1] are clamped. Repeated keys accumulate.
- **Advice.** `odk_brain_advise` returns a bonus in [-1, 1] per option: the net
  win rate `(wins - losses) / visits`, shrunk by `visits / (visits + 4)`. Options
  never seen, or seen fewer than twice, get 0.0.
- **Evaluation.** `odk_brain_evaluate_choice` takes the kernel's evidence score of
  each option, shifts it by `weight * bonus` inside a wide band, and recomputes
  the probability distribution. The output has the same five-key shape as
  `odk_evaluate_choice`. Repeated penalties can push an option with no
  evidence below its peers, and repeated rewards can lift one above them.
- **Bounded by construction.** Bonuses shrink toward zero without repeated
  confirmation: a single outcome has no effect at all (an entry needs at least
  two visits to produce a bonus).

Only `choice` has a learning evaluator. You can record outcomes for any
primitive, but `odk_brain_advise` is how you apply them elsewhere.

## Snapshots

`odk_brain_save_file` writes a plain JSON file. Saving is atomic: the data goes
to `<path>.tmp` and is renamed. A snapshot records the text configuration it
was made with:

```json
{"version":3,"semantics_version":"2.0.0","unicode_version":"15.1.0",
 "text_mode":"ascii_compat","stopwords":"en", ...}
```

`semantics_version` is the library version that wrote the file; it is informational
and is not checked on load. Loading ignores unknown fields and caps hostile input (entry count, key length);
a structurally broken file fails with `ODK_ERR_ARGUMENT`. Versions 1 and 2 of the
snapshot format still load; version 1 files migrate transparently.

### The text-configuration check

Memory keys depend on how text is tokenized. To stop you from silently mixing
keys made under different settings, loading with an expected configuration
fails loudly when it disagrees:

```c
odk_brain *b = odk_brain_load_file_ex(path, ODK_TEXT_UNICODE, ODK_STOPWORDS_EN, &err);
/* snapshot text config mismatch: snapshot text_mode=ascii_compat stopwords=en
   but brain is text_mode=unicode stopwords=en; re-key or retrain
   (see docs/LEARNING.md) */
```

If you see that error, you have three options:

1. **Load with the configuration the snapshot was saved with** (shown in the
   message), or use `odk_brain_load_file(path, &err)`, which loads without an
   expectation.
2. **Re-key the existing entries** under the new configuration:
   `odk_brain_entries_report_alloc` lists them, `odk_brain_canonical_key_alloc`
   builds a key, and `odk_brain_rekey_entry` / `odk_brain_rekey_entry_raw`
   moves an entry (it refuses to merge into an existing key).
3. **Retrain** from scratch with `odk_brain_create` and the configuration you want.

Set a new brain's configuration with `odk_brain_set_text_config(brain, mode,
pack, &err)` and `odk_brain_set_language(brain, lang, &err)` (the language of a
non-empty brain cannot be changed). `odk_brain_load_file_lang` and the `_ex2`
variants load with an expected language or with extended diagnostics.

### Migrating older snapshots (legacy entries)

Version 1 and 2 snapshots can contain entries whose key parts were cut at an
older 199-byte boundary, which could make different long keys collide. Such
entries load **marked as legacy** and are never merged into new recordings.
To resolve them:

1. `odk_brain_entries_report_alloc(brain)` returns a JSON listing of every entry
   (`index`, `key`, `legacy`, `truncated`, `wins`, `losses`, `draws`, `visits`,
   `ema`, `bonus`). Free it with `free()`.
2. For each legacy entry, either re-key it to its full canonical key with
   `odk_brain_rekey_entry_raw(brain, index, primitive, instruction, option, &err)`
   (or build the key with `odk_brain_canonical_key_alloc` and use
   `odk_brain_rekey_entry`), or drop it with `odk_brain_discard_entry`.
3. A re-key onto a key that already exists is refused (no silent merging), and
   every key part must fit the brain's `key_part_max`.

## Learned word links (advanced, off by default)

The brain can also learn from rewarded outcomes that two context words behave
as the same evidence (for example "outage" and "downtime"). This is controlled
by an `odk_link_policy`:

```c
odk_link_policy policy;
odk_link_policy_defaults(&policy);
policy.link_weight = 0.5;            /* 0 = off (the default) */
odk_brain_set_link_policy(brain, &policy, &err);
odk_brain_record_outcome_ctx(brain, "choice", question, option, context, 1.0, &err);
odk_brain_evaluate_choice_ex(brain, kernel, question, options, n, context, NULL,
                             1.0, &explain_json, &result, &err);  /* explain_json: which links fired */
```

- Links are learned only from outcomes recorded **with context**
  (`odk_brain_record_outcome_ctx`), and only after a word has enough support (by
  default 4 positive outcomes in 3 distinct contexts).
- Their total influence is capped (`link_weight * link_cap`), and they never
  replace strong direct evidence.
- `odk_brain_links_report` returns a JSON report of every link for review.
  `odk_brain_link_pin`, `odk_brain_link_ban` and `odk_brain_link_unpin` let you
  curate them; `odk_brain_links_maintain` recomputes them (also done at save).
- With `link_weight = 0` the mechanism is off and decisions are bit-identical.
- `odk_kernel_lexicon_seed_links` can pre-pin links from a concept lexicon you
  load with `odk_kernel_load_lexicon`.

Word links are the least exercised part of the API in this repository's smoke
test. Evaluate them on your own data before relying on them.

## Thread safety

`odk_brain_advise` is `const` and documented as safe beside concurrent
evaluations. Recording outcomes mutates the brain; sharing one brain across
threads was not tested, so protect it with your own lock.
