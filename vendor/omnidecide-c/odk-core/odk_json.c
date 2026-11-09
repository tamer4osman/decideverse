/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_json.c - compact strict-JSON parser and writer.
 *
 * The parser preserves object key insertion order (for duplicate
 * keys the LAST value wins but the
 * key keeps its original position).  The writer emits the shortest
 * round-trip representation for doubles, so
 * parsed values compare bit-identical to the originals.
 *
 */

#include "odk_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#define ODK_JSON_MAX_DEPTH 64

/* ------------------------------------------------------------------ */
/* Allocation helpers                                                  */
/* ------------------------------------------------------------------ */

static odk_json_value *json_new(odk_json_type type)
{
    odk_json_value *value = calloc(1, sizeof(*value));
    if (value == NULL) {
        return NULL;
    }
    value->type = type;
    return value;
}

void odk_json_free(odk_json_value *value)
{
    size_t i;
    if (value == NULL) {
        return;
    }
    free(value->string);
    for (i = 0; i < value->item_count; i++) {
        odk_json_free(value->items[i]);
    }
    free(value->items);
    for (i = 0; i < value->member_count; i++) {
        odk_json_free(value->members[i]);
        free(value->keys[i]);
    }
    free(value->keys);
    free(value->members);
    free(value);
}

static void json_set_error(odk_error *err, const char *msg)
{
    if (err != NULL) {
        snprintf(err->text, sizeof(err->text), "%s", msg);
    }
}

