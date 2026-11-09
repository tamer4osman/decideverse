/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_classify.h - internal API of the Tier A classification engine
 * (F1). Not installed; the spine orchestrates it.
 *
 * The engine is deterministic and bit-replayable: no wall clock, no RNG,
 * no locale (explicit ASCII ranges, no ctype), array iteration only.
 */

#ifndef ODK_CLASSIFY_H
#define ODK_CLASSIFY_H

#include "odk_spine_types.h"

/* Tokenize text for matching: maximal runs of ASCII letters and digits,
 * case-folded (A-Z -> a-z).  Non-ASCII bytes are dropped; they never
 * appear in tokens and never split a run.  The list is malloc'd;
 * release with odk_classify_tokens_free(). */
void odk_classify_tokenize(const char *text, odk_spine_str_list *tokens);
void odk_classify_tokens_free(odk_spine_str_list *tokens);

/* Is `term` present in the token stream? A single-word term matches as a
 * token; a multi-word term matches as a consecutive run. */
int odk_classify_term_present(const odk_spine_str_list *tokens,
                              const char *term);

/* Append one evidence term to a matched_terms list (best-effort: on OOM
 * the evidence shrinks but the label never changes). */
void odk_classify_push_term(odk_spine_str_list *list, const char *term);

/* Tier A: deterministic lexical/rule matching of `item_text` against one
 * scheme. Rules are evaluated in declaration order; among the rules whose
 * condition holds, the highest priority wins, ties broken by the earlier
 * declaration. No rule matched -> default_label, rule_index -1,
 * confidence 0.5. */
odk_spine_status odk_classify_scheme_tier_a(const odk_spine_scheme *scheme,
                                            const char *item_text,
                                            odk_spine_classification *out,
                                            odk_spine_error *err);

/* Tier B (implemented in odk-brain): vocabulary canonicalization plus
 * learned-link evidence from the registered brain snapshot. The result
 * is labeled tier "B" and attributed to snapshot_id. */
odk_spine_status odk_classify_scheme_tier_b(const odk_spine_scheme *scheme,
                                            const odk_spine_vocabulary *vocab,
                                            const odk_brain *brain,
                                            const char *item_text,
                                            const char *snapshot_id,
                                            odk_spine_classification *out,
                                            odk_spine_error *err);

#endif /* ODK_CLASSIFY_H */
