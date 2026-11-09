/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_links.c - learned word links.
 *
 * Data model (all integer counters, order-independent by construction):
 *
 *   tokens:    interned context tokens (insertion-ordered array + hash)
 *   options:   interned canonical option keys (insertion-ordered)
 *   assoc:     per token, per option: pos / neg integer counters plus a
 *              bounded set of 8 stored context fingerprints (FNV-1a of
 *              the sorted token list) for distinct-context gating
 *   cooc:      bounded set of unordered token-id pairs that appeared in
 *              the SAME context (co-occurring tokens are never linked)
 *   links:     recomputed at maintain()/save time: candidate + active
 *              links with strength; pinned/banned are stored overrides
 *
 * Determinism: enumeration is always in byte order (strcmp) or
 * insertion order; strength math follows a fixed expression order.
 * No RNG, no wall clock; decay (optional, default off) uses the epoch
 * counter supplied by the brain at maintain time.
 *
 */

#include "odk_links.h"

#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Tunables                                           */
/* ------------------------------------------------------------------ */

#define LINK_FPS_MAX 8u          /* stored distinct-context fingerprints */
#define LINK_COOC_MAX 65536u     /* co-occurring pair cap (fail-safe)    */
#define LINK_COOC_TOKENS 32u     /* tokens per context recorded as cooc  */
#define LINK_MAINTAIN_BUDGET 2000000u /* pair evaluations per maintain   */

/* ------------------------------------------------------------------ */
/* Interned tokens / options                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    char *key;      /* byte string (owned)                              */
    size_t id;      /* index into arrays                                 */
} intern_entry;

typedef struct {
    intern_entry *entries;   /* insertion ordered */
    size_t count;
    size_t cap;
    size_t *buckets;         /* entry index + 1; 0 = empty */
    size_t bucket_count;
} intern_table;