/* ------------------------------------------------------------------ */
/* Parser                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *p;
    const char *end;
    unsigned depth;
    odk_error *err;
} json_parser;

static odk_json_value *json_parse_value(json_parser *ps);

static void json_skip_ws(json_parser *ps)
{
    while (ps->p < ps->end) {
        char c = *ps->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ps->p++;
        } else {
            break;
        }
    }
}

static int json_hex4(json_parser *ps, unsigned *out)
{
    unsigned value = 0;
    int i;
    for (i = 0; i < 4; i++) {
        char c;
        if (ps->p >= ps->end) {
            json_set_error(ps->err, "unterminated unicode escape");
            return -1;
        }
        c = *ps->p++;
        value <<= 4;
        if (c >= '0' && c <= '9') {
            value |= (unsigned)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value |= (unsigned)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            value |= (unsigned)(c - 'A' + 10);
        } else {
            json_set_error(ps->err, "invalid unicode escape");
            return -1;
        }
    }
    *out = value;
    return 0;
}

static int json_utf8_emit(sb_t *sb, unsigned cp)
{
    if (cp <= 0x7F) {
        return sb_putc(sb, (char)cp);
    }
    if (cp <= 0x7FF) {
        if (sb_putc(sb, (char)(0xC0 | (cp >> 6))) != 0) {
            return -1;
        }
        return sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    }
    if (cp <= 0xFFFF) {
        if (sb_putc(sb, (char)(0xE0 | (cp >> 12))) != 0) {
            return -1;
        }
        if (sb_putc(sb, (char)(0x80 | ((cp >> 6) & 0x3F))) != 0) {
            return -1;
        }
        return sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
    }
    if (sb_putc(sb, (char)(0xF0 | (cp >> 18))) != 0) {
        return -1;
    }
    if (sb_putc(sb, (char)(0x80 | ((cp >> 12) & 0x3F))) != 0) {
        return -1;
    }
    if (sb_putc(sb, (char)(0x80 | ((cp >> 6) & 0x3F))) != 0) {
        return -1;
    }
    return sb_putc(sb, (char)(0x80 | (cp & 0x3F)));
}

static int json_parse_escape(json_parser *ps, sb_t *sb)
{
    char c;

    if (ps->p >= ps->end) {
        json_set_error(ps->err, "unterminated escape sequence");
        return -1;
    }
    c = *ps->p++;
    switch (c) {
    case '"':
        return sb_putc(sb, '"');
    case '\\':
        return sb_putc(sb, '\\');
    case '/':
        return sb_putc(sb, '/');
    case 'b':
        return sb_putc(sb, '\b');
    case 'f':
        return sb_putc(sb, '\f');
    case 'n':
        return sb_putc(sb, '\n');
    case 'r':
        return sb_putc(sb, '\r');
    case 't':
        return sb_putc(sb, '\t');
    case 'u': {
        unsigned cp;
        if (json_hex4(ps, &cp) != 0) {
            return -1;
        }
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            unsigned low;
            if (ps->end - ps->p < 2 || ps->p[0] != '\\' || ps->p[1] != 'u') {
                json_set_error(ps->err, "lone high surrogate");
                return -1;
            }
            ps->p += 2;
            if (json_hex4(ps, &low) != 0) {
                return -1;
            }
            if (low < 0xDC00 || low > 0xDFFF) {
                json_set_error(ps->err, "invalid low surrogate");
                return -1;
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            json_set_error(ps->err, "lone low surrogate");
            return -1;
        }
        if (cp == 0) {
            json_set_error(ps->err, "NUL character in string");
            return -1;
        }
        return json_utf8_emit(sb, cp);
    }
    default:
        json_set_error(ps->err, "invalid escape character");
        return -1;
    }
}

static char *json_parse_string_raw(json_parser *ps)
{
    sb_t sb;
    if (sb_init(&sb) != 0) {
        json_set_error(ps->err, "out of memory");
        return NULL;
    }
    if (sb_reserve(&sb, (size_t)(ps->end - ps->p)) != 0) {
        sb_free(&sb);
        json_set_error(ps->err, "out of memory");
        return NULL;
    }
    for (;;) {
        char c;
        if (ps->p >= ps->end) {
            json_set_error(ps->err, "unterminated string");
            sb_free(&sb);
            return NULL;
        }
        c = *ps->p++;
        if (c == '"') {
            return sb_detach(&sb);
        }
        if ((unsigned char)c < 0x20) {
            json_set_error(ps->err, "unescaped control character in string");
            sb_free(&sb);
            return NULL;
        }
        if (c == '\\') {
            if (json_parse_escape(ps, &sb) != 0) {
                sb_free(&sb);
                return NULL;
            }
        } else if (sb_putc(&sb, c) != 0) {
            sb_free(&sb);
            json_set_error(ps->err, "out of memory");
            return NULL;
        }
    }
}

static int json_parse_number(json_parser *ps, double *out, int *is_integer,
                             long long *integer_out)
{
    const char *start = ps->p;
    const char *q = ps->p;
    char buffer[64];
    size_t digits;
    int has_frac_or_exp = 0;
    char *endptr = NULL;

    if (q < ps->end && (*q == '-' || *q == '+')) {
        q++;
    }
    {
        size_t int_digits = 0;
        while (q < ps->end && *q >= '0' && *q <= '9') {
            q++;
            int_digits++;
        }
        if (int_digits == 0) {
            json_set_error(ps->err, "invalid number");
            return -1;
        }
        if (int_digits > 1 && *start == '0') {
            json_set_error(ps->err, "leading zeros are not allowed");
            return -1;
        }
    }
    if (q < ps->end && *q == '.') {
        has_frac_or_exp = 1;
        q++;
        {
            size_t frac_digits = 0;
            while (q < ps->end && *q >= '0' && *q <= '9') {
                q++;
                frac_digits++;
            }
            if (frac_digits == 0) {
                json_set_error(ps->err, "invalid number");
                return -1;
            }
        }
    }
    if (q < ps->end && (*q == 'e' || *q == 'E')) {
        has_frac_or_exp = 1;
        q++;
        if (q < ps->end && (*q == '-' || *q == '+')) {
            q++;
        }
        {
            size_t exp_digits = 0;
            while (q < ps->end && *q >= '0' && *q <= '9') {
                q++;
                exp_digits++;
            }
            if (exp_digits == 0) {
                json_set_error(ps->err, "invalid number");
                return -1;
            }
        }
    }
    digits = (size_t)(q - start);
    ps->p = q;
    if (digits >= sizeof(buffer)) {
        json_set_error(ps->err, "number literal too long");
        return -1;
    }
    memcpy(buffer, start, digits);
    buffer[digits] = '\0';

    errno = 0;
    *out = strtod(buffer, &endptr);
    if (endptr == NULL || *endptr != '\0' || errno == ERANGE) {
        json_set_error(ps->err, "number out of range");
        return -1;
    }
    *is_integer = !has_frac_or_exp;
    if (*is_integer) {
        errno = 0;
        *integer_out = strtoll(buffer, &endptr, 10);
        if (endptr == NULL || *endptr != '\0' || errno == ERANGE) {
            *is_integer = 0;
        }
    } else {
        *integer_out = 0;
    }
    return 0;
}

static int json_grow_array(odk_json_value ***items, size_t count, size_t *cap)
{
    if (count == *cap) {
        size_t new_cap = *cap ? *cap * 2 : 8;
        odk_json_value **grown;
        if (new_cap > SIZE_MAX / sizeof(odk_json_value *)) {
            return -1;
        }
        grown = realloc(*items, new_cap * sizeof(odk_json_value *));
        if (grown == NULL) {
            return -1;
        }
        *items = grown;
        *cap = new_cap;
    }
    return 0;
}

/* Grow the parallel keys/members arrays of an object together. */
static int json_grow_members(char ***keys, odk_json_value ***members,
                             size_t count, size_t *cap)
{
    if (count == *cap) {
        size_t new_cap = *cap ? *cap * 2 : 8;
        char **grown_keys;
        odk_json_value **grown_members;
        if (new_cap > SIZE_MAX / sizeof(char *)) {
            return -1;
        }
        grown_keys = realloc(*keys, new_cap * sizeof(char *));
        if (grown_keys == NULL) {
            return -1;
        }
        *keys = grown_keys;
        grown_members = realloc(*members, new_cap * sizeof(odk_json_value *));
        if (grown_members == NULL) {
            return -1;
        }
        *members = grown_members;
        *cap = new_cap;
    }
    return 0;
}

