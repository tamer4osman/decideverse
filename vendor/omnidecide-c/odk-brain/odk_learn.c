/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_learn.c - opt-in outcome-learning layer for the OmniDecide C library.
 *
 * A persistent, pure-data experience brain that sits ON TOP of the
 * deterministic kernel without touching the stateless decision path:
 *
 *   - every decision the application feeds back as a reward updates a
 *     memory entry keyed by (primitive, instruction, option)
 *   - advise() turns memory into a bounded bonus in [-1, 1] per option:
 *     the net win rate ((wins - losses) / visits), shrunk by
 *     visits / (visits + 4) so thin evidence stays near zero
 *   - odk_brain_evaluate_choice() re-scores the options with the kernel's
 *     own evidence scorer and shifts them by base + w * bonus (clamped to
 *     a wide [-1, 2] band so penalties can demote a zero-evidence option
 *     BELOW its peers and rewards can promote one above them - the band
 *     never binds at weight <= 1 and weight 0 stays bit-identical to
 *     odk_evaluate_choice), then re-calibrates with the standard softmax
 *   - identical (history, brain snapshot) => identical decision: no RNG,
 *     no wall-clock input anywhere in this file
 *
 * Memory-safety conventions (see odk_internal.h): every allocation is
 * checked, all formatting goes through snprintf, hostile inputs are
 * capped (entry count, key length, reward range), and saving is atomic
 * (tmp file + rename).
 *
 */

#include "odk_internal.h"
#include "odk_links.h"
#include "odk_lang.h"
#include "omnidecide.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* ------------------------------------------------------------------ */
/* Tunables (all deterministic constants - no environment dependence)  */
/* ------------------------------------------------------------------ */

#define ODK_BRAIN_FORMAT_VERSION 3
#define ODK_BRAIN_KEY_PART_DEFAULT 4096 /* per-part canonical text bound  */
#define ODK_BRAIN_KEY_PART_FLOOR 64
#define ODK_BRAIN_KEY_PART_CEILING 65536
#define ODK_BRAIN_KEY_TEXT_MAX 196704  /* 3 parts + separators + prim   */
#define ODK_BRAIN_LEGACY_PART_LEN 199 /* truncation boundary        */
#define ODK_BRAIN_LEGACY_KEY_MAX 512 /* v1/v2 load bound (legacy value)     */
#define ODK_BRAIN_MAX_ENTRIES 200000 /* hostile-input cap for load/insert */
#define ODK_BRAIN_MIN_VISITS 2       /* evidence needed before advising   */
#define ODK_BRAIN_SHRINK 4.0         /* visits / (visits + SHRINK)        */
#define ODK_BRAIN_ADJUSTED_MIN (-1.0) /* experience-shift band, see below */
#define ODK_BRAIN_ADJUSTED_MAX 2.0
#define ODK_BRAIN_EMA_ALPHA 0.25     /* ema += alpha * (reward - ema)     */
#define ODK_BRAIN_REWARD_EPS 0.05    /* |reward| below this counts a draw */
#define ODK_BRAIN_TOP_SUMMARY 5      /* entries in the summary payload    */
#define ODK_BRAIN_INIT_BUCKETS 64    /* power of two                      */
#define ODK_BRAIN_ERR_EXCERPT 120    /* bounded caller-data excerpt   */

/* ------------------------------------------------------------------ */
/* Brain handle                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    char *key;      /* canonical "primitive \x1f instruction \x1f option",
                     * truncated per part to key_part_max    */
    uint64_t khi;   /* 128-bit full-text hash, high lane    */
    uint64_t klo;   /* 128-bit full-text hash, low lane                */
    size_t wins;
    size_t losses;
    size_t draws;
    size_t visits;
    double ema_reward;
    unsigned char kt; /* key parts were truncated at key_part_max      */
    unsigned char lg; /* legacy entry loaded from a v1/v2 snapshot     */
} learn_entry;

struct odk_brain {
    learn_entry *entries; /* insertion-ordered                           */
    size_t count;
    size_t cap;
    size_t *buckets;      /* entry index + 1; 0 = empty slot            */
    size_t bucket_count;
    size_t matches;
    size_t wins;
    size_t losses;
    size_t draws;
    odk_link_store *links;      /* learned word links               */
    odk_text_mode text_mode;    /* tokenization for keys + link records */
    odk_stopword_pack pack;
    odk_language language;      /* UND = baseline keys */
    char *loaded_text_mode;     /* snapshot provenance (v2), for checks */
    char *loaded_stopwords;
    size_t key_part_max;        /* per-part canonical bound          */
    size_t key_truncations;    /* reported, never silent            */
};

/* ------------------------------------------------------------------ */
/* Errors, small helpers                                               */
/* ------------------------------------------------------------------ */

static void learn_set_error(odk_error *err, const char *fmt, ...)
{
    va_list args;

    if (err == NULL) {
        return;
    }
    err->text[0] = '\0';
    va_start(args, fmt);
    vsnprintf(err->text, sizeof(err->text), fmt, args);
    va_end(args);
}

/* ------------------------------------------------------------------ */
/* Bounded caller-data excerpts                                */
/* ------------------------------------------------------------------ */

/* Visible truncation marker. */
static const char ODK_BRAIN_ERR_MARKER[] = " ... [truncated]";

/* Copy at most `limit` bytes of caller-controlled data into out,
 * cutting on a UTF-8 code-point boundary, and append the visible
 * marker when a cut happened.  `cap` MUST be at least limit +
 * sizeof(ODK_BRAIN_ERR_MARKER).  Returns the number of data bytes
 * copied (marker excluded).  Bounded by construction: the output can
 * never exceed cap - 1 bytes, so a diagnostic that embeds it never
 * overflows and never truncates silently. */
static size_t learn_bound_excerpt(char *out, size_t cap, const char *data,
                                  size_t limit)
{
    size_t len;
    size_t take;
    int cut = 0;

    out[0] = '\0';
    if (data == NULL) {
        return 0;
    }
    len = strlen(data);
    take = len < limit ? len : limit;
    if (take < len) {
        cut = 1;
        while (take > 0 && ((unsigned char)data[take] & 0xC0) == 0x80) {
            take--; /* back up to the start of the UTF-8 sequence */
        }
    }
    if (take + sizeof(ODK_BRAIN_ERR_MARKER) >= cap) {
        take = cap - sizeof(ODK_BRAIN_ERR_MARKER) - 1;
        cut = 1;
        while (take > 0 && ((unsigned char)data[take] & 0xC0) == 0x80) {
            take--;
        }
    }
    memcpy(out, data, take);
    out[take] = '\0';
    if (cut) {
        memcpy(out + take, ODK_BRAIN_ERR_MARKER,
               sizeof(ODK_BRAIN_ERR_MARKER));
    }
    return take;
}

/* Set BOTH diagnostics for a failure that embeds one caller datum:
 * the legacy odk_error gets prefix + bounded excerpt + marker; the
 * odk_error_ex (may be NULL) gets prefix + the FULL data. */
static void learn_fail_data(odk_error *err, odk_error_ex *errx,
                            odk_status code, const char *prefix,
                            const char *data)
{
    char excerpt[ODK_BRAIN_ERR_EXCERPT + 32]; /* 120 + marker + slack */

    learn_bound_excerpt(excerpt, sizeof(excerpt), data,
                        ODK_BRAIN_ERR_EXCERPT);
    learn_set_error(err, "%s: %s", prefix, excerpt);
    if (errx != NULL) {
        size_t plen = strlen(prefix);
        size_t dlen = data != NULL ? strlen(data) : 0;
        /* "<prefix>: <data>" or "<prefix>" when there is no data */
        size_t total = plen + (dlen > 0 ? 2 + dlen : 0);
        char *msg = malloc(total + 1);

        errx->code = code;
        free(errx->message);
        errx->message = msg;
        if (msg != NULL) {
            memcpy(msg, prefix, plen);
            if (dlen > 0) {
                msg[plen] = ':';
                msg[plen + 1] = ' ';
                memcpy(msg + plen + 2, data, dlen + 1);
            } else {
                msg[plen] = '\0';
            }
        }
    }
}

void odk_error_ex_release(odk_error_ex *err)
{
    if (err == NULL) {
        return;
    }
    free(err->message);
    err->message = NULL;
    err->code = ODK_OK;
}

/* Mismatch failure (snapshot text config vs the live brain):
 * the legacy diagnostic embeds two bounded excerpts (28 bytes each, so
 * the whole message is bounded by construction at 249 bytes); the _ex
 * diagnostic embeds the complete snapshot strings. */
static void learn_fail_mismatch(odk_error *err, odk_error_ex *errx,
                                const char *snap_mode,
                                const char *snap_pack, const char *want_mode,
                                const char *want_pack)
{
    char mode_ex[64];
    char pack_ex[64];

    learn_bound_excerpt(mode_ex, sizeof(mode_ex), snap_mode, 28);
    learn_bound_excerpt(pack_ex, sizeof(pack_ex), snap_pack, 28);
    learn_set_error(err,
                    "snapshot text config mismatch: snapshot "
                    "text_mode=%s stopwords=%s but brain is text_mode=%s "
                    "stopwords=%s; re-key or retrain "
                    "(see docs/LEARNING.md)",
                    mode_ex, pack_ex, want_mode, want_pack);
    if (errx != NULL) {
        static const char fmt[] =
            "snapshot text config mismatch: snapshot "
            "text_mode=%s stopwords=%s but brain is text_mode=%s "
            "stopwords=%s; re-key or retrain "
            "(see docs/LEARNING.md)";
        /* measure exactly, then allocate: the _ex message is unbounded
         * by design (that is the point of odk_error_ex) */
        int need = snprintf(NULL, 0, fmt, snap_mode, snap_pack, want_mode,
                            want_pack);

        errx->code = ODK_ERR_ARGUMENT;
        free(errx->message);
        errx->message = NULL;
        if (need > 0) {
            char *msg = malloc((size_t)need + 1);
            if (msg != NULL) {
                snprintf(msg, (size_t)need + 1, fmt, snap_mode, snap_pack,
                         want_mode, want_pack);
                errx->message = msg;
            }
        }
    }
}