static uint64_t links_hash(const char *s, size_t len)
{
    uint64_t h = 1469598103934665603ULL;
    size_t i;
    for (i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static void intern_init(intern_table *t)
{
    t->entries = NULL;
    t->count = 0;
    t->cap = 0;
    t->buckets = NULL;
    t->bucket_count = 0;
}

static void intern_free(intern_table *t)
{
    size_t i;
    for (i = 0; i < t->count; i++) {
        free(t->entries[i].key);
    }
    free(t->entries);
    free(t->buckets);
    intern_init(t);
}

static int intern_rehash(intern_table *t, size_t new_count)
{
    size_t *grown = calloc(new_count, sizeof(size_t));
    size_t i;
    if (grown == NULL) {
        return -1;
    }
    free(t->buckets);
    t->buckets = grown;
    t->bucket_count = new_count;
    for (i = 0; i < t->count; i++) {
        uint64_t h = links_hash(t->entries[i].key, strlen(t->entries[i].key));
        size_t slot = (size_t)h & (new_count - 1);
        while (t->buckets[slot] != 0) {
            slot = (slot + 1) & (new_count - 1);
        }
        t->buckets[slot] = i + 1;
    }
    return 0;
}

/* Intern key; *out_is_new set when inserted.  Returns id or
 * (size_t)-1 on OOM. */
static size_t intern_intern(intern_table *t, const char *key,
                            int *out_is_new)
{
    size_t len = strlen(key);
    uint64_t h;
    size_t slot;
    *out_is_new = 0;
    if (t->bucket_count == 0) {
        if (intern_rehash(t, 64) != 0) {
            return (size_t)-1;
        }
    } else if ((t->count + 1) * 10 >= t->bucket_count * 7) {
        if (intern_rehash(t, t->bucket_count * 2) != 0) {
            return (size_t)-1;
        }
    }
    h = links_hash(key, len);
    slot = (size_t)h & (t->bucket_count - 1);
    for (;;) {
        size_t idx = t->buckets[slot];
        if (idx == 0) {
            break;
        }
        if (strcmp(t->entries[idx - 1].key, key) == 0) {
            return idx - 1;
        }
        slot = (slot + 1) & (t->bucket_count - 1);
    }
    /* insert */
    if (t->count == t->cap) {
        size_t grown_cap = t->cap ? t->cap * 2 : 32;
        intern_entry *grown =
            realloc(t->entries, grown_cap * sizeof(intern_entry));
        if (grown == NULL) {
            return (size_t)-1;
        }
        t->entries = grown;
        t->cap = grown_cap;
    }
    t->entries[t->count].key = odk_strdup(key);
    if (t->entries[t->count].key == NULL) {
        return (size_t)-1;
    }
    t->entries[t->count].id = t->count;
    t->buckets[slot] = t->count + 1;
    t->count++;
    *out_is_new = 1;
    return t->count - 1;
}

static const char *intern_get(const intern_table *t, size_t id)
{
    return id < t->count ? t->entries[id].key : NULL;
}

/* ------------------------------------------------------------------ */
/* Association rows                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t option_id;
    uint32_t pos;
    uint32_t neg;
} assoc_row;

typedef struct {
    assoc_row *rows;
    size_t count;
    size_t cap;
} assoc_list;

/* ------------------------------------------------------------------ */
/* Links                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    size_t a;             /* token id (a < b by token byte order)        */
    size_t b;
    double strength;      /* recomputed at maintain time                 */
    unsigned char status; /* odk_link_status                             */
    size_t created_epoch; /* brain epoch when first learned              */
} link_entry;

/* Pinned/banned pair set (token string pairs, byte-order normalised). */
typedef struct {
    char *pair; /* "a\x1fb" (owned), a < b lexicographically */
} pair_entry;

typedef struct {
    pair_entry *items;
    size_t count;
    size_t cap;
    size_t *buckets;
    size_t bucket_count;
} pair_set;

static void pair_set_init(pair_set *s)
{
    s->items = NULL;
    s->count = 0;
    s->cap = 0;
    s->buckets = NULL;
    s->bucket_count = 0;
}

static void pair_set_free(pair_set *s)
{
    size_t i;
    for (i = 0; i < s->count; i++) {
        free(s->items[i].pair);
    }
    free(s->items);
    free(s->buckets);
    pair_set_init(s);
}

/* normalised "a\x1fb" with a < b */
static char *pair_key(const char *a, const char *b)
{
    const char *lo = strcmp(a, b) <= 0 ? a : b;
    const char *hi = lo == a ? b : a;
    size_t la = strlen(lo);
    size_t lb = strlen(hi);
    char *key = malloc(la + lb + 2);
    if (key == NULL) {
        return NULL;
    }
    memcpy(key, lo, la);
    key[la] = '\x1f';
    memcpy(key + la + 1, hi, lb + 1);
    return key;
}

static int pair_set_rehash(pair_set *s, size_t new_count)
{
    size_t *grown = calloc(new_count, sizeof(size_t));
    size_t i;
    if (grown == NULL) {
        return -1;
    }
    free(s->buckets);
    s->buckets = grown;
    s->bucket_count = new_count;
    for (i = 0; i < s->count; i++) {
        uint64_t h = links_hash(s->items[i].pair, strlen(s->items[i].pair));
        size_t slot = (size_t)h & (new_count - 1);
        while (s->buckets[slot] != 0) {
            slot = (slot + 1) & (new_count - 1);
        }
        s->buckets[slot] = i + 1;
    }
    return 0;
}

/* membership only (does not insert) */
static int pair_set_has(const pair_set *s, const char *a, const char *b)
{
    char *key;
    uint64_t h;
    size_t slot;
    int found = 0;
    if (s->bucket_count == 0) {
        return 0;
    }
    key = pair_key(a, b);
    if (key == NULL) {
        return 0;
    }
    h = links_hash(key, strlen(key));
    slot = (size_t)h & (s->bucket_count - 1);
    for (;;) {
        size_t idx = s->buckets[slot];
        if (idx == 0) {
            break;
        }
        if (strcmp(s->items[idx - 1].pair, key) == 0) {
            found = 1;
            break;
        }
        slot = (slot + 1) & (s->bucket_count - 1);
    }
    free(key);
    return found;
}

/* insert-or-nothing; returns 0 ok, -1 OOM, 1 when already present */
static int pair_set_add(pair_set *s, const char *a, const char *b)
{
    char *key;
    uint64_t h;
    size_t slot;
    if (s->bucket_count == 0) {
        if (pair_set_rehash(s, 64) != 0) {
            return -1;
        }
    } else if ((s->count + 1) * 10 >= s->bucket_count * 7) {
        if (pair_set_rehash(s, s->bucket_count * 2) != 0) {
            return -1;
        }
    }
    key = pair_key(a, b);
    if (key == NULL) {
        return -1;
    }
    h = links_hash(key, strlen(key));
    slot = (size_t)h & (s->bucket_count - 1);
    for (;;) {
        size_t idx = s->buckets[slot];
        if (idx == 0) {
            break;
        }
        if (strcmp(s->items[idx - 1].pair, key) == 0) {
            free(key);
            return 1;
        }
        slot = (slot + 1) & (s->bucket_count - 1);
    }
    if (s->count == s->cap) {
        size_t grown_cap = s->cap ? s->cap * 2 : 32;
        pair_entry *grown = realloc(s->items, grown_cap * sizeof(pair_entry));
        if (grown == NULL) {
            free(key);
            return -1;
        }
        s->items = grown;
        s->cap = grown_cap;
    }
    s->items[s->count].pair = key;
    s->buckets[slot] = s->count + 1;
    s->count++;
    return 0;
}

static void pair_set_remove(pair_set *s, const char *a, const char *b)
{
    char *key;
    uint64_t h;
    size_t slot;
    if (s->bucket_count == 0) {
        return;
    }
    key = pair_key(a, b);
    if (key == NULL) {
        return;
    }
    h = links_hash(key, strlen(key));
    slot = (size_t)h & (s->bucket_count - 1);
    for (;;) {
        size_t idx = s->buckets[slot];
        if (idx == 0) {
            break;
        }
        if (strcmp(s->items[idx - 1].pair, key) == 0) {
            /* mark removed by emptying the string; membership checks
             * compare full keys so "" never matches a real pair */
            free(s->items[idx - 1].pair);
            s->items[idx - 1].pair = odk_strdup("");
            free(key);
            return;
        }
        slot = (slot + 1) & (s->bucket_count - 1);
    }
    free(key);
}

/* ------------------------------------------------------------------ */
/* Co-occurrence pair set (token-id pairs, bounded)                    */
/* ------------------------------------------------------------------ */

typedef struct {
    uint64_t *keys;  /* (a << 32) | b with a < b                       */
    size_t count;
    size_t cap;
    size_t *buckets;
    size_t bucket_count;
    int full; /* fail-safe: new pairs are assumed co-occurring */
} cooc_set;

static void cooc_init(cooc_set *s)
{
    s->keys = NULL;
    s->count = 0;
    s->cap = 0;
    s->buckets = NULL;
    s->bucket_count = 0;
    s->full = 0;
}

static void cooc_free(cooc_set *s)
{
    free(s->keys);
    free(s->buckets);
    cooc_init(s);
}

static uint64_t cooc_key(size_t a, size_t b)
{
    size_t lo = a < b ? a : b;
    size_t hi = a < b ? b : a;
    return ((uint64_t)lo << 32) | (uint64_t)hi;
}

static int cooc_rehash(cooc_set *s, size_t new_count)
{
    size_t *grown = calloc(new_count, sizeof(size_t));
    size_t i;
    if (grown == NULL) {
        return -1;
    }
    free(s->buckets);
    s->buckets = grown;
    s->bucket_count = new_count;
    for (i = 0; i < s->count; i++) {
        uint64_t k = s->keys[i];
        size_t slot = (size_t)(k ^ (k >> 32)) & (new_count - 1);
        while (s->buckets[slot] != 0) {
            slot = (slot + 1) & (new_count - 1);
        }
        s->buckets[slot] = i + 1;
    }
    return 0;
}

static int cooc_has(const cooc_set *s, size_t a, size_t b)
{
    uint64_t k;
    size_t slot;
    if (s->bucket_count == 0) {
        return 0;
    }
    k = cooc_key(a, b);
    slot = (size_t)(k ^ (k >> 32)) & (s->bucket_count - 1);
    for (;;) {
        size_t idx = s->buckets[slot];
        if (idx == 0) {
            return 0;
        }
        if (s->keys[idx - 1] == k) {
            return 1;
        }
        slot = (slot + 1) & (s->bucket_count - 1);
    }
}

static int cooc_add(cooc_set *s, size_t a, size_t b)
{
    uint64_t k;
    size_t slot;
    if (a == b) {
        return 0;
    }
    if (s->full) {
        return 0; /* fail-safe: treat as co-occurring */
    }
    if (s->count >= LINK_COOC_MAX) {
        s->full = 1; /* deterministic saturation */
        return 0;
    }
    if (s->bucket_count == 0) {
        if (cooc_rehash(s, 256) != 0) {
            return -1;
        }
    } else if ((s->count + 1) * 10 >= s->bucket_count * 7) {
        if (cooc_rehash(s, s->bucket_count * 2) != 0) {
            return -1;
        }
    }
    k = cooc_key(a, b);
    slot = (size_t)(k ^ (k >> 32)) & (s->bucket_count - 1);
    for (;;) {
        size_t idx = s->buckets[slot];
        if (idx == 0) {
            break;
        }
        if (s->keys[idx - 1] == k) {
            return 1; /* already present */
        }
        slot = (slot + 1) & (s->bucket_count - 1);
    }
    if (s->count == s->cap) {
        size_t grown_cap = s->cap ? s->cap * 2 : 256;
        uint64_t *grown = realloc(s->keys, grown_cap * sizeof(uint64_t));
        if (grown == NULL) {
            return -1;
        }
        s->keys = grown;
        s->cap = grown_cap;
    }
    s->keys[s->count] = k;
    s->buckets[slot] = s->count + 1;
    s->count++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* The store                                                            */
/* ------------------------------------------------------------------ */

struct odk_link_store {
    odk_link_policy policy;
    uint32_t *token_mentions; /* exact per-token record count            */
    intern_table tokens;
    intern_table options;
    assoc_list *token_assocs; /* per token id                          */
    uint32_t *token_contexts; /* distinct contexts per token            */
    uint64_t (*token_fps)[LINK_FPS_MAX]; /* context fingerprints        */
    int *token_fps_saturated;
    size_t token_slots;       /* allocated slots for the above arrays   */
    cooc_set cooc;
    link_entry *links;
    size_t link_count;
    size_t link_cap;
    /* Adjacency index over the canonical link array:
     * adj_items concatenates the link indices touching each token id;
     * adj_offsets[id]..adj_offsets[id+1] is id's slice.  Rebuilt at
     * maintain time (the only place links change); NULL when absent. */
    size_t *adj_items;
    size_t *adj_offsets;
    pair_set pinned;
    pair_set banned;
    size_t total_contexts;
    size_t epoch; /* supplied by the brain at maintain time */
};

/* Build the adjacency index over store->links.  0 ok, -1 OOM
 * (partners then falls back to the linear scan - correctness is never
 * affected, only speed). */
static int links_build_adjacency(odk_link_store *store)
{
    size_t token_count = store->tokens.count;
    size_t *offsets = NULL;
    size_t *items = NULL;
    size_t *cursor = NULL;
    size_t i;

    free(store->adj_items);
    free(store->adj_offsets);
    store->adj_items = NULL;
    store->adj_offsets = NULL;
    if (store->link_count == 0) {
        return 0;
    }
    offsets = calloc(token_count + 1, sizeof(size_t));
    if (offsets == NULL) {
        return -1;
    }
    for (i = 0; i < store->link_count; i++) {
        offsets[store->links[i].a + 1]++;
        offsets[store->links[i].b + 1]++;
    }
    for (i = 0; i < token_count; i++) {
        offsets[i + 1] += offsets[i];
    }
    items = malloc(2 * store->link_count * sizeof(size_t));
    cursor = malloc((token_count + 1) * sizeof(size_t));
    if (items == NULL || cursor == NULL) {
        free(offsets);
        free(items);
        free(cursor);
        return -1;
    }
    memcpy(cursor, offsets, (token_count + 1) * sizeof(size_t));
    for (i = 0; i < store->link_count; i++) {
        items[cursor[store->links[i].a]++] = i;
        items[cursor[store->links[i].b]++] = i;
    }
    free(cursor);
    store->adj_items = items;
    store->adj_offsets = offsets;
    return 0;
}

odk_link_store *odk_links_create(void)
{
    odk_link_store *store = calloc(1, sizeof(*store));
    if (store == NULL) {
        return NULL;
    }
    odk_link_policy_defaults(&store->policy);
    intern_init(&store->tokens);
    intern_init(&store->options);
    cooc_init(&store->cooc);
    pair_set_init(&store->pinned);
    pair_set_init(&store->banned);
    return store;
}

void odk_links_free(odk_link_store *store)
{
    size_t i;
    if (store == NULL) {
        return;
    }
    intern_free(&store->tokens);
    intern_free(&store->options);
    for (i = 0; i < store->token_slots; i++) {
        if (store->token_assocs != NULL) {
            free(store->token_assocs[i].rows);
        }
    }
    free(store->token_assocs);
    free(store->token_contexts);
    free(store->token_fps);
    free(store->token_fps_saturated);
    free(store->token_mentions);
    cooc_free(&store->cooc);
    free(store->links);
    free(store->adj_items);
    free(store->adj_offsets);
    pair_set_free(&store->pinned);
    pair_set_free(&store->banned);
    free(store);
}

void odk_link_policy_defaults(odk_link_policy *policy)
{
    if (policy == NULL) {
        return;
    }
    memset(policy, 0, sizeof(*policy));
    policy->link_weight = 0.0;
    policy->min_support = 4;
    policy->min_contexts = 3;
    policy->active_threshold = 0.6;
    policy->link_cap = 0.5;
    policy->link_gain = 0.5;
    policy->generic_fraction = 0.5;
    policy->max_links = 16;
    policy->max_tokens = 10000;
    policy->max_total_links = 50000;
    policy->decay_half_life = 0;
}

int odk_links_set_policy(odk_link_store *store, const odk_link_policy *policy)
{
    if (store == NULL) {
        return -1;
    }
    if (policy == NULL) {
        odk_link_policy_defaults(&store->policy);
        return 0;
    }
    store->policy = *policy;
    /* hostile-value clamps */
    if (store->policy.link_weight < 0.0) {
        store->policy.link_weight = 0.0;
    }
    if (store->policy.link_weight > 1.0) {
        store->policy.link_weight = 1.0;
    }
    if (store->policy.link_cap <= 0.0 || store->policy.link_cap > 1.0) {
        store->policy.link_cap = 0.5;
    }
    if (store->policy.link_gain <= 0.0 || store->policy.link_gain > 0.5) {
        store->policy.link_gain = 0.5;
    }
    if (store->policy.active_threshold <= 0.0 ||
        store->policy.active_threshold > 1.0) {
        store->policy.active_threshold = 0.6;
    }
    if (store->policy.min_support == 0) {
        store->policy.min_support = 1;
    }
    if (store->policy.min_contexts == 0) {
        store->policy.min_contexts = 1;
    }
    if (store->policy.max_links == 0) {
        store->policy.max_links = 16;
    }
    if (store->policy.max_tokens == 0) {
        store->policy.max_tokens = 10000;
    }
    if (store->policy.max_total_links == 0) {
        store->policy.max_total_links = 50000;
    }
    if (store->policy.generic_fraction <= 0.0 ||
        store->policy.generic_fraction > 1.0) {
        store->policy.generic_fraction = 0.5;
    }
    return 0;
}

int odk_links_get_policy(const odk_link_store *store, odk_link_policy *out)
{
    if (store == NULL || out == NULL) {
        return -1;
    }
    *out = store->policy;
    return 0;
}

int odk_links_has_active(const odk_link_store *store)
{
    size_t i;
    if (store == NULL) {
        return 0;
    }
    for (i = 0; i < store->link_count; i++) {
        if (store->links[i].status == ODK_LINK_ACTIVE ||
            store->links[i].status == ODK_LINK_PINNED) {
            return 1;
        }
    }
    return 0;
}

void odk_links_params(const odk_link_store *store, double *out_weight,
                      double *out_cap, double *out_gain,
                      double *out_threshold)
{
    if (out_weight != NULL) {
        *out_weight = store != NULL ? store->policy.link_weight : 0.0;
    }
    if (out_cap != NULL) {
        *out_cap = store != NULL ? store->policy.link_cap : 0.5;
    }
    if (out_gain != NULL) {
        *out_gain = store != NULL ? store->policy.link_gain : 0.5;
    }
    if (out_threshold != NULL) {
        *out_threshold = store != NULL ? store->policy.active_threshold : 0.6;
    }
}

/* ------------------------------------------------------------------ */
/* Recording                                                            */
/* ------------------------------------------------------------------ */

static int ensure_token_slot(odk_link_store *store, size_t id)
{
    if (id < store->token_slots) {
        return 0;
    }
    {
        size_t grown = store->token_slots ? store->token_slots * 2 : 64;
        assoc_list *assocs;
        uint32_t *contexts;
        uint64_t (*fps)[LINK_FPS_MAX];
        int *saturated;
        while (grown <= id) {
            grown *= 2;
        }
        assocs = realloc(store->token_assocs, grown * sizeof(assoc_list));
        if (assocs == NULL) {
            return -1;
        }
        store->token_assocs = assocs;
        contexts =
            realloc(store->token_contexts, grown * sizeof(uint32_t));
        if (contexts == NULL) {
            return -1;
        }
        store->token_contexts = contexts;
        fps = realloc(store->token_fps,
                      grown * sizeof(uint64_t[LINK_FPS_MAX]));
        if (fps == NULL) {
            return -1;
        }
        store->token_fps = fps;
        saturated = realloc(store->token_fps_saturated, grown * sizeof(int));
        if (saturated == NULL) {
            return -1;
        }
        store->token_fps_saturated = saturated;
        {
            uint32_t *mentions =
                realloc(store->token_mentions, grown * sizeof(uint32_t));
            if (mentions == NULL) {
                return -1;
            }
            store->token_mentions = mentions;
        }
        {
            size_t i;
            for (i = store->token_slots; i < grown; i++) {
                store->token_assocs[i].rows = NULL;
                store->token_assocs[i].count = 0;
                store->token_assocs[i].cap = 0;
                store->token_contexts[i] = 0;
                memset(store->token_fps[i], 0,
                       sizeof(uint64_t) * LINK_FPS_MAX);
                store->token_fps_saturated[i] = 0;
                store->token_mentions[i] = 0;
            }
        }
        store->token_slots = grown;
    }
    return 0;
}

static assoc_row *find_assoc(assoc_list *list, size_t option_id)
{
    size_t i;
    for (i = 0; i < list->count; i++) {
        if (list->rows[i].option_id == option_id) {
            return &list->rows[i];
        }
    }
    return NULL;
}

static assoc_row *touch_assoc(assoc_list *list, size_t option_id)
{
    assoc_row *row = find_assoc(list, option_id);
    if (row != NULL) {
        return row;
    }
    if (list->count == list->cap) {
        size_t grown_cap = list->cap ? list->cap * 2 : 4;
        assoc_row *grown = realloc(list->rows, grown_cap * sizeof(assoc_row));
        if (grown == NULL) {
            return NULL;
        }
        list->rows = grown;
        list->cap = grown_cap;
    }
    row = &list->rows[list->count++];
    row->option_id = option_id;
    row->pos = 0;
    row->neg = 0;
    return row;
}

/* fingerprint of the sorted distinct token list */
static uint64_t context_fingerprint(char **sorted, size_t n)
{
    uint64_t h = 1469598103934665603ULL;
    size_t i;
    for (i = 0; i < n; i++) {
        size_t j;
        for (j = 0; sorted[i][j] != '\0'; j++) {
            h ^= (unsigned char)sorted[i][j];
            h *= 1099511628211ULL;
        }
        h ^= 0x1f;
        h *= 1099511628211ULL;
    }
    return h;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int odk_links_record(odk_link_store *store, const char *option_key,
                     const char *const *tokens, size_t token_count,
                     double reward)
{
    char **distinct = NULL;
    size_t distinct_count = 0;
    size_t *ids = NULL;
    uint64_t fp;
    size_t option_id;
    size_t i;
    int is_new;

    if (store == NULL || option_key == NULL || tokens == NULL) {
        return -1;
    }
    if (reward > 1.0) {
        reward = 1.0;
    }
    if (reward < -1.0) {
        reward = -1.0;
    }
    if (reward != reward) {
        reward = 0.0;
    }

    /* deduplicate tokens preserving first-occurrence order */
    distinct = malloc((token_count ? token_count : 1) * sizeof(char *));
    if (distinct == NULL) {
        return -1;
    }
    for (i = 0; i < token_count; i++) {
        size_t j;
        int seen = 0;
        if (tokens[i] == NULL || tokens[i][0] == '\0') {
            continue;
        }
        for (j = 0; j < distinct_count; j++) {
            if (strcmp(distinct[j], tokens[i]) == 0) {
                seen = 1;
                break;
            }
        }
        if (!seen) {
            distinct[distinct_count++] = (char *)tokens[i];
        }
    }
    if (distinct_count == 0) {
        free(distinct);
        return 0; /* nothing to learn from an empty context */
    }

    /* intern the option */
    option_id = intern_intern(&store->options, option_key, &is_new);
    if (option_id == (size_t)-1) {
        free(distinct);
        return -1;
    }

    /* fingerprint of the sorted distinct list (order-insensitive) */
    {
        char **sorted = malloc(distinct_count * sizeof(char *));
        if (sorted == NULL) {
            free(distinct);
            return -1;
        }
        memcpy(sorted, distinct, distinct_count * sizeof(char *));
        qsort(sorted, distinct_count, sizeof(char *), cmp_str);
        fp = context_fingerprint(sorted, distinct_count);
        free(sorted);
    }

    store->total_contexts++;

    /* intern tokens, update contexts + associations */
    ids = malloc(distinct_count * sizeof(size_t));
    if (ids == NULL) {
        free(distinct);
        return -1;
    }
    for (i = 0; i < distinct_count; i++) {
        size_t id = intern_intern(&store->tokens, distinct[i], &is_new);
        if (id == (size_t)-1) {
            free(distinct);
            free(ids);
            return -1;
        }
        ids[i] = id;
        if (ensure_token_slot(store, id) != 0) {
            free(distinct);
            free(ids);
            return -1;
        }
        if (store->token_mentions[id] < 0xFFFFFFFFu) {
            store->token_mentions[id]++; /* exact count (generic gate) */
        }
        /* distinct-context tracking (bounded fingerprint set) */
        {
            int seen_fp = 0;
            size_t k;
            for (k = 0; k < LINK_FPS_MAX; k++) {
                if (store->token_fps[id][k] == fp) {
                    seen_fp = 1;
                    break;
                }
            }
            if (!seen_fp && !store->token_fps_saturated[id]) {
                int placed = 0;
                for (k = 0; k < LINK_FPS_MAX; k++) {
                    if (store->token_fps[id][k] == 0) {
                        store->token_fps[id][k] = fp;
                        placed = 1;
                        break;
                    }
                }
                if (!placed) {
                    /* more than LINK_FPS_MAX distinct contexts: mark
                     * saturated (the count stays at "many" - always
                     * above any realistic min_contexts gate) */
                    store->token_fps_saturated[id] = 1;
                    store->token_contexts[id] = LINK_FPS_MAX + 1;
                } else {
                    store->token_contexts[id]++;
                }
            }
        }
        /* association counters (order-independent integers) */
        {
            assoc_row *row = touch_assoc(&store->token_assocs[id], option_id);
            if (row == NULL) {
                free(distinct);
                free(ids);
                return -1;
            }
            if (reward >= 0.05) {
                if (row->pos < 0xFFFFFFFFu) {
                    row->pos++;
                }
            } else if (reward <= -0.05) {
                if (row->neg < 0xFFFFFFFFu) {
                    row->neg++;
                }
            }
        }
    }

    /* co-occurrence pairs among the first 32 distinct tokens
     * (first-occurrence order) */
    {
        size_t limit = distinct_count < LINK_COOC_TOKENS
                           ? distinct_count
                           : LINK_COOC_TOKENS;
        for (i = 0; i < limit; i++) {
            size_t j;
            for (j = i + 1; j < limit; j++) {
                if (cooc_add(&store->cooc, ids[i], ids[j]) != 0) {
                    /* OOM: stop recording co-occurrence (fail-safe) */
                    i = limit;
                    break;
                }
            }
        }
    }

    free(distinct);
    free(ids);
    return 0;
}

/* Direct state restoration for the snapshot loader: sets the
 * exact counters without replaying records, so save/load is lossless. */
int odk_links_restore_row(odk_link_store *store, const char *token,
                          unsigned contexts, unsigned mentions,
                          const char *option_key, unsigned pos, unsigned neg)
{
    int is_new;
    size_t id;
    size_t option_id;
    assoc_row *row;
    if (store == NULL || token == NULL || token[0] == '\0' ||
        option_key == NULL) {
        return -1;
    }
    id = intern_intern(&store->tokens, token, &is_new);
    if (id == (size_t)-1 || ensure_token_slot(store, id) != 0) {
        return -1;
    }
    option_id = intern_intern(&store->options, option_key, &is_new);
    if (option_id == (size_t)-1) {
        return -1;
    }
    if (store->token_contexts[id] < contexts) {
        store->token_contexts[id] = contexts;
    }
    if (store->token_mentions[id] < mentions) {
        store->token_mentions[id] = mentions;
    }
    row = touch_assoc(&store->token_assocs[id], option_id);
    if (row == NULL) {
        return -1;
    }
    if (row->pos < pos) {
        row->pos = pos;
    }
    if (row->neg < neg) {
        row->neg = neg;
    }
    return 0;
}

void odk_links_set_total_contexts(odk_link_store *store, size_t total)
{
    if (store != NULL) {
        store->total_contexts = total;
    }
}

void odk_links_set_epoch(odk_link_store *store, size_t epoch)
{
    if (store != NULL) {
        store->epoch = epoch;
    }
}

/* ------------------------------------------------------------------ */
/* Link strength + maintenance                                          */
/* ------------------------------------------------------------------ */

static double token_total_pos(const odk_link_store *store, size_t id,
                              double *out_decayed_total)
{
    double total = 0.0;
    size_t i;
    (void)out_decayed_total;
    for (i = 0; i < store->token_assocs[id].count; i++) {
        total += (double)store->token_assocs[id].rows[i].pos;
    }
    return total;
}

/* cosine of the positive-support vectors + damping.  Fixed
 * iteration order: ascending option-key byte order over the union. */
static double link_strength(const odk_link_store *store, size_t a,
                            size_t b, int *out_conflict, size_t *out_support_a,
                            size_t *out_support_b)
{
    /* gather the union of option keys with pos > 0 for either token,
     * in ascending byte order */
    const assoc_list *la = &store->token_assocs[a];
    const assoc_list *lb = &store->token_assocs[b];
    size_t n = la->count + lb->count;
    size_t *opt_ids = malloc((n ? n : 1) * sizeof(size_t));
    size_t opt_count = 0;
    double dot = 0.0;
    double na = 0.0;
    double nb = 0.0;
    double support_a = 0.0;
    double support_b = 0.0;
    double damp_a;
    double damp_b;
    double cosine;
    size_t i;
    size_t j;

    *out_conflict = 0;
    if (opt_ids == NULL) {
        return 0.0;
    }
    for (i = 0; i < la->count; i++) {
        opt_ids[opt_count++] = la->rows[i].option_id;
    }
    for (i = 0; i < lb->count; i++) {
        int seen = 0;
        for (j = 0; j < opt_count; j++) {
            if (opt_ids[j] == lb->rows[i].option_id) {
                seen = 1;
                break;
            }
        }
        if (!seen) {
            opt_ids[opt_count++] = lb->rows[i].option_id;
        }
    }
    /* sort option ids by key byte order */
    for (i = 1; i < opt_count; i++) {
        size_t key = opt_ids[i];
        j = i;
        while (j > 0 &&
               strcmp(intern_get(&store->options, opt_ids[j - 1]),
                      intern_get(&store->options, key)) > 0) {
            opt_ids[j] = opt_ids[j - 1];
            j--;
        }
        opt_ids[j] = key;
    }

    for (i = 0; i < opt_count; i++) {
        const assoc_row *ra = find_assoc((assoc_list *)la, opt_ids[i]);
        const assoc_row *rb = find_assoc((assoc_list *)lb, opt_ids[i]);
        double ppos_a = ra != NULL ? (double)ra->pos : 0.0;
        double ppos_b = rb != NULL ? (double)rb->pos : 0.0;
        dot += ppos_a * ppos_b;
        na += ppos_a * ppos_a;
        nb += ppos_b * ppos_b;
        /* conflict: opposite net predictions for the same option */
        if (ra != NULL && rb != NULL) {
            double net_a = (double)ra->pos - (double)ra->neg;
            double net_b = (double)rb->pos - (double)rb->neg;
            if (net_a * net_b < 0.0 &&
                (fabs(net_a) >= 1.0 && fabs(net_b) >= 1.0)) {
                *out_conflict = 1;
            }
        }
    }
    support_a = token_total_pos(store, a, NULL);
    support_b = token_total_pos(store, b, NULL);
    *out_support_a = (size_t)(support_a + 0.5);
    *out_support_b = (size_t)(support_b + 0.5);
    free(opt_ids);
    if (na <= 0.0 || nb <= 0.0) {
        return 0.0;
    }
    cosine = dot / (sqrt(na) * sqrt(nb));
    if (cosine > 1.0) {
        cosine = 1.0;
    }
    damp_a = support_a / (double)store->policy.min_support;
    if (damp_a > 1.0) {
        damp_a = 1.0;
    }
    damp_b = support_b / (double)store->policy.min_support;
    if (damp_b > 1.0) {
        damp_b = 1.0;
    }
    return cosine * damp_a * damp_b;
}

int odk_links_maintain(odk_link_store *store)
{
    size_t i;
    size_t j;
    link_entry *fresh = NULL;
    size_t fresh_count = 0;
    size_t fresh_cap = 0;
    size_t budget = LINK_MAINTAIN_BUDGET;

    if (store == NULL) {
        return -1;
    }
    if (store->total_contexts == 0) {
        /* With no recorded
         * contexts there are no LEARNED candidates, but pinned pairs
         * are curated lexicon entries and still materialize - the
         * earlier code cleared the whole table here, so a brain that only
         * pinned pairs (zero training) silently lost them. */
        free(store->links);
        store->links = NULL;
        store->link_count = 0;
        store->link_cap = 0;
        free(store->adj_items);
        free(store->adj_offsets);
        store->adj_items = NULL;
        store->adj_offsets = NULL;
        goto pinned_only;
    }

    /* candidate pairs: tokens sharing an option with positive support on
     * both sides.  Enumerate per option (byte order over options is not
     * required for the RESULT because strengths are pure per-pair
     * functions; ordering only affects pruning ties, which are resolved
     * by explicit tie-breaks below). */
    {
        size_t opt_i;
        for (opt_i = 0; opt_i < store->options.count && budget > 0; opt_i++) {
        /* tokens with pos > 0 for option opt_i, byte order */
        size_t *toks = NULL;
        size_t tok_count = 0;
        for (j = 0; j < store->tokens.count; j++) {
            const assoc_row *row;
            if (j >= store->token_slots) {
                break;
            }
            row = find_assoc(&store->token_assocs[j], opt_i);
            if (row != NULL && row->pos > 0) {
                size_t *grown = realloc(toks, (tok_count + 1) * sizeof(size_t));
                if (grown == NULL) {
                    free(toks);
                    return -1;
                }
                toks = grown;
                toks[tok_count++] = j;
            }
        }
        /* toks are collected in ascending id == insertion order; sort by
         * token byte order for deterministic pair enumeration */
        {
            size_t a;
            for (a = 1; a < tok_count; a++) {
                size_t key = toks[a];
                size_t b = a;
                while (b > 0 && strcmp(intern_get(&store->tokens, toks[b - 1]),
                                       intern_get(&store->tokens, key)) > 0) {
                    toks[b] = toks[b - 1];
                    b--;
                }
                toks[b] = key;
            }
        }
        {
            size_t pa_i;
            size_t pb_i;
            for (pa_i = 0; pa_i + 1 < tok_count && budget > 0; pa_i++) {
            for (pb_i = pa_i + 1; pb_i < tok_count && budget > 0; pb_i++) {
                size_t ta = toks[pa_i];
                size_t tb = toks[pb_i];
                const char *sa = intern_get(&store->tokens, ta);
                const char *sb = intern_get(&store->tokens, tb);
                int conflict = 0;
                size_t support_a = 0;
                size_t support_b = 0;
                double strength;
                unsigned char status;
                if (cooc_has(&store->cooc, ta, tb)) {
                    budget--;
                    continue; /* co-occurring tokens never link */
                }
                if (pair_set_has(&store->banned, sa, sb)) {
                    budget--;
                    continue; /* banned pairs never reform */
                }
                strength = link_strength(store, ta, tb, &conflict,
                                         &support_a, &support_b);
                budget--;
                if (strength <= 0.0 || conflict) {
                    continue;
                }
                if (support_a < store->policy.min_support ||
                    support_b < store->policy.min_support) {
                    continue;
                }
                if (store->token_contexts[ta] < store->policy.min_contexts ||
                    store->token_contexts[tb] < store->policy.min_contexts) {
                    continue;
                }
                /* generic-word gate: exact mention counts (the
                 * distinct-context count saturates at 9 and cannot back
                 * this gate - a documented deviation) */
                if ((double)store->token_mentions[ta] >
                        store->policy.generic_fraction *
                            (double)store->total_contexts ||
                    (double)store->token_mentions[tb] >
                        store->policy.generic_fraction *
                            (double)store->total_contexts) {
                    continue;
                }
                status = strength >= store->policy.active_threshold
                             ? ODK_LINK_ACTIVE
                             : ODK_LINK_CANDIDATE;
                if (pair_set_has(&store->pinned, sa, sb)) {
                    status = ODK_LINK_PINNED;
                }
                /* append to fresh (dedup by pair via linear scan is fine:
                 * the same pair can appear via multiple options) */
                {
                    int dup = 0;
                    size_t k;
                    for (k = 0; k < fresh_count; k++) {
                        if ((fresh[k].a == ta && fresh[k].b == tb) ||
                            (fresh[k].a == tb && fresh[k].b == ta)) {
                            dup = 1;
                            break;
                        }
                    }
                    if (!dup) {
                        if (fresh_count == fresh_cap) {
                            size_t grown_cap =
                                fresh_cap ? fresh_cap * 2 : 64;
                            link_entry *grown = realloc(
                                fresh, grown_cap * sizeof(link_entry));
                            if (grown == NULL) {
                                free(toks);
                                free(fresh);
                                return -1;
                            }
                            fresh = grown;
                            fresh_cap = grown_cap;
                        }
                        {
                            const char *str_a =
                                intern_get(&store->tokens, ta);
                            const char *str_b =
                                intern_get(&store->tokens, tb);
                            int a_first = strcmp(str_a, str_b) <= 0;
                            fresh[fresh_count].a = a_first ? ta : tb;
                            fresh[fresh_count].b = a_first ? tb : ta;
                        }
                        fresh[fresh_count].strength = strength;
                        fresh[fresh_count].status = status;
                        fresh[fresh_count].created_epoch = store->epoch;
                        fresh_count++;
                    }
                }
                }
            }
        }
        free(toks);
        }
    }

    /* pinned pairs are curated lexicon entries: they join the link
     * table as PINNED (always active, exempt from pruning) even when
     * the statistics alone would not produce them. */
pinned_only:
    {
        size_t p;
        for (p = 0; p < store->pinned.count; p++) {
            const char *pair = store->pinned.items[p].pair;
            const char *sep;
            if (pair == NULL || pair[0] == '\0') {
                continue; /* removed slot */
            }
            sep = strchr(pair, '\x1f');
            if (sep == NULL) {
                continue;
            }
            {
                char *a = odk_strdup_range(pair, (size_t)(sep - pair));
                char *b = odk_strdup(sep + 1);
                if (a == NULL || b == NULL) {
                    free(a);
                    free(b);
                    free(fresh);
                    return -1;
                }
                {
                    int is_new_a;
                    int is_new_b;
                    size_t ia = intern_intern(&store->tokens, a, &is_new_a);
                    size_t ib = intern_intern(&store->tokens, b, &is_new_b);
                    if (ia == (size_t)-1 || ib == (size_t)-1 ||
                        ensure_token_slot(store, ia) != 0 ||
                        ensure_token_slot(store, ib) != 0) {
                        free(a);
                        free(b);
                        free(fresh);
                        return -1;
                    }
                    /* dedup against the freshly computed links */
                    {
                        int dup = 0;
                        size_t k;
                        for (k = 0; k < fresh_count; k++) {
                            if ((fresh[k].a == ia && fresh[k].b == ib) ||
                                (fresh[k].a == ib && fresh[k].b == ia)) {
                                fresh[k].status = ODK_LINK_PINNED;
                                dup = 1;
                                break;
                            }
                        }
                        if (!dup) {
                            if (fresh_count == fresh_cap) {
                                size_t grown_cap =
                                    fresh_cap ? fresh_cap * 2 : 64;
                                link_entry *grown = realloc(
                                    fresh, grown_cap * sizeof(link_entry));
                                if (grown == NULL) {
                                    free(a);
                                    free(b);
                                    free(fresh);
                                    return -1;
                                }
                                fresh = grown;
                                fresh_cap = grown_cap;
                            }
                            {
                                int a_first =
                                    strcmp(pair, sep) <= 0; /* pair is
                                    already "lo\x1fhi" - a is lo */
                                (void)a_first;
                                fresh[fresh_count].a =
                                    strcmp(intern_get(&store->tokens, ia),
                                           intern_get(&store->tokens, ib)) <= 0
                                        ? ia
                                        : ib;
                                fresh[fresh_count].b =
                                    strcmp(intern_get(&store->tokens, ia),
                                           intern_get(&store->tokens, ib)) <= 0
                                        ? ib
                                        : ia;
                            }
                            fresh[fresh_count].strength = 1.0;
                            fresh[fresh_count].status = ODK_LINK_PINNED;
                            fresh[fresh_count].created_epoch = store->epoch;
                            fresh_count++;
                        }
                    }
                }
                free(a);
                free(b);
            }
        }
    }

    /* pruning: total cap (strength desc, then epoch, then byte order);
     * the per-token cap is enforced at lookup time (partners are cut at
     * max_links in byte order - documented). */
    if (fresh_count > store->policy.max_total_links) {
        /* simple deterministic selection: keep the strongest links; ties
         * broken by created_epoch then token byte order */
        typedef struct {
            double strength;
            size_t epoch;
            const char *key_a;
            const char *key_b;
            size_t index;
        } sort_key;
        sort_key *keys = malloc(fresh_count * sizeof(sort_key));
        size_t keep = store->policy.max_total_links;
        if (keys == NULL) {
            free(fresh);
            return -1;
        }
        for (i = 0; i < fresh_count; i++) {
            keys[i].strength = fresh[i].strength;
            keys[i].epoch = fresh[i].created_epoch;
            keys[i].key_a = intern_get(&store->tokens, fresh[i].a);
            keys[i].key_b = intern_get(&store->tokens, fresh[i].b);
            keys[i].index = i;
        }
        {
            /* insertion sort by (strength desc, epoch desc, a, b) */
            for (i = 1; i < fresh_count; i++) {
                sort_key key = keys[i];
                j = i;
                while (j > 0) {
                    sort_key prev = keys[j - 1];
                    int swap = 0;
                    if (prev.strength < key.strength) {
                        swap = 1;
                    } else if (prev.strength == key.strength) {
                        if (prev.epoch < key.epoch) {
                            swap = 1;
                        } else if (prev.epoch == key.epoch) {
                            int cmp = strcmp(prev.key_a, key.key_a);
                            if (cmp > 0 || (cmp == 0 &&
                                            strcmp(prev.key_b, key.key_b) > 0)) {
                                swap = 1;
                            }
                        }
                    }
                    if (!swap) {
                        break;
                    }
                    keys[j] = keys[j - 1];
                    j--;
                }
                keys[j] = key;
            }
        }
        /* mark pinned links as always kept: move them to the front by
         * simple two-pass compaction */
        {
            link_entry *kept = malloc(keep * sizeof(link_entry));
            size_t kept_count = 0;
            if (kept == NULL) {
                free(keys);
                free(fresh);
                return -1;
            }
            /* pinned first */
            for (i = 0; i < fresh_count && kept_count < keep; i++) {
                if (fresh[keys[i].index].status == ODK_LINK_PINNED) {
                    kept[kept_count++] = fresh[keys[i].index];
                }
            }
            for (i = 0; i < fresh_count && kept_count < keep; i++) {
                if (fresh[keys[i].index].status != ODK_LINK_PINNED) {
                    kept[kept_count++] = fresh[keys[i].index];
                }
            }
            free(keys);
            free(fresh);
            fresh = kept;
            fresh_count = kept_count;
            fresh_cap = keep;
        }
    }

    /* canonical order: sort the link array by token byte order so the
     * report, the snapshot and the partner enumeration are independent
     * of the internal interning order (order-independence). */
    {
        size_t a;
        for (a = 1; a < fresh_count; a++) {
            link_entry key = fresh[a];
            size_t b = a;
            while (b > 0) {
                const char *ka = intern_get(&store->tokens, fresh[b - 1].a);
                const char *kb = intern_get(&store->tokens, fresh[b - 1].b);
                const char *la = intern_get(&store->tokens, key.a);
                const char *lb = intern_get(&store->tokens, key.b);
                int cmp = strcmp(ka, la);
                int swap = 0;
                if (cmp > 0 || (cmp == 0 && strcmp(kb, lb) > 0)) {
                    swap = 1;
                }
                if (!swap) {
                    break;
                }
                fresh[b] = fresh[b - 1];
                b--;
            }
            fresh[b] = key;
        }
    }

    free(store->links);
    store->links = fresh;
    store->link_count = fresh_count;
    store->link_cap = fresh_cap;
    /* Refresh the adjacency index for the new canonical
     * array; on OOM partners() falls back to the linear scan. */
    (void)links_build_adjacency(store);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Partner lookup for scoring                                           */
/* ------------------------------------------------------------------ */

size_t odk_links_partners(const odk_link_store *store, const char *x,
                          const char **out_tokens, double *out_strengths,
                          size_t max_out)
{
    size_t id;
    size_t i;
    size_t written = 0;
    if (store == NULL || x == NULL || max_out == 0) {
        return 0;
    }
    /* find the token id (hash lookup) */
    {
        uint64_t h = links_hash(x, strlen(x));
        size_t slot;
        if (store->tokens.bucket_count == 0) {
            return 0;
        }
        slot = (size_t)h & (store->tokens.bucket_count - 1);
        for (;;) {
            size_t idx = store->tokens.buckets[slot];
            if (idx == 0) {
                return 0;
            }
            if (strcmp(store->tokens.entries[idx - 1].key, x) == 0) {
                id = idx - 1;
                break;
            }
            slot = (slot + 1) & (store->tokens.bucket_count - 1);
        }
    }
    /* links are stored with a < b by TOKEN ID; collect those touching
     * id (bounded by max_links in byte order - links are generated in
     * per-option loops, so we sort the candidates before cutting).
     * the adjacency index walks only this token's links
     * (O(links-of-id)); the full scan is the fallback when the index
     * could not be built. */
    {
        const char **cand_tokens = NULL;
        double *cand_strengths = NULL;
        size_t cand_count = 0;
        cand_tokens = malloc((store->policy.max_links
                                  ? store->policy.max_links
                                  : 1) *
                             sizeof(char *));
        cand_strengths = malloc((store->policy.max_links
                                     ? store->policy.max_links
                                     : 1) *
                                sizeof(double));
        if (cand_tokens == NULL || cand_strengths == NULL) {
            free(cand_tokens);
            free(cand_strengths);
            return 0;
        }
        if (store->adj_items != NULL && store->adj_offsets != NULL &&
            id < store->tokens.count) {
            size_t begin = store->adj_offsets[id];
            size_t end = store->adj_offsets[id + 1];
            size_t k;
            for (k = begin; k < end; k++) {
                const link_entry *l = &store->links[store->adj_items[k]];
                size_t other = l->a == id ? l->b : l->a;
                if (l->status != ODK_LINK_ACTIVE &&
                    l->status != ODK_LINK_PINNED) {
                    continue;
                }
                if (l->strength < store->policy.active_threshold &&
                    l->status != ODK_LINK_PINNED) {
                    continue;
                }
                if (cand_count < store->policy.max_links) {
                    cand_tokens[cand_count] =
                        intern_get(&store->tokens, other);
                    cand_strengths[cand_count] = l->strength;
                    cand_count++;
                }
            }
        } else {
            for (i = 0; i < store->link_count; i++) {
                const link_entry *l = &store->links[i];
                size_t other;
                if (l->a == id) {
                    other = l->b;
                } else if (l->b == id) {
                    other = l->a;
                } else {
                    continue;
                }
                if (l->status != ODK_LINK_ACTIVE &&
                    l->status != ODK_LINK_PINNED) {
                    continue;
                }
                if (l->strength < store->policy.active_threshold &&
                    l->status != ODK_LINK_PINNED) {
                    continue;
                }
                if (cand_count < store->policy.max_links) {
                    cand_tokens[cand_count] =
                        intern_get(&store->tokens, other);
                    cand_strengths[cand_count] = l->strength;
                    cand_count++;
                }
            }
        }
        /* byte-order sort of the candidates (insertion sort, small n) */
        {
            size_t a;
            for (a = 1; a < cand_count; a++) {
                const char *tok = cand_tokens[a];
                double st = cand_strengths[a];
                size_t b = a;
                while (b > 0 && strcmp(cand_tokens[b - 1], tok) > 0) {
                    cand_tokens[b] = cand_tokens[b - 1];
                    cand_strengths[b] = cand_strengths[b - 1];
                    b--;
                }
                cand_tokens[b] = tok;
                cand_strengths[b] = st;
            }
        }
        for (i = 0; i < cand_count && i < max_out; i++) {
            out_tokens[i] = cand_tokens[i];
            out_strengths[i] = cand_strengths[i];
            written++;
        }
        free(cand_tokens);
        free(cand_strengths);
    }
    return written;
}

/* ------------------------------------------------------------------ */
/* Report / curation / serialization                                    */
/* ------------------------------------------------------------------ */

static const char *link_status_name(unsigned char status)
{
    switch (status) {
    case ODK_LINK_CANDIDATE:
        return "candidate";
    case ODK_LINK_ACTIVE:
        return "active";
    case ODK_LINK_PINNED:
        return "pinned";
    case ODK_LINK_BANNED:
        return "banned";
    default:
        return "unknown";
    }
}

char *odk_links_report(const odk_link_store *store)
{
    sb_t sb;
    size_t i;
    if (store == NULL) {
        return NULL;
    }
    if (sb_init(&sb) != 0) {
        return NULL;
    }
    if (sb_puts(&sb, "[") != 0) {
        sb_free(&sb);
        return NULL;
    }
    for (i = 0; i < store->link_count; i++) {
        const link_entry *l = &store->links[i];
        char *la = odk_json_string_literal(intern_get(&store->tokens, l->a),
                                           strlen(intern_get(&store->tokens,
                                                             l->a)));
        char *lb = odk_json_string_literal(intern_get(&store->tokens, l->b),
                                           strlen(intern_get(&store->tokens,
                                                             l->b)));
        int rc;
        if (la == NULL || lb == NULL) {
            free(la);
            free(lb);
            sb_free(&sb);
            return NULL;
        }
        rc = sb_printf(&sb,
                       "%s{\"a\":%s,\"b\":%s,\"strength\":%.4f,"
                       "\"status\":\"%s\",\"support_a\":%u,"
                       "\"support_b\":%u,\"contexts_a\":%u,"
                       "\"contexts_b\":%u,\"created_epoch\":%llu}",
                       i == 0 ? "" : ",", la, lb, l->strength,
                       link_status_name(l->status),
                       (unsigned)token_total_pos(store, l->a, NULL),
                       (unsigned)token_total_pos(store, l->b, NULL),
                       store->token_contexts[l->a],
                       store->token_contexts[l->b],
                       (unsigned long long)l->created_epoch);
        free(la);
        free(lb);
        if (rc != 0) {
            sb_free(&sb);
            return NULL;
        }
    }
    if (sb_puts(&sb, "]") != 0) {
        sb_free(&sb);
        return NULL;
    }
    return sb_detach(&sb);
}

int odk_links_pin(odk_link_store *store, const char *a, const char *b)
{
    if (store == NULL || a == NULL || b == NULL || strcmp(a, b) == 0) {
        return -1;
    }
    if (pair_set_has(&store->banned, a, b)) {
        return 0; /* banned stays banned */
    }
    return pair_set_add(&store->pinned, a, b) < 0 ? -1 : 0;
}

int odk_links_ban(odk_link_store *store, const char *a, const char *b)
{
    if (store == NULL || a == NULL || b == NULL || strcmp(a, b) == 0) {
        return -1;
    }
    pair_set_remove(&store->pinned, a, b);
    return pair_set_add(&store->banned, a, b) < 0 ? -1 : 0;
}

int odk_links_unpin(odk_link_store *store, const char *a, const char *b)
{
    if (store == NULL || a == NULL || b == NULL) {
        return -1;
    }
    pair_set_remove(&store->pinned, a, b);
    return 0;
}

int odk_links_serialize(const odk_link_store *store, sb_t *out)
{
    size_t i;
    size_t j;
    if (store == NULL || out == NULL) {
        return -1;
    }
    /* assoc: [{token, contexts, mentions, opts:[{key,pos,neg}]}] plus
     * the exact total_contexts for the generic gate */
    if (sb_printf(out, ",\"total_contexts\":%zu", store->total_contexts) !=
        0) {
        return -1;
    }
    if (sb_puts(out, ",\"assoc\":[") != 0) {
        return -1;
    }
    {
        int first_token = 1;
        for (i = 0; i < store->tokens.count; i++) {
            char *tok;
            int rc;
            if (i >= store->token_slots ||
                store->token_assocs[i].count == 0) {
                continue; /* tokens without associations carry no data */
            }
            tok = odk_json_string_literal(
                intern_get(&store->tokens, i),
                strlen(intern_get(&store->tokens, i)));
            if (tok == NULL) {
                return -1;
            }
            rc = sb_printf(out,
                           "%s{\"token\":%s,\"contexts\":%u,"
                           "\"mentions\":%u,\"opts\":[",
                           first_token ? "" : ",", tok,
                           store->token_contexts[i],
                           store->token_mentions[i]);
            free(tok);
            if (rc != 0) {
                return -1;
            }
            first_token = 0;
            {
                int first_opt = 1;
                for (j = 0; j < store->token_assocs[i].count; j++) {
                    const assoc_row *row = &store->token_assocs[i].rows[j];
                    char *opt = odk_json_string_literal(
                        intern_get(&store->options, row->option_id),
                        strlen(intern_get(&store->options, row->option_id)));
                    int rc2;
                    if (opt == NULL) {
                        return -1;
                    }
                    rc2 = sb_printf(out,
                                    "%s{\"key\":%s,\"pos\":%u,\"neg\":%u}",
                                    first_opt ? "" : ",", opt, row->pos,
                                    row->neg);
                    free(opt);
                    if (rc2 != 0) {
                        return -1;
                    }
                    first_opt = 0;
                }
            }
            if (sb_puts(out, "]}") != 0) {
                return -1;
            }
        }
    }
    if (sb_puts(out, "],\"links\":[") != 0) {
        return -1;
    }
    for (i = 0; i < store->link_count; i++) {
        const link_entry *l = &store->links[i];
        char *la = odk_json_string_literal(intern_get(&store->tokens, l->a),
                                           strlen(intern_get(&store->tokens,
                                                             l->a)));
        char *lb = odk_json_string_literal(intern_get(&store->tokens, l->b),
                                           strlen(intern_get(&store->tokens,
                                                             l->b)));
        int rc;
        if (la == NULL || lb == NULL) {
            free(la);
            free(lb);
            return -1;
        }
        rc = sb_printf(out,
                       "%s{\"a\":%s,\"b\":%s,\"strength\":%.17g,"
                       "\"status\":\"%s\",\"created_epoch\":%llu}",
                       i == 0 ? "" : ",", la, lb, l->strength,
                       link_status_name(l->status),
                       (unsigned long long)l->created_epoch);
        free(la);
        free(lb);
        if (rc != 0) {
            return -1;
        }
    }
    /* pinned/banned pairs persist as statuses */
    if (sb_puts(out, "],\"pinned\":[") != 0) {
        return -1;
    }
    {
        int first = 1;
        for (i = 0; i < store->pinned.count; i++) {
            const char *pair = store->pinned.items[i].pair;
            if (pair != NULL && pair[0] != '\0') {
                const char *sep = strchr(pair, '\x1f');
                if (sep != NULL) {
                    char *a = odk_strdup_range(pair, (size_t)(sep - pair));
                    char *b = odk_strdup(sep + 1);
                    char *ja;
                    char *jb;
                    if (a == NULL || b == NULL) {
                        free(a);
                        free(b);
                        return -1;
                    }
                    ja = odk_json_string_literal(a, strlen(a));
                    jb = odk_json_string_literal(b, strlen(b));
                    if (ja != NULL && jb != NULL &&
                        sb_printf(out, "%s[%s,%s]", first ? "" : ",", ja,
                                  jb) == 0) {
                        first = 0;
                    }
                    free(a);
                    free(b);
                    free(ja);
                    free(jb);
                }
            }
        }
    }
    if (sb_puts(out, "],\"banned\":[") != 0) {
        return -1;
    }
    {
        int first = 1;
        for (i = 0; i < store->banned.count; i++) {
            const char *pair = store->banned.items[i].pair;
            if (pair != NULL && pair[0] != '\0') {
                const char *sep = strchr(pair, '\x1f');
                if (sep != NULL) {
                    char *a = odk_strdup_range(pair, (size_t)(sep - pair));
                    char *b = odk_strdup(sep + 1);
                    char *ja;
                    char *jb;
                    if (a == NULL || b == NULL) {
                        free(a);
                        free(b);
                        return -1;
                    }
                    ja = odk_json_string_literal(a, strlen(a));
                    jb = odk_json_string_literal(b, strlen(b));
                    if (ja != NULL && jb != NULL &&
                        sb_printf(out, "%s[%s,%s]", first ? "" : ",", ja,
                                  jb) == 0) {
                        first = 0;
                    }
                    free(a);
                    free(b);
                    free(ja);
                    free(jb);
                }
            }
        }
    }
    if (sb_puts(out, "]") != 0) {
        return -1;
    }
    return 0;
}

int odk_links_load_json(odk_link_store *store, const odk_json_value *root)
{
    const odk_json_value *assoc;
    const odk_json_value *links;
    const odk_json_value *pinned;
    const odk_json_value *banned;
    size_t i;

    if (store == NULL || root == NULL || root->type != ODK_JSON_OBJECT) {
        return -1;
    }
    assoc = odk_json_object_get(root, "assoc");
    if (assoc != NULL && assoc->type == ODK_JSON_ARRAY) {
        for (i = 0; i < assoc->item_count; i++) {
            const odk_json_value *item = assoc->items[i];
            const odk_json_value *field;
            const char *token;
            unsigned contexts = 0;
            if (item == NULL || item->type != ODK_JSON_OBJECT) {
                continue;
            }
            field = odk_json_object_get(item, "token");
            if (field == NULL || field->type != ODK_JSON_STRING ||
                field->string == NULL || field->string[0] == '\0') {
                continue;
            }
            token = field->string;
            field = odk_json_object_get(item, "contexts");
            if (field != NULL && field->type == ODK_JSON_NUMBER &&
                field->number > 0.0) {
                contexts = (unsigned)field->number;
            }
            {
                unsigned mentions = contexts;
                const odk_json_value *mf =
                    odk_json_object_get(item, "mentions");
                if (mf != NULL && mf->type == ODK_JSON_NUMBER &&
                    mf->number > 0.0) {
                    mentions = (unsigned)mf->number;
                }
                field = odk_json_object_get(item, "opts");
                if (field != NULL && field->type == ODK_JSON_ARRAY) {
                    size_t k;
                    for (k = 0; k < field->item_count; k++) {
                        const odk_json_value *row = field->items[k];
                        const odk_json_value *key;
                        unsigned pos = 0;
                        unsigned neg = 0;
                        if (row == NULL || row->type != ODK_JSON_OBJECT) {
                            continue;
                        }
                        key = odk_json_object_get(row, "key");
                        if (key == NULL || key->type != ODK_JSON_STRING ||
                            key->string == NULL || key->string[0] == '\0') {
                            continue;
                        }
                        {
                            const odk_json_value *f2;
                            f2 = odk_json_object_get(row, "pos");
                            if (f2 != NULL && f2->type == ODK_JSON_NUMBER &&
                                f2->number > 0.0) {
                                pos = (unsigned)f2->number;
                            }
                            f2 = odk_json_object_get(row, "neg");
                            if (f2 != NULL && f2->type == ODK_JSON_NUMBER &&
                                f2->number > 0.0) {
                                neg = (unsigned)f2->number;
                            }
                        }
                        /* direct state restoration: lossless round-trip */
                        if (odk_links_restore_row(store, token, contexts,
                                                  mentions, key->string, pos,
                                                  neg) != 0) {
                            return -1;
                        }
                    }
                }
            }
        }
    }
    links = odk_json_object_get(root, "links");
    (void)links; /* links are recomputed at maintain() from assoc */
    {
        const odk_json_value *tc = odk_json_object_get(root,
                                                       "total_contexts");
        if (tc != NULL && tc->type == ODK_JSON_NUMBER &&
            tc->number > 0.0) {
            odk_links_set_total_contexts(store, (size_t)tc->number);
        }
    }
    pinned = odk_json_object_get(root, "pinned");
    if (pinned != NULL && pinned->type == ODK_JSON_ARRAY) {
        for (i = 0; i < pinned->item_count; i++) {
            const odk_json_value *pair = pinned->items[i];
            if (pair != NULL && pair->type == ODK_JSON_ARRAY &&
                pair->item_count == 2 &&
                pair->items[0] != NULL && pair->items[1] != NULL &&
                pair->items[0]->type == ODK_JSON_STRING &&
                pair->items[1]->type == ODK_JSON_STRING) {
                (void)odk_links_pin(store, pair->items[0]->string,
                                    pair->items[1]->string);
            }
        }
    }
    banned = odk_json_object_get(root, "banned");
    if (banned != NULL && banned->type == ODK_JSON_ARRAY) {
        for (i = 0; i < banned->item_count; i++) {
            const odk_json_value *pair = banned->items[i];
            if (pair != NULL && pair->type == ODK_JSON_ARRAY &&
                pair->item_count == 2 &&
                pair->items[0] != NULL && pair->items[1] != NULL &&
                pair->items[0]->type == ODK_JSON_STRING &&
                pair->items[1]->type == ODK_JSON_STRING) {
                (void)odk_links_ban(store, pair->items[0]->string,
                                    pair->items[1]->string);
            }
        }
    }
    return 0;
}

void odk_links_stats(const odk_link_store *store, size_t *tokens,
                     size_t *assoc_rows, size_t *links)
{
    size_t rows = 0;
    size_t i;
    if (tokens != NULL) {
        *tokens = store != NULL ? store->tokens.count : 0;
    }
    if (assoc_rows != NULL) {
        for (i = 0; store != NULL && i < store->token_slots; i++) {
            rows += store->token_assocs[i].count;
        }
        *assoc_rows = rows;
    }
    if (links != NULL) {
        *links = store != NULL ? store->link_count : 0;
    }
}