static odk_json_value *json_parse_object(json_parser *ps)
{
    odk_json_value *obj = json_new(ODK_JSON_OBJECT);
    size_t cap = 0;
    if (obj == NULL) {
        json_set_error(ps->err, "out of memory");
        return NULL;
    }
    json_skip_ws(ps);
    if (ps->p < ps->end && *ps->p == '}') {
        ps->p++;
        return obj;
    }
    for (;;) {
        char *key;
        odk_json_value *member;
        long existing = -1;
        size_t i;

        json_skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != '"') {
            json_set_error(ps->err, "expected object key");
            goto fail;
        }
        ps->p++;
        key = json_parse_string_raw(ps);
        if (key == NULL) {
            goto fail;
        }
        json_skip_ws(ps);
        if (ps->p >= ps->end || *ps->p != ':') {
            free(key);
            json_set_error(ps->err, "expected ':' after object key");
            goto fail;
        }
        ps->p++;
        member = json_parse_value(ps);
        if (member == NULL) {
            free(key);
            goto fail;
        }
        /* Duplicate key: last value wins, key keeps original position. */
        for (i = 0; i < obj->member_count; i++) {
            if (strcmp(obj->keys[i], key) == 0) {
                existing = (long)i;
                break;
            }
        }
        if (existing >= 0) {
            free(key);
            odk_json_free(obj->members[existing]);
            obj->members[existing] = member;
        } else {
            if (json_grow_members(&obj->keys, &obj->members,
                                  obj->member_count, &cap) != 0) {
                free(key);
                odk_json_free(member);
                json_set_error(ps->err, "out of memory");
                goto fail;
            }
            obj->keys[obj->member_count] = key;
            obj->members[obj->member_count] = member;
            obj->member_count++;
        }
        json_skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == '}') {
            ps->p++;
            return obj;
        }
        json_set_error(ps->err, "expected ',' or '}' in object");
        goto fail;
    }
fail:
    odk_json_free(obj);
    return NULL;
}

static odk_json_value *json_parse_array(json_parser *ps)
{
    odk_json_value *arr = json_new(ODK_JSON_ARRAY);
    size_t cap = 0;
    if (arr == NULL) {
        json_set_error(ps->err, "out of memory");
        return NULL;
    }
    json_skip_ws(ps);
    if (ps->p < ps->end && *ps->p == ']') {
        ps->p++;
        return arr;
    }
    for (;;) {
        odk_json_value *item;
        json_skip_ws(ps);
        item = json_parse_value(ps);
        if (item == NULL) {
            goto fail;
        }
        if (json_grow_array(&arr->items, arr->item_count, &cap) != 0) {
            odk_json_free(item);
            json_set_error(ps->err, "out of memory");
            goto fail;
        }
        arr->items[arr->item_count++] = item;
        json_skip_ws(ps);
        if (ps->p < ps->end && *ps->p == ',') {
            ps->p++;
            continue;
        }
        if (ps->p < ps->end && *ps->p == ']') {
            ps->p++;
            return arr;
        }
        json_set_error(ps->err, "expected ',' or ']' in array");
        goto fail;
    }
fail:
    odk_json_free(arr);
    return NULL;
}

