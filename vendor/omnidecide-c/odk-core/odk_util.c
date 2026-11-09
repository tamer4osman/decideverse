/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_util.c - dynamic buffers, string lists, counters and small helpers.
 * Every allocation is checked; nothing leaks on failure paths.
 *
 */

#include "odk_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* sb_t                                                                */
/* ------------------------------------------------------------------ */

int sb_init(sb_t *sb)
{
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    return sb_reserve(sb, 1);
}

void sb_free(sb_t *sb)
{
    if (sb != NULL) {
        free(sb->data);
        sb->data = NULL;
        sb->len = 0;
        sb->cap = 0;
    }
}

int sb_reserve(sb_t *sb, size_t extra)
{
    size_t needed;
    size_t new_cap;
    char *grown;

    if (sb->cap > SIZE_MAX - extra - 1) {
        return -1; /* overflow */
    }
    needed = sb->len + extra + 1;
    if (needed <= sb->cap) {
        return 0;
    }
    new_cap = sb->cap ? sb->cap : 64;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = needed;
            break;
        }
        new_cap *= 2;
    }
    grown = realloc(sb->data, new_cap);
    if (grown == NULL) {
        return -1;
    }
    sb->data = grown;
    sb->cap = new_cap;
    /* The buffer is always kept NUL-terminated at the current length, so
     * a detached buffer is a valid C string even when nothing was ever
     * appended. */
    sb->data[sb->len] = '\0';
    return 0;
}

