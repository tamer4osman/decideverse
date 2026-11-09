/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * minijson.c - implementation of the parity runner's JSON helpers.
 */

#include "minijson.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    return p;
}

/* Scan one JSON string starting at the opening quote; returns pointer
 * past the closing quote or NULL. */
static const char *scan_string(const char *p)
{
    if (*p != '"') {
        return NULL;
    }
    p++;
    while (*p != '\0') {
        if (*p == '\\') {
            if (p[1] == '\0') {
                return NULL;
            }
            p += 2;
            continue;
        }
        if (*p == '"') {
            return p + 1;
        }
        p++;
    }
    return NULL;
}

/* Scan one JSON value; returns pointer past it or NULL. */
static const char *scan_value(const char *p)
{
    int depth;

    p = skip_ws(p);
    if (*p == '"') {
        return scan_string(p);
    }
    if (*p == '{' || *p == '[') {
        char open = *p;
        char close = (open == '{') ? '}' : ']';
        depth = 0;
        while (*p != '\0') {
            if (*p == '"') {
                p = scan_string(p);
                if (p == NULL) {
                    return NULL;
                }
                continue;
            }
            if (*p == open) {
                depth++;
            } else if (*p == close) {
                depth--;
                if (depth == 0) {
                    return p + 1;
                }
            }
            p++;
        }
        return NULL;
    }
    /* number / true / false / null */
    while (*p != '\0' && *p != ',' && *p != '}' && *p != ']' &&
           *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') {
        p++;
    }
    return p;
}

int mj_member(const char *obj, const char *key, mj_slice *out)
{
    const char *p = skip_ws(obj);
    size_t key_len = strlen(key);

    if (*p != '{') {
        return -1;
    }
    p++;
    for (;;) {
        const char *key_end;
        const char *val;
        p = skip_ws(p);
        if (*p == '}') {
            return 0; /* end of object, key not found */
        }
        if (*p != '"') {
            return -1;
        }
        key_end = scan_string(p);
        if (key_end == NULL) {
            return -1;
        }
        if ((size_t)(key_end - p - 2) == key_len &&
            memcmp(p + 1, key, key_len) == 0) {
            val = skip_ws(key_end);
            if (*val != ':') {
                return -1;
            }
            val = skip_ws(val + 1);
            out->start = val;
            val = scan_value(val);
            if (val == NULL) {
                return -1;
            }
            while (val > out->start && (val[-1] == ' ' || val[-1] == '\t')) {
                val--;
            }
            out->len = (size_t)(val - out->start);
            return 1;
        }
        p = skip_ws(key_end);
        if (*p != ':') {
            return -1;
        }
        p = scan_value(p + 1);
        if (p == NULL) {
            return -1;
        }
        p = skip_ws(p);
        if (*p == ',') {
            p++;
        } else if (*p != '}') {
            return -1;
        }
    }
}

static int hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    int i;

    for (i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= (unsigned)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v |= (unsigned)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            v |= (unsigned)(c - 'A' + 10);
        } else {
            return -1;
        }
    }
    *out = v;
    return 0;
}