static odk_json_value *json_parse_value(json_parser *ps)
{
    json_skip_ws(ps);
    if (ps->p >= ps->end) {
        json_set_error(ps->err, "unexpected end of input");
        return NULL;
    }
    if (ps->depth >= ODK_JSON_MAX_DEPTH) {
        json_set_error(ps->err, "nesting depth exceeded");
        return NULL;
    }
    {
        char c = *ps->p;
        if (c == '"') {
            char *s;
            odk_json_value *value;
            ps->p++;
            s = json_parse_string_raw(ps);
            if (s == NULL) {
                return NULL;
            }
            value = json_new(ODK_JSON_STRING);
            if (value == NULL) {
                free(s);
                json_set_error(ps->err, "out of memory");
                return NULL;
            }
            value->string = s;
            return value;
        }
        if (c == '{') {
            odk_json_value *value;
            ps->p++;
            ps->depth++;
            value = json_parse_object(ps);
            ps->depth--;
            return value;
        }
        if (c == '[') {
            odk_json_value *value;
            ps->p++;
            ps->depth++;
            value = json_parse_array(ps);
            ps->depth--;
            return value;
        }
        if (c == 't') {
            if (ps->end - ps->p >= 4 && memcmp(ps->p, "true", 4) == 0) {
                odk_json_value *value = json_new(ODK_JSON_BOOL);
                ps->p += 4;
                if (value != NULL) {
                    value->boolean = 1;
                }
                return value;
            }
            json_set_error(ps->err, "invalid literal");
            return NULL;
        }
        if (c == 'f') {
            if (ps->end - ps->p >= 5 && memcmp(ps->p, "false", 5) == 0) {
                odk_json_value *value = json_new(ODK_JSON_BOOL);
                ps->p += 5;
                return value;
            }
            json_set_error(ps->err, "invalid literal");
            return NULL;
        }
        if (c == 'n') {
            if (ps->end - ps->p >= 4 && memcmp(ps->p, "null", 4) == 0) {
                ps->p += 4;
                return json_new(ODK_JSON_NULL);
            }
            json_set_error(ps->err, "invalid literal");
            return NULL;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            double number;
            int is_integer;
            long long integer;
            odk_json_value *value;
            if (json_parse_number(ps, &number, &is_integer, &integer) != 0) {
                return NULL;
            }
            value = json_new(ODK_JSON_NUMBER);
            if (value == NULL) {
                json_set_error(ps->err, "out of memory");
                return NULL;
            }
            value->number = number;
            value->is_integer = is_integer;
            value->integer = integer;
            return value;
        }
        json_set_error(ps->err, "unexpected character");
        return NULL;
    }
}

odk_json_value *odk_json_parse(const char *text, size_t len, odk_error *err)
{
    json_parser ps;
    odk_json_value *value;

    if (text == NULL) {
        json_set_error(err, "null input");
        return NULL;
    }
    ps.p = text;
    ps.end = text + len;
    ps.depth = 0;
    ps.err = err;
    if (err != NULL) {
        err->text[0] = '\0';
    }
    value = json_parse_value(&ps);
    if (value == NULL) {
        return NULL;
    }
    json_skip_ws(&ps);
    if (ps.p != ps.end) {
        json_set_error(err, "trailing content after JSON value");
        odk_json_free(value);
        return NULL;
    }
    return value;
}