static uint64_t learn_hash(const char *key, size_t len)
{
    uint64_t hash = 1469598103934665603ULL; /* FNV-1a 64, like cmap */
    size_t i;

    for (i = 0; i < len; i++) {
        hash ^= (unsigned char)key[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

/* 128-bit composite hash of the FULL canonical key
 * text - two independent FNV-1a 64 lanes with different constants.
 * Non-cryptographic, documented; collision probability per pair is
 * ~2^-128 and entry equality ALSO compares the stored text. */
static void learn_hash128(const char *text, size_t len, uint64_t *hi,
                           uint64_t *lo)
{
    uint64_t h = 1469598103934665603ULL;
    uint64_t l = 0x9e3779b97f4a7c15ULL;
    size_t i;

    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        h ^= c;
        h *= 1099511628211ULL;
        l ^= c;
        l *= 0xd6e8feb86659fd93ULL;
    }
    *hi = h;
    *lo = l;
}

static int learn_rehash(odk_brain *brain, size_t new_bucket_count)
{
    size_t *grown = calloc(new_bucket_count, sizeof(size_t));
    size_t i;

    if (grown == NULL) {
        return -1;
    }
    free(brain->buckets);
    brain->buckets = grown;
    brain->bucket_count = new_bucket_count;
    for (i = 0; i < brain->count; i++) {
        uint64_t h = learn_hash(brain->entries[i].key,
                                strlen(brain->entries[i].key));
        size_t slot = (size_t)h & (new_bucket_count - 1);
        while (grown[slot] != 0) {
            slot = (slot + 1) & (new_bucket_count - 1);
        }
        grown[slot] = i + 1;
    }
    return 0;
}

/* Find the entry slot for (key, khi, klo); *out_index is the entry
 * index when found, otherwise the insertion point (empty slot) with
 * *out_found = 0.  Equality is stored text AND both hash lanes
 *. */
static size_t learn_find_slot(const odk_brain *brain, const char *key,
                              size_t key_len, uint64_t khi, uint64_t klo,
                              int *out_found)
{
    size_t slot;

    *out_found = 0;
    if (brain->bucket_count == 0) {
        return 0;
    }
    slot = (size_t)learn_hash(key, key_len) & (brain->bucket_count - 1);
    for (;;) {
        size_t held = brain->buckets[slot];
        if (held == 0) {
            return slot;
        }
        if (brain->entries[held - 1].khi == khi &&
            brain->entries[held - 1].klo == klo &&
            strcmp(brain->entries[held - 1].key, key) == 0) {
            *out_found = 1;
            return slot;
        }
        slot = (slot + 1) & (brain->bucket_count - 1);
    }
}

static learn_entry *learn_find(const odk_brain *brain, const char *key,
                               size_t key_len, uint64_t khi, uint64_t klo)
{
    int found = 0;
    size_t slot;

    if (brain == NULL || brain->bucket_count == 0) {
        return NULL;
    }
    slot = learn_find_slot(brain, key, key_len, khi, klo, &found);
    if (!found) {
        return NULL;
    }
    return &brain->entries[brain->buckets[slot] - 1];
}

/* Insert or fetch the entry for (key, khi, klo).  NULL on OOM or when
 * the hostile input cap is reached (the caller keeps working without
 * memory). */
static learn_entry *learn_fetch_entry(odk_brain *brain, const char *key,
                                       uint64_t khi, uint64_t klo,
                                       unsigned char kt)
{
    size_t key_len = strlen(key);
    int found = 0;
    size_t slot;
    learn_entry *entry;

    if (brain->bucket_count == 0) {
        if (learn_rehash(brain, ODK_BRAIN_INIT_BUCKETS) != 0) {
            return NULL;
        }
    } else if (brain->count * 10 >= brain->bucket_count * 7) {
        if (learn_rehash(brain, brain->bucket_count * 2) != 0) {
            return NULL;
        }
    }
    slot = learn_find_slot(brain, key, key_len, khi, klo, &found);
    if (found) {
        return &brain->entries[brain->buckets[slot] - 1];
    }
    if (brain->count >= ODK_BRAIN_MAX_ENTRIES) {
        return NULL; /* cap reached: memory stops growing, still deterministic */
    }
    if (brain->count == brain->cap) {
        size_t grown_cap = brain->cap == 0 ? 16 : brain->cap * 2;
        learn_entry *grown = realloc(brain->entries,
                                     grown_cap * sizeof(learn_entry));
        if (grown == NULL) {
            return NULL;
        }
        brain->entries = grown;
        brain->cap = grown_cap;
    }
    entry = &brain->entries[brain->count];
    memset(entry, 0, sizeof(*entry));
    entry->key = odk_strdup(key);
    if (entry->key == NULL) {
        return NULL;
    }
    entry->khi = khi;
    entry->klo = klo;
    entry->kt = kt;
    brain->buckets[slot] = brain->count + 1;
    brain->count++;
    return entry;
}

/* ------------------------------------------------------------------ */
/* Canonical memory keys                                */
/* ------------------------------------------------------------------ */

/* Canonical form of one key part: the filtered token stream joined by
 * single spaces - the same normalisation the kernel applies to text.
 * tokenization honors the BRAIN's text mode and stopword pack
 * (an earlier design wrongly used the ASCII tokenizer for every brain; pure-ASCII
 * text with the EN pack is byte-identical either way).  Falls back to
 * the raw text when the filter empties it (identity matters more than
 * generality for single-character options).  Returns a malloc'd string
 * with NO truncation (the caller bounds storage); NULL on OOM. */
static char *learn_canonical_part_alloc(const odk_brain *brain,
                                        const char *text)
{
    str_list_t tokens;
    char *joined = NULL;
    /* NULL brain = the legacy "no learning" advise path: the defaults
     * match a fresh ASCII_COMPAT/EN brain, and the lookup that follows
     * finds nothing anyway */
    odk_text_mode mode =
        brain != NULL ? brain->text_mode : ODK_TEXT_ASCII_COMPAT;
    odk_stopword_pack pack =
        brain != NULL ? brain->pack : ODK_STOPWORDS_EN;
    odk_language lang =
        brain != NULL ? brain->language : ODK_LANG_UND;

    if (text == NULL) {
        return odk_strdup("");
    }
    str_list_init(&tokens);
    if (odk_extract_tokens_lang(text, mode, pack, lang, NULL, 1,
                                &tokens) != 0) {
        str_list_free(&tokens);
        return NULL;
    }
    if (tokens.count == 0) {
        str_list_free(&tokens);
        return odk_strdup(text);
    }
    joined = odk_str_join((const char *const *)tokens.items, tokens.count,
                          " ");
    str_list_free(&tokens);
    return joined;
}

/* Truncate one canonical part to the brain's key_part_max, cutting on
 * a code-point boundary in UNICODE mode (byte cut in ASCII_COMPAT,
 * where canonical text of the legacy path is pure ASCII; the fallback raw
 * text may carry multi-byte characters - the cp cut keeps stored keys
 * valid UTF-8).  *truncated is set when a cut happened. */
static char *learn_clip_part(const odk_brain *brain, const char *part,
                             int *truncated)
{
    size_t len = strlen(part);
    size_t take = len;
    size_t part_max = brain != NULL ? brain->key_part_max
                                    : ODK_BRAIN_KEY_PART_DEFAULT;
    odk_text_mode mode = brain != NULL ? brain->text_mode
                                       : ODK_TEXT_ASCII_COMPAT;
    char *out;

    if (take > part_max) {
        take = part_max;
        if (mode == ODK_TEXT_UNICODE) {
            while (take > 0 && ((unsigned char)part[take] & 0xC0) == 0x80) {
                take--; /* back up to the start of the sequence */
            }
        }
        *truncated = 1;
    }
    out = malloc(take + 1);
    if (out == NULL) {
        return NULL;
    }
    memcpy(out, part, take);
    out[take] = '\0';
    return out;
}

/* Build the memory key for (primitive, instruction, option):
 *   full    = canonical parts, untruncated (for the 128-bit hash)
 *   stored  = parts clipped to key_part_max, joined \x1f
 *   khi/klo = learn_hash128(full joined key)
 * Parts of at most 199 bytes are byte-identical to the legacy key
 *.  When any part was clipped, *truncated is set (the RECORD
 * path reports it via brain->key_truncations; advise is a read path
 * and never mutates the counter).  *part_mask receives a bitmask of
 * the parts that were clipped (bit0=prim, bit1=instr, bit2=opt) - the
 * per-part truncation report.  Returns the malloc'd stored
 * key text (caller frees), or NULL on OOM. */
static char *learn_build_key_alloc(const odk_brain *brain,
                                   const char *primitive,
                                   const char *instruction,
                                   const char *option, uint64_t *khi,
                                   uint64_t *klo, unsigned *part_mask)
{
    char *prim = NULL;
    char *instr = NULL;
    char *opt = NULL;
    char *prim_c = NULL;
    char *instr_c = NULL;
    char *opt_c = NULL;
    char *full = NULL;
    char *stored = NULL;
    sb_t sb;
    unsigned mask = 0;
    size_t prim_len;
    size_t instr_len;
    size_t opt_len;

    prim = learn_canonical_part_alloc(brain, primitive);
    instr = learn_canonical_part_alloc(brain, instruction);
    opt = learn_canonical_part_alloc(brain, option);
    if (prim == NULL || instr == NULL || opt == NULL) {
        goto oom;
    }
    if (sb_init(&sb) != 0) {
        goto oom;
    }
    if (sb_puts(&sb, prim) != 0 || sb_putc(&sb, '\x1f') != 0 ||
        sb_puts(&sb, instr) != 0 || sb_putc(&sb, '\x1f') != 0 ||
        sb_puts(&sb, opt) != 0) {
        sb_free(&sb);
        goto oom;
    }
    full = sb_detach(&sb);
    if (full == NULL) {
        goto oom;
    }
    learn_hash128(full, strlen(full), khi, klo);

    {
        int t = 0;
        prim_c = learn_clip_part(brain, prim, &t);
        if (t) {
            mask |= 1u;
        }
        t = 0;
        instr_c = learn_clip_part(brain, instr, &t);
        if (t) {
            mask |= 2u;
        }
        t = 0;
        opt_c = learn_clip_part(brain, opt, &t);
        if (t) {
            mask |= 4u;
        }
    }
    if (prim_c == NULL || instr_c == NULL || opt_c == NULL) {
        goto oom;
    }
    prim_len = strlen(prim_c);
    instr_len = strlen(instr_c);
    opt_len = strlen(opt_c);
    stored = malloc(prim_len + instr_len + opt_len + 3);
    if (stored == NULL) {
        goto oom;
    }
    memcpy(stored, prim_c, prim_len);
    stored[prim_len] = '\x1f';
    memcpy(stored + prim_len + 1, instr_c, instr_len);
    stored[prim_len + 1 + instr_len] = '\x1f';
    memcpy(stored + prim_len + instr_len + 2, opt_c, opt_len + 1);

    *part_mask = mask;
    free(prim);
    free(instr);
    free(opt);
    free(prim_c);
    free(instr_c);
    free(opt_c);
    free(full);
    return stored;

oom:
    free(prim);
    free(instr);
    free(opt);
    free(prim_c);
    free(instr_c);
    free(opt_c);
    free(full);
    return NULL;
}

/* The advise bonus of one entry - the single rule every consumer shares.
 * 0.0 while visits < ODK_BRAIN_MIN_VISITS; otherwise the net win rate
 * shrunk by visits / (visits + ODK_BRAIN_SHRINK), clamped to [-1, 1]. */
static double learn_entry_bonus(const learn_entry *entry)
{
    double net;
    double rate;
    double confidence;

    if (entry == NULL || entry->visits < ODK_BRAIN_MIN_VISITS) {
        return 0.0;
    }
    /* A legacy entry (loaded from v1/v2 with a key part
     * at the 199-byte legacy truncation boundary) conflates experience
     * from every original that shared the truncated prefix; advising
     * from it would mis-attribute that merged experience to whichever
     * option happens to build the exact stored key.  The documented
     * semantics ("orphaned, never merged or mis-attributed") are now
     * enforced on the read path too: the entry answers 0 until the
     * owner re-keys it to one full key (odk_brain_rekey_entry_raw)
     * or discards it. */
    if (entry->lg) {
        return 0.0;
    }
    net = (double)entry->wins - (double)entry->losses;
    rate = net / (double)entry->visits;
    confidence = (double)entry->visits /
                 ((double)entry->visits + ODK_BRAIN_SHRINK);
    if (rate > 1.0) {
        rate = 1.0;
    }
    if (rate < -1.0) {
        rate = -1.0;
    }
    return rate * confidence;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

odk_brain *odk_brain_create(void)
{
    odk_brain *brain = calloc(1, sizeof(odk_brain));
    if (brain == NULL) {
        return NULL;
    }
    brain->links = odk_links_create();
    if (brain->links == NULL) {
        free(brain);
        return NULL;
    }
    brain->text_mode = ODK_TEXT_ASCII_COMPAT;
    brain->pack = ODK_STOPWORDS_EN;
    brain->language = ODK_LANG_UND;
    brain->key_part_max = ODK_BRAIN_KEY_PART_DEFAULT; /* */
    return brain;
}

size_t odk_brain_set_key_limits(odk_brain *brain, size_t key_part_max)
{
    size_t previous;

    if (brain == NULL) {
        return 0;
    }
    previous = brain->key_part_max;
    if (key_part_max < ODK_BRAIN_KEY_PART_FLOOR) {
        key_part_max = ODK_BRAIN_KEY_PART_FLOOR;
    }
    if (key_part_max > ODK_BRAIN_KEY_PART_CEILING) {
        key_part_max = ODK_BRAIN_KEY_PART_CEILING;
    }
    brain->key_part_max = key_part_max;
    return previous;
}

size_t odk_brain_key_truncations(const odk_brain *brain)
{
    return brain != NULL ? brain->key_truncations : 0;
}

void odk_brain_free(odk_brain *brain)
{
    size_t i;

    if (brain == NULL) {
        return;
    }
    for (i = 0; i < brain->count; i++) {
        free(brain->entries[i].key);
    }
    free(brain->entries);
    free(brain->buckets);
    odk_links_free(brain->links);
    free(brain->loaded_text_mode);
    free(brain->loaded_stopwords);
    free(brain);
}

void odk_brain_reset(odk_brain *brain)
{
    size_t i;

    if (brain == NULL) {
        return;
    }
    for (i = 0; i < brain->count; i++) {
        free(brain->entries[i].key);
    }
    brain->count = 0;
    if (brain->buckets != NULL) {
        memset(brain->buckets, 0, brain->bucket_count * sizeof(size_t));
    }
    brain->matches = 0;
    brain->wins = 0;
    brain->losses = 0;
    brain->draws = 0;
    if (brain->links != NULL) {
        /* the POLICY is configuration, not learned data: carry it over
         * to the fresh store so reset does not silently disable links */
        odk_link_policy policy;
        odk_link_policy_defaults(&policy);
        (void)odk_links_get_policy(brain->links, &policy);
        odk_links_free(brain->links);
        brain->links = odk_links_create();
        if (brain->links != NULL) {
            (void)odk_links_set_policy(brain->links, &policy);
        }
    }
    free(brain->loaded_text_mode);
    brain->loaded_text_mode = NULL;
    free(brain->loaded_stopwords);
    brain->loaded_stopwords = NULL;
}

size_t odk_brain_epoch(const odk_brain *brain)
{
    return brain != NULL ? brain->matches : 0;
}

void odk_brain_record_get(const odk_brain *brain, size_t *wins,
                          size_t *losses, size_t *draws)
{
    if (wins != NULL) {
        *wins = brain != NULL ? brain->wins : 0;
    }
    if (losses != NULL) {
        *losses = brain != NULL ? brain->losses : 0;
    }
    if (draws != NULL) {
        *draws = brain != NULL ? brain->draws : 0;
    }
}

/* ------------------------------------------------------------------ */
/* Recording                                                           */
/* ------------------------------------------------------------------ */

odk_status odk_brain_record_outcome(odk_brain *brain, const char *primitive,
                                    const char *instruction,
                                    const char *option, double reward,
                                    odk_error *err)
{
    return odk_brain_record_outcome_ctx(brain, primitive, instruction,
                                        option, NULL, reward, err);
}

int odk_brain_record_outcome_ctx(odk_brain *brain, const char *primitive,
                                 const char *instruction, const char *option,
                                 const char *context, double reward,
                                 odk_error *err)
{
    char *key = NULL;
    uint64_t khi = 0;
    uint64_t klo = 0;
    unsigned part_mask = 0;
    learn_entry *entry;

    if (brain == NULL || primitive == NULL || option == NULL) {
        learn_set_error(err, "brain recording requires brain, primitive "
                             "and option");
        return ODK_ERR_ARGUMENT;
    }
    if (reward != reward) { /* NaN guard, keeps the brain pure data */
        reward = 0.0;
    }
    if (reward > 1.0) {
        reward = 1.0;
    }
    if (reward < -1.0) {
        reward = -1.0;
    }
    key = learn_build_key_alloc(brain, primitive, instruction, option, &khi,
                                &klo, &part_mask);
    if (key == NULL) {
        learn_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    if (part_mask != 0) {
        /* never-silent reporting: the counter and the v3 kt flag carry
         * this to the snapshot and the summary JSON */
        brain->key_truncations++;
    }
    entry = learn_fetch_entry(brain, key, khi, klo,
                              (unsigned char)(part_mask != 0));
    if (entry != NULL && entry->lg) {
        /* The exact stored key of a legacy entry - new
         * experience must not merge into the ambiguous entry the
         * legacy truncation created; resolve it first (re-key to one
         * full key or discard), then record. */
        free(key);
        learn_set_error(err, "legacy entry holds this key: re-key it to "
                             "one full key or discard it before recording "
                             "(odk_brain_rekey_entry_raw / "
                             "odk_brain_discard_entry)");
        return ODK_ERR_ARGUMENT;
    }
    free(key);
    if (entry == NULL) {
        learn_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    entry->visits++;
    if (reward >= ODK_BRAIN_REWARD_EPS) {
        entry->wins++;
    } else if (reward <= -ODK_BRAIN_REWARD_EPS) {
        entry->losses++;
    } else {
        entry->draws++;
    }
    entry->ema_reward += ODK_BRAIN_EMA_ALPHA * (reward - entry->ema_reward);

    /* Word-level learning: association update from the context
     * tokens (no context -> no link learning, exactly legacy behaviour). */
    if (context != NULL && context[0] != '\0' && brain->links != NULL) {
        str_list_t tokens;
        str_list_init(&tokens);
        if (odk_extract_tokens_lang(context, brain->text_mode,
                                    brain->pack, brain->language, NULL, 1,
                                    &tokens) != 0) {
            str_list_free(&tokens);
            learn_set_error(err, "out of memory");
            return ODK_ERR_MEMORY;
        }
        {
            /* canonical option key part (same normalisation as the
             * memory key, full precision - heap based) */
            str_list_t opt_tokens;
            char *opt_key = NULL;
            str_list_init(&opt_tokens);
            if (odk_extract_tokens_lang(option, brain->text_mode,
                                        brain->pack, brain->language, NULL,
                                        1, &opt_tokens) == 0) {
                if (opt_tokens.count > 0) {
                    opt_key = odk_str_join(
                        (const char *const *)opt_tokens.items,
                        opt_tokens.count, " ");
                }
            }
            str_list_free(&opt_tokens);
            if (opt_key == NULL) {
                opt_key = odk_strdup(option != NULL ? option : "");
            }
            if (opt_key != NULL) {
                if (odk_links_record(brain->links, opt_key,
                                     (const char *const *)tokens.items,
                                     tokens.count, reward) != 0) {
                    free(opt_key);
                    str_list_free(&tokens);
                    learn_set_error(err, "out of memory");
                    return ODK_ERR_MEMORY;
                }
                free(opt_key);
            }
        }
        str_list_free(&tokens);
    }
    return ODK_OK;
}

odk_status odk_brain_record_match(odk_brain *brain, odk_match_outcome outcome,
                                  odk_error *err)
{
    if (brain == NULL) {
        learn_set_error(err, "brain handle is required");
        return ODK_ERR_ARGUMENT;
    }
    brain->matches++;
    switch (outcome) {
    case ODK_MATCH_WIN:
        brain->wins++;
        break;
    case ODK_MATCH_LOSS:
        brain->losses++;
        break;
    case ODK_MATCH_DRAW:
        brain->draws++;
        break;
    default:
        brain->matches--;
        learn_set_error(err, "unknown match outcome %d", (int)outcome);
        return ODK_ERR_ARGUMENT;
    }
    return ODK_OK;
}

/* ------------------------------------------------------------------ */
/* Advising and learned evaluation                                     */
/* ------------------------------------------------------------------ */

odk_status odk_brain_advise(const odk_brain *brain, const char *primitive,
                            const char *instruction,
                            const char *const *options, size_t option_count,
                            double *bonuses, odk_error *err)
{
    size_t i;

    if (options == NULL || option_count == 0 || bonuses == NULL) {
        learn_set_error(err, "advise requires options and a bonus buffer");
        return ODK_ERR_ARGUMENT;
    }
    if (primitive == NULL) {
        learn_set_error(err, "advise requires a primitive name");
        return ODK_ERR_ARGUMENT;
    }
    for (i = 0; i < option_count; i++) {
        bonuses[i] = 0.0;
        if (options[i] == NULL) {
            continue;
        }
        {
            uint64_t khi = 0;
            uint64_t klo = 0;
            unsigned part_mask = 0;
            char *key = learn_build_key_alloc(brain, primitive, instruction,
                                              options[i], &khi, &klo,
                                              &part_mask);
            if (key == NULL) {
                learn_set_error(err, "out of memory");
                return ODK_ERR_MEMORY;
            }
            bonuses[i] = learn_entry_bonus(
                learn_find(brain, key, strlen(key), khi, klo));
            free(key);
        }
    }
    return ODK_OK;
}

/* Same labelling rules as the kernel's result_set_distribution: blanks
 * become "(blank)", duplicates get a 1-based "#n" suffix. */
static int learn_set_distribution(odk_result *out, const char *const *labels,
                                  const double *values, size_t count)
{
    size_t i;
    size_t j;

    out->distribution.entries = NULL;
    out->distribution.count = 0;
    if (count == 0) {
        return 0;
    }
    out->distribution.entries = calloc(count, sizeof(odk_distribution_entry));
    if (out->distribution.entries == NULL) {
        return -1;
    }
    out->distribution.count = count;
    for (i = 0; i < count; i++) {
        const char *base = (labels[i] != NULL && labels[i][0] != '\0')
                               ? labels[i]
                               : "(blank)";
        size_t occurrence = 0;
        char suffixed[ODK_ERROR_MAX];
        for (j = 0; j < i; j++) {
            const char *prior = (labels[j] != NULL && labels[j][0] != '\0')
                                    ? labels[j]
                                    : "(blank)";
            if (strcmp(prior, base) == 0) {
                occurrence++;
            }
        }
        if (occurrence == 0) {
            out->distribution.entries[i].key = odk_strdup(base);
        } else {
            snprintf(suffixed, sizeof(suffixed), "%s#%zu", base,
                     occurrence + 1);
            out->distribution.entries[i].key = odk_strdup(suffixed);
        }
        if (out->distribution.entries[i].key == NULL) {
            return -1;
        }
        out->distribution.entries[i].value = odk_round4(values[i]);
    }
    return 0;
}

/* Same labelling rules as the kernel's result_set_primitive. */
static int learn_set_primitive(odk_result *out, const char *name)
{
    out->primitive = odk_strdup(name);
    return out->primitive != NULL ? 0 : -1;
}

/* Learning-path calibration: identical to the kernel's standard
 * calibration for every score in [0, 1] (so weight 0.0 stays bit-identical
 * to odk_evaluate_choice), but honest beyond the unit range, where the
 * experience shift is allowed to move a penalised option below 0 or a
 * rewarded option above 1.  Numerically stable softmax over
 * ODK_LOGIT_SCALE * (clamp(s, [-1, 2]) - 0.5); 0 ok, -1 OOM. */
static int learn_calibrated_distribution(const double *scores, size_t count,
                                         double *probs)
{
    double *logits;
    double peak;
    double total = 0.0;
    size_t i;

    if (count == 0) {
        return 0;
    }
    logits = malloc(count * sizeof(double));
    if (logits == NULL) {
        return -1;
    }
    peak = -HUGE_VAL;
    for (i = 0; i < count; i++) {
        double s = scores[i];
        if (s < ODK_BRAIN_ADJUSTED_MIN) {
            s = ODK_BRAIN_ADJUSTED_MIN;
        }
        if (s > ODK_BRAIN_ADJUSTED_MAX) {
            s = ODK_BRAIN_ADJUSTED_MAX;
        }
        logits[i] = ODK_LOGIT_SCALE * (s - 0.5);
        if (logits[i] > peak) {
            peak = logits[i];
        }
    }
    for (i = 0; i < count; i++) {
        probs[i] = exp(logits[i] - peak);
        total += probs[i];
    }
    free(logits);
    if (total <= 0.0) {
        for (i = 0; i < count; i++) {
            probs[i] = 1.0 / (double)count;
        }
        return 0;
    }
    for (i = 0; i < count; i++) {
        probs[i] = probs[i] / total;
    }
    return 0;
}

odk_status odk_brain_evaluate_choice(odk_brain *brain, odk_kernel *kernel,
                                     const char *instruction,
                                     const char *const *options,
                                     size_t option_count, const char *context,
                                     const odk_image *image, double weight,
                                     odk_result *out, odk_error *err)
{
    return odk_brain_evaluate_choice_ex(brain, kernel, instruction, options,
                                        option_count, context, image, weight,
                                        NULL, out, err);
}

int odk_brain_evaluate_choice_ex(odk_brain *brain, odk_kernel *kernel,
                                 const char *instruction,
                                 const char *const *options,
                                 size_t option_count, const char *context,
                                 const odk_image *image, double weight,
                                 char **explain_json, odk_result *out,
                                 odk_error *err)
{
    double *base = NULL;
    double *bonus = NULL;
    double *adjusted = NULL;
    double *probs = NULL;
    size_t best_index = 0;
    size_t i;
    int rc = 0;

    if (out == NULL) {
        learn_set_error(err, "output result pointer is required");
        return ODK_ERR_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    if (kernel == NULL) {
        learn_set_error(err, "kernel handle is required");
        return ODK_ERR_ARGUMENT;
    }
    if (options == NULL || option_count == 0) {
        learn_set_error(err,
                        "choice evaluation requires at least one option");
        return ODK_ERR_INFERENCE;
    }
    base = malloc(option_count * sizeof(double));
    bonus = malloc(option_count * sizeof(double));
    adjusted = malloc(option_count * sizeof(double));
    probs = malloc(option_count * sizeof(double));
    if (base == NULL || bonus == NULL || adjusted == NULL || probs == NULL) {
        goto oom;
    }
    {
        sb_t explain;
        int explain_ready = 0;
        const odk_link_store *links = NULL;
        double link_weight = 0.0;
        if (brain != NULL && brain->links != NULL) {
            odk_links_params(brain->links, &link_weight, NULL, NULL, NULL);
            if (link_weight > 0.0 && odk_links_has_active(brain->links)) {
                links = brain->links;
            }
        }
        if (explain_json != NULL) {
            *explain_json = NULL;
        }
        if (explain_json != NULL && links != NULL) {
            if (sb_init(&explain) != 0) {
                goto oom;
            }
            if (sb_puts(&explain, "[") != 0) {
                sb_free(&explain);
                goto oom;
            }
            /* The explain array opens with the
             * language marker when the brain runs with a concrete
             * language (language-pack contribution context; UND and
             * legacy brains are unchanged). */
            if (brain->language != ODK_LANG_UND) {
                if (sb_printf(&explain, "{\"language\":\"%s\"}",
                              odk_lang_to_code(brain->language)) != 0) {
                    sb_free(&explain);
                    goto oom;
                }
            }
            explain_ready = 1;
            if (odk_internal_score_options_linked(
                    kernel, links, options, option_count, instruction,
                    context, image, base, &explain) != 0) {
                sb_free(&explain);
                goto oom;
            }
            if (sb_puts(&explain, "]") != 0) {
                sb_free(&explain);
                goto oom;
            }
            *explain_json = sb_detach(&explain);
            if (*explain_json == NULL) {
                goto oom;
            }
        } else {
            if (odk_internal_score_options_linked(
                    kernel, links, options, option_count, instruction,
                    context, image, base, NULL) != 0) {
                goto oom;
            }
        }
        (void)explain_ready;
    }
    if (brain == NULL) {
        for (i = 0; i < option_count; i++) {
            bonus[i] = 0.0;
        }
    } else if (odk_brain_advise(brain, "choice", instruction, options,
                                option_count, bonus, err) != ODK_OK) {
        goto fail;
    }
    for (i = 0; i < option_count; i++) {
        /* The band is a numeric guard, not a floor at 0: with base in
         * [0, 1] and |w * bonus| <= 1 the sum stays inside [-1, 2], so at
         * weight 0.0 the result is bit-identical to odk_evaluate_choice,
         * while penalties can demote a zero-evidence option BELOW its
         * peers (and rewards promote one above them). */
        adjusted[i] = base[i] + weight * bonus[i];
        if (adjusted[i] < ODK_BRAIN_ADJUSTED_MIN) {
            adjusted[i] = ODK_BRAIN_ADJUSTED_MIN;
        }
        if (adjusted[i] > ODK_BRAIN_ADJUSTED_MAX) {
            adjusted[i] = ODK_BRAIN_ADJUSTED_MAX;
        }
        if (i > 0 && adjusted[i] > adjusted[best_index]) {
            best_index = i;
        }
    }
    if (learn_calibrated_distribution(adjusted, option_count, probs) != 0) {
        goto oom;
    }
    rc = learn_set_primitive(out, "choice");
    if (rc == 0) {
        out->kind = ODK_VALUE_STRING;
        out->string = odk_strdup(options[best_index]);
        if (out->string == NULL) {
            rc = -1;
        }
    }
    if (rc == 0) {
        out->confidence = odk_round4(probs[best_index]);
        out->probability = odk_round4(probs[best_index]);
        rc = learn_set_distribution(out, options, probs, option_count);
    }
    free(base);
    free(bonus);
    free(adjusted);
    free(probs);
    if (rc != 0) {
        odk_result_release(out);
        learn_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    return ODK_OK;

oom:
    learn_set_error(err, "out of memory");
fail:
    free(base);
    free(bonus);
    free(adjusted);
    free(probs);
    odk_result_release(out);
    return ODK_ERR_MEMORY;
}

/* ------------------------------------------------------------------ */
/* Summary payload                                                     */
/* ------------------------------------------------------------------ */

char *odk_brain_summary_json(const odk_brain *brain)
{
    sb_t sb;
    char num[64];
    size_t i;
    size_t picked[ODK_BRAIN_TOP_SUMMARY];
    size_t picked_count = 0;
    size_t total_visits = 0;

    if (brain == NULL) {
        return NULL;
    }
    if (sb_init(&sb) != 0) {
        return NULL;
    }
    for (i = 0; i < brain->count; i++) {
        total_visits += brain->entries[i].visits;
    }
    if (sb_printf(&sb,
                  "{\"epoch\":%llu,\"matches\":%llu,\"record\":{\"wins\":%llu,"
                  "\"losses\":%llu,\"draws\":%llu},\"memory_entries\":%llu,"
                  "\"memory_visits\":%llu",
                  (unsigned long long)brain->matches,
                  (unsigned long long)brain->matches,
                  (unsigned long long)brain->wins,
                  (unsigned long long)brain->losses,
                  (unsigned long long)brain->draws,
                  (unsigned long long)brain->count,
                  (unsigned long long)total_visits) != 0) {
        sb_free(&sb);
        return NULL;
    }
    /* Report the language ONLY when a concrete
     * language is set, so every UND-era output stays
     * byte-identical. */
    if (brain->language != ODK_LANG_UND) {
        if (sb_printf(&sb, ",\"language\":\"%s\"",
                      odk_lang_to_code(brain->language)) != 0) {
            sb_free(&sb);
            return NULL;
        }
    }
    /* Report key provenance ONLY when non-zero, so
     * every legacy output stays
     * byte-identical while truncations and legacy entries are never
     * silent. */
    {
        size_t legacy = 0;
        size_t j;

        for (j = 0; j < brain->count; j++) {
            if (brain->entries[j].lg) {
                legacy++;
            }
        }
        if (brain->key_truncations > 0 &&
            sb_printf(&sb, ",\"key_truncations\":%llu",
                      (unsigned long long)brain->key_truncations) != 0) {
            sb_free(&sb);
            return NULL;
        }
        if (legacy > 0 &&
            sb_printf(&sb, ",\"legacy_entries\":%llu",
                      (unsigned long long)legacy) != 0) {
            sb_free(&sb);
            return NULL;
        }
    }
    if (sb_puts(&sb, ",\"top\":[") != 0) {
        sb_free(&sb);
        return NULL;
    }
    /* Top entries by visits; insertion order breaks ties. */
    while (picked_count < ODK_BRAIN_TOP_SUMMARY) {
        size_t best = brain->count;
        for (i = 0; i < brain->count; i++) {
            size_t p;
            int already = 0;
            for (p = 0; p < picked_count; p++) {
                if (picked[p] == i) {
                    already = 1;
                    break;
                }
            }
            if (already) {
                continue;
            }
            if (best == brain->count ||
                brain->entries[i].visits > brain->entries[best].visits) {
                best = i;
            }
        }
        if (best == brain->count) {
            break;
        }
        picked[picked_count++] = best;
    }
    for (i = 0; i < picked_count; i++) {
        const learn_entry *entry = &brain->entries[picked[i]];
        char *literal = odk_json_string_literal(entry->key, strlen(entry->key));
        if (literal == NULL) {
            sb_free(&sb);
            return NULL;
        }
        snprintf(num, sizeof(num), "%.17g", odk_round4(entry->ema_reward));
        if (sb_printf(&sb,
                      "%s{\"key\":%s,\"wins\":%llu,\"losses\":%llu,"
                      "\"draws\":%llu,\"visits\":%llu,\"ema\":%s,\"bonus\":"
                      "%.4f}",
                      i == 0 ? "" : ",", literal,
                      (unsigned long long)entry->wins,
                      (unsigned long long)entry->losses,
                      (unsigned long long)entry->draws,
                      (unsigned long long)entry->visits, num,
                      learn_entry_bonus(entry)) != 0) {
            free(literal);
            sb_free(&sb);
            return NULL;
        }
        free(literal);
    }
    if (sb_puts(&sb, "]}") != 0) {
        sb_free(&sb);
        return NULL;
    }
    return sb_detach(&sb);
}

/* ------------------------------------------------------------------ */
/* Learned word links                           */
/* ------------------------------------------------------------------ */

int odk_brain_set_link_policy(odk_brain *brain,
                              const odk_link_policy *policy, odk_error *err)
{
    if (brain == NULL || brain->links == NULL) {
        learn_set_error(err, "brain handle is required");
        return ODK_ERR_ARGUMENT;
    }
    if (odk_links_set_policy(brain->links, policy) != 0) {
        learn_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    return ODK_OK;
}

int odk_brain_get_link_policy(const odk_brain *brain, odk_link_policy *out)
{
    if (brain == NULL || brain->links == NULL || out == NULL) {
        return ODK_ERR_ARGUMENT;
    }
    if (odk_links_get_policy(brain->links, out) != 0) {
        return ODK_ERR_MEMORY;
    }
    return ODK_OK;
}

int odk_brain_links_maintain(odk_brain *brain, odk_error *err)
{
    if (brain == NULL || brain->links == NULL) {
        learn_set_error(err, "brain handle is required");
        return ODK_ERR_ARGUMENT;
    }
    odk_links_set_epoch(brain->links, brain->matches);
    if (odk_links_maintain(brain->links) != 0) {
        learn_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    return ODK_OK;
}

int odk_brain_links_report(const odk_brain *brain, char **json_out,
                           odk_error *err)
{
    char *json;
    if (json_out == NULL) {
        learn_set_error(err, "output pointer is required");
        return ODK_ERR_ARGUMENT;
    }
    *json_out = NULL;
    if (brain == NULL || brain->links == NULL) {
        learn_set_error(err, "brain handle is required");
        return ODK_ERR_ARGUMENT;
    }
    json = odk_links_report(brain->links);
    if (json == NULL) {
        learn_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    *json_out = json;
    return ODK_OK;
}

int odk_brain_link_pin(odk_brain *brain, const char *a, const char *b,
                       odk_error *err)
{
    if (brain == NULL || brain->links == NULL || a == NULL || b == NULL) {
        learn_set_error(err, "pin requires a brain and two tokens");
        return ODK_ERR_ARGUMENT;
    }
    if (odk_links_pin(brain->links, a, b) != 0) {
        learn_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    return ODK_OK;
}

int odk_brain_link_ban(odk_brain *brain, const char *a, const char *b,
                       odk_error *err)
{
    if (brain == NULL || brain->links == NULL || a == NULL || b == NULL) {
        learn_set_error(err, "ban requires a brain and two tokens");
        return ODK_ERR_ARGUMENT;
    }
    if (odk_links_ban(brain->links, a, b) != 0) {
        learn_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    return ODK_OK;
}

int odk_brain_link_unpin(odk_brain *brain, const char *a, const char *b,
                         odk_error *err)
{
    if (brain == NULL || brain->links == NULL || a == NULL || b == NULL) {
        learn_set_error(err, "unpin requires a brain and two tokens");
        return ODK_ERR_ARGUMENT;
    }
    if (odk_links_unpin(brain->links, a, b) != 0) {
        learn_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    return ODK_OK;
}

int odk_brain_set_text_config(odk_brain *brain, odk_text_mode mode,
                              odk_stopword_pack pack, odk_error *err)
{
    if (brain == NULL) {
        learn_set_error(err, "brain handle is required");
        return ODK_ERR_ARGUMENT;
    }
    if (mode < ODK_TEXT_ASCII_COMPAT || mode > ODK_TEXT_UNICODE) {
        learn_set_error(err, "unknown text mode");
        return ODK_ERR_ARGUMENT;
    }
    if (pack < ODK_STOPWORDS_EN || pack > ODK_STOPWORDS_NONE) {
        learn_set_error(err, "unknown stopword pack");
        return ODK_ERR_ARGUMENT;
    }
    brain->text_mode = mode;
    brain->pack = pack;
    return ODK_OK;
}

/* ------------------------------------------------------------------ */
/* Migration kit - review and re-key legacy entries        */
/*                                                                    */
/* v1/v2 snapshots can carry entries whose key parts were truncated   */
/* At the 199-byte legacy boundary (the key-collision shape).  They load  */
/* marked legacy and are never merged into new recordings; these      */
/* additive functions give the owner a review-and-rekey path (see     */
/* docs/LEARNING.md): list every entry with its provenance, map a     */
/* legacy entry to its full key, or discard it.                       */
/* No silent merges: a re-key onto an existing key is refused.        */
/* ------------------------------------------------------------------ */

char *odk_brain_entries_report_alloc(const odk_brain *brain)
{
    sb_t sb;
    size_t i;
    size_t legacy = 0;

    if (brain == NULL) {
        return NULL;
    }
    if (sb_init(&sb) != 0) {
        return NULL;
    }
    for (i = 0; i < brain->count; i++) {
        if (brain->entries[i].lg) {
            legacy++;
        }
    }
    if (sb_printf(&sb,
                  "{\"memory_entries\":%llu,\"legacy_entries\":%llu,"
                  "\"key_truncations\":%llu,\"entries\":[",
                  (unsigned long long)brain->count,
                  (unsigned long long)legacy,
                  (unsigned long long)brain->key_truncations) != 0) {
        goto oom;
    }
    for (i = 0; i < brain->count; i++) {
        const learn_entry *e = &brain->entries[i];
        char *lit = odk_json_string_literal(e->key, strlen(e->key));
        char ema[64];
        if (lit == NULL) {
            goto oom;
        }
        snprintf(ema, sizeof(ema), "%.17g", e->ema_reward);
        if (i > 0 && sb_putc(&sb, ',') != 0) {
            free(lit);
            goto oom;
        }
        if (sb_printf(&sb,
                      "{\"index\":%zu,\"key\":%s,\"legacy\":%s,"
                      "\"truncated\":%s,\"wins\":%llu,\"losses\":%llu,"
                      "\"draws\":%llu,\"visits\":%llu,\"ema\":%s,"
                      "\"bonus\":%.4f}",
                      i, lit, e->lg ? "true" : "false",
                      e->kt ? "true" : "false",
                      (unsigned long long)e->wins,
                      (unsigned long long)e->losses,
                      (unsigned long long)e->draws,
                      (unsigned long long)e->visits, ema,
                      learn_entry_bonus(e)) != 0) {
            free(lit);
            goto oom;
        }
        free(lit);
    }
    if (sb_puts(&sb, "]}") != 0) {
        goto oom;
    }
    return sb_detach(&sb);

oom:
    sb_free(&sb);
    return NULL;
}

/* Build the FULL canonical key text for raw
 * (primitive, instruction, option) inputs under THIS brain's text
 * mode and stopword pack - the untruncated parts joined by \x1f.
 * This is the exact text the RECORD/ADVISE path hashes; the stored
 * form clips parts to key_part_max, so a caller re-keying an entry
 * must additionally keep every part within that limit (rekey refuses
 * otherwise).  Returns a malloc'd string (caller frees); NULL + err
 * on allocation failure.  A NULL brain uses the same defaults as the
 * no-learning advise path (ASCII_COMPAT + EN stopwords). */
static char *learn_full_key_alloc(const odk_brain *brain,
                                  const char *primitive,
                                  const char *instruction,
                                  const char *option)
{
    char *prim = learn_canonical_part_alloc(brain, primitive);
    char *instr = learn_canonical_part_alloc(brain, instruction);
    char *opt = learn_canonical_part_alloc(brain, option);
    char *full = NULL;
    sb_t sb;

    if (prim == NULL || instr == NULL || opt == NULL) {
        goto done;
    }
    if (sb_init(&sb) != 0) {
        goto done;
    }
    if (sb_puts(&sb, prim) != 0 || sb_putc(&sb, '\x1f') != 0 ||
        sb_puts(&sb, instr) != 0 || sb_putc(&sb, '\x1f') != 0 ||
        sb_puts(&sb, opt) != 0) {
        sb_free(&sb);
        goto done;
    }
    full = sb_detach(&sb);

done:
    free(prim);
    free(instr);
    free(opt);
    return full;
}

char *odk_brain_canonical_key_alloc(const odk_brain *brain,
                                    const char *primitive,
                                    const char *instruction,
                                    const char *option, odk_error *err)
{
    char *full = learn_full_key_alloc(brain, primitive, instruction,
                                      option);

    if (full == NULL) {
        learn_set_error(err, "out of memory");
        return NULL;
    }
    return full;
}

/* Canonical-form validation for re-key keys.  new_key
 * must split into exactly three \x1f-separated parts and each part
 * must be a fixed point of THIS brain's canonicalisation - i.e.
 * re-canonicalising the part yields the part itself.  Returns 0 and
 * sets *bad_part (0-based, -1 for a structural fault) with a filled
 * error message on the first violation. */
static int learn_key_is_canonical(const odk_brain *brain,
                                  const char *new_key, int *bad_part,
                                  odk_error *err)
{
    const char *p = new_key;
    int part;

    *bad_part = -1;
    for (part = 0; part < 3; part++) {
        const char *sep = strchr(p, '\x1f');
        size_t len = (part < 2)
                         ? (size_t)(sep - p)
                         : strlen(p);
        char *piece;
        char *canon;
        int mismatch;

        if (part < 2 && sep == NULL) {
            learn_set_error(err,
                            "re-key key must be three \\x1f-separated "
                            "parts (primitive, instruction, option); use "
                            "odk_brain_canonical_key_alloc or "
                            "odk_brain_rekey_entry_raw");
            *bad_part = -1;
            return 0;
        }
        if (part == 2 && strchr(p, '\x1f') != NULL) {
            learn_set_error(err,
                            "re-key key must be exactly three "
                            "parts (found a fourth separator)");
            *bad_part = -1;
            return 0;
        }
        piece = malloc(len + 1);
        if (piece == NULL) {
            learn_set_error(err, "out of memory");
            return 0;
        }
        memcpy(piece, p, len);
        piece[len] = '\0';
        canon = learn_canonical_part_alloc(brain, piece);
        mismatch = canon == NULL || strcmp(canon, piece) != 0;
        free(canon);
        free(piece);
        if (mismatch) {
            learn_set_error(err,
                            "re-key part %d is not in canonical form "
                            "(this brain's text mode and stopword pack "
                            "normalise it differently); build the key "
                            "with odk_brain_canonical_key_alloc or call "
                            "odk_brain_rekey_entry_raw", part);
            *bad_part = part;
            return 0;
        }
        if (part < 2) {
            p = sep + 1;
        }
    }
    return 1;
}

odk_status odk_brain_rekey_entry(odk_brain *brain, size_t index,
                                 const char *new_key, odk_error *err)
{
    uint64_t khi = 0;
    uint64_t klo = 0;
    size_t key_len;
    size_t part_len = 0;
    size_t i;
    char *copy;
    char *old_key;
    uint64_t old_khi;
    uint64_t old_klo;
    unsigned char old_kt;
    unsigned char old_lg;

    if (brain == NULL) {
        learn_set_error(err, "brain handle is required");
        return ODK_ERR_ARGUMENT;
    }
    if (index >= brain->count) {
        learn_set_error(err, "entry index %zu out of range (%zu entries)",
                        index, brain->count);
        return ODK_ERR_ARGUMENT;
    }
    if (new_key == NULL || new_key[0] == '\0') {
        learn_set_error(err, "re-key requires a non-empty key");
        return ODK_ERR_ARGUMENT;
    }
    key_len = strlen(new_key);
    if (key_len >= ODK_BRAIN_KEY_TEXT_MAX) {
        learn_set_error(err, "re-key exceeds the key text bound (%d bytes)",
                        ODK_BRAIN_KEY_TEXT_MAX - 1);
        return ODK_ERR_ARGUMENT;
    }
    /* A key that is not in canonical form would store an
     * entry no RECORD or ADVISE path can ever reach - the brain
     * builds keys by canonicalising, so accept nothing else.  (An
     * earlier API took any text and silently stranded the experience;
     * the reviewer's hand-built key is now refused here.) */
    {
        int bad_part = -1;

        if (!learn_key_is_canonical(brain, new_key, &bad_part, err)) {
            return ODK_ERR_ARGUMENT;
        }
    }
    /* the owner maps the entry to its FULL key: every part must fit
     * key_part_max so the stored form needs no further clipping */
    for (i = 0; i <= key_len; i++) {
        if (new_key[i] == '\x1f' || new_key[i] == '\0') {
            if (part_len > brain->key_part_max) {
                learn_set_error(err,
                                "re-key part exceeds key_part_max (%zu "
                                "bytes); provide the full key or raise "
                                "the limit",
                                brain->key_part_max);
                return ODK_ERR_ARGUMENT;
            }
            part_len = 0;
        } else {
            part_len++;
        }
    }
    learn_hash128(new_key, key_len, &khi, &klo);
    /* refuse silent merges AND no-op re-keys: the target key must not
     * exist anywhere in the brain - including the entry itself (an
     * entry already carrying new_key needs no re-key; letting it pass
     * would hide caller bugs in migration tooling) */
    for (i = 0; i < brain->count; i++) {
        if (brain->entries[i].khi == khi &&
            brain->entries[i].klo == klo &&
            strcmp(brain->entries[i].key, new_key) == 0) {
            learn_set_error(err,
                            "re-key target already exists at index %zu "
                            "(no silent merge, no no-op re-key); discard "
                            "or pick a distinct full key",
                            i);
            return ODK_ERR_ARGUMENT;
        }
    }
    copy = odk_strdup(new_key);
    if (copy == NULL) {
        learn_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    old_key = brain->entries[index].key;
    old_khi = brain->entries[index].khi;
    old_klo = brain->entries[index].klo;
    old_kt = brain->entries[index].kt;
    old_lg = brain->entries[index].lg;
    brain->entries[index].key = copy;
    brain->entries[index].khi = khi;
    brain->entries[index].klo = klo;
    brain->entries[index].kt = 0;
    brain->entries[index].lg = 0;
    /* rebuild the lookup index; on failure roll the entry back (the
     * buckets were left untouched, so the old state stays consistent) */
    if (brain->bucket_count != 0 &&
        learn_rehash(brain, brain->bucket_count) != 0) {
        brain->entries[index].key = old_key;
        brain->entries[index].khi = old_khi;
        brain->entries[index].klo = old_klo;
        brain->entries[index].kt = old_kt;
        brain->entries[index].lg = old_lg;
        free(copy);
        learn_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    free(old_key);
    return ODK_OK;
}

odk_status odk_brain_rekey_entry_raw(odk_brain *brain, size_t index,
                                        const char *primitive,
                                        const char *instruction,
                                        const char *option,
                                        odk_error *err)
{
    char *full;

    if (brain == NULL) {
        learn_set_error(err, "brain handle is required");
        return ODK_ERR_ARGUMENT;
    }
    if (primitive == NULL || instruction == NULL || option == NULL) {
        learn_set_error(err, "re-key needs primitive, instruction and "
                             "option text");
        return ODK_ERR_ARGUMENT;
    }
    full = learn_full_key_alloc(brain, primitive, instruction, option);
    if (full == NULL) {
        learn_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    {
        odk_status rc = odk_brain_rekey_entry(brain, index, full, err);

        free(full);
        return rc;
    }
}

odk_status odk_brain_discard_entry(odk_brain *brain, size_t index,
                                   odk_error *err)
{
    learn_entry *new_entries = NULL;
    size_t *new_buckets = NULL;
    size_t new_count;
    size_t i;
    size_t j;

    if (brain == NULL) {
        learn_set_error(err, "brain handle is required");
        return ODK_ERR_ARGUMENT;
    }
    if (index >= brain->count) {
        learn_set_error(err, "entry index %zu out of range (%zu entries)",
                        index, brain->count);
        return ODK_ERR_ARGUMENT;
    }
    new_count = brain->count - 1;
    /* failure-atomic discard: build the shifted entry array and a
     * matching fresh index FIRST (keys are shallow-copied - ownership
     * moves to the new array), and only then swap */
    new_entries = malloc((new_count ? new_count : 1) * sizeof(learn_entry));
    if (new_entries == NULL) {
        learn_set_error(err, "out of memory");
        return ODK_ERR_MEMORY;
    }
    for (i = 0, j = 0; i < brain->count; i++) {
        if (i != index) {
            new_entries[j++] = brain->entries[i];
        }
    }
    if (brain->bucket_count != 0) {
        new_buckets = calloc(brain->bucket_count, sizeof(size_t));
        if (new_buckets == NULL) {
            free(new_entries); /* shallow copies: keys stay owned */
            learn_set_error(err, "out of memory");
            return ODK_ERR_MEMORY;
        }
        for (j = 0; j < new_count; j++) {
            uint64_t h = learn_hash(new_entries[j].key,
                                    strlen(new_entries[j].key));
            size_t slot = (size_t)h & (brain->bucket_count - 1);
            while (new_buckets[slot] != 0) {
                slot = (slot + 1) & (brain->bucket_count - 1);
            }
            new_buckets[slot] = j + 1;
        }
    }
    free(brain->entries[index].key); /* the discarded entry's key */
    free(brain->entries);
    free(brain->buckets);
    brain->entries = new_entries;
    brain->count = new_count;
    brain->cap = new_count ? new_count : 1;
    brain->buckets = new_buckets;
    return ODK_OK;
}

/* ------------------------------------------------------------------ */
/* Persistence                                                         */
/* ------------------------------------------------------------------ */

/* Shared save core: every failure reports through BOTH channels
 * (legacy odk_error bounded, odk_error_ex full).  err / errx may each
 * be NULL (the corresponding channel is simply skipped). */
static odk_status learn_save_core(const odk_brain *brain, const char *path,
                                  odk_error *err, odk_error_ex *errx)
{
    odk_link_policy policy_snapshot;
    sb_t sb;
    char *tmp_path = NULL;
    FILE *fp = NULL;
    size_t i;
    size_t path_len;
    size_t legacy_count = 0;

    if (brain == NULL || path == NULL || path[0] == '\0') {
        learn_fail_data(err, errx, ODK_ERR_ARGUMENT,
                        "saving requires a brain and a path", NULL);
        return ODK_ERR_ARGUMENT;
    }
    if (sb_init(&sb) != 0) {
        learn_fail_data(err, errx, ODK_ERR_MEMORY, "out of memory", NULL);
        return ODK_ERR_MEMORY;
    }
    /* Maintain links so the snapshot is self-consistent, then
     * write the v3 header with the text-layer provenance and the key
     * limits. */
    {
        odk_link_policy policy;
        odk_link_policy_defaults(&policy);
        if (brain->links != NULL) {
            odk_links_set_epoch(brain->links, brain->matches);
            if (odk_links_maintain(brain->links) != 0) {
                sb_free(&sb);
                learn_fail_data(err, errx, ODK_ERR_MEMORY, "out of memory",
                                NULL);
                return ODK_ERR_MEMORY;
            }
            (void)odk_links_get_policy(brain->links, &policy);
        }
        policy_snapshot = policy;
    }
    for (i = 0; i < brain->count; i++) {
        if (brain->entries[i].lg) {
            legacy_count++;
        }
    }
    if (sb_printf(&sb,
                  "{\"version\":%d,\"semantics_version\":\"%s\","
                  "\"unicode_version\":\"%s\",\"text_mode\":\"%s\","
                  "\"stopwords\":\"%s\","
                  "\"key_part_max\":%zu,\"key_truncations\":%zu,"
                  "\"legacy_entries\":%zu,"
                  "\"link_policy\":{\"weight\":%.17g,"
                  "\"min_support\":%u,\"min_contexts\":%u,"
                  "\"active_threshold\":%.17g,\"link_cap\":%.17g,"
                  "\"link_gain\":%.17g,\"generic_fraction\":%.17g,"
                  "\"max_links\":%zu,\"max_tokens\":%zu,"
                  "\"max_total_links\":%zu,\"decay_half_life\":%d},"
                  "\"epoch\":%llu,\"matches\":%llu,"
                  "\"record\":{\"wins\":%llu,\"losses\":%llu,\"draws\":%llu},",
                  ODK_BRAIN_FORMAT_VERSION,
                  ODK_LIBRARY_VERSION,
                  ODK_UNICODE_VERSION,
                  brain->text_mode == ODK_TEXT_UNICODE ? "unicode"
                                                       : "ascii_compat",
                  brain->pack == ODK_STOPWORDS_DE
                      ? "de"
                      : brain->pack == ODK_STOPWORDS_FR
                            ? "fr"
                            : brain->pack == ODK_STOPWORDS_ES
                                  ? "es"
                                  : brain->pack == ODK_STOPWORDS_NONE
                                        ? "none"
                                        : "en",
                  brain->key_part_max,
                  brain->key_truncations,
                  legacy_count,
                  policy_snapshot.link_weight,
                  policy_snapshot.min_support,
                  policy_snapshot.min_contexts,
                  policy_snapshot.active_threshold,
                  policy_snapshot.link_cap,
                  policy_snapshot.link_gain,
                  policy_snapshot.generic_fraction,
                  policy_snapshot.max_links,
                  policy_snapshot.max_tokens,
                  policy_snapshot.max_total_links,
                  policy_snapshot.decay_half_life,
                  (unsigned long long)brain->matches,
                  (unsigned long long)brain->matches,
                  (unsigned long long)brain->wins,
                  (unsigned long long)brain->losses,
                  (unsigned long long)brain->draws) != 0) {
        sb_free(&sb);
        learn_fail_data(err, errx, ODK_ERR_MEMORY, "out of memory", NULL);
        return ODK_ERR_MEMORY;
    }
    /* The language field is OPTIONAL on the
     * v3 format - it is written ONLY for a concrete language, right
     * after the stopwords field.  A UND brain therefore saves
     * byte-identically to the language-less format (forwards AND backwards compatible),
     * and the loader keys the mismatch/provenance policy on field
     * presence. */
    if (brain->language != ODK_LANG_UND) {
        if (sb_printf(&sb, "\"language\":\"%s\",",
                      odk_lang_to_code(brain->language)) != 0) {
            sb_free(&sb);
            learn_fail_data(err, errx, ODK_ERR_MEMORY, "out of memory",
                            NULL);
            return ODK_ERR_MEMORY;
        }
    }
    if (sb_puts(&sb, "\"entries\":[") != 0) {
        sb_free(&sb);
        learn_fail_data(err, errx, ODK_ERR_MEMORY, "out of memory", NULL);
        return ODK_ERR_MEMORY;
    }
    for (i = 0; i < brain->count; i++) {
        const learn_entry *entry = &brain->entries[i];
        char *literal =
            odk_json_string_literal(entry->key, strlen(entry->key));
        if (literal == NULL) {
            sb_free(&sb);
            learn_fail_data(err, errx, ODK_ERR_MEMORY, "out of memory",
                            NULL);
            return ODK_ERR_MEMORY;
        }
        if (sb_printf(&sb,
                      "%s{\"key\":%s,\"khi\":\"%016llx\",\"klo\":\"%016llx\","
                      "\"kt\":%s,\"lg\":%s,"
                      "\"wins\":%llu,\"losses\":%llu,"
                      "\"draws\":%llu,\"visits\":%llu,\"ema\":%.17g}",
                      i == 0 ? "" : ",", literal,
                      (unsigned long long)entry->khi,
                      (unsigned long long)entry->klo,
                      entry->kt ? "true" : "false",
                      entry->lg ? "true" : "false",
                      (unsigned long long)entry->wins,
                      (unsigned long long)entry->losses,
                      (unsigned long long)entry->draws,
                      (unsigned long long)entry->visits,
                      entry->ema_reward) != 0) {
            free(literal);
            sb_free(&sb);
            learn_fail_data(err, errx, ODK_ERR_MEMORY, "out of memory",
                            NULL);
            return ODK_ERR_MEMORY;
        }
        free(literal);
    }
    if (sb_puts(&sb, "]") != 0) {
        sb_free(&sb);
        learn_fail_data(err, errx, ODK_ERR_MEMORY, "out of memory", NULL);
        return ODK_ERR_MEMORY;
    }
    if (brain->links != NULL && odk_links_serialize(brain->links, &sb) != 0) {
        sb_free(&sb);
        learn_fail_data(err, errx, ODK_ERR_MEMORY, "out of memory", NULL);
        return ODK_ERR_MEMORY;
    }
    if (sb_puts(&sb, "}") != 0) {
        sb_free(&sb);
        learn_fail_data(err, errx, ODK_ERR_MEMORY, "out of memory", NULL);
        return ODK_ERR_MEMORY;
    }

    path_len = strlen(path);
    tmp_path = malloc(path_len + 5);
    if (tmp_path == NULL) {
        sb_free(&sb);
        learn_fail_data(err, errx, ODK_ERR_MEMORY, "out of memory", NULL);
        return ODK_ERR_MEMORY;
    }
    snprintf(tmp_path, path_len + 5, "%s.tmp", path);

    fp = fopen(tmp_path, "wb");
    if (fp == NULL) {
        free(tmp_path);
        sb_free(&sb);
        learn_fail_data(err, errx, ODK_ERR_INFERENCE,
                        "cannot write brain snapshot", path);
        return ODK_ERR_INFERENCE;
    }
    if (fwrite(sb.data, 1, sb.len, fp) != sb.len) {
        fclose(fp);
        remove(tmp_path);
        free(tmp_path);
        sb_free(&sb);
        learn_fail_data(err, errx, ODK_ERR_INFERENCE,
                        "cannot write brain snapshot", path);
        return ODK_ERR_INFERENCE;
    }
    if (fclose(fp) != 0) {
        remove(tmp_path);
        free(tmp_path);
        sb_free(&sb);
        learn_fail_data(err, errx, ODK_ERR_INFERENCE,
                        "cannot write brain snapshot", path);
        return ODK_ERR_INFERENCE;
    }
    if (rename(tmp_path, path) != 0) {
        remove(tmp_path);
        free(tmp_path);
        sb_free(&sb);
        learn_fail_data(err, errx, ODK_ERR_INFERENCE,
                        "cannot rename brain snapshot", path);
        return ODK_ERR_INFERENCE;
    }
    free(tmp_path);
    sb_free(&sb);
    return ODK_OK;
}

odk_status odk_brain_save_file(const odk_brain *brain, const char *path,
                               odk_error *err)
{
    return learn_save_core(brain, path, err, NULL);
}

odk_status odk_brain_save_file_ex(const odk_brain *brain, const char *path,
                                  odk_error_ex *errx)
{
    return learn_save_core(brain, path, NULL, errx);
}

odk_brain *odk_brain_load_file(const char *path, odk_error *err)
{
    return odk_brain_load_file_ex(path, ODK_TEXT_ASCII_COMPAT,
                                  ODK_STOPWORDS_EN, err);
}

/* Unicode-version mismatch failure: legacy embeds a bounded excerpt of
 * the snapshot's unicode_version string, the _ex embeds it fully. */
static void learn_fail_unicode(odk_error *err, odk_error_ex *errx,
                               const char *snap_uni)
{
    char ex[ODK_BRAIN_ERR_EXCERPT + 32];

    learn_bound_excerpt(ex, sizeof(ex), snap_uni, ODK_BRAIN_ERR_EXCERPT);
    learn_set_error(err,
                    "brain snapshot was written against Unicode %s but "
                    "this build pins %s; regenerate the snapshot",
                    ex, ODK_UNICODE_VERSION);
    if (errx != NULL) {
        static const char fmt[] =
            "brain snapshot was written against Unicode %s but "
            "this build pins %s; regenerate the snapshot";
        int need = snprintf(NULL, 0, fmt, snap_uni, ODK_UNICODE_VERSION);

        errx->code = ODK_ERR_ARGUMENT;
        free(errx->message);
        errx->message = NULL;
        if (need > 0) {
            char *msg = malloc((size_t)need + 1);
            if (msg != NULL) {
                snprintf(msg, (size_t)need + 1, fmt, snap_uni,
                         ODK_UNICODE_VERSION);
                errx->message = msg;
            }
        }
    }
}

/* Parse a 16-character lowercase hex lane ("khi"/"klo").  Returns 0 and
 * stores the value on success; -1 on any malformed input (defensive:
 * hostile v3 files fall back to hashing the stored key text). */
static int learn_parse_hex16(const char *s, uint64_t *out)
{
    uint64_t v = 0;
    int i;

    if (s == NULL || strlen(s) != 16) {
        return -1;
    }
    for (i = 0; i < 16; i++) {
        char c = s[i];
        uint64_t d;

        if (c >= '0' && c <= '9') {
            d = (uint64_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (uint64_t)(c - 'a' + 10);
        } else {
            return -1;
        }
        v = (v << 4) | d;
    }
    *out = v;
    return 0;
}

/* TRUE only for a JSON boolean true value. */
static int learn_json_true(const odk_json_value *v)
{
    return v != NULL && v->type == ODK_JSON_BOOL && v->boolean;
}

/* A v1/v2 stored key is legacy when any \x1f-separated canonical part
 * is at least 199 bytes: legacy keys could only ever store a 199-byte prefix of
 * the part, so the entry may be unreachable by new recordings
 * (orphaned, never merged or mis-attributed). */
static int learn_key_is_legacy(const char *key)
{
    size_t part_len = 0;
    const char *p;

    for (p = key; *p != '\0'; p++) {
        if (*p == '\x1f') {
            if (part_len >= ODK_BRAIN_LEGACY_PART_LEN) {
                return 1;
            }
            part_len = 0;
        } else {
            part_len++;
        }
    }
    return part_len >= ODK_BRAIN_LEGACY_PART_LEN;
}

/* Shared load core: reports through BOTH channels on failure
 * (legacy bounded, _ex full).  err / errx may each be NULL. */
static odk_brain *learn_load_core(const char *path, odk_text_mode mode,
                                  odk_stopword_pack pack, odk_language lang,
                                  odk_error *err, odk_error_ex *errx)
{
    FILE *fp = NULL;
    char *text = NULL;
    long file_len = 0;
    size_t len;
    odk_json_value *root = NULL;
    const odk_json_value *version;
    const odk_json_value *entries;
    const odk_json_value *record;
    odk_brain *brain = NULL;
    size_t i;
    size_t epoch = 0;
    size_t matches = 0;
    size_t wins = 0;
    size_t losses = 0;
    size_t draws = 0;
    int snapshot_version = 1;

    if (path == NULL || path[0] == '\0') {
        learn_fail_data(err, errx, ODK_ERR_ARGUMENT,
                        "loading requires a path", NULL);
        return NULL;
    }
    fp = fopen(path, "rb");
    if (fp == NULL) {
        learn_fail_data(err, errx, ODK_ERR_INFERENCE,
                        "cannot read brain snapshot", path);
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0 || (file_len = ftell(fp)) < 0 ||
        fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        learn_fail_data(err, errx, ODK_ERR_INFERENCE,
                        "cannot read brain snapshot", path);
        return NULL;
    }
    len = (size_t)file_len;
    text = malloc(len + 1);
    if (text == NULL) {
        fclose(fp);
        learn_fail_data(err, errx, ODK_ERR_MEMORY, "out of memory", NULL);
        return NULL;
    }
    if (fread(text, 1, len, fp) != len) {
        fclose(fp);
        free(text);
        learn_fail_data(err, errx, ODK_ERR_INFERENCE,
                        "cannot read brain snapshot", path);
        return NULL;
    }
    fclose(fp);
    text[len] = '\0';

    root = odk_json_parse(text, len, err);
    free(text);
    if (root == NULL) {
        learn_fail_data(err, errx, ODK_ERR_INFERENCE,
                        "brain snapshot is not valid JSON", path);
        return NULL;
    }
    version = odk_json_object_get(root, "version");
    if (root->type != ODK_JSON_OBJECT || version == NULL ||
        version->type != ODK_JSON_NUMBER ||
        ((int)version->number != 1 && (int)version->number != 2 &&
         (int)version->number != 3)) {
        learn_set_error(err, "brain snapshot has an unsupported version");
        if (errx != NULL) {
            errx->code = ODK_ERR_ARGUMENT;
            free(errx->message);
            errx->message =
                odk_strdup("brain snapshot has an unsupported version");
        }
        odk_json_free(root);
        return NULL;
    }
    snapshot_version = (int)version->number;
    if (lang < ODK_LANG_UND || lang > ODK_LANG_UR) {
        learn_fail_data(err, errx, ODK_ERR_ARGUMENT,
                        "unknown language", NULL);
        odk_json_free(root);
        return NULL;
    }
    brain = odk_brain_create();
    if (brain == NULL) {
        learn_fail_data(err, errx, ODK_ERR_MEMORY, "out of memory", NULL);
        odk_json_free(root);
        return NULL;
    }
    /* the caller's expected text configuration (v2/v3 provenance) */
    if (odk_brain_set_text_config(brain, mode, pack, NULL) != ODK_OK) {
        odk_brain_free(brain);
        odk_json_free(root);
        learn_fail_data(err, errx, ODK_ERR_ARGUMENT,
                        "invalid text configuration", NULL);
        return NULL;
    }

    brain->language = lang; /* v4 == snapshot language; v1-v3 == UND */

    record = odk_json_object_get(root, "record");
    if (record != NULL && record->type == ODK_JSON_OBJECT) {
        const odk_json_value *field;
        field = odk_json_object_get(record, "wins");
        if (field != NULL && field->type == ODK_JSON_NUMBER &&
            field->number > 0.0) {
            wins = (size_t)field->number;
        }
        field = odk_json_object_get(record, "losses");
        if (field != NULL && field->type == ODK_JSON_NUMBER &&
            field->number > 0.0) {
            losses = (size_t)field->number;
        }
        field = odk_json_object_get(record, "draws");
        if (field != NULL && field->type == ODK_JSON_NUMBER &&
            field->number > 0.0) {
            draws = (size_t)field->number;
        }
    }
    {
        const odk_json_value *field = odk_json_object_get(root, "matches");
        if (field != NULL && field->type == ODK_JSON_NUMBER &&
            field->number > 0.0) {
            matches = (size_t)field->number;
        }
        field = odk_json_object_get(root, "epoch");
        if (field != NULL && field->type == ODK_JSON_NUMBER &&
            field->number > 0.0) {
            epoch = (size_t)field->number;
        }
    }
    brain->wins = wins;
    brain->losses = losses;
    brain->draws = draws;
    brain->matches = matches > epoch ? matches : epoch;

    if (snapshot_version >= 2) {
        /* v2/v3 provenance: text_mode / stopwords / unicode_version must
         * match the LIVE brain configuration, otherwise memory keys and
         * associations would silently mis-key. */
        const odk_json_value *mode_field =
            odk_json_object_get(root, "text_mode");
        const odk_json_value *pack_field =
            odk_json_object_get(root, "stopwords");
        const odk_json_value *uni_field =
            odk_json_object_get(root, "unicode_version");
        const char *mode_str =
            (mode_field != NULL && mode_field->type == ODK_JSON_STRING)
                ? mode_field->string
                : "ascii_compat";
        const char *pack_str =
            (pack_field != NULL && pack_field->type == ODK_JSON_STRING)
                ? pack_field->string
                : "en";
        const char *uni_str =
            (uni_field != NULL && uni_field->type == ODK_JSON_STRING)
                ? uni_field->string
                : ODK_UNICODE_VERSION;
        int want_unicode = brain->text_mode == ODK_TEXT_UNICODE;
        int snap_unicode = strcmp(mode_str, "unicode") == 0;
        const char *want_pack =
            brain->pack == ODK_STOPWORDS_DE
                ? "de"
                : brain->pack == ODK_STOPWORDS_FR
                      ? "fr"
                      : brain->pack == ODK_STOPWORDS_ES
                            ? "es"
                            : brain->pack == ODK_STOPWORDS_NONE
                                  ? "none"
                                  : "en";
        if (want_unicode != snap_unicode || strcmp(pack_str, want_pack) != 0) {
            learn_fail_mismatch(err, errx, mode_str, pack_str,
                                want_unicode ? "unicode" : "ascii_compat",
                                want_pack);
            odk_brain_free(brain);
            odk_json_free(root);
            return NULL;
        }
        /* Language provenance is an OPTIONAL
 * field on the v3 format (readers ignore unknown fields, so
 * snapshots with the field stay loadable by language-less readers as UND).  A snapshot that
 * carries the field and disagrees with the expected language fails
 * LOUDLY (same policy as text_mode/stopwords): memory keys and link
 * records are language-sensitive, so a silent
 * cross-language load would mis-key.  Snapshots without the field
 * (from language-less builds) load as UND; expecting a concrete language
 * from a provenance-free snapshot is refused for the same reason. */
        {
            const odk_json_value *lang_field =
                odk_json_object_get(root, "language");
            if (lang_field != NULL &&
                lang_field->type == ODK_JSON_STRING) {
                const char *snap_lang = lang_field->string;
                const char *want_lang = odk_lang_to_code(lang);
                if (strcmp(snap_lang, want_lang) != 0) {
                    char msg[96];
                    snprintf(msg, sizeof(msg),
                             "snapshot language %s, expected %s", snap_lang,
                             want_lang);
                    learn_fail_data(err, errx, ODK_ERR_ARGUMENT,
                                    "snapshot language mismatch: ", msg);
                    odk_brain_free(brain);
                    odk_json_free(root);
                    return NULL;
                }
            } else if (lang != ODK_LANG_UND) {
                char msg[96];
                snprintf(msg, sizeof(msg),
                         "v%d snapshot (no language field), expected %s",
                         snapshot_version, odk_lang_to_code(lang));
                learn_fail_data(err, errx, ODK_ERR_ARGUMENT,
                                "snapshot has no language provenance: ", msg);
                odk_brain_free(brain);
                odk_json_free(root);
                return NULL;
            }
        }
        if (strcmp(uni_str, ODK_UNICODE_VERSION) != 0) {
            learn_fail_unicode(err, errx, uni_str);
            odk_brain_free(brain);
            odk_json_free(root);
            return NULL;
        }
        /* link policy from the snapshot (weights etc.); associations and
         * pinned/banned pairs follow; links are recomputed on maintain. */
        {
            const odk_json_value *lp = odk_json_object_get(root,
                                                           "link_policy");
            if (lp != NULL && lp->type == ODK_JSON_OBJECT &&
                brain->links != NULL) {
                odk_link_policy policy;
                odk_link_policy_defaults(&policy);
                (void)odk_links_get_policy(brain->links, &policy);
                const odk_json_value *f;
                f = odk_json_object_get(lp, "weight");
                if (f != NULL && f->type == ODK_JSON_NUMBER) {
                    policy.link_weight = f->number;
                }
                f = odk_json_object_get(lp, "min_support");
                if (f != NULL && f->type == ODK_JSON_NUMBER &&
                    f->number > 0.0) {
                    policy.min_support = (unsigned)f->number;
                }
                f = odk_json_object_get(lp, "min_contexts");
                if (f != NULL && f->type == ODK_JSON_NUMBER &&
                    f->number > 0.0) {
                    policy.min_contexts = (unsigned)f->number;
                }
                f = odk_json_object_get(lp, "active_threshold");
                if (f != NULL && f->type == ODK_JSON_NUMBER) {
                    policy.active_threshold = f->number;
                }
                f = odk_json_object_get(lp, "link_cap");
                if (f != NULL && f->type == ODK_JSON_NUMBER) {
                    policy.link_cap = f->number;
                }
                f = odk_json_object_get(lp, "link_gain");
                if (f != NULL && f->type == ODK_JSON_NUMBER) {
                    policy.link_gain = f->number;
                }
                f = odk_json_object_get(lp, "generic_fraction");
                if (f != NULL && f->type == ODK_JSON_NUMBER) {
                    policy.generic_fraction = f->number;
                }
                f = odk_json_object_get(lp, "max_links");
                if (f != NULL && f->type == ODK_JSON_NUMBER &&
                    f->number > 0.0) {
                    policy.max_links = (size_t)f->number;
                }
                f = odk_json_object_get(lp, "max_tokens");
                if (f != NULL && f->type == ODK_JSON_NUMBER &&
                    f->number > 0.0) {
                    policy.max_tokens = (size_t)f->number;
                }
                f = odk_json_object_get(lp, "max_total_links");
                if (f != NULL && f->type == ODK_JSON_NUMBER &&
                    f->number > 0.0) {
                    policy.max_total_links = (size_t)f->number;
                }
                f = odk_json_object_get(lp, "decay_half_life");
                if (f != NULL && f->type == ODK_JSON_NUMBER) {
                    policy.decay_half_life = (int)f->number;
                }
                (void)odk_links_set_policy(brain->links, &policy);
            }
        }
    }

    if (snapshot_version == 3) {
        /* v3 key provenance: key_part_max (clamped into range) and the
         * never-silent truncation counter (saturating read). */
        const odk_json_value *f = odk_json_object_get(root, "key_part_max");
        if (f != NULL && f->type == ODK_JSON_NUMBER &&
            f->number == f->number && f->number >= 1.0) {
            size_t want = f->number > 65536.0 ? 65536 : (size_t)f->number;
            (void)odk_brain_set_key_limits(brain, want);
        }
        f = odk_json_object_get(root, "key_truncations");
        if (f != NULL && f->type == ODK_JSON_NUMBER &&
            f->number == f->number && f->number > 0.0) {
            brain->key_truncations = f->number > 1.0e15
                                         ? (size_t)1.0e15
                                         : (size_t)f->number;
        }
    }

    entries = odk_json_object_get(root, "entries");
    if (entries != NULL && entries->type == ODK_JSON_ARRAY) {
        for (i = 0; i < entries->item_count; i++) {
            const odk_json_value *item = entries->items[i];
            const odk_json_value *field;
            const char *key;
            learn_entry *entry;
            size_t w = 0;
            size_t l = 0;
            size_t d = 0;
            double ema = 0.0;
            uint64_t khi = 0;
            uint64_t klo = 0;
            int kt = 0;
            int lg;

            if (item == NULL || item->type != ODK_JSON_OBJECT) {
                continue;
            }
            field = odk_json_object_get(item, "key");
            if (field == NULL || field->type != ODK_JSON_STRING ||
                field->string == NULL) {
                continue;
            }
            key = field->string;
            if (snapshot_version == 3) {
                /* v3 hostile bound: the widest key save can emit at the
                 * 65 536 ceiling is 3*65536 + 2 separators. */
                if (strlen(key) == 0 ||
                    strlen(key) >= ODK_BRAIN_KEY_TEXT_MAX) {
                    continue;
                }
            } else {
                /* v1/v2 keep the exact legacy bound (512). */
                if (strlen(key) == 0 || strlen(key) >= ODK_BRAIN_LEGACY_KEY_MAX) {
                    continue;
                }
            }
            field = odk_json_object_get(item, "wins");
            if (field != NULL && field->type == ODK_JSON_NUMBER &&
                field->number > 0.0) {
                w = (size_t)field->number;
            }
            field = odk_json_object_get(item, "losses");
            if (field != NULL && field->type == ODK_JSON_NUMBER &&
                field->number > 0.0) {
                l = (size_t)field->number;
            }
            field = odk_json_object_get(item, "draws");
            if (field != NULL && field->type == ODK_JSON_NUMBER &&
                field->number > 0.0) {
                d = (size_t)field->number;
            }
            field = odk_json_object_get(item, "ema");
            if (field != NULL && field->type == ODK_JSON_NUMBER) {
                ema = field->number;
            }
            if (brain->count >= ODK_BRAIN_MAX_ENTRIES) {
                break; /* hostile-input cap: keep the first entries */
            }
            if (snapshot_version == 3) {
                /* v3 carries the lanes; malformed lanes fall back to
                 * hashing the stored text (deterministic, defensive) */
                const odk_json_value *lane =
                    odk_json_object_get(item, "khi");
                if (lane == NULL || lane->type != ODK_JSON_STRING ||
                    learn_parse_hex16(lane->string, &khi) != 0) {
                    learn_hash128(key, strlen(key), &khi, &klo);
                } else {
                    lane = odk_json_object_get(item, "klo");
                    if (lane == NULL || lane->type != ODK_JSON_STRING ||
                        learn_parse_hex16(lane->string, &klo) != 0) {
                        learn_hash128(key, strlen(key), &khi, &klo);
                    }
                }
                kt = learn_json_true(odk_json_object_get(item, "kt"));
                lg = learn_json_true(odk_json_object_get(item, "lg"));
            } else {
                /* v1/v2 migration: lanes = hash of the
                 * STORED key text as-is; entries whose parts hit the
                 * 199-byte legacy truncation boundary are legacy. */
                learn_hash128(key, strlen(key), &khi, &klo);
                lg = learn_key_is_legacy(key);
            }
            entry = learn_fetch_entry(brain, key, khi, klo,
                                      (unsigned char)kt);
            if (entry == NULL) {
                odk_brain_free(brain);
                odk_json_free(root);
                learn_fail_data(err, errx, ODK_ERR_MEMORY, "out of memory",
                                NULL);
                return NULL;
            }
            entry->wins = w;
            entry->losses = l;
            entry->draws = d;
            entry->visits = w + l + d;
            entry->ema_reward = ema;
            entry->kt = (unsigned char)kt;
            entry->lg = (unsigned char)lg;
        }
    }
    /* link data (v2/v3; harmless when absent) */
    if (snapshot_version >= 2 && brain->links != NULL) {
        odk_links_set_epoch(brain->links, brain->matches);
        if (odk_links_load_json(brain->links, root) != 0) {
            odk_brain_free(brain);
            odk_json_free(root);
            learn_fail_data(err, errx, ODK_ERR_MEMORY, "out of memory",
                            NULL);
            return NULL;
        }
        if (odk_links_maintain(brain->links) != 0) {
            odk_brain_free(brain);
            odk_json_free(root);
            learn_fail_data(err, errx, ODK_ERR_MEMORY, "out of memory",
                            NULL);
            return NULL;
        }
    }
    odk_json_free(root);
    return brain;
}

odk_brain *odk_brain_load_file_ex(const char *path, odk_text_mode mode,
                                  odk_stopword_pack pack, odk_error *err)
{
    return learn_load_core(path, mode, pack, ODK_LANG_UND, err, NULL);
}

odk_brain *odk_brain_load_file_ex2(const char *path, odk_text_mode mode,
                                   odk_stopword_pack pack, odk_error_ex *errx)
{
    return learn_load_core(path, mode, pack, ODK_LANG_UND, NULL, errx);
}

/* Load with an EXPECTED language.  A v4 snapshot
 * whose language differs fails loudly (same policy as text_mode and
 * stopwords); v1-v3 snapshots load as UND and refuse a concrete
 * expected language (no provenance - silent mis-keying risk). */
odk_brain *odk_brain_load_file_lang(const char *path, odk_text_mode mode,
                                    odk_stopword_pack pack, odk_language lang,
                                    odk_error *err)
{
    return learn_load_core(path, mode, pack, lang, err, NULL);
}

/* The brain's own language (memory keys + link records).
 * Default UND = exact baseline keys.  Setting a language AFTER entries
 * exist is refused (keys are language-sensitive); re-create the brain
 * or load a matching snapshot instead.  Snapshots record the language
 * as an optional field on the v3 format. */
odk_status odk_brain_set_language(odk_brain *brain, odk_language lang,
                                  odk_error *err)
{
    if (brain == NULL) {
        learn_set_error(err, "brain handle is required");
        return ODK_ERR_ARGUMENT;
    }
    if (lang < ODK_LANG_UND || lang > ODK_LANG_UR) {
        learn_set_error(err, "unknown language");
        return ODK_ERR_ARGUMENT;
    }
    if (brain->count > 0 && lang != brain->language) {
        learn_set_error(err, "cannot change language on a non-empty brain");
        return ODK_ERR_ARGUMENT;
    }
    brain->language = lang;
    return ODK_OK;
}

odk_language odk_brain_language(const odk_brain *brain)
{
    return brain != NULL ? brain->language : ODK_LANG_UND;
}

/* ------------------------------------------------------------------ */
/* Internal test hooks (no global state)                 */
/* ------------------------------------------------------------------ */

long odk_internal_brain_insert_raw(odk_brain *brain, const char *key,
                                   uint64_t khi, uint64_t klo)
{
    learn_entry *entry;

    if (brain == NULL || key == NULL || key[0] == '\0') {
        return -1;
    }
    entry = learn_fetch_entry(brain, key, khi, klo, 0);
    if (entry == NULL) {
        return -1;
    }
    entry->visits++;
    return (long)entry->visits;
}

long odk_internal_brain_find_raw(const odk_brain *brain, const char *key,
                                 uint64_t khi, uint64_t klo)
{
    const learn_entry *entry;

    if (brain == NULL || key == NULL) {
        return -1;
    }
    entry = learn_find(brain, key, strlen(key), khi, klo);
    return entry != NULL ? (long)entry->visits : -1;
}

char *odk_internal_brain_build_key_raw(const odk_brain *brain,
                                       const char *primitive,
                                       const char *instruction,
                                       const char *option, uint64_t *khi,
                                       uint64_t *klo,
                                       unsigned *part_mask)
{
    if (khi != NULL) {
        *khi = 0;
    }
    if (klo != NULL) {
        *klo = 0;
    }
    if (part_mask != NULL) {
        *part_mask = 0;
    }
    if (brain == NULL || primitive == NULL) {
        return NULL;
    }
    return learn_build_key_alloc(brain, primitive, instruction, option, khi,
                                 klo, part_mask);
}