static int utf8_emit(char *dst, size_t cap, size_t *used, unsigned cp)
{
    if (cp < 0x80u) {
        if (*used + 1 >= cap) return -1;
        dst[(*used)++] = (char)cp;
    } else if (cp < 0x800u) {
        if (*used + 2 >= cap) return -1;
        dst[(*used)++] = (char)(0xC0u | (cp >> 6));
        dst[(*used)++] = (char)(0x80u | (cp & 0x3Fu));
    } else if (cp < 0x10000u) {
        if (*used + 3 >= cap) return -1;
        dst[(*used)++] = (char)(0xE0u | (cp >> 12));
        dst[(*used)++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        dst[(*used)++] = (char)(0x80u | (cp & 0x3Fu));
    } else {
        if (*used + 4 >= cap) return -1;
        dst[(*used)++] = (char)(0xF0u | (cp >> 18));
        dst[(*used)++] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
        dst[(*used)++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        dst[(*used)++] = (char)(0x80u | (cp & 0x3Fu));
    }
    return 0;
}

int mj_string(const mj_slice *v, char *dst, size_t cap)
{
    const char *p = v->start;
    const char *end = v->start + v->len;
    size_t used = 0;

    if (v->len < 2 || *p != '"' || end[-1] != '"') {
        return -1;
    }
    p++;
    while (p < end - 1) {
        if (*p != '"') {
            if (*p == '\\') {
                p++;
                if (p >= end - 1) {
                    return -1;
                }
                switch (*p) {
                case '"': dst[used++] = '"'; break;
                case '\\': dst[used++] = '\\'; break;
                case '/': dst[used++] = '/'; break;
                case 'b': dst[used++] = '\b'; break;
                case 'f': dst[used++] = '\f'; break;
                case 'n': dst[used++] = '\n'; break;
                case 'r': dst[used++] = '\r'; break;
                case 't': dst[used++] = '\t'; break;
                case 'u': {
                    unsigned cp;
                    if (hex4(p + 1, &cp) != 0) {
                        return -1;
                    }
                    p += 4;
                    if (cp >= 0xD800u && cp <= 0xDBFFu && p + 6 < end &&
                        p[1] == '\\' && p[2] == 'u') {
                        unsigned lo;
                        if (hex4(p + 3, &lo) != 0 || lo < 0xDC00u ||
                            lo > 0xDFFFu) {
                            return -1;
                        }
                        cp = 0x10000u + ((cp - 0xD800u) << 10) +
                             (lo - 0xDC00u);
                        p += 6;
                    }
                    if (utf8_emit(dst, cap, &used, cp) != 0) {
                        return -1;
                    }
                    break;
                }
                default:
                    return -1;
                }
                p++;
            } else {
                if (used + 1 >= cap) {
                    return -1;
                }
                dst[used++] = *p++;
            }
        } else {
            return -1;
        }
    }
    dst[used] = '\0';
    return 0;
}

int mj_int(const mj_slice *v, long *out)
{
    char buf[32];
    char *endp;

    if (v->len == 0 || v->len >= sizeof(buf)) {
        return -1;
    }
    memcpy(buf, v->start, v->len);
    buf[v->len] = '\0';
    *out = strtol(buf, &endp, 10);
    if (endp == buf || *endp != '\0') {
        return -1;
    }
    return 0;
}

int mj_bool(const mj_slice *v, int *out)
{
    if (v->len == 4 && memcmp(v->start, "true", 4) == 0) {
        *out = 1;
        return 0;
    }
    if (v->len == 5 && memcmp(v->start, "false", 5) == 0) {
        *out = 0;
        return 0;
    }
    return -1;
}

int mj_array_len(const mj_slice *v, size_t *out)
{
    const char *p = v->start;
    const char *end = v->start + v->len;
    size_t n = 0;

    if (v->len < 2 || *p != '[' || end[-1] != ']') {
        return -1;
    }
    p++;
    while (p < end - 1) {
        const char *val = skip_ws(p);
        if (val >= end - 1) {
            break;
        }
        val = scan_value(val);
        if (val == NULL) {
            return -1;
        }
        n++;
        val = skip_ws(val);
        if (*val == ',') {
            p = val + 1;
        } else {
            p = val;
        }
    }
    *out = n;
    return 0;
}

int mj_array_string(const mj_slice *v, size_t i, char *dst, size_t cap)
{
    const char *p = v->start;
    const char *end = v->start + v->len;
    size_t n = 0;

    if (v->len < 2 || *p != '[' || end[-1] != ']') {
        return -1;
    }
    p++;
    while (p < end - 1) {
        const char *val = skip_ws(p);
        mj_slice s;

        if (val >= end - 1) {
            break;
        }
        s.start = val;
        val = scan_value(val);
        if (val == NULL) {
            return -1;
        }
        while (val > s.start && (val[-1] == ' ' || val[-1] == '\t')) {
            val--;
        }
        s.len = (size_t)(val - s.start);
        if (n == i) {
            return mj_string(&s, dst, cap);
        }
        n++;
        val = skip_ws(val);
        if (*val == ',') {
            p = val + 1;
        } else {
            p = val;
        }
    }
    return -1;
}