int sb_append(sb_t *sb, const char *s, size_t n)
{
    if (n == 0) {
        if (sb_reserve(sb, 1) != 0) {
            return -1;
        }
        sb->data[sb->len] = '\0';
        return 0;
    }
    if (sb_reserve(sb, n) != 0) {
        return -1;
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
    return 0;
}

int sb_puts(sb_t *sb, const char *s)
{
    return sb_append(sb, s, strlen(s));
}

int sb_putc(sb_t *sb, char c)
{
    return sb_append(sb, &c, 1);
}

int sb_printf(sb_t *sb, const char *fmt, ...)
{
    va_list args;
    int needed;
    char stackbuf[256];

    va_start(args, fmt);
    needed = vsnprintf(stackbuf, sizeof(stackbuf), fmt, args);
    va_end(args);
    if (needed < 0) {
        return -1;
    }
    if ((size_t)needed < sizeof(stackbuf)) {
        return sb_append(sb, stackbuf, (size_t)needed);
    }
    {
        char *heap = malloc((size_t)needed + 1);
        int rc;
        if (heap == NULL) {
            return -1;
        }
        va_start(args, fmt);
        needed = vsnprintf(heap, (size_t)needed + 1, fmt, args);
        va_end(args);
        rc = (needed >= 0) ? sb_append(sb, heap, (size_t)needed) : -1;
        free(heap);
        return rc;
    }
}

char *sb_detach(sb_t *sb)
{
    char *out = sb->data;
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    return out;
}

/* ------------------------------------------------------------------ */
/* Small string utilities                                              */
/* ------------------------------------------------------------------ */

char *odk_strdup_range(const char *s, size_t len)
{
    char *copy = malloc(len + 1);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

char *odk_strdup(const char *s)
{
    return s != NULL ? odk_strdup_range(s, strlen(s)) : NULL;
}

/* ------------------------------------------------------------------ */
/* Small string utilities                                              */
/* ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ */
/* str_list_t                                                          */
/* ------------------------------------------------------------------ */

void str_list_init(str_list_t *list)
{
    list->items = NULL;
    list->count = 0;
    list->cap = 0;
}

void str_list_free(str_list_t *list)
{
    size_t i;
    if (list == NULL) {
        return;
    }
    for (i = 0; i < list->count; i++) {
        free(list->items[i]);
    }
    free(list->items);
    str_list_init(list);
}

int str_list_push(str_list_t *list, const char *s, size_t n)
{
    char *copy;
    if (list->count == list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 8;
        char **grown;
        if (new_cap > SIZE_MAX / sizeof(char *)) {
            return -1;
        }
        grown = realloc(list->items, new_cap * sizeof(char *));
        if (grown == NULL) {
            return -1;
        }
        list->items = grown;
        list->cap = new_cap;
    }
    copy = malloc(n + 1);
    if (copy == NULL) {
        return -1;
    }
    memcpy(copy, s, n);
    copy[n] = '\0';
    list->items[list->count++] = copy;
    return 0;
}

/* ------------------------------------------------------------------ */
/* cmap_t                                                              */
/* ------------------------------------------------------------------ */

static uint64_t cmap_hash(const char *key, size_t len)
{
    uint64_t hash = 1469598103934665603ULL; /* FNV-1a 64 offset basis */
    size_t i;
    for (i = 0; i < len; i++) {
        hash ^= (unsigned char)key[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

int cmap_init(cmap_t *map)
{
    map->entries = NULL;
    map->count = 0;
    map->cap = 0;
    map->buckets = NULL;
    map->bucket_count = 0;
    return 0;
}

void cmap_free(cmap_t *map)
{
    size_t i;
    if (map == NULL) {
        return;
    }
    for (i = 0; i < map->count; i++) {
        free(map->entries[i].key);
    }
    free(map->entries);
    free(map->buckets);
    cmap_init(map);
}

static int cmap_rehash(cmap_t *map, size_t new_bucket_count)
{
    size_t *grown = calloc(new_bucket_count, sizeof(size_t));
    size_t i;
    if (grown == NULL) {
        return -1;
    }
    free(map->buckets);
    map->buckets = grown;
    map->bucket_count = new_bucket_count;
    for (i = 0; i < map->count; i++) {
        uint64_t h = cmap_hash(map->entries[i].key, strlen(map->entries[i].key));
        size_t slot = (size_t)h & (new_bucket_count - 1);
        while (map->buckets[slot] != 0) {
            slot = (slot + 1) & (new_bucket_count - 1);
        }
        map->buckets[slot] = i + 1;
    }
    return 0;
}

static long cmap_find(const cmap_t *map, const char *key, size_t key_len)
{
    size_t slot;
    if (map->bucket_count == 0) {
        return -1;
    }
    slot = (size_t)cmap_hash(key, key_len) & (map->bucket_count - 1);
    for (;;) {
        size_t idx = map->buckets[slot];
        if (idx == 0) {
            return -1;
        }
        if (strlen(map->entries[idx - 1].key) == key_len &&
            memcmp(map->entries[idx - 1].key, key, key_len) == 0) {
            return (long)(idx - 1);
        }
        slot = (slot + 1) & (map->bucket_count - 1);
    }
}

int cmap_increment(cmap_t *map, const char *key, size_t key_len)
{
    long found = cmap_find(map, key, key_len);
    if (found >= 0) {
        map->entries[found].count++;
        return 0;
    }
    if (map->count == map->cap) {
        size_t new_cap = map->cap ? map->cap * 2 : 16;
        cmap_entry *grown;
        if (new_cap > SIZE_MAX / sizeof(cmap_entry)) {
            return -1;
        }
        grown = realloc(map->entries, new_cap * sizeof(cmap_entry));
        if (grown == NULL) {
            return -1;
        }
        map->entries = grown;
        map->cap = new_cap;
    }
    {
        char *copy = malloc(key_len + 1);
        size_t index;
        if (copy == NULL) {
            return -1;
        }
        memcpy(copy, key, key_len);
        copy[key_len] = '\0';
        index = map->count++;
        map->entries[index].key = copy;
        map->entries[index].count = 1;
    }
    if (map->bucket_count == 0 || (map->count + 1) * 10 >= map->bucket_count * 7) {
        size_t target = map->bucket_count ? map->bucket_count * 2 : 32;
        if (cmap_rehash(map, target) != 0) {
            return -1;
        }
    }
    {
        uint64_t h = cmap_hash(key, key_len);
        size_t slot = (size_t)h & (map->bucket_count - 1);
        while (map->buckets[slot] != 0) {
            slot = (slot + 1) & (map->bucket_count - 1);
        }
        map->buckets[slot] = map->count; /* index + 1 */
    }
    return 0;
}

size_t cmap_get(const cmap_t *map, const char *key, size_t key_len)
{
    long found = cmap_find(map, key, key_len);
    return found >= 0 ? map->entries[found].count : 0;
}

/* ------------------------------------------------------------------ */
/* Text helpers                                                        */
/* ------------------------------------------------------------------ */

int odk_ascii_isspace(unsigned char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

int odk_ascii_islower_token(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}

/* shared binary search over a sorted word list */
static int stopword_lookup(const char *s, const char *const *words,
                           size_t count)
{
    size_t lo = 0;
    size_t hi = count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = strcmp(s, words[mid]);
        if (cmp == 0) {
            return 1;
        }
        if (cmp < 0) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return 0;
}

static int stopword_find(const char *s)
{
    /* Compact English stopword list, sorted for binary search.  Single-character entries are unreachable
     * (the len>1 filter drops them first) but kept for fidelity. */
    static const char *const kStopwords[] = {
        "a", "about", "above", "after", "again", "against", "all", "am",
        "an", "and", "any", "are", "as", "at", "be", "because", "been",
        "before", "being", "below", "between", "both", "but", "by", "can",
        "could", "did", "do", "does", "doing", "down", "during", "each",
        "few", "for", "from", "further", "had", "has", "have", "having",
        "he", "her", "here", "hers", "him", "his", "how", "i", "if", "in",
        "into", "is", "it", "its", "itself", "just", "me", "more", "most",
        "my", "no", "nor", "not", "now", "of", "off", "on", "once", "only",
        "or", "other", "our", "ours", "out", "over", "own", "same", "she",
        "should", "so", "some", "such", "than", "that", "the", "their",
        "theirs", "them", "then", "there", "these", "they", "this", "those",
        "through", "to", "too", "under", "until", "up", "very", "was", "we",
        "were", "what", "when", "where", "which", "while", "who", "whom",
        "why", "will", "with", "would", "you", "your", "yours"
    };
    return stopword_lookup(s, kStopwords,
                           sizeof(kStopwords) / sizeof(kStopwords[0]));
}

/* The EN pack is exactly the legacy list above (odk_unicode.c). */
int odk_util_stopword_en_find(const char *s)
{
    return stopword_find(s);
}

int odk_extract_tokens(const char *text, int filtered, str_list_t *out)
{
    size_t i = 0;
    size_t n;

    str_list_init(out);
    if (text == NULL) {
        return 0;
    }
    n = strlen(text);
    while (i < n) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c - 'A' + 'a');
        }
        if (odk_ascii_islower_token(c)) {
            size_t start = i;
            size_t len;
            while (i < n) {
                unsigned char d = (unsigned char)text[i];
                if (d >= 'A' && d <= 'Z') {
                    d = (unsigned char)(d - 'A' + 'a');
                }
                if (!odk_ascii_islower_token(d)) {
                    break;
                }
                i++;
            }
            len = i - start;
            if (!filtered || len > 1) {
                /* Build the lowercased token. */
                char buf[65];
                char *heap = NULL;
                const char *src;
                if (len <= 64) {
                    size_t k;
                    for (k = 0; k < len; k++) {
                        char ch = text[start + k];
                        if (ch >= 'A' && ch <= 'Z') {
                            ch = (char)(ch - 'A' + 'a');
                        }
                        buf[k] = ch;
                    }
                    buf[len] = '\0';
                    src = buf;
                } else {
                    size_t k;
                    heap = malloc(len + 1);
                    if (heap == NULL) {
                        str_list_free(out);
                        return -1;
                    }
                    for (k = 0; k < len; k++) {
                        char ch = text[start + k];
                        if (ch >= 'A' && ch <= 'Z') {
                            ch = (char)(ch - 'A' + 'a');
                        }
                        heap[k] = ch;
                    }
                    heap[len] = '\0';
                    src = heap;
                }
                if (filtered && stopword_find(src)) {
                    free(heap);
                    continue;
                }
                if (str_list_push(out, src, len) != 0) {
                    free(heap);
                    str_list_free(out);
                    return -1;
                }
                free(heap);
            }
        } else {
            i++;
        }
    }
    return 0;
}

double odk_counter_cosine(const cmap_t *left, const cmap_t *right)
{
    double dot = 0.0;
    double left_norm = 0.0;
    double right_norm = 0.0;
    size_t i;

    if (left->count == 0 || right->count == 0) {
        return 0.0;
    }
    for (i = 0; i < left->count; i++) {
        double value = (double)left->entries[i].count;
        size_t rc = cmap_get(right, left->entries[i].key,
                             strlen(left->entries[i].key));
        left_norm += value * value;
        if (rc != 0) {
            dot += value * (double)rc;
        }
    }
    for (i = 0; i < right->count; i++) {
        double value = (double)right->entries[i].count;
        right_norm += value * value;
    }
    if (left_norm <= 0.0 || right_norm <= 0.0) {
        return 0.0;
    }
    return dot / (sqrt(left_norm) * sqrt(right_norm));
}

char *odk_str_join(const char *const *items, size_t count, const char *sep)
{
    sb_t sb;
    size_t i;
    size_t sep_len = strlen(sep);

    if (sb_init(&sb) != 0) {
        return NULL;
    }
    for (i = 0; i < count; i++) {
        if (i > 0 && sb_append(&sb, sep, sep_len) != 0) {
            sb_free(&sb);
            return NULL;
        }
        if (sb_puts(&sb, items[i] != NULL ? items[i] : "") != 0) {
            sb_free(&sb);
            return NULL;
        }
    }
    return sb_detach(&sb);
}

void odk_strip_bounds(const char *s, size_t len, size_t *start, size_t *end)
{
    size_t lo = 0;
    size_t hi = len;
    while (lo < hi && odk_ascii_isspace((unsigned char)s[lo])) {
        lo++;
    }
    while (hi > lo && odk_ascii_isspace((unsigned char)s[hi - 1])) {
        hi--;
    }
    *start = lo;
    *end = hi;
}

double odk_round_half_even(double x)
{
    return rint(x);
}

double odk_round4(double x)
{
    return rint(x * 10000.0) / 10000.0;
}

double odk_round3(double x)
{
    return rint(x * 1000.0) / 1000.0;
}

double odk_round2(double x)
{
    return rint(x * 100.0) / 100.0;
}

double odk_clamp01(double x)
{
    if (x < 0.0) {
        return 0.0;
    }
    if (x > 1.0) {
        return 1.0;
    }
    return x;
}

size_t odk_utf8_codepoints(const char *s, size_t len)
{
    size_t i;
    size_t count = 0;
    for (i = 0; i < len; i++) {
        unsigned char b = (unsigned char)s[i];
        if ((b & 0xC0) != 0x80) {
            count++;
        }
    }
    return count;
}

void odk_set_error(odk_error *err, const char *fmt, ...)
{
    va_list args;
    if (err == NULL) {
        return;
    }
    va_start(args, fmt);
    vsnprintf(err->text, sizeof(err->text), fmt, args);
    va_end(args);
}
