<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- SPDX-FileCopyrightText: 2026 Tamer Osman -->

# Extension guide — the F7 customization seam

OmniDecide v2.0.0 ships the **generic core only**. The kernel and the
spine contain no application-specific content of any kind: no domain
vocabularies, no concrete scheme sets, no source adapters, no named
integrations. Every customization is supplied by the consuming
application, as versioned data, through the extension API described
here — never by editing the kernel.

## What can be registered

| Kind | Constant | Record | Supplied as |
|---|---|---|---|
| scheme sets | `ODK_SPINE_KIND_SCHEME_SET` | `Scheme` (array) | file, JSON text, provider |
| decomposition templates | `ODK_SPINE_KIND_TEMPLATE` | `DecompositionTemplate` (array) | file, JSON text, provider |
| vocabularies | `ODK_SPINE_KIND_VOCABULARY` | `Vocabulary` | file, JSON text, provider |
| golden corpora | `ODK_SPINE_KIND_CORPUS` | JSON or JSONL (opaque) | file, JSON text, provider |
| brain snapshots | `ODK_SPINE_KIND_BRAIN_SNAPSHOT` | `odk_brain *` | application handle |

## Lifecycle: register → validate → activate (atomically)

```c
odk_spine *spine = odk_spine_create();

/* from a file */
odk_spine_register_file(spine, ODK_SPINE_KIND_SCHEME_SET,
                        "myapp/schemes.json", &err);

/* from memory */
odk_spine_register_json(spine, ODK_SPINE_KIND_TEMPLATE, tmpl_json,
                        "myapp://templates", &err);

/* from a provider callback (no file access required) */
int my_provider(void *user, char **out_text, odk_spine_error *err);
odk_spine_register_provider(spine, ODK_SPINE_KIND_VOCABULARY,
                            my_provider, NULL, "myapp://vocab", &err);

/* a brain snapshot (the brain stays application-owned) */
odk_spine_register_brain(spine, brain, "myapp-brain-2026-05", &err);
```

Validation happens **before** anything is stored: schema validation of
every record plus referential integrity (rule facets must exist in the
scheme's `facet_names`; extraction rules must reference declared
component specs; vocabulary groups must be non-empty). Activation is
atomic: a failed registration changes nothing.

Re-registration is a **versioned append**: registering a new version
supersedes the old one (which stays queryable as-of its validity
window). Re-registering the same version with identical content is
idempotent; with different content it is an `ODK_SPINE_ERR_STATE` error.

## Determinism across customization

Identical registered data → identical results. The active
customization set (names + content hashes) is recorded in every
decision pin (`registrations`), so a replay reconstructs the exact
customization state that produced the original decision. The listing is
available any time:

```c
odk_spine_registration_ref_list regs;
odk_spine_registrations(spine, &regs, &err);
/* ... */
odk_spine_registration_ref_list_release(&regs);
```

## Versioned, temporally valid data

Every registered record carries `version`, `valid_from`, `valid_to`
(`-1` = open ended). As-of lookups (`odk_spine_scheme_at` etc.) select,
among the versions covering `t`, the one with the newest `valid_from`
(tie: highest version). Classification and decomposition resolve their
scheme/template the same way, so history stays answerable without
recomputing.

## Rules for applications

1. **Never edit kernel or spine code.** If something seems to require
   it, it is an architecture bug — file an issue.
2. **All domain knowledge is data**: dictionaries, schemes, templates,
   corpora, brain snapshots.
3. **The seam-grep gate is yours too**: replace
   `tests/seam/prohibited_words.txt` with your domain vocabulary in CI;
   `tools/check_seam.sh` fails if any of it leaks into core or spine.
4. **Version your data.** Scheme and template evolution is append-only;
   as-of queries keep old answers true.
5. The in-repo proof is `examples/synthetic/` — explicitly
   non-normative, replaceable wholesale by your own content.
