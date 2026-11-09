/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_links.h - internal interface of the learned word-link subsystem
 *.  Not public API.
 *
 * A link store is owned by the brain (odk_learn.c) and is the only
 * mutable state besides the memory entries.  All counters are integer
 * and all iteration orders are pinned (byte order / insertion order),
 * so results are order-independent and reproducible.
 *
 */

#ifndef ODK_LINKS_H
#define ODK_LINKS_H

#include "odk_internal.h"

typedef struct odk_link_store odk_link_store;

/* Status of a learned or curated link. */
typedef enum {
    ODK_LINK_CANDIDATE = 0, /* learned, below the promotion threshold */
    ODK_LINK_ACTIVE = 1,   /* used in scoring                        */
    ODK_LINK_PINNED = 2,   /* human-approved: always active          */
    ODK_LINK_BANNED = 3    /* human-rejected: never forms again      */
} odk_link_status;

odk_link_store *odk_links_create(void);
void odk_links_free(odk_link_store *store);

/* Policy copy (NULL resets to defaults).  Returns 0/-1. */
int odk_links_set_policy(odk_link_store *store, const odk_link_policy *policy);
int odk_links_get_policy(const odk_link_store *store, odk_link_policy *out);

/* True when the store has any ACTIVE or PINNED link (fast scoring gate). */
int odk_links_has_active(const odk_link_store *store);

/* Scoring parameters the kernel needs. */
void odk_links_params(const odk_link_store *store, double *out_weight,
                      double *out_cap, double *out_gain,
                      double *out_threshold);

/* Enumerate the ACTIVE/PINNED partners of token `x` with their
 * strengths, in ascending partner byte order.  Returns the number
 * written (<= max_out).  Only links with strength >= threshold are
 * returned. */
size_t odk_links_partners(const odk_link_store *store, const char *x,
                          const char **out_tokens, double *out_strengths,
                          size_t max_out);

/* Record one outcome: option_key is the canonical option text; tokens
 * is the filtered context token list (may contain duplicates; the
 * store deduplicates).  reward in [-1, 1].  Returns 0/-1 (OOM). */
int odk_links_record(odk_link_store *store, const char *option_key,
                     const char *const *tokens, size_t token_count,
                     double reward);

/* Recompute candidate/active links from the associations;
 * applies pinned/banned overrides and caps with deterministic pruning.
 * Returns 0/-1. */
int odk_links_maintain(odk_link_store *store);

/* JSON report (malloc'd) of every link with status, strength, support,
 * contexts and example option keys - for human review. */
char *odk_links_report(const odk_link_store *store);

/* Human curation.  Pin/ban operate on token strings (order-insensitive).
 * Unpin removes the pinned status.  Ban persists forever.  0 ok,
 * ODK_ERR_ARGUMENT for NULL args, -1 OOM. */
int odk_links_pin(odk_link_store *store, const char *a, const char *b);
int odk_links_ban(odk_link_store *store, const char *a, const char *b);
int odk_links_unpin(odk_link_store *store, const char *a, const char *b);

/* Snapshot v2: append the "assoc" and "links" JSON fragments
 * (without the enclosing braces / commas - the caller places them). */
int odk_links_serialize(const odk_link_store *store, sb_t *out);

/* Load from a v2 snapshot's parsed JSON (root object); unknown fields
 * are ignored, hostile counts are capped.  Returns 0/-1. */
int odk_links_load_json(odk_link_store *store, const odk_json_value *root);

/* Snapshot-v2 state restoration: set exact counters without
 * replaying records (lossless round-trip). */
int odk_links_restore_row(odk_link_store *store, const char *token,
                          unsigned contexts, unsigned mentions,
                          const char *option_key, unsigned pos, unsigned neg);
void odk_links_set_total_contexts(odk_link_store *store, size_t total);
void odk_links_set_epoch(odk_link_store *store, size_t epoch);

/* Number of tracked tokens / associations / links (for reports). */
void odk_links_stats(const odk_link_store *store, size_t *tokens,
                     size_t *assoc_rows, size_t *links);

#endif /* ODK_LINKS_H */
