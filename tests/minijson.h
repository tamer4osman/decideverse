/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * minijson.h - tiny JSON helpers for the native parity runner.
 *
 * Sufficient for the fixed tests/requests.jsonl schema: flat objects
 * with string/number/bool values plus arrays of strings and one nested
 * "caps" object.  Not a general-purpose JSON library.
 */

#ifndef ODK_MINIJSON_H
#define ODK_MINIJSON_H

#include <stddef.h>

typedef struct {
    const char *start; /* pointer into the source line          */
    size_t len;        /* raw slice length (still JSON-escaped) */
} mj_slice;

/* Find a top-level member of the object at `obj`.  Returns 1 and fills
 * *out on success, 0 when the key is absent, -1 on malformed input. */
int mj_member(const char *obj, const char *key, mj_slice *out);

/* Unescape a JSON string slice into dst (NUL-terminated).  Supports
 * \" \\ \/ \b \f \n \r \t and \uXXXX (including surrogate pairs).
 * Returns 0 on success, -1 on overflow/malformed input. */
int mj_string(const mj_slice *v, char *dst, size_t cap);

int mj_int(const mj_slice *v, long *out);
int mj_bool(const mj_slice *v, int *out);

/* Count elements of an array slice. */
int mj_array_len(const mj_slice *v, size_t *out);

/* Unescape element i of an array slice into dst. */
int mj_array_string(const mj_slice *v, size_t i, char *dst, size_t cap);

#endif /* ODK_MINIJSON_H */