const odk_json_value *odk_json_object_get(const odk_json_value *object,
                                          const char *key)
{
    size_t i;
    if (object == NULL || object->type != ODK_JSON_OBJECT || key == NULL) {
        return NULL;
    }
    for (i = 0; i < object->member_count; i++) {
        if (strcmp(object->keys[i], key) == 0) {
            return object->members[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Writer                                                              */
/* ------------------------------------------------------------------ */

char *odk_json_string_literal(const char *utf8, size_t len)
{
    sb_t sb;
    size_t i;

    if (sb_init(&sb) != 0) {
        return NULL;
    }
    if (sb_putc(&sb, '"') != 0) {
        sb_free(&sb);
        return NULL;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)utf8[i];
        int rc = 0;
        switch (c) {
        case '"':
            rc = sb_puts(&sb, "\\\"");
            break;
        case '\\':
            rc = sb_puts(&sb, "\\\\");
            break;
        case '\b':
            rc = sb_puts(&sb, "\\b");
            break;
        case '\f':
            rc = sb_puts(&sb, "\\f");
            break;
        case '\n':
            rc = sb_puts(&sb, "\\n");
            break;
        case '\r':
            rc = sb_puts(&sb, "\\r");
            break;
        case '\t':
            rc = sb_puts(&sb, "\\t");
            break;
        default:
            if (c < 0x20) {
                rc = sb_printf(&sb, "\\u%04x", (unsigned)c);
            } else {
                rc = sb_putc(&sb, (char)c);
            }
            break;
        }
        if (rc != 0) {
            sb_free(&sb);
            return NULL;
        }
    }
    if (sb_putc(&sb, '"') != 0) {
        sb_free(&sb);
        return NULL;
    }
    return sb_detach(&sb);
}

char *odk_json_number_literal(double value)
{
    char buf[40];
    int prec;
    char *end = NULL;

    if (!isfinite(value)) {
        char *out = malloc(5);
        if (out != NULL) {
            memcpy(out, "null", 5);
        }
        return out;
    }
    for (prec = 15; prec <= 17; prec++) {
        snprintf(buf, sizeof(buf), "%.*g", prec, value);
        errno = 0;
        if (strtod(buf, &end) == value && end != NULL && *end == '\0') {
            break;
        }
    }
    /* Keep the float/int distinction visible, so a double never reads back as an integer. */
    if (strchr(buf, '.') == NULL && strchr(buf, 'e') == NULL &&
        strchr(buf, 'E') == NULL) {
        size_t len = strlen(buf);
        if (len + 2 < sizeof(buf)) {
            buf[len] = '.';
            buf[len + 1] = '0';
            buf[len + 2] = '\0';
        }
    }
    {
        char *out = malloc(strlen(buf) + 1);
        if (out != NULL) {
            strcpy(out, buf);
        }
        return out;
    }
}

char *odk_json_int_literal(long long value)
{
    char buf[32];
    char *out;
    int n = snprintf(buf, sizeof(buf), "%lld", value);
    if (n < 0 || (size_t)n >= sizeof(buf)) {
        return NULL;
    }
    out = malloc((size_t)n + 1);
    if (out != NULL) {
        memcpy(out, buf, (size_t)n + 1);
    }
    return out;
}

static int json_dump_into(const odk_json_value *value, sb_t *sb)
{
    size_t i;
    switch (value->type) {
    case ODK_JSON_NULL:
        return sb_puts(sb, "null");
    case ODK_JSON_BOOL:
        return sb_puts(sb, value->boolean ? "true" : "false");
    case ODK_JSON_NUMBER: {
        char *literal = value->is_integer
                            ? odk_json_int_literal(value->integer)
                            : odk_json_number_literal(value->number);
        int rc;
        if (literal == NULL) {
            return -1;
        }
        rc = sb_puts(sb, literal);
        free(literal);
        return rc;
    }
    case ODK_JSON_STRING: {
        char *literal = odk_json_string_literal(value->string,
                                                strlen(value->string));
        int rc;
        if (literal == NULL) {
            return -1;
        }
        rc = sb_puts(sb, literal);
        free(literal);
        return rc;
    }
    case ODK_JSON_ARRAY:
        if (sb_putc(sb, '[') != 0) {
            return -1;
        }
        for (i = 0; i < value->item_count; i++) {
            if (i > 0 && sb_putc(sb, ',') != 0) {
                return -1;
            }
            if (json_dump_into(value->items[i], sb) != 0) {
                return -1;
            }
        }
        return sb_putc(sb, ']');
    case ODK_JSON_OBJECT:
        if (sb_putc(sb, '{') != 0) {
            return -1;
        }
        for (i = 0; i < value->member_count; i++) {
            char *key_literal;
            int rc;
            if (i > 0 && sb_putc(sb, ',') != 0) {
                return -1;
            }
            key_literal = odk_json_string_literal(value->keys[i],
                                                  strlen(value->keys[i]));
            if (key_literal == NULL) {
                return -1;
            }
            rc = sb_puts(sb, key_literal);
            free(key_literal);
            if (rc != 0 || sb_putc(sb, ':') != 0) {
                return -1;
            }
            if (json_dump_into(value->members[i], sb) != 0) {
                return -1;
            }
        }
        return sb_putc(sb, '}');
    default:
        return -1;
    }
}

char *odk_json_dump(const odk_json_value *value)
{
    sb_t sb;
    if (value == NULL || sb_init(&sb) != 0) {
        return NULL;
    }
    if (json_dump_into(value, &sb) != 0) {
        sb_free(&sb);
        return NULL;
    }
    return sb_detach(&sb);
}
