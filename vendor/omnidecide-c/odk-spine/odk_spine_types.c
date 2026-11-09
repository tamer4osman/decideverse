/* GENERATED FILE - DO NOT EDIT.
 *
 * Source of truth: schema/omnidecide.schema
 * Generator:       tools/gen_types.py
 * Schema version:  1.0.0
 * Schema fingerprint (FNV-1a 64 of the schema file bytes): 88ffaefc9143dcc0
 *
 * Hand-maintained duplicates of these types are forbidden and CI-checked
 * (tools/check_schema_dup.sh). Regenerate with: python3 tools/gen_types.py
 */

#include "odk_spine_types.h"

/* ------------------------------------------------------------------ */
/* Generated helpers (private to this file)                            */
/* ------------------------------------------------------------------ */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#if defined(__GNUC__)
#define G_UNUSED __attribute__((unused))
#else
#define G_UNUSED
#endif

static void G_UNUSED gerr(odk_spine_error *err, const char *fmt, ...)
{
    va_list ap;

    if (err == NULL) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err->text, sizeof(err->text), fmt, ap);
    va_end(ap);
}

static char *G_UNUSED g_strdup(const char *s)
{
    size_t n;
    char *out;

    if (s == NULL) {
        return NULL;
    }
    n = strlen(s) + 1;
    out = malloc(n);
    if (out != NULL) {
        memcpy(out, s, n);
    }
    return out;
}

/* Growable output buffer (byte-deterministic; no locale, no wall clock). */
typedef struct {
    char *data;
    size_t len;
    size_t cap;
    int oom;
} gbuf;

typedef char *(*gbuf_obj_to_json)(const void *record);

static int G_UNUSED gbuf_init(gbuf *b)
{
    b->cap = 256;
    b->len = 0;
    b->oom = 0;
    b->data = malloc(b->cap);
    return b->data == NULL ? -1 : 0;
}

static void G_UNUSED gbuf_free(gbuf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static char *G_UNUSED gbuf_detach(gbuf *b)
{
    char *out = b->data;
    b->data = NULL;
    b->len = b->cap = 0;
    return out;
}

static int G_UNUSED gbuf_reserve(gbuf *b, size_t extra)
{
    size_t need = b->len + extra + 1;
    char *nd;

    if (b->oom) {
        return -1;
    }
    if (need <= b->cap) {
        return 0;
    }
    while (b->cap < need) {
        b->cap *= 2;
    }
    nd = realloc(b->data, b->cap);
    if (nd == NULL) {
        b->oom = 1;
        return -1;
    }
    b->data = nd;
    return 0;
}

static int G_UNUSED gbuf_putc(gbuf *b, char c)
{
    if (gbuf_reserve(b, 1) != 0) {
        return -1;
    }
    b->data[b->len++] = c;
    b->data[b->len] = '\0';
    return 0;
}

static int G_UNUSED gbuf_puts(gbuf *b, const char *s)
{
    size_t n = strlen(s);
    if (gbuf_reserve(b, n) != 0) {
        return -1;
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

static int G_UNUSED gbuf_put_json_string(gbuf *b, const char *s)
{
    char *lit;
    int rc;

    if (s == NULL) {
        s = "";
    }
    lit = odk_json_string_literal(s, strlen(s));
    if (lit == NULL) {
        return -1;
    }
    rc = gbuf_puts(b, lit);
    free(lit);
    return rc;
}

static int G_UNUSED gbuf_put_bool(gbuf *b, int v)
{
    return gbuf_puts(b, v ? "true" : "false");
}

static int G_UNUSED gbuf_put_int(gbuf *b, int64_t v)
{
    char *lit;
    int rc;

    lit = odk_json_int_literal((long long)v);
    if (lit == NULL) {
        return -1;
    }
    rc = gbuf_puts(b, lit);
    free(lit);
    return rc;
}

static int G_UNUSED gbuf_put_double(gbuf *b, double v)
{
    char *lit;
    int rc;

    if (!isfinite(v)) {
        return -1; /* schema forbids NaN/infinity in records */
    }
    lit = odk_json_number_literal(v);
    if (lit == NULL) {
        return -1;
    }
    rc = gbuf_puts(b, lit);
    free(lit);
    return rc;
}

static int G_UNUSED gbuf_put_str_list(gbuf *b, const odk_spine_str_list *l)
{
    size_t i;

    if (gbuf_putc(b, '[') != 0) {
        return -1;
    }
    for (i = 0; i < l->count; i++) {
        if (i > 0 && gbuf_putc(b, ',') != 0) {
            return -1;
        }
        if (gbuf_put_json_string(b, l->items[i]) != 0) {
            return -1;
        }
    }
    return gbuf_putc(b, ']');
}

static int G_UNUSED gbuf_put_map(gbuf *b, const odk_spine_map *m)
{
    size_t i;

    if (gbuf_putc(b, '{') != 0) {
        return -1;
    }
    for (i = 0; i < m->count; i++) {
        if (i > 0 && gbuf_putc(b, ',') != 0) {
            return -1;
        }
        if (gbuf_put_json_string(b, m->items[i].key) != 0 ||
            gbuf_putc(b, ':') != 0 ||
            gbuf_put_json_string(b, m->items[i].value) != 0) {
            return -1;
        }
    }
    return gbuf_putc(b, '}');
}

static int G_UNUSED gbuf_put_obj_list_s(gbuf *b, gbuf_obj_to_json to_json,
                               const void *items, size_t count,
                               size_t elem_size)
{
    size_t i;

    /* items only; the caller writes the surrounding brackets */
    for (i = 0; i < count; i++) {
        char *lit;
        int rc;

        if (i > 0 && gbuf_putc(b, ',') != 0) {
            return -1;
        }
        lit = to_json((const char *)items + i * elem_size);
        if (lit == NULL) {
            return -1;
        }
        rc = gbuf_puts(b, lit);
        free(lit);
        if (rc != 0) {
            return -1;
        }
    }
    return 0;
}

static int G_UNUSED gbuf_put_embedded(gbuf *b, gbuf_obj_to_json to_json,
                             const void *record)
{
    char *lit;
    int rc;

    lit = to_json(record);
    if (lit == NULL) {
        return -1;
    }
    rc = gbuf_puts(b, lit);
    free(lit);
    return rc;
}


void odk_spine_registration_ref_init(odk_spine_registration_ref *r)
{
    if (r != NULL) {
        memset(r, 0, sizeof(*r));
    }
}

void odk_spine_registration_ref_release(odk_spine_registration_ref *r)
{
    if (r == NULL) {
        return;
    }
    free(r->kind);
    free(r->id);
    free(r->content_hash);
    memset(r, 0, sizeof(*r));
}

char *odk_spine_registration_ref_to_json(const odk_spine_registration_ref *r)
{
    gbuf b;

    if (r == NULL || gbuf_init(&b) != 0) {
        return NULL;
    }
    if (gbuf_puts(&b, "{\"kind\":") != 0 ||
        gbuf_put_json_string(&b, r->kind) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"id\":") != 0 ||
        gbuf_put_json_string(&b, r->id) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"version\":") != 0 ||
        gbuf_put_int(&b, r->version) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"content_hash\":") != 0 ||
        gbuf_put_json_string(&b, r->content_hash) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_putc(&b, '}') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    return gbuf_detach(&b);
}

odk_spine_status odk_spine_registration_ref_from_dom(const odk_json_value *v,
                                       odk_spine_registration_ref *r,
                                       odk_spine_error *err)
{
    static const char *const g_fields[] = {
        "kind",
        "id",
        "version",
        "content_hash",
        NULL
    };
    const odk_json_value *fv;
    size_t i;
    size_t k;

    if (v == NULL || v->type != ODK_JSON_OBJECT) {
        gerr(err, "RegistrationRef: expected an object");
        return ODK_SPINE_ERR_SCHEMA;
    }
    for (k = 0; k < v->member_count; k++) {
        int known = 0;
        for (i = 0; g_fields[i] != NULL; i++) {
            if (strcmp(v->keys[k], g_fields[i]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            gerr(err, "RegistrationRef: unknown field '%s'", v->keys[k]);
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    fv = odk_json_object_get(v, "kind");
    if (fv == NULL) {
        gerr(err, "RegistrationRef.kind: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->kind = g_strdup(fv->string);
    if (r->kind == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "id");
    if (fv == NULL) {
        gerr(err, "RegistrationRef.id: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->id = g_strdup(fv->string);
    if (r->id == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "version");
    if (fv == NULL) {
        gerr(err, "RegistrationRef.version: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {
        gerr(err, "RegistrationRef.version: expected an integer");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->version = fv->integer;
    fv = odk_json_object_get(v, "content_hash");
    if (fv == NULL) {
        gerr(err, "RegistrationRef.content_hash: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->content_hash = g_strdup(fv->string);
    if (r->content_hash == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    return ODK_SPINE_OK;
}

odk_spine_status odk_spine_registration_ref_from_json(const char *text,
                                        odk_spine_registration_ref *out,
                                        odk_spine_error *err)
{
    odk_json_value *root;
    odk_error jerr;
    odk_spine_status st;

    if (text == NULL || out == NULL) {
        gerr(err, "RegistrationRef: null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    odk_spine_registration_ref_init(out);
    root = odk_json_parse(text, strlen(text), &jerr);
    if (root == NULL) {
        gerr(err, "RegistrationRef: invalid JSON: %s", jerr.text);
        return ODK_SPINE_ERR_SCHEMA;
    }
    st = odk_spine_registration_ref_from_dom(root, out, err);
    odk_json_free(root);
    if (st != ODK_SPINE_OK) {
        odk_spine_registration_ref_release(out);
        return st;
    }
    return ODK_SPINE_OK;
}

void odk_spine_observation_init(odk_spine_observation *r)
{
    if (r != NULL) {
        memset(r, 0, sizeof(*r));
    }
}

void odk_spine_observation_release(odk_spine_observation *r)
{
    size_t i;
    if (r == NULL) {
        return;
    }
    free(r->observation_id);
    free(r->kind);
    free(r->scope);
    free(r->subject_ref);
    free(r->primitive);
    free(r->scheme_id);
    free(r->payload_json);
    free(r->tier);
    free(r->brain_snapshot_id);
    free(r->outcome);
    free(r->kernel_version);
    free(r->schema_fp);
    for (i = 0; i < r->registrations.count; i++) {
        odk_spine_registration_ref_release(&r->registrations.items[i]);
    }
    free(r->registrations.items);
    memset(r, 0, sizeof(*r));
}

char *odk_spine_observation_to_json(const odk_spine_observation *r)
{
    gbuf b;

    if (r == NULL || gbuf_init(&b) != 0) {
        return NULL;
    }
    if (gbuf_puts(&b, "{\"observation_id\":") != 0 ||
        gbuf_put_json_string(&b, r->observation_id) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"kind\":") != 0 ||
        gbuf_put_json_string(&b, r->kind) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"t\":") != 0 ||
        gbuf_put_int(&b, r->t) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"scope\":") != 0 ||
        gbuf_put_json_string(&b, r->scope) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"subject_ref\":") != 0 ||
        gbuf_put_json_string(&b, r->subject_ref) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"primitive\":") != 0 ||
        gbuf_put_json_string(&b, r->primitive) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"scheme_id\":") != 0 ||
        gbuf_put_json_string(&b, r->scheme_id) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"payload_json\":") != 0 ||
        gbuf_put_json_string(&b, r->payload_json) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"confidence\":") != 0 ||
        gbuf_put_double(&b, r->confidence) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"tier\":") != 0 ||
        gbuf_put_json_string(&b, r->tier) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"brain_snapshot_id\":") != 0 ||
        gbuf_put_json_string(&b, r->brain_snapshot_id) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"outcome\":") != 0 ||
        gbuf_put_json_string(&b, r->outcome) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"kernel_version\":") != 0 ||
        gbuf_put_json_string(&b, r->kernel_version) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"schema_fp\":") != 0 ||
        gbuf_put_json_string(&b, r->schema_fp) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"registrations\":") != 0 ||
        gbuf_putc(&b, '[') != 0 ||
        gbuf_put_obj_list_s(&b, (gbuf_obj_to_json)odk_spine_registration_ref_to_json,
                           r->registrations.items,
                           r->registrations.count,
                           sizeof(odk_spine_registration_ref)) != 0 ||
        gbuf_putc(&b, ']') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_putc(&b, '}') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    return gbuf_detach(&b);
}

odk_spine_status odk_spine_observation_from_dom(const odk_json_value *v,
                                       odk_spine_observation *r,
                                       odk_spine_error *err)
{
    static const char *const g_fields[] = {
        "observation_id",
        "kind",
        "t",
        "scope",
        "subject_ref",
        "primitive",
        "scheme_id",
        "payload_json",
        "confidence",
        "tier",
        "brain_snapshot_id",
        "outcome",
        "kernel_version",
        "schema_fp",
        "registrations",
        NULL
    };
    const odk_json_value *fv;
    size_t i;
    size_t k;

    if (v == NULL || v->type != ODK_JSON_OBJECT) {
        gerr(err, "Observation: expected an object");
        return ODK_SPINE_ERR_SCHEMA;
    }
    for (k = 0; k < v->member_count; k++) {
        int known = 0;
        for (i = 0; g_fields[i] != NULL; i++) {
            if (strcmp(v->keys[k], g_fields[i]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            gerr(err, "Observation: unknown field '%s'", v->keys[k]);
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    fv = odk_json_object_get(v, "observation_id");
    if (fv == NULL) {
        gerr(err, "Observation.observation_id: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->observation_id = g_strdup(fv->string);
    if (r->observation_id == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "kind");
    if (fv == NULL) {
        gerr(err, "Observation.kind: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_STRING) {
        gerr(err, "Observation.kind: expected a string");
        return ODK_SPINE_ERR_SCHEMA;
    }
    {
        int ok_av = 0;
        static const char *const g_av[] = {
            "decision",
            "classification",
            "outcome",
            "custom",
            NULL
        };
        for (i = 0; g_av[i] != NULL; i++) {
            if (strcmp(fv->string, g_av[i]) == 0) {
                ok_av = 1;
                break;
            }
        }
        if (!ok_av) {
            gerr(err, "Observation.kind: value not allowed");
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    r->kind = g_strdup(fv->string);
    if (r->kind == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "t");
    if (fv == NULL) {
        gerr(err, "Observation.t: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {
        gerr(err, "Observation.t: expected an integer");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->t = fv->integer;
    fv = odk_json_object_get(v, "scope");
    if (fv == NULL) {
        gerr(err, "Observation.scope: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->scope = g_strdup(fv->string);
    if (r->scope == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "subject_ref");
    if (fv == NULL) {
        gerr(err, "Observation.subject_ref: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->subject_ref = g_strdup(fv->string);
    if (r->subject_ref == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "primitive");
    if (fv == NULL) {
        gerr(err, "Observation.primitive: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->primitive = g_strdup(fv->string);
    if (r->primitive == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "scheme_id");
    if (fv == NULL) {
        gerr(err, "Observation.scheme_id: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->scheme_id = g_strdup(fv->string);
    if (r->scheme_id == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "payload_json");
    if (fv == NULL) {
        gerr(err, "Observation.payload_json: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->payload_json = g_strdup(fv->string);
    if (r->payload_json == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "confidence");
    if (fv == NULL) {
        gerr(err, "Observation.confidence: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER) {
        gerr(err, "Observation.confidence: expected a number");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->confidence = fv->number;
    fv = odk_json_object_get(v, "tier");
    if (fv == NULL) {
        gerr(err, "Observation.tier: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_STRING) {
        gerr(err, "Observation.tier: expected a string");
        return ODK_SPINE_ERR_SCHEMA;
    }
    {
        int ok_av = 0;
        static const char *const g_av[] = {
            "",
            "A",
            "B",
            NULL
        };
        for (i = 0; g_av[i] != NULL; i++) {
            if (strcmp(fv->string, g_av[i]) == 0) {
                ok_av = 1;
                break;
            }
        }
        if (!ok_av) {
            gerr(err, "Observation.tier: value not allowed");
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    r->tier = g_strdup(fv->string);
    if (r->tier == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "brain_snapshot_id");
    if (fv == NULL) {
        gerr(err, "Observation.brain_snapshot_id: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->brain_snapshot_id = g_strdup(fv->string);
    if (r->brain_snapshot_id == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "outcome");
    if (fv == NULL) {
        gerr(err, "Observation.outcome: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_STRING) {
        gerr(err, "Observation.outcome: expected a string");
        return ODK_SPINE_ERR_SCHEMA;
    }
    {
        int ok_av = 0;
        static const char *const g_av[] = {
            "",
            "success",
            "failure",
            NULL
        };
        for (i = 0; g_av[i] != NULL; i++) {
            if (strcmp(fv->string, g_av[i]) == 0) {
                ok_av = 1;
                break;
            }
        }
        if (!ok_av) {
            gerr(err, "Observation.outcome: value not allowed");
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    r->outcome = g_strdup(fv->string);
    if (r->outcome == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "kernel_version");
    if (fv == NULL) {
        gerr(err, "Observation.kernel_version: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->kernel_version = g_strdup(fv->string);
    if (r->kernel_version == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "schema_fp");
    if (fv == NULL) {
        gerr(err, "Observation.schema_fp: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->schema_fp = g_strdup(fv->string);
    if (r->schema_fp == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "registrations");
    if (fv == NULL) {
        gerr(err, "Observation.registrations: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_ARRAY) {
        gerr(err, "Observation.registrations: expected an array");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->item_count > 0) {
        r->registrations.items = calloc(fv->item_count, sizeof(odk_spine_registration_ref));
        if (r->registrations.items == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    for (i = 0; i < fv->item_count; i++) {
        odk_spine_status st = odk_spine_registration_ref_from_dom(fv->items[i],
                                    &r->registrations.items[i], err);
        if (st != ODK_SPINE_OK) {
            return st;
        }
    }
    r->registrations.count = fv->item_count;
    return ODK_SPINE_OK;
}

odk_spine_status odk_spine_observation_from_json(const char *text,
                                        odk_spine_observation *out,
                                        odk_spine_error *err)
{
    odk_json_value *root;
    odk_error jerr;
    odk_spine_status st;

    if (text == NULL || out == NULL) {
        gerr(err, "Observation: null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    odk_spine_observation_init(out);
    root = odk_json_parse(text, strlen(text), &jerr);
    if (root == NULL) {
        gerr(err, "Observation: invalid JSON: %s", jerr.text);
        return ODK_SPINE_ERR_SCHEMA;
    }
    st = odk_spine_observation_from_dom(root, out, err);
    odk_json_free(root);
    if (st != ODK_SPINE_OK) {
        odk_spine_observation_release(out);
        return st;
    }
    return ODK_SPINE_OK;
}

void odk_spine_scheme_rule_init(odk_spine_scheme_rule *r)
{
    if (r != NULL) {
        memset(r, 0, sizeof(*r));
    }
}

void odk_spine_scheme_rule_release(odk_spine_scheme_rule *r)
{
    size_t i;
    if (r == NULL) {
        return;
    }
    free(r->label);
    for (i = 0; i < r->match_terms.count; i++) {
        free(r->match_terms.items[i]);
    }
    free(r->match_terms.items);
    for (i = 0; i < r->facets.count; i++) {
        free(r->facets.items[i].key);
        free(r->facets.items[i].value);
    }
    free(r->facets.items);
    memset(r, 0, sizeof(*r));
}

char *odk_spine_scheme_rule_to_json(const odk_spine_scheme_rule *r)
{
    gbuf b;

    if (r == NULL || gbuf_init(&b) != 0) {
        return NULL;
    }
    if (gbuf_puts(&b, "{\"label\":") != 0 ||
        gbuf_put_json_string(&b, r->label) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"match_terms\":") != 0 ||
        gbuf_put_str_list(&b, &r->match_terms) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"any_term\":") != 0 ||
        gbuf_put_bool(&b, r->any_term) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"priority\":") != 0 ||
        gbuf_put_int(&b, r->priority) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"facets\":") != 0 ||
        gbuf_put_map(&b, &r->facets) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_putc(&b, '}') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    return gbuf_detach(&b);
}

odk_spine_status odk_spine_scheme_rule_from_dom(const odk_json_value *v,
                                       odk_spine_scheme_rule *r,
                                       odk_spine_error *err)
{
    static const char *const g_fields[] = {
        "label",
        "match_terms",
        "any_term",
        "priority",
        "facets",
        NULL
    };
    const odk_json_value *fv;
    size_t i;
    size_t k;

    if (v == NULL || v->type != ODK_JSON_OBJECT) {
        gerr(err, "SchemeRule: expected an object");
        return ODK_SPINE_ERR_SCHEMA;
    }
    for (k = 0; k < v->member_count; k++) {
        int known = 0;
        for (i = 0; g_fields[i] != NULL; i++) {
            if (strcmp(v->keys[k], g_fields[i]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            gerr(err, "SchemeRule: unknown field '%s'", v->keys[k]);
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    fv = odk_json_object_get(v, "label");
    if (fv == NULL) {
        gerr(err, "SchemeRule.label: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->label = g_strdup(fv->string);
    if (r->label == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "match_terms");
    if (fv == NULL) {
        gerr(err, "SchemeRule.match_terms: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_ARRAY) {
        gerr(err, "SchemeRule.match_terms: expected an array of strings");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->item_count > 0) {
        r->match_terms.items = calloc(fv->item_count, sizeof(char *));
        if (r->match_terms.items == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    for (i = 0; i < fv->item_count; i++) {
        if (fv->items[i]->type != ODK_JSON_STRING) {
            gerr(err, "SchemeRule.match_terms[%zu]: expected a string", i);
            return ODK_SPINE_ERR_SCHEMA;
        }
        r->match_terms.items[i] = g_strdup(fv->items[i]->string);
        if (r->match_terms.items[i] == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    r->match_terms.count = fv->item_count;
    fv = odk_json_object_get(v, "any_term");
    if (fv == NULL) {
        gerr(err, "SchemeRule.any_term: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_BOOL) {
        gerr(err, "SchemeRule.any_term: expected true or false");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->any_term = fv->boolean;
    fv = odk_json_object_get(v, "priority");
    if (fv == NULL) {
        gerr(err, "SchemeRule.priority: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {
        gerr(err, "SchemeRule.priority: expected an integer");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->priority = fv->integer;
    fv = odk_json_object_get(v, "facets");
    if (fv == NULL) {
        gerr(err, "SchemeRule.facets: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_OBJECT) {
        gerr(err, "SchemeRule.facets: expected an object");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->member_count > 0) {
        r->facets.items = calloc(fv->member_count, sizeof(odk_spine_kv));
        if (r->facets.items == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    for (i = 0; i < fv->member_count; i++) {
        if (fv->members[i]->type != ODK_JSON_STRING) {
            gerr(err, "SchemeRule.facets: values must be strings");
            return ODK_SPINE_ERR_SCHEMA;
        }
        r->facets.items[i].key = g_strdup(fv->keys[i]);
        r->facets.items[i].value = g_strdup(fv->members[i]->string);
        if (r->facets.items[i].key == NULL ||
            r->facets.items[i].value == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    r->facets.count = fv->member_count;
    return ODK_SPINE_OK;
}

odk_spine_status odk_spine_scheme_rule_from_json(const char *text,
                                        odk_spine_scheme_rule *out,
                                        odk_spine_error *err)
{
    odk_json_value *root;
    odk_error jerr;
    odk_spine_status st;

    if (text == NULL || out == NULL) {
        gerr(err, "SchemeRule: null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    odk_spine_scheme_rule_init(out);
    root = odk_json_parse(text, strlen(text), &jerr);
    if (root == NULL) {
        gerr(err, "SchemeRule: invalid JSON: %s", jerr.text);
        return ODK_SPINE_ERR_SCHEMA;
    }
    st = odk_spine_scheme_rule_from_dom(root, out, err);
    odk_json_free(root);
    if (st != ODK_SPINE_OK) {
        odk_spine_scheme_rule_release(out);
        return st;
    }
    return ODK_SPINE_OK;
}

void odk_spine_scheme_init(odk_spine_scheme *r)
{
    if (r != NULL) {
        memset(r, 0, sizeof(*r));
    }
}

void odk_spine_scheme_release(odk_spine_scheme *r)
{
    size_t i;
    if (r == NULL) {
        return;
    }
    free(r->id);
    free(r->default_label);
    for (i = 0; i < r->facet_names.count; i++) {
        free(r->facet_names.items[i]);
    }
    free(r->facet_names.items);
    for (i = 0; i < r->rules.count; i++) {
        odk_spine_scheme_rule_release(&r->rules.items[i]);
    }
    free(r->rules.items);
    free(r->notes);
    memset(r, 0, sizeof(*r));
}

char *odk_spine_scheme_to_json(const odk_spine_scheme *r)
{
    gbuf b;

    if (r == NULL || gbuf_init(&b) != 0) {
        return NULL;
    }
    if (gbuf_puts(&b, "{\"id\":") != 0 ||
        gbuf_put_json_string(&b, r->id) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"version\":") != 0 ||
        gbuf_put_int(&b, r->version) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"valid_from\":") != 0 ||
        gbuf_put_int(&b, r->valid_from) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"valid_to\":") != 0 ||
        gbuf_put_int(&b, r->valid_to) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"default_label\":") != 0 ||
        gbuf_put_json_string(&b, r->default_label) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"facet_names\":") != 0 ||
        gbuf_put_str_list(&b, &r->facet_names) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"rules\":") != 0 ||
        gbuf_putc(&b, '[') != 0 ||
        gbuf_put_obj_list_s(&b, (gbuf_obj_to_json)odk_spine_scheme_rule_to_json,
                           r->rules.items,
                           r->rules.count,
                           sizeof(odk_spine_scheme_rule)) != 0 ||
        gbuf_putc(&b, ']') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"notes\":") != 0 ||
        gbuf_put_json_string(&b, r->notes) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_putc(&b, '}') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    return gbuf_detach(&b);
}

odk_spine_status odk_spine_scheme_from_dom(const odk_json_value *v,
                                       odk_spine_scheme *r,
                                       odk_spine_error *err)
{
    static const char *const g_fields[] = {
        "id",
        "version",
        "valid_from",
        "valid_to",
        "default_label",
        "facet_names",
        "rules",
        "notes",
        NULL
    };
    const odk_json_value *fv;
    size_t i;
    size_t k;

    if (v == NULL || v->type != ODK_JSON_OBJECT) {
        gerr(err, "Scheme: expected an object");
        return ODK_SPINE_ERR_SCHEMA;
    }
    for (k = 0; k < v->member_count; k++) {
        int known = 0;
        for (i = 0; g_fields[i] != NULL; i++) {
            if (strcmp(v->keys[k], g_fields[i]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            gerr(err, "Scheme: unknown field '%s'", v->keys[k]);
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    fv = odk_json_object_get(v, "id");
    if (fv == NULL) {
        gerr(err, "Scheme.id: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->id = g_strdup(fv->string);
    if (r->id == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "version");
    if (fv == NULL) {
        gerr(err, "Scheme.version: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {
        gerr(err, "Scheme.version: expected an integer");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->version = fv->integer;
    fv = odk_json_object_get(v, "valid_from");
    if (fv == NULL) {
        gerr(err, "Scheme.valid_from: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {
        gerr(err, "Scheme.valid_from: expected an integer");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->valid_from = fv->integer;
    fv = odk_json_object_get(v, "valid_to");
    if (fv == NULL) {
        gerr(err, "Scheme.valid_to: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {
        gerr(err, "Scheme.valid_to: expected an integer");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->valid_to = fv->integer;
    fv = odk_json_object_get(v, "default_label");
    if (fv == NULL) {
        gerr(err, "Scheme.default_label: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->default_label = g_strdup(fv->string);
    if (r->default_label == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "facet_names");
    if (fv == NULL) {
        gerr(err, "Scheme.facet_names: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_ARRAY) {
        gerr(err, "Scheme.facet_names: expected an array of strings");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->item_count > 0) {
        r->facet_names.items = calloc(fv->item_count, sizeof(char *));
        if (r->facet_names.items == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    for (i = 0; i < fv->item_count; i++) {
        if (fv->items[i]->type != ODK_JSON_STRING) {
            gerr(err, "Scheme.facet_names[%zu]: expected a string", i);
            return ODK_SPINE_ERR_SCHEMA;
        }
        r->facet_names.items[i] = g_strdup(fv->items[i]->string);
        if (r->facet_names.items[i] == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    r->facet_names.count = fv->item_count;
    fv = odk_json_object_get(v, "rules");
    if (fv == NULL) {
        gerr(err, "Scheme.rules: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_ARRAY) {
        gerr(err, "Scheme.rules: expected an array");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->item_count > 0) {
        r->rules.items = calloc(fv->item_count, sizeof(odk_spine_scheme_rule));
        if (r->rules.items == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    for (i = 0; i < fv->item_count; i++) {
        odk_spine_status st = odk_spine_scheme_rule_from_dom(fv->items[i],
                                    &r->rules.items[i], err);
        if (st != ODK_SPINE_OK) {
            return st;
        }
    }
    r->rules.count = fv->item_count;
    fv = odk_json_object_get(v, "notes");
    if (fv == NULL) {
        gerr(err, "Scheme.notes: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->notes = g_strdup(fv->string);
    if (r->notes == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    return ODK_SPINE_OK;
}

odk_spine_status odk_spine_scheme_from_json(const char *text,
                                        odk_spine_scheme *out,
                                        odk_spine_error *err)
{
    odk_json_value *root;
    odk_error jerr;
    odk_spine_status st;

    if (text == NULL || out == NULL) {
        gerr(err, "Scheme: null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    odk_spine_scheme_init(out);
    root = odk_json_parse(text, strlen(text), &jerr);
    if (root == NULL) {
        gerr(err, "Scheme: invalid JSON: %s", jerr.text);
        return ODK_SPINE_ERR_SCHEMA;
    }
    st = odk_spine_scheme_from_dom(root, out, err);
    odk_json_free(root);
    if (st != ODK_SPINE_OK) {
        odk_spine_scheme_release(out);
        return st;
    }
    return ODK_SPINE_OK;
}

void odk_spine_synonym_group_init(odk_spine_synonym_group *r)
{
    if (r != NULL) {
        memset(r, 0, sizeof(*r));
    }
}

void odk_spine_synonym_group_release(odk_spine_synonym_group *r)
{
    size_t i;
    if (r == NULL) {
        return;
    }
    free(r->canonical);
    for (i = 0; i < r->terms.count; i++) {
        free(r->terms.items[i]);
    }
    free(r->terms.items);
    memset(r, 0, sizeof(*r));
}

char *odk_spine_synonym_group_to_json(const odk_spine_synonym_group *r)
{
    gbuf b;

    if (r == NULL || gbuf_init(&b) != 0) {
        return NULL;
    }
    if (gbuf_puts(&b, "{\"canonical\":") != 0 ||
        gbuf_put_json_string(&b, r->canonical) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"terms\":") != 0 ||
        gbuf_put_str_list(&b, &r->terms) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_putc(&b, '}') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    return gbuf_detach(&b);
}

odk_spine_status odk_spine_synonym_group_from_dom(const odk_json_value *v,
                                       odk_spine_synonym_group *r,
                                       odk_spine_error *err)
{
    static const char *const g_fields[] = {
        "canonical",
        "terms",
        NULL
    };
    const odk_json_value *fv;
    size_t i;
    size_t k;

    if (v == NULL || v->type != ODK_JSON_OBJECT) {
        gerr(err, "SynonymGroup: expected an object");
        return ODK_SPINE_ERR_SCHEMA;
    }
    for (k = 0; k < v->member_count; k++) {
        int known = 0;
        for (i = 0; g_fields[i] != NULL; i++) {
            if (strcmp(v->keys[k], g_fields[i]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            gerr(err, "SynonymGroup: unknown field '%s'", v->keys[k]);
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    fv = odk_json_object_get(v, "canonical");
    if (fv == NULL) {
        gerr(err, "SynonymGroup.canonical: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->canonical = g_strdup(fv->string);
    if (r->canonical == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "terms");
    if (fv == NULL) {
        gerr(err, "SynonymGroup.terms: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_ARRAY) {
        gerr(err, "SynonymGroup.terms: expected an array of strings");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->item_count > 0) {
        r->terms.items = calloc(fv->item_count, sizeof(char *));
        if (r->terms.items == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    for (i = 0; i < fv->item_count; i++) {
        if (fv->items[i]->type != ODK_JSON_STRING) {
            gerr(err, "SynonymGroup.terms[%zu]: expected a string", i);
            return ODK_SPINE_ERR_SCHEMA;
        }
        r->terms.items[i] = g_strdup(fv->items[i]->string);
        if (r->terms.items[i] == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    r->terms.count = fv->item_count;
    return ODK_SPINE_OK;
}

odk_spine_status odk_spine_synonym_group_from_json(const char *text,
                                        odk_spine_synonym_group *out,
                                        odk_spine_error *err)
{
    odk_json_value *root;
    odk_error jerr;
    odk_spine_status st;

    if (text == NULL || out == NULL) {
        gerr(err, "SynonymGroup: null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    odk_spine_synonym_group_init(out);
    root = odk_json_parse(text, strlen(text), &jerr);
    if (root == NULL) {
        gerr(err, "SynonymGroup: invalid JSON: %s", jerr.text);
        return ODK_SPINE_ERR_SCHEMA;
    }
    st = odk_spine_synonym_group_from_dom(root, out, err);
    odk_json_free(root);
    if (st != ODK_SPINE_OK) {
        odk_spine_synonym_group_release(out);
        return st;
    }
    return ODK_SPINE_OK;
}

void odk_spine_vocabulary_init(odk_spine_vocabulary *r)
{
    if (r != NULL) {
        memset(r, 0, sizeof(*r));
    }
}

void odk_spine_vocabulary_release(odk_spine_vocabulary *r)
{
    size_t i;
    if (r == NULL) {
        return;
    }
    free(r->id);
    for (i = 0; i < r->groups.count; i++) {
        odk_spine_synonym_group_release(&r->groups.items[i]);
    }
    free(r->groups.items);
    memset(r, 0, sizeof(*r));
}

char *odk_spine_vocabulary_to_json(const odk_spine_vocabulary *r)
{
    gbuf b;

    if (r == NULL || gbuf_init(&b) != 0) {
        return NULL;
    }
    if (gbuf_puts(&b, "{\"id\":") != 0 ||
        gbuf_put_json_string(&b, r->id) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"version\":") != 0 ||
        gbuf_put_int(&b, r->version) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"valid_from\":") != 0 ||
        gbuf_put_int(&b, r->valid_from) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"valid_to\":") != 0 ||
        gbuf_put_int(&b, r->valid_to) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"groups\":") != 0 ||
        gbuf_putc(&b, '[') != 0 ||
        gbuf_put_obj_list_s(&b, (gbuf_obj_to_json)odk_spine_synonym_group_to_json,
                           r->groups.items,
                           r->groups.count,
                           sizeof(odk_spine_synonym_group)) != 0 ||
        gbuf_putc(&b, ']') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_putc(&b, '}') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    return gbuf_detach(&b);
}

odk_spine_status odk_spine_vocabulary_from_dom(const odk_json_value *v,
                                       odk_spine_vocabulary *r,
                                       odk_spine_error *err)
{
    static const char *const g_fields[] = {
        "id",
        "version",
        "valid_from",
        "valid_to",
        "groups",
        NULL
    };
    const odk_json_value *fv;
    size_t i;
    size_t k;

    if (v == NULL || v->type != ODK_JSON_OBJECT) {
        gerr(err, "Vocabulary: expected an object");
        return ODK_SPINE_ERR_SCHEMA;
    }
    for (k = 0; k < v->member_count; k++) {
        int known = 0;
        for (i = 0; g_fields[i] != NULL; i++) {
            if (strcmp(v->keys[k], g_fields[i]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            gerr(err, "Vocabulary: unknown field '%s'", v->keys[k]);
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    fv = odk_json_object_get(v, "id");
    if (fv == NULL) {
        gerr(err, "Vocabulary.id: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->id = g_strdup(fv->string);
    if (r->id == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "version");
    if (fv == NULL) {
        gerr(err, "Vocabulary.version: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {
        gerr(err, "Vocabulary.version: expected an integer");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->version = fv->integer;
    fv = odk_json_object_get(v, "valid_from");
    if (fv == NULL) {
        gerr(err, "Vocabulary.valid_from: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {
        gerr(err, "Vocabulary.valid_from: expected an integer");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->valid_from = fv->integer;
    fv = odk_json_object_get(v, "valid_to");
    if (fv == NULL) {
        gerr(err, "Vocabulary.valid_to: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {
        gerr(err, "Vocabulary.valid_to: expected an integer");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->valid_to = fv->integer;
    fv = odk_json_object_get(v, "groups");
    if (fv == NULL) {
        gerr(err, "Vocabulary.groups: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_ARRAY) {
        gerr(err, "Vocabulary.groups: expected an array");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->item_count > 0) {
        r->groups.items = calloc(fv->item_count, sizeof(odk_spine_synonym_group));
        if (r->groups.items == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    for (i = 0; i < fv->item_count; i++) {
        odk_spine_status st = odk_spine_synonym_group_from_dom(fv->items[i],
                                    &r->groups.items[i], err);
        if (st != ODK_SPINE_OK) {
            return st;
        }
    }
    r->groups.count = fv->item_count;
    return ODK_SPINE_OK;
}

odk_spine_status odk_spine_vocabulary_from_json(const char *text,
                                        odk_spine_vocabulary *out,
                                        odk_spine_error *err)
{
    odk_json_value *root;
    odk_error jerr;
    odk_spine_status st;

    if (text == NULL || out == NULL) {
        gerr(err, "Vocabulary: null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    odk_spine_vocabulary_init(out);
    root = odk_json_parse(text, strlen(text), &jerr);
    if (root == NULL) {
        gerr(err, "Vocabulary: invalid JSON: %s", jerr.text);
        return ODK_SPINE_ERR_SCHEMA;
    }
    st = odk_spine_vocabulary_from_dom(root, out, err);
    odk_json_free(root);
    if (st != ODK_SPINE_OK) {
        odk_spine_vocabulary_release(out);
        return st;
    }
    return ODK_SPINE_OK;
}

void odk_spine_classification_init(odk_spine_classification *r)
{
    if (r != NULL) {
        memset(r, 0, sizeof(*r));
    }
}

void odk_spine_classification_release(odk_spine_classification *r)
{
    size_t i;
    if (r == NULL) {
        return;
    }
    free(r->scheme_id);
    free(r->label);
    for (i = 0; i < r->facets.count; i++) {
        free(r->facets.items[i].key);
        free(r->facets.items[i].value);
    }
    free(r->facets.items);
    free(r->tier);
    free(r->brain_snapshot_id);
    for (i = 0; i < r->matched_terms.count; i++) {
        free(r->matched_terms.items[i]);
    }
    free(r->matched_terms.items);
    memset(r, 0, sizeof(*r));
}

char *odk_spine_classification_to_json(const odk_spine_classification *r)
{
    gbuf b;

    if (r == NULL || gbuf_init(&b) != 0) {
        return NULL;
    }
    if (gbuf_puts(&b, "{\"scheme_id\":") != 0 ||
        gbuf_put_json_string(&b, r->scheme_id) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"scheme_version\":") != 0 ||
        gbuf_put_int(&b, r->scheme_version) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"label\":") != 0 ||
        gbuf_put_json_string(&b, r->label) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"facets\":") != 0 ||
        gbuf_put_map(&b, &r->facets) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"tier\":") != 0 ||
        gbuf_put_json_string(&b, r->tier) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"brain_snapshot_id\":") != 0 ||
        gbuf_put_json_string(&b, r->brain_snapshot_id) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"confidence\":") != 0 ||
        gbuf_put_double(&b, r->confidence) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"matched_terms\":") != 0 ||
        gbuf_put_str_list(&b, &r->matched_terms) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"rule_index\":") != 0 ||
        gbuf_put_int(&b, r->rule_index) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_putc(&b, '}') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    return gbuf_detach(&b);
}

odk_spine_status odk_spine_classification_from_dom(const odk_json_value *v,
                                       odk_spine_classification *r,
                                       odk_spine_error *err)
{
    static const char *const g_fields[] = {
        "scheme_id",
        "scheme_version",
        "label",
        "facets",
        "tier",
        "brain_snapshot_id",
        "confidence",
        "matched_terms",
        "rule_index",
        NULL
    };
    const odk_json_value *fv;
    size_t i;
    size_t k;

    if (v == NULL || v->type != ODK_JSON_OBJECT) {
        gerr(err, "Classification: expected an object");
        return ODK_SPINE_ERR_SCHEMA;
    }
    for (k = 0; k < v->member_count; k++) {
        int known = 0;
        for (i = 0; g_fields[i] != NULL; i++) {
            if (strcmp(v->keys[k], g_fields[i]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            gerr(err, "Classification: unknown field '%s'", v->keys[k]);
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    fv = odk_json_object_get(v, "scheme_id");
    if (fv == NULL) {
        gerr(err, "Classification.scheme_id: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->scheme_id = g_strdup(fv->string);
    if (r->scheme_id == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "scheme_version");
    if (fv == NULL) {
        gerr(err, "Classification.scheme_version: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {
        gerr(err, "Classification.scheme_version: expected an integer");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->scheme_version = fv->integer;
    fv = odk_json_object_get(v, "label");
    if (fv == NULL) {
        gerr(err, "Classification.label: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->label = g_strdup(fv->string);
    if (r->label == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "facets");
    if (fv == NULL) {
        gerr(err, "Classification.facets: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_OBJECT) {
        gerr(err, "Classification.facets: expected an object");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->member_count > 0) {
        r->facets.items = calloc(fv->member_count, sizeof(odk_spine_kv));
        if (r->facets.items == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    for (i = 0; i < fv->member_count; i++) {
        if (fv->members[i]->type != ODK_JSON_STRING) {
            gerr(err, "Classification.facets: values must be strings");
            return ODK_SPINE_ERR_SCHEMA;
        }
        r->facets.items[i].key = g_strdup(fv->keys[i]);
        r->facets.items[i].value = g_strdup(fv->members[i]->string);
        if (r->facets.items[i].key == NULL ||
            r->facets.items[i].value == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    r->facets.count = fv->member_count;
    fv = odk_json_object_get(v, "tier");
    if (fv == NULL) {
        gerr(err, "Classification.tier: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_STRING) {
        gerr(err, "Classification.tier: expected a string");
        return ODK_SPINE_ERR_SCHEMA;
    }
    {
        int ok_av = 0;
        static const char *const g_av[] = {
            "A",
            "B",
            NULL
        };
        for (i = 0; g_av[i] != NULL; i++) {
            if (strcmp(fv->string, g_av[i]) == 0) {
                ok_av = 1;
                break;
            }
        }
        if (!ok_av) {
            gerr(err, "Classification.tier: value not allowed");
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    r->tier = g_strdup(fv->string);
    if (r->tier == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "brain_snapshot_id");
    if (fv == NULL) {
        gerr(err, "Classification.brain_snapshot_id: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->brain_snapshot_id = g_strdup(fv->string);
    if (r->brain_snapshot_id == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "confidence");
    if (fv == NULL) {
        gerr(err, "Classification.confidence: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER) {
        gerr(err, "Classification.confidence: expected a number");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->confidence = fv->number;
    fv = odk_json_object_get(v, "matched_terms");
    if (fv == NULL) {
        gerr(err, "Classification.matched_terms: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_ARRAY) {
        gerr(err, "Classification.matched_terms: expected an array of strings");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->item_count > 0) {
        r->matched_terms.items = calloc(fv->item_count, sizeof(char *));
        if (r->matched_terms.items == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    for (i = 0; i < fv->item_count; i++) {
        if (fv->items[i]->type != ODK_JSON_STRING) {
            gerr(err, "Classification.matched_terms[%zu]: expected a string", i);
            return ODK_SPINE_ERR_SCHEMA;
        }
        r->matched_terms.items[i] = g_strdup(fv->items[i]->string);
        if (r->matched_terms.items[i] == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    r->matched_terms.count = fv->item_count;
    fv = odk_json_object_get(v, "rule_index");
    if (fv == NULL) {
        gerr(err, "Classification.rule_index: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {
        gerr(err, "Classification.rule_index: expected an integer");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->rule_index = fv->integer;
    return ODK_SPINE_OK;
}

odk_spine_status odk_spine_classification_from_json(const char *text,
                                        odk_spine_classification *out,
                                        odk_spine_error *err)
{
    odk_json_value *root;
    odk_error jerr;
    odk_spine_status st;

    if (text == NULL || out == NULL) {
        gerr(err, "Classification: null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    odk_spine_classification_init(out);
    root = odk_json_parse(text, strlen(text), &jerr);
    if (root == NULL) {
        gerr(err, "Classification: invalid JSON: %s", jerr.text);
        return ODK_SPINE_ERR_SCHEMA;
    }
    st = odk_spine_classification_from_dom(root, out, err);
    odk_json_free(root);
    if (st != ODK_SPINE_OK) {
        odk_spine_classification_release(out);
        return st;
    }
    return ODK_SPINE_OK;
}

void odk_spine_classification_result_init(odk_spine_classification_result *r)
{
    if (r != NULL) {
        memset(r, 0, sizeof(*r));
    }
}

void odk_spine_classification_result_release(odk_spine_classification_result *r)
{
    size_t i;
    if (r == NULL) {
        return;
    }
    free(r->subject_ref);
    for (i = 0; i < r->classifications.count; i++) {
        odk_spine_classification_release(&r->classifications.items[i]);
    }
    free(r->classifications.items);
    memset(r, 0, sizeof(*r));
}

char *odk_spine_classification_result_to_json(const odk_spine_classification_result *r)
{
    gbuf b;

    if (r == NULL || gbuf_init(&b) != 0) {
        return NULL;
    }
    if (gbuf_puts(&b, "{\"subject_ref\":") != 0 ||
        gbuf_put_json_string(&b, r->subject_ref) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"t\":") != 0 ||
        gbuf_put_int(&b, r->t) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"classifications\":") != 0 ||
        gbuf_putc(&b, '[') != 0 ||
        gbuf_put_obj_list_s(&b, (gbuf_obj_to_json)odk_spine_classification_to_json,
                           r->classifications.items,
                           r->classifications.count,
                           sizeof(odk_spine_classification)) != 0 ||
        gbuf_putc(&b, ']') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_putc(&b, '}') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    return gbuf_detach(&b);
}

odk_spine_status odk_spine_classification_result_from_dom(const odk_json_value *v,
                                       odk_spine_classification_result *r,
                                       odk_spine_error *err)
{
    static const char *const g_fields[] = {
        "subject_ref",
        "t",
        "classifications",
        NULL
    };
    const odk_json_value *fv;
    size_t i;
    size_t k;

    if (v == NULL || v->type != ODK_JSON_OBJECT) {
        gerr(err, "ClassificationResult: expected an object");
        return ODK_SPINE_ERR_SCHEMA;
    }
    for (k = 0; k < v->member_count; k++) {
        int known = 0;
        for (i = 0; g_fields[i] != NULL; i++) {
            if (strcmp(v->keys[k], g_fields[i]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            gerr(err, "ClassificationResult: unknown field '%s'", v->keys[k]);
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    fv = odk_json_object_get(v, "subject_ref");
    if (fv == NULL) {
        gerr(err, "ClassificationResult.subject_ref: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->subject_ref = g_strdup(fv->string);
    if (r->subject_ref == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "t");
    if (fv == NULL) {
        gerr(err, "ClassificationResult.t: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {
        gerr(err, "ClassificationResult.t: expected an integer");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->t = fv->integer;
    fv = odk_json_object_get(v, "classifications");
    if (fv == NULL) {
        gerr(err, "ClassificationResult.classifications: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_ARRAY) {
        gerr(err, "ClassificationResult.classifications: expected an array");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->item_count > 0) {
        r->classifications.items = calloc(fv->item_count, sizeof(odk_spine_classification));
        if (r->classifications.items == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    for (i = 0; i < fv->item_count; i++) {
        odk_spine_status st = odk_spine_classification_from_dom(fv->items[i],
                                    &r->classifications.items[i], err);
        if (st != ODK_SPINE_OK) {
            return st;
        }
    }
    r->classifications.count = fv->item_count;
    return ODK_SPINE_OK;
}

odk_spine_status odk_spine_classification_result_from_json(const char *text,
                                        odk_spine_classification_result *out,
                                        odk_spine_error *err)
{
    odk_json_value *root;
    odk_error jerr;
    odk_spine_status st;

    if (text == NULL || out == NULL) {
        gerr(err, "ClassificationResult: null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    odk_spine_classification_result_init(out);
    root = odk_json_parse(text, strlen(text), &jerr);
    if (root == NULL) {
        gerr(err, "ClassificationResult: invalid JSON: %s", jerr.text);
        return ODK_SPINE_ERR_SCHEMA;
    }
    st = odk_spine_classification_result_from_dom(root, out, err);
    odk_json_free(root);
    if (st != ODK_SPINE_OK) {
        odk_spine_classification_result_release(out);
        return st;
    }
    return ODK_SPINE_OK;
}

void odk_spine_classification_interval_init(odk_spine_classification_interval *r)
{
    if (r != NULL) {
        memset(r, 0, sizeof(*r));
    }
}

void odk_spine_classification_interval_release(odk_spine_classification_interval *r)
{
    size_t i;
    if (r == NULL) {
        return;
    }
    free(r->subject_ref);
    free(r->scheme_id);
    free(r->label);
    for (i = 0; i < r->facets.count; i++) {
        free(r->facets.items[i].key);
        free(r->facets.items[i].value);
    }
    free(r->facets.items);
    free(r->tier);
    free(r->brain_snapshot_id);
    free(r->observation_id);
    memset(r, 0, sizeof(*r));
}

char *odk_spine_classification_interval_to_json(const odk_spine_classification_interval *r)
{
    gbuf b;

    if (r == NULL || gbuf_init(&b) != 0) {
        return NULL;
    }
    if (gbuf_puts(&b, "{\"subject_ref\":") != 0 ||
        gbuf_put_json_string(&b, r->subject_ref) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"scheme_id\":") != 0 ||
        gbuf_put_json_string(&b, r->scheme_id) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"scheme_version\":") != 0 ||
        gbuf_put_int(&b, r->scheme_version) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"label\":") != 0 ||
        gbuf_put_json_string(&b, r->label) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"facets\":") != 0 ||
        gbuf_put_map(&b, &r->facets) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"tier\":") != 0 ||
        gbuf_put_json_string(&b, r->tier) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"brain_snapshot_id\":") != 0 ||
        gbuf_put_json_string(&b, r->brain_snapshot_id) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"valid_from\":") != 0 ||
        gbuf_put_int(&b, r->valid_from) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"valid_to\":") != 0 ||
        gbuf_put_int(&b, r->valid_to) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"observation_id\":") != 0 ||
        gbuf_put_json_string(&b, r->observation_id) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_putc(&b, '}') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    return gbuf_detach(&b);
}

odk_spine_status odk_spine_classification_interval_from_dom(const odk_json_value *v,
                                       odk_spine_classification_interval *r,
                                       odk_spine_error *err)
{
    static const char *const g_fields[] = {
        "subject_ref",
        "scheme_id",
        "scheme_version",
        "label",
        "facets",
        "tier",
        "brain_snapshot_id",
        "valid_from",
        "valid_to",
        "observation_id",
        NULL
    };
    const odk_json_value *fv;
    size_t i;
    size_t k;

    if (v == NULL || v->type != ODK_JSON_OBJECT) {
        gerr(err, "ClassificationInterval: expected an object");
        return ODK_SPINE_ERR_SCHEMA;
    }
    for (k = 0; k < v->member_count; k++) {
        int known = 0;
        for (i = 0; g_fields[i] != NULL; i++) {
            if (strcmp(v->keys[k], g_fields[i]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            gerr(err, "ClassificationInterval: unknown field '%s'", v->keys[k]);
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    fv = odk_json_object_get(v, "subject_ref");
    if (fv == NULL) {
        gerr(err, "ClassificationInterval.subject_ref: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->subject_ref = g_strdup(fv->string);
    if (r->subject_ref == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "scheme_id");
    if (fv == NULL) {
        gerr(err, "ClassificationInterval.scheme_id: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->scheme_id = g_strdup(fv->string);
    if (r->scheme_id == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "scheme_version");
    if (fv == NULL) {
        gerr(err, "ClassificationInterval.scheme_version: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {
        gerr(err, "ClassificationInterval.scheme_version: expected an integer");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->scheme_version = fv->integer;
    fv = odk_json_object_get(v, "label");
    if (fv == NULL) {
        gerr(err, "ClassificationInterval.label: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->label = g_strdup(fv->string);
    if (r->label == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "facets");
    if (fv == NULL) {
        gerr(err, "ClassificationInterval.facets: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_OBJECT) {
        gerr(err, "ClassificationInterval.facets: expected an object");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->member_count > 0) {
        r->facets.items = calloc(fv->member_count, sizeof(odk_spine_kv));
        if (r->facets.items == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    for (i = 0; i < fv->member_count; i++) {
        if (fv->members[i]->type != ODK_JSON_STRING) {
            gerr(err, "ClassificationInterval.facets: values must be strings");
            return ODK_SPINE_ERR_SCHEMA;
        }
        r->facets.items[i].key = g_strdup(fv->keys[i]);
        r->facets.items[i].value = g_strdup(fv->members[i]->string);
        if (r->facets.items[i].key == NULL ||
            r->facets.items[i].value == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    r->facets.count = fv->member_count;
    fv = odk_json_object_get(v, "tier");
    if (fv == NULL) {
        gerr(err, "ClassificationInterval.tier: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_STRING) {
        gerr(err, "ClassificationInterval.tier: expected a string");
        return ODK_SPINE_ERR_SCHEMA;
    }
    {
        int ok_av = 0;
        static const char *const g_av[] = {
            "A",
            "B",
            "",
            NULL
        };
        for (i = 0; g_av[i] != NULL; i++) {
            if (strcmp(fv->string, g_av[i]) == 0) {
                ok_av = 1;
                break;
            }
        }
        if (!ok_av) {
            gerr(err, "ClassificationInterval.tier: value not allowed");
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    r->tier = g_strdup(fv->string);
    if (r->tier == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "brain_snapshot_id");
    if (fv == NULL) {
        gerr(err, "ClassificationInterval.brain_snapshot_id: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->brain_snapshot_id = g_strdup(fv->string);
    if (r->brain_snapshot_id == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "valid_from");
    if (fv == NULL) {
        gerr(err, "ClassificationInterval.valid_from: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {
        gerr(err, "ClassificationInterval.valid_from: expected an integer");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->valid_from = fv->integer;
    fv = odk_json_object_get(v, "valid_to");
    if (fv == NULL) {
        gerr(err, "ClassificationInterval.valid_to: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {
        gerr(err, "ClassificationInterval.valid_to: expected an integer");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->valid_to = fv->integer;
    fv = odk_json_object_get(v, "observation_id");
    if (fv == NULL) {
        gerr(err, "ClassificationInterval.observation_id: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->observation_id = g_strdup(fv->string);
    if (r->observation_id == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    return ODK_SPINE_OK;
}

odk_spine_status odk_spine_classification_interval_from_json(const char *text,
                                        odk_spine_classification_interval *out,
                                        odk_spine_error *err)
{
    odk_json_value *root;
    odk_error jerr;
    odk_spine_status st;

    if (text == NULL || out == NULL) {
        gerr(err, "ClassificationInterval: null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    odk_spine_classification_interval_init(out);
    root = odk_json_parse(text, strlen(text), &jerr);
    if (root == NULL) {
        gerr(err, "ClassificationInterval: invalid JSON: %s", jerr.text);
        return ODK_SPINE_ERR_SCHEMA;
    }
    st = odk_spine_classification_interval_from_dom(root, out, err);
    odk_json_free(root);
    if (st != ODK_SPINE_OK) {
        odk_spine_classification_interval_release(out);
        return st;
    }
    return ODK_SPINE_OK;
}

void odk_spine_template_parameter_init(odk_spine_template_parameter *r)
{
    if (r != NULL) {
        memset(r, 0, sizeof(*r));
    }
}

void odk_spine_template_parameter_release(odk_spine_template_parameter *r)
{
    if (r == NULL) {
        return;
    }
    free(r->name);
    free(r->type);
    free(r->default_value);
    memset(r, 0, sizeof(*r));
}

char *odk_spine_template_parameter_to_json(const odk_spine_template_parameter *r)
{
    gbuf b;

    if (r == NULL || gbuf_init(&b) != 0) {
        return NULL;
    }
    if (gbuf_puts(&b, "{\"name\":") != 0 ||
        gbuf_put_json_string(&b, r->name) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"type\":") != 0 ||
        gbuf_put_json_string(&b, r->type) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"default_value\":") != 0 ||
        gbuf_put_json_string(&b, r->default_value) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"required\":") != 0 ||
        gbuf_put_bool(&b, r->required) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_putc(&b, '}') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    return gbuf_detach(&b);
}

odk_spine_status odk_spine_template_parameter_from_dom(const odk_json_value *v,
                                       odk_spine_template_parameter *r,
                                       odk_spine_error *err)
{
    static const char *const g_fields[] = {
        "name",
        "type",
        "default_value",
        "required",
        NULL
    };
    const odk_json_value *fv;
    size_t i;
    size_t k;

    if (v == NULL || v->type != ODK_JSON_OBJECT) {
        gerr(err, "TemplateParameter: expected an object");
        return ODK_SPINE_ERR_SCHEMA;
    }
    for (k = 0; k < v->member_count; k++) {
        int known = 0;
        for (i = 0; g_fields[i] != NULL; i++) {
            if (strcmp(v->keys[k], g_fields[i]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            gerr(err, "TemplateParameter: unknown field '%s'", v->keys[k]);
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    fv = odk_json_object_get(v, "name");
    if (fv == NULL) {
        gerr(err, "TemplateParameter.name: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->name = g_strdup(fv->string);
    if (r->name == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "type");
    if (fv == NULL) {
        gerr(err, "TemplateParameter.type: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_STRING) {
        gerr(err, "TemplateParameter.type: expected a string");
        return ODK_SPINE_ERR_SCHEMA;
    }
    {
        int ok_av = 0;
        static const char *const g_av[] = {
            "string",
            "int",
            "double",
            NULL
        };
        for (i = 0; g_av[i] != NULL; i++) {
            if (strcmp(fv->string, g_av[i]) == 0) {
                ok_av = 1;
                break;
            }
        }
        if (!ok_av) {
            gerr(err, "TemplateParameter.type: value not allowed");
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    r->type = g_strdup(fv->string);
    if (r->type == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "default_value");
    if (fv == NULL) {
        gerr(err, "TemplateParameter.default_value: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->default_value = g_strdup(fv->string);
    if (r->default_value == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "required");
    if (fv == NULL) {
        gerr(err, "TemplateParameter.required: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_BOOL) {
        gerr(err, "TemplateParameter.required: expected true or false");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->required = fv->boolean;
    return ODK_SPINE_OK;
}

odk_spine_status odk_spine_template_parameter_from_json(const char *text,
                                        odk_spine_template_parameter *out,
                                        odk_spine_error *err)
{
    odk_json_value *root;
    odk_error jerr;
    odk_spine_status st;

    if (text == NULL || out == NULL) {
        gerr(err, "TemplateParameter: null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    odk_spine_template_parameter_init(out);
    root = odk_json_parse(text, strlen(text), &jerr);
    if (root == NULL) {
        gerr(err, "TemplateParameter: invalid JSON: %s", jerr.text);
        return ODK_SPINE_ERR_SCHEMA;
    }
    st = odk_spine_template_parameter_from_dom(root, out, err);
    odk_json_free(root);
    if (st != ODK_SPINE_OK) {
        odk_spine_template_parameter_release(out);
        return st;
    }
    return ODK_SPINE_OK;
}

void odk_spine_component_spec_init(odk_spine_component_spec *r)
{
    if (r != NULL) {
        memset(r, 0, sizeof(*r));
    }
}

void odk_spine_component_spec_release(odk_spine_component_spec *r)
{
    if (r == NULL) {
        return;
    }
    free(r->name);
    free(r->kind);
    memset(r, 0, sizeof(*r));
}

char *odk_spine_component_spec_to_json(const odk_spine_component_spec *r)
{
    gbuf b;

    if (r == NULL || gbuf_init(&b) != 0) {
        return NULL;
    }
    if (gbuf_puts(&b, "{\"name\":") != 0 ||
        gbuf_put_json_string(&b, r->name) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"kind\":") != 0 ||
        gbuf_put_json_string(&b, r->kind) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"max_items\":") != 0 ||
        gbuf_put_int(&b, r->max_items) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"required\":") != 0 ||
        gbuf_put_bool(&b, r->required) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_putc(&b, '}') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    return gbuf_detach(&b);
}

odk_spine_status odk_spine_component_spec_from_dom(const odk_json_value *v,
                                       odk_spine_component_spec *r,
                                       odk_spine_error *err)
{
    static const char *const g_fields[] = {
        "name",
        "kind",
        "max_items",
        "required",
        NULL
    };
    const odk_json_value *fv;
    size_t i;
    size_t k;

    if (v == NULL || v->type != ODK_JSON_OBJECT) {
        gerr(err, "ComponentSpec: expected an object");
        return ODK_SPINE_ERR_SCHEMA;
    }
    for (k = 0; k < v->member_count; k++) {
        int known = 0;
        for (i = 0; g_fields[i] != NULL; i++) {
            if (strcmp(v->keys[k], g_fields[i]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            gerr(err, "ComponentSpec: unknown field '%s'", v->keys[k]);
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    fv = odk_json_object_get(v, "name");
    if (fv == NULL) {
        gerr(err, "ComponentSpec.name: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->name = g_strdup(fv->string);
    if (r->name == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "kind");
    if (fv == NULL) {
        gerr(err, "ComponentSpec.kind: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_STRING) {
        gerr(err, "ComponentSpec.kind: expected a string");
        return ODK_SPINE_ERR_SCHEMA;
    }
    {
        int ok_av = 0;
        static const char *const g_av[] = {
            "sentences",
            "keywords",
            "keyvalue",
            "whole",
            NULL
        };
        for (i = 0; g_av[i] != NULL; i++) {
            if (strcmp(fv->string, g_av[i]) == 0) {
                ok_av = 1;
                break;
            }
        }
        if (!ok_av) {
            gerr(err, "ComponentSpec.kind: value not allowed");
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    r->kind = g_strdup(fv->string);
    if (r->kind == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "max_items");
    if (fv == NULL) {
        gerr(err, "ComponentSpec.max_items: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {
        gerr(err, "ComponentSpec.max_items: expected an integer");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->max_items = fv->integer;
    fv = odk_json_object_get(v, "required");
    if (fv == NULL) {
        gerr(err, "ComponentSpec.required: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_BOOL) {
        gerr(err, "ComponentSpec.required: expected true or false");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->required = fv->boolean;
    return ODK_SPINE_OK;
}

odk_spine_status odk_spine_component_spec_from_json(const char *text,
                                        odk_spine_component_spec *out,
                                        odk_spine_error *err)
{
    odk_json_value *root;
    odk_error jerr;
    odk_spine_status st;

    if (text == NULL || out == NULL) {
        gerr(err, "ComponentSpec: null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    odk_spine_component_spec_init(out);
    root = odk_json_parse(text, strlen(text), &jerr);
    if (root == NULL) {
        gerr(err, "ComponentSpec: invalid JSON: %s", jerr.text);
        return ODK_SPINE_ERR_SCHEMA;
    }
    st = odk_spine_component_spec_from_dom(root, out, err);
    odk_json_free(root);
    if (st != ODK_SPINE_OK) {
        odk_spine_component_spec_release(out);
        return st;
    }
    return ODK_SPINE_OK;
}

void odk_spine_extraction_rule_init(odk_spine_extraction_rule *r)
{
    if (r != NULL) {
        memset(r, 0, sizeof(*r));
    }
}

void odk_spine_extraction_rule_release(odk_spine_extraction_rule *r)
{
    size_t i;
    if (r == NULL) {
        return;
    }
    free(r->component);
    for (i = 0; i < r->terms.count; i++) {
        free(r->terms.items[i]);
    }
    free(r->terms.items);
    free(r->mode);
    memset(r, 0, sizeof(*r));
}

char *odk_spine_extraction_rule_to_json(const odk_spine_extraction_rule *r)
{
    gbuf b;

    if (r == NULL || gbuf_init(&b) != 0) {
        return NULL;
    }
    if (gbuf_puts(&b, "{\"component\":") != 0 ||
        gbuf_put_json_string(&b, r->component) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"terms\":") != 0 ||
        gbuf_put_str_list(&b, &r->terms) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"mode\":") != 0 ||
        gbuf_put_json_string(&b, r->mode) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"window\":") != 0 ||
        gbuf_put_int(&b, r->window) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_putc(&b, '}') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    return gbuf_detach(&b);
}

odk_spine_status odk_spine_extraction_rule_from_dom(const odk_json_value *v,
                                       odk_spine_extraction_rule *r,
                                       odk_spine_error *err)
{
    static const char *const g_fields[] = {
        "component",
        "terms",
        "mode",
        "window",
        NULL
    };
    const odk_json_value *fv;
    size_t i;
    size_t k;

    if (v == NULL || v->type != ODK_JSON_OBJECT) {
        gerr(err, "ExtractionRule: expected an object");
        return ODK_SPINE_ERR_SCHEMA;
    }
    for (k = 0; k < v->member_count; k++) {
        int known = 0;
        for (i = 0; g_fields[i] != NULL; i++) {
            if (strcmp(v->keys[k], g_fields[i]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            gerr(err, "ExtractionRule: unknown field '%s'", v->keys[k]);
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    fv = odk_json_object_get(v, "component");
    if (fv == NULL) {
        gerr(err, "ExtractionRule.component: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->component = g_strdup(fv->string);
    if (r->component == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "terms");
    if (fv == NULL) {
        gerr(err, "ExtractionRule.terms: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_ARRAY) {
        gerr(err, "ExtractionRule.terms: expected an array of strings");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->item_count > 0) {
        r->terms.items = calloc(fv->item_count, sizeof(char *));
        if (r->terms.items == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    for (i = 0; i < fv->item_count; i++) {
        if (fv->items[i]->type != ODK_JSON_STRING) {
            gerr(err, "ExtractionRule.terms[%zu]: expected a string", i);
            return ODK_SPINE_ERR_SCHEMA;
        }
        r->terms.items[i] = g_strdup(fv->items[i]->string);
        if (r->terms.items[i] == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    r->terms.count = fv->item_count;
    fv = odk_json_object_get(v, "mode");
    if (fv == NULL) {
        gerr(err, "ExtractionRule.mode: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_STRING) {
        gerr(err, "ExtractionRule.mode: expected a string");
        return ODK_SPINE_ERR_SCHEMA;
    }
    {
        int ok_av = 0;
        static const char *const g_av[] = {
            "sentence",
            "before",
            "after",
            "around",
            "keyvalue",
            NULL
        };
        for (i = 0; g_av[i] != NULL; i++) {
            if (strcmp(fv->string, g_av[i]) == 0) {
                ok_av = 1;
                break;
            }
        }
        if (!ok_av) {
            gerr(err, "ExtractionRule.mode: value not allowed");
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    r->mode = g_strdup(fv->string);
    if (r->mode == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "window");
    if (fv == NULL) {
        gerr(err, "ExtractionRule.window: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {
        gerr(err, "ExtractionRule.window: expected an integer");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->window = fv->integer;
    return ODK_SPINE_OK;
}

odk_spine_status odk_spine_extraction_rule_from_json(const char *text,
                                        odk_spine_extraction_rule *out,
                                        odk_spine_error *err)
{
    odk_json_value *root;
    odk_error jerr;
    odk_spine_status st;

    if (text == NULL || out == NULL) {
        gerr(err, "ExtractionRule: null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    odk_spine_extraction_rule_init(out);
    root = odk_json_parse(text, strlen(text), &jerr);
    if (root == NULL) {
        gerr(err, "ExtractionRule: invalid JSON: %s", jerr.text);
        return ODK_SPINE_ERR_SCHEMA;
    }
    st = odk_spine_extraction_rule_from_dom(root, out, err);
    odk_json_free(root);
    if (st != ODK_SPINE_OK) {
        odk_spine_extraction_rule_release(out);
        return st;
    }
    return ODK_SPINE_OK;
}

void odk_spine_decomposition_template_init(odk_spine_decomposition_template *r)
{
    if (r != NULL) {
        memset(r, 0, sizeof(*r));
    }
}

void odk_spine_decomposition_template_release(odk_spine_decomposition_template *r)
{
    size_t i;
    if (r == NULL) {
        return;
    }
    free(r->id);
    for (i = 0; i < r->parameters.count; i++) {
        odk_spine_template_parameter_release(&r->parameters.items[i]);
    }
    free(r->parameters.items);
    for (i = 0; i < r->component_specs.count; i++) {
        odk_spine_component_spec_release(&r->component_specs.items[i]);
    }
    free(r->component_specs.items);
    for (i = 0; i < r->extraction_rules.count; i++) {
        odk_spine_extraction_rule_release(&r->extraction_rules.items[i]);
    }
    free(r->extraction_rules.items);
    memset(r, 0, sizeof(*r));
}

char *odk_spine_decomposition_template_to_json(const odk_spine_decomposition_template *r)
{
    gbuf b;

    if (r == NULL || gbuf_init(&b) != 0) {
        return NULL;
    }
    if (gbuf_puts(&b, "{\"id\":") != 0 ||
        gbuf_put_json_string(&b, r->id) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"version\":") != 0 ||
        gbuf_put_int(&b, r->version) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"valid_from\":") != 0 ||
        gbuf_put_int(&b, r->valid_from) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"valid_to\":") != 0 ||
        gbuf_put_int(&b, r->valid_to) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"parameters\":") != 0 ||
        gbuf_putc(&b, '[') != 0 ||
        gbuf_put_obj_list_s(&b, (gbuf_obj_to_json)odk_spine_template_parameter_to_json,
                           r->parameters.items,
                           r->parameters.count,
                           sizeof(odk_spine_template_parameter)) != 0 ||
        gbuf_putc(&b, ']') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"component_specs\":") != 0 ||
        gbuf_putc(&b, '[') != 0 ||
        gbuf_put_obj_list_s(&b, (gbuf_obj_to_json)odk_spine_component_spec_to_json,
                           r->component_specs.items,
                           r->component_specs.count,
                           sizeof(odk_spine_component_spec)) != 0 ||
        gbuf_putc(&b, ']') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"extraction_rules\":") != 0 ||
        gbuf_putc(&b, '[') != 0 ||
        gbuf_put_obj_list_s(&b, (gbuf_obj_to_json)odk_spine_extraction_rule_to_json,
                           r->extraction_rules.items,
                           r->extraction_rules.count,
                           sizeof(odk_spine_extraction_rule)) != 0 ||
        gbuf_putc(&b, ']') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_putc(&b, '}') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    return gbuf_detach(&b);
}

odk_spine_status odk_spine_decomposition_template_from_dom(const odk_json_value *v,
                                       odk_spine_decomposition_template *r,
                                       odk_spine_error *err)
{
    static const char *const g_fields[] = {
        "id",
        "version",
        "valid_from",
        "valid_to",
        "parameters",
        "component_specs",
        "extraction_rules",
        NULL
    };
    const odk_json_value *fv;
    size_t i;
    size_t k;

    if (v == NULL || v->type != ODK_JSON_OBJECT) {
        gerr(err, "DecompositionTemplate: expected an object");
        return ODK_SPINE_ERR_SCHEMA;
    }
    for (k = 0; k < v->member_count; k++) {
        int known = 0;
        for (i = 0; g_fields[i] != NULL; i++) {
            if (strcmp(v->keys[k], g_fields[i]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            gerr(err, "DecompositionTemplate: unknown field '%s'", v->keys[k]);
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    fv = odk_json_object_get(v, "id");
    if (fv == NULL) {
        gerr(err, "DecompositionTemplate.id: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->id = g_strdup(fv->string);
    if (r->id == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "version");
    if (fv == NULL) {
        gerr(err, "DecompositionTemplate.version: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {
        gerr(err, "DecompositionTemplate.version: expected an integer");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->version = fv->integer;
    fv = odk_json_object_get(v, "valid_from");
    if (fv == NULL) {
        gerr(err, "DecompositionTemplate.valid_from: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {
        gerr(err, "DecompositionTemplate.valid_from: expected an integer");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->valid_from = fv->integer;
    fv = odk_json_object_get(v, "valid_to");
    if (fv == NULL) {
        gerr(err, "DecompositionTemplate.valid_to: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {
        gerr(err, "DecompositionTemplate.valid_to: expected an integer");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->valid_to = fv->integer;
    fv = odk_json_object_get(v, "parameters");
    if (fv == NULL) {
        gerr(err, "DecompositionTemplate.parameters: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_ARRAY) {
        gerr(err, "DecompositionTemplate.parameters: expected an array");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->item_count > 0) {
        r->parameters.items = calloc(fv->item_count, sizeof(odk_spine_template_parameter));
        if (r->parameters.items == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    for (i = 0; i < fv->item_count; i++) {
        odk_spine_status st = odk_spine_template_parameter_from_dom(fv->items[i],
                                    &r->parameters.items[i], err);
        if (st != ODK_SPINE_OK) {
            return st;
        }
    }
    r->parameters.count = fv->item_count;
    fv = odk_json_object_get(v, "component_specs");
    if (fv == NULL) {
        gerr(err, "DecompositionTemplate.component_specs: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_ARRAY) {
        gerr(err, "DecompositionTemplate.component_specs: expected an array");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->item_count > 0) {
        r->component_specs.items = calloc(fv->item_count, sizeof(odk_spine_component_spec));
        if (r->component_specs.items == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    for (i = 0; i < fv->item_count; i++) {
        odk_spine_status st = odk_spine_component_spec_from_dom(fv->items[i],
                                    &r->component_specs.items[i], err);
        if (st != ODK_SPINE_OK) {
            return st;
        }
    }
    r->component_specs.count = fv->item_count;
    fv = odk_json_object_get(v, "extraction_rules");
    if (fv == NULL) {
        gerr(err, "DecompositionTemplate.extraction_rules: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_ARRAY) {
        gerr(err, "DecompositionTemplate.extraction_rules: expected an array");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->item_count > 0) {
        r->extraction_rules.items = calloc(fv->item_count, sizeof(odk_spine_extraction_rule));
        if (r->extraction_rules.items == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    for (i = 0; i < fv->item_count; i++) {
        odk_spine_status st = odk_spine_extraction_rule_from_dom(fv->items[i],
                                    &r->extraction_rules.items[i], err);
        if (st != ODK_SPINE_OK) {
            return st;
        }
    }
    r->extraction_rules.count = fv->item_count;
    return ODK_SPINE_OK;
}

odk_spine_status odk_spine_decomposition_template_from_json(const char *text,
                                        odk_spine_decomposition_template *out,
                                        odk_spine_error *err)
{
    odk_json_value *root;
    odk_error jerr;
    odk_spine_status st;

    if (text == NULL || out == NULL) {
        gerr(err, "DecompositionTemplate: null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    odk_spine_decomposition_template_init(out);
    root = odk_json_parse(text, strlen(text), &jerr);
    if (root == NULL) {
        gerr(err, "DecompositionTemplate: invalid JSON: %s", jerr.text);
        return ODK_SPINE_ERR_SCHEMA;
    }
    st = odk_spine_decomposition_template_from_dom(root, out, err);
    odk_json_free(root);
    if (st != ODK_SPINE_OK) {
        odk_spine_decomposition_template_release(out);
        return st;
    }
    return ODK_SPINE_OK;
}

void odk_spine_component_init(odk_spine_component *r)
{
    if (r != NULL) {
        memset(r, 0, sizeof(*r));
    }
}

void odk_spine_component_release(odk_spine_component *r)
{
    if (r == NULL) {
        return;
    }
    free(r->name);
    free(r->kind);
    free(r->content);
    memset(r, 0, sizeof(*r));
}

char *odk_spine_component_to_json(const odk_spine_component *r)
{
    gbuf b;

    if (r == NULL || gbuf_init(&b) != 0) {
        return NULL;
    }
    if (gbuf_puts(&b, "{\"name\":") != 0 ||
        gbuf_put_json_string(&b, r->name) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"kind\":") != 0 ||
        gbuf_put_json_string(&b, r->kind) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"content\":") != 0 ||
        gbuf_put_json_string(&b, r->content) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"item_count\":") != 0 ||
        gbuf_put_int(&b, r->item_count) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"complete\":") != 0 ||
        gbuf_put_bool(&b, r->complete) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_putc(&b, '}') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    return gbuf_detach(&b);
}

odk_spine_status odk_spine_component_from_dom(const odk_json_value *v,
                                       odk_spine_component *r,
                                       odk_spine_error *err)
{
    static const char *const g_fields[] = {
        "name",
        "kind",
        "content",
        "item_count",
        "complete",
        NULL
    };
    const odk_json_value *fv;
    size_t i;
    size_t k;

    if (v == NULL || v->type != ODK_JSON_OBJECT) {
        gerr(err, "Component: expected an object");
        return ODK_SPINE_ERR_SCHEMA;
    }
    for (k = 0; k < v->member_count; k++) {
        int known = 0;
        for (i = 0; g_fields[i] != NULL; i++) {
            if (strcmp(v->keys[k], g_fields[i]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            gerr(err, "Component: unknown field '%s'", v->keys[k]);
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    fv = odk_json_object_get(v, "name");
    if (fv == NULL) {
        gerr(err, "Component.name: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->name = g_strdup(fv->string);
    if (r->name == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "kind");
    if (fv == NULL) {
        gerr(err, "Component.kind: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->kind = g_strdup(fv->string);
    if (r->kind == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "content");
    if (fv == NULL) {
        gerr(err, "Component.content: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->content = g_strdup(fv->string);
    if (r->content == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "item_count");
    if (fv == NULL) {
        gerr(err, "Component.item_count: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {
        gerr(err, "Component.item_count: expected an integer");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->item_count = fv->integer;
    fv = odk_json_object_get(v, "complete");
    if (fv == NULL) {
        gerr(err, "Component.complete: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_BOOL) {
        gerr(err, "Component.complete: expected true or false");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->complete = fv->boolean;
    return ODK_SPINE_OK;
}

odk_spine_status odk_spine_component_from_json(const char *text,
                                        odk_spine_component *out,
                                        odk_spine_error *err)
{
    odk_json_value *root;
    odk_error jerr;
    odk_spine_status st;

    if (text == NULL || out == NULL) {
        gerr(err, "Component: null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    odk_spine_component_init(out);
    root = odk_json_parse(text, strlen(text), &jerr);
    if (root == NULL) {
        gerr(err, "Component: invalid JSON: %s", jerr.text);
        return ODK_SPINE_ERR_SCHEMA;
    }
    st = odk_spine_component_from_dom(root, out, err);
    odk_json_free(root);
    if (st != ODK_SPINE_OK) {
        odk_spine_component_release(out);
        return st;
    }
    return ODK_SPINE_OK;
}

void odk_spine_components_init(odk_spine_components *r)
{
    if (r != NULL) {
        memset(r, 0, sizeof(*r));
    }
}

void odk_spine_components_release(odk_spine_components *r)
{
    size_t i;
    if (r == NULL) {
        return;
    }
    free(r->template_id);
    for (i = 0; i < r->params_used.count; i++) {
        free(r->params_used.items[i].key);
        free(r->params_used.items[i].value);
    }
    free(r->params_used.items);
    for (i = 0; i < r->components.count; i++) {
        odk_spine_component_release(&r->components.items[i]);
    }
    free(r->components.items);
    memset(r, 0, sizeof(*r));
}

char *odk_spine_components_to_json(const odk_spine_components *r)
{
    gbuf b;

    if (r == NULL || gbuf_init(&b) != 0) {
        return NULL;
    }
    if (gbuf_puts(&b, "{\"template_id\":") != 0 ||
        gbuf_put_json_string(&b, r->template_id) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"template_version\":") != 0 ||
        gbuf_put_int(&b, r->template_version) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"params_used\":") != 0 ||
        gbuf_put_map(&b, &r->params_used) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"components\":") != 0 ||
        gbuf_putc(&b, '[') != 0 ||
        gbuf_put_obj_list_s(&b, (gbuf_obj_to_json)odk_spine_component_to_json,
                           r->components.items,
                           r->components.count,
                           sizeof(odk_spine_component)) != 0 ||
        gbuf_putc(&b, ']') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_putc(&b, '}') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    return gbuf_detach(&b);
}

odk_spine_status odk_spine_components_from_dom(const odk_json_value *v,
                                       odk_spine_components *r,
                                       odk_spine_error *err)
{
    static const char *const g_fields[] = {
        "template_id",
        "template_version",
        "params_used",
        "components",
        NULL
    };
    const odk_json_value *fv;
    size_t i;
    size_t k;

    if (v == NULL || v->type != ODK_JSON_OBJECT) {
        gerr(err, "Components: expected an object");
        return ODK_SPINE_ERR_SCHEMA;
    }
    for (k = 0; k < v->member_count; k++) {
        int known = 0;
        for (i = 0; g_fields[i] != NULL; i++) {
            if (strcmp(v->keys[k], g_fields[i]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            gerr(err, "Components: unknown field '%s'", v->keys[k]);
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    fv = odk_json_object_get(v, "template_id");
    if (fv == NULL) {
        gerr(err, "Components.template_id: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->template_id = g_strdup(fv->string);
    if (r->template_id == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "template_version");
    if (fv == NULL) {
        gerr(err, "Components.template_version: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {
        gerr(err, "Components.template_version: expected an integer");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->template_version = fv->integer;
    fv = odk_json_object_get(v, "params_used");
    if (fv == NULL) {
        gerr(err, "Components.params_used: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_OBJECT) {
        gerr(err, "Components.params_used: expected an object");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->member_count > 0) {
        r->params_used.items = calloc(fv->member_count, sizeof(odk_spine_kv));
        if (r->params_used.items == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    for (i = 0; i < fv->member_count; i++) {
        if (fv->members[i]->type != ODK_JSON_STRING) {
            gerr(err, "Components.params_used: values must be strings");
            return ODK_SPINE_ERR_SCHEMA;
        }
        r->params_used.items[i].key = g_strdup(fv->keys[i]);
        r->params_used.items[i].value = g_strdup(fv->members[i]->string);
        if (r->params_used.items[i].key == NULL ||
            r->params_used.items[i].value == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    r->params_used.count = fv->member_count;
    fv = odk_json_object_get(v, "components");
    if (fv == NULL) {
        gerr(err, "Components.components: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_ARRAY) {
        gerr(err, "Components.components: expected an array");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->item_count > 0) {
        r->components.items = calloc(fv->item_count, sizeof(odk_spine_component));
        if (r->components.items == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    for (i = 0; i < fv->item_count; i++) {
        odk_spine_status st = odk_spine_component_from_dom(fv->items[i],
                                    &r->components.items[i], err);
        if (st != ODK_SPINE_OK) {
            return st;
        }
    }
    r->components.count = fv->item_count;
    return ODK_SPINE_OK;
}

odk_spine_status odk_spine_components_from_json(const char *text,
                                        odk_spine_components *out,
                                        odk_spine_error *err)
{
    odk_json_value *root;
    odk_error jerr;
    odk_spine_status st;

    if (text == NULL || out == NULL) {
        gerr(err, "Components: null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    odk_spine_components_init(out);
    root = odk_json_parse(text, strlen(text), &jerr);
    if (root == NULL) {
        gerr(err, "Components: invalid JSON: %s", jerr.text);
        return ODK_SPINE_ERR_SCHEMA;
    }
    st = odk_spine_components_from_dom(root, out, err);
    odk_json_free(root);
    if (st != ODK_SPINE_OK) {
        odk_spine_components_release(out);
        return st;
    }
    return ODK_SPINE_OK;
}

void odk_spine_decision_pin_init(odk_spine_decision_pin *r)
{
    if (r != NULL) {
        memset(r, 0, sizeof(*r));
    }
}

void odk_spine_decision_pin_release(odk_spine_decision_pin *r)
{
    size_t i;
    if (r == NULL) {
        return;
    }
    free(r->pin_id);
    free(r->schema_fp);
    free(r->kernel_version);
    free(r->brain_snapshot_id);
    free(r->context_hash);
    free(r->inputs_ref);
    free(r->request_json);
    free(r->decision);
    free(r->tier);
    free(r->replay_class);
    for (i = 0; i < r->registrations.count; i++) {
        odk_spine_registration_ref_release(&r->registrations.items[i]);
    }
    free(r->registrations.items);
    memset(r, 0, sizeof(*r));
}

char *odk_spine_decision_pin_to_json(const odk_spine_decision_pin *r)
{
    gbuf b;

    if (r == NULL || gbuf_init(&b) != 0) {
        return NULL;
    }
    if (gbuf_puts(&b, "{\"pin_id\":") != 0 ||
        gbuf_put_json_string(&b, r->pin_id) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"schema_fp\":") != 0 ||
        gbuf_put_json_string(&b, r->schema_fp) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"kernel_version\":") != 0 ||
        gbuf_put_json_string(&b, r->kernel_version) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"brain_snapshot_id\":") != 0 ||
        gbuf_put_json_string(&b, r->brain_snapshot_id) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"context_hash\":") != 0 ||
        gbuf_put_json_string(&b, r->context_hash) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"inputs_ref\":") != 0 ||
        gbuf_put_json_string(&b, r->inputs_ref) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"request_json\":") != 0 ||
        gbuf_put_json_string(&b, r->request_json) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"decision\":") != 0 ||
        gbuf_put_json_string(&b, r->decision) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"confidence\":") != 0 ||
        gbuf_put_double(&b, r->confidence) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"tier\":") != 0 ||
        gbuf_put_json_string(&b, r->tier) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"replay_class\":") != 0 ||
        gbuf_put_json_string(&b, r->replay_class) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"timestamp\":") != 0 ||
        gbuf_put_int(&b, r->timestamp) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"registrations\":") != 0 ||
        gbuf_putc(&b, '[') != 0 ||
        gbuf_put_obj_list_s(&b, (gbuf_obj_to_json)odk_spine_registration_ref_to_json,
                           r->registrations.items,
                           r->registrations.count,
                           sizeof(odk_spine_registration_ref)) != 0 ||
        gbuf_putc(&b, ']') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_putc(&b, '}') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    return gbuf_detach(&b);
}

odk_spine_status odk_spine_decision_pin_from_dom(const odk_json_value *v,
                                       odk_spine_decision_pin *r,
                                       odk_spine_error *err)
{
    static const char *const g_fields[] = {
        "pin_id",
        "schema_fp",
        "kernel_version",
        "brain_snapshot_id",
        "context_hash",
        "inputs_ref",
        "request_json",
        "decision",
        "confidence",
        "tier",
        "replay_class",
        "timestamp",
        "registrations",
        NULL
    };
    const odk_json_value *fv;
    size_t i;
    size_t k;

    if (v == NULL || v->type != ODK_JSON_OBJECT) {
        gerr(err, "DecisionPin: expected an object");
        return ODK_SPINE_ERR_SCHEMA;
    }
    for (k = 0; k < v->member_count; k++) {
        int known = 0;
        for (i = 0; g_fields[i] != NULL; i++) {
            if (strcmp(v->keys[k], g_fields[i]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            gerr(err, "DecisionPin: unknown field '%s'", v->keys[k]);
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    fv = odk_json_object_get(v, "pin_id");
    if (fv == NULL) {
        gerr(err, "DecisionPin.pin_id: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->pin_id = g_strdup(fv->string);
    if (r->pin_id == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "schema_fp");
    if (fv == NULL) {
        gerr(err, "DecisionPin.schema_fp: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->schema_fp = g_strdup(fv->string);
    if (r->schema_fp == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "kernel_version");
    if (fv == NULL) {
        gerr(err, "DecisionPin.kernel_version: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->kernel_version = g_strdup(fv->string);
    if (r->kernel_version == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "brain_snapshot_id");
    if (fv == NULL) {
        gerr(err, "DecisionPin.brain_snapshot_id: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->brain_snapshot_id = g_strdup(fv->string);
    if (r->brain_snapshot_id == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "context_hash");
    if (fv == NULL) {
        gerr(err, "DecisionPin.context_hash: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->context_hash = g_strdup(fv->string);
    if (r->context_hash == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "inputs_ref");
    if (fv == NULL) {
        gerr(err, "DecisionPin.inputs_ref: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->inputs_ref = g_strdup(fv->string);
    if (r->inputs_ref == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "request_json");
    if (fv == NULL) {
        gerr(err, "DecisionPin.request_json: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->request_json = g_strdup(fv->string);
    if (r->request_json == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "decision");
    if (fv == NULL) {
        gerr(err, "DecisionPin.decision: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->decision = g_strdup(fv->string);
    if (r->decision == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "confidence");
    if (fv == NULL) {
        gerr(err, "DecisionPin.confidence: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER) {
        gerr(err, "DecisionPin.confidence: expected a number");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->confidence = fv->number;
    fv = odk_json_object_get(v, "tier");
    if (fv == NULL) {
        gerr(err, "DecisionPin.tier: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_STRING) {
        gerr(err, "DecisionPin.tier: expected a string");
        return ODK_SPINE_ERR_SCHEMA;
    }
    {
        int ok_av = 0;
        static const char *const g_av[] = {
            "",
            "A",
            "B",
            NULL
        };
        for (i = 0; g_av[i] != NULL; i++) {
            if (strcmp(fv->string, g_av[i]) == 0) {
                ok_av = 1;
                break;
            }
        }
        if (!ok_av) {
            gerr(err, "DecisionPin.tier: value not allowed");
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    r->tier = g_strdup(fv->string);
    if (r->tier == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "replay_class");
    if (fv == NULL) {
        gerr(err, "DecisionPin.replay_class: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_STRING) {
        gerr(err, "DecisionPin.replay_class: expected a string");
        return ODK_SPINE_ERR_SCHEMA;
    }
    {
        int ok_av = 0;
        static const char *const g_av[] = {
            "exact",
            "semantic",
            "foreign",
            NULL
        };
        for (i = 0; g_av[i] != NULL; i++) {
            if (strcmp(fv->string, g_av[i]) == 0) {
                ok_av = 1;
                break;
            }
        }
        if (!ok_av) {
            gerr(err, "DecisionPin.replay_class: value not allowed");
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    r->replay_class = g_strdup(fv->string);
    if (r->replay_class == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "timestamp");
    if (fv == NULL) {
        gerr(err, "DecisionPin.timestamp: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {
        gerr(err, "DecisionPin.timestamp: expected an integer");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->timestamp = fv->integer;
    fv = odk_json_object_get(v, "registrations");
    if (fv == NULL) {
        gerr(err, "DecisionPin.registrations: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_ARRAY) {
        gerr(err, "DecisionPin.registrations: expected an array");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->item_count > 0) {
        r->registrations.items = calloc(fv->item_count, sizeof(odk_spine_registration_ref));
        if (r->registrations.items == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    for (i = 0; i < fv->item_count; i++) {
        odk_spine_status st = odk_spine_registration_ref_from_dom(fv->items[i],
                                    &r->registrations.items[i], err);
        if (st != ODK_SPINE_OK) {
            return st;
        }
    }
    r->registrations.count = fv->item_count;
    return ODK_SPINE_OK;
}

odk_spine_status odk_spine_decision_pin_from_json(const char *text,
                                        odk_spine_decision_pin *out,
                                        odk_spine_error *err)
{
    odk_json_value *root;
    odk_error jerr;
    odk_spine_status st;

    if (text == NULL || out == NULL) {
        gerr(err, "DecisionPin: null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    odk_spine_decision_pin_init(out);
    root = odk_json_parse(text, strlen(text), &jerr);
    if (root == NULL) {
        gerr(err, "DecisionPin: invalid JSON: %s", jerr.text);
        return ODK_SPINE_ERR_SCHEMA;
    }
    st = odk_spine_decision_pin_from_dom(root, out, err);
    odk_json_free(root);
    if (st != ODK_SPINE_OK) {
        odk_spine_decision_pin_release(out);
        return st;
    }
    return ODK_SPINE_OK;
}

void odk_spine_calibration_bin_init(odk_spine_calibration_bin *r)
{
    if (r != NULL) {
        memset(r, 0, sizeof(*r));
    }
}

void odk_spine_calibration_bin_release(odk_spine_calibration_bin *r)
{
    if (r == NULL) {
        return;
    }
    free(r->primitive);
    free(r->scheme_id);
    free(r->tier);
    memset(r, 0, sizeof(*r));
}

char *odk_spine_calibration_bin_to_json(const odk_spine_calibration_bin *r)
{
    gbuf b;

    if (r == NULL || gbuf_init(&b) != 0) {
        return NULL;
    }
    if (gbuf_puts(&b, "{\"primitive\":") != 0 ||
        gbuf_put_json_string(&b, r->primitive) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"scheme_id\":") != 0 ||
        gbuf_put_json_string(&b, r->scheme_id) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"tier\":") != 0 ||
        gbuf_put_json_string(&b, r->tier) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"bin_index\":") != 0 ||
        gbuf_put_int(&b, r->bin_index) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"lo\":") != 0 ||
        gbuf_put_double(&b, r->lo) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"hi\":") != 0 ||
        gbuf_put_double(&b, r->hi) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"sample_count\":") != 0 ||
        gbuf_put_int(&b, r->sample_count) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"predicted_mean\":") != 0 ||
        gbuf_put_double(&b, r->predicted_mean) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"observed_rate\":") != 0 ||
        gbuf_put_double(&b, r->observed_rate) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"calibration_error\":") != 0 ||
        gbuf_put_double(&b, r->calibration_error) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_putc(&b, '}') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    return gbuf_detach(&b);
}

odk_spine_status odk_spine_calibration_bin_from_dom(const odk_json_value *v,
                                       odk_spine_calibration_bin *r,
                                       odk_spine_error *err)
{
    static const char *const g_fields[] = {
        "primitive",
        "scheme_id",
        "tier",
        "bin_index",
        "lo",
        "hi",
        "sample_count",
        "predicted_mean",
        "observed_rate",
        "calibration_error",
        NULL
    };
    const odk_json_value *fv;
    size_t i;
    size_t k;

    if (v == NULL || v->type != ODK_JSON_OBJECT) {
        gerr(err, "CalibrationBin: expected an object");
        return ODK_SPINE_ERR_SCHEMA;
    }
    for (k = 0; k < v->member_count; k++) {
        int known = 0;
        for (i = 0; g_fields[i] != NULL; i++) {
            if (strcmp(v->keys[k], g_fields[i]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            gerr(err, "CalibrationBin: unknown field '%s'", v->keys[k]);
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    fv = odk_json_object_get(v, "primitive");
    if (fv == NULL) {
        gerr(err, "CalibrationBin.primitive: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->primitive = g_strdup(fv->string);
    if (r->primitive == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "scheme_id");
    if (fv == NULL) {
        gerr(err, "CalibrationBin.scheme_id: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->scheme_id = g_strdup(fv->string);
    if (r->scheme_id == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "tier");
    if (fv == NULL) {
        gerr(err, "CalibrationBin.tier: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_STRING) {
        gerr(err, "CalibrationBin.tier: expected a string");
        return ODK_SPINE_ERR_SCHEMA;
    }
    {
        int ok_av = 0;
        static const char *const g_av[] = {
            "",
            "A",
            "B",
            NULL
        };
        for (i = 0; g_av[i] != NULL; i++) {
            if (strcmp(fv->string, g_av[i]) == 0) {
                ok_av = 1;
                break;
            }
        }
        if (!ok_av) {
            gerr(err, "CalibrationBin.tier: value not allowed");
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    r->tier = g_strdup(fv->string);
    if (r->tier == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "bin_index");
    if (fv == NULL) {
        gerr(err, "CalibrationBin.bin_index: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {
        gerr(err, "CalibrationBin.bin_index: expected an integer");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->bin_index = fv->integer;
    fv = odk_json_object_get(v, "lo");
    if (fv == NULL) {
        gerr(err, "CalibrationBin.lo: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER) {
        gerr(err, "CalibrationBin.lo: expected a number");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->lo = fv->number;
    fv = odk_json_object_get(v, "hi");
    if (fv == NULL) {
        gerr(err, "CalibrationBin.hi: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER) {
        gerr(err, "CalibrationBin.hi: expected a number");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->hi = fv->number;
    fv = odk_json_object_get(v, "sample_count");
    if (fv == NULL) {
        gerr(err, "CalibrationBin.sample_count: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {
        gerr(err, "CalibrationBin.sample_count: expected an integer");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->sample_count = fv->integer;
    fv = odk_json_object_get(v, "predicted_mean");
    if (fv == NULL) {
        gerr(err, "CalibrationBin.predicted_mean: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER) {
        gerr(err, "CalibrationBin.predicted_mean: expected a number");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->predicted_mean = fv->number;
    fv = odk_json_object_get(v, "observed_rate");
    if (fv == NULL) {
        gerr(err, "CalibrationBin.observed_rate: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER) {
        gerr(err, "CalibrationBin.observed_rate: expected a number");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->observed_rate = fv->number;
    fv = odk_json_object_get(v, "calibration_error");
    if (fv == NULL) {
        gerr(err, "CalibrationBin.calibration_error: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER) {
        gerr(err, "CalibrationBin.calibration_error: expected a number");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->calibration_error = fv->number;
    return ODK_SPINE_OK;
}

odk_spine_status odk_spine_calibration_bin_from_json(const char *text,
                                        odk_spine_calibration_bin *out,
                                        odk_spine_error *err)
{
    odk_json_value *root;
    odk_error jerr;
    odk_spine_status st;

    if (text == NULL || out == NULL) {
        gerr(err, "CalibrationBin: null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    odk_spine_calibration_bin_init(out);
    root = odk_json_parse(text, strlen(text), &jerr);
    if (root == NULL) {
        gerr(err, "CalibrationBin: invalid JSON: %s", jerr.text);
        return ODK_SPINE_ERR_SCHEMA;
    }
    st = odk_spine_calibration_bin_from_dom(root, out, err);
    odk_json_free(root);
    if (st != ODK_SPINE_OK) {
        odk_spine_calibration_bin_release(out);
        return st;
    }
    return ODK_SPINE_OK;
}

void odk_spine_calibration_report_init(odk_spine_calibration_report *r)
{
    if (r != NULL) {
        memset(r, 0, sizeof(*r));
    }
}

void odk_spine_calibration_report_release(odk_spine_calibration_report *r)
{
    size_t i;
    if (r == NULL) {
        return;
    }
    free(r->report_id);
    free(r->generated_from);
    for (i = 0; i < r->bins.count; i++) {
        odk_spine_calibration_bin_release(&r->bins.items[i]);
    }
    free(r->bins.items);
    memset(r, 0, sizeof(*r));
}

char *odk_spine_calibration_report_to_json(const odk_spine_calibration_report *r)
{
    gbuf b;

    if (r == NULL || gbuf_init(&b) != 0) {
        return NULL;
    }
    if (gbuf_puts(&b, "{\"report_id\":") != 0 ||
        gbuf_put_json_string(&b, r->report_id) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"generated_from\":") != 0 ||
        gbuf_put_json_string(&b, r->generated_from) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"bins\":") != 0 ||
        gbuf_putc(&b, '[') != 0 ||
        gbuf_put_obj_list_s(&b, (gbuf_obj_to_json)odk_spine_calibration_bin_to_json,
                           r->bins.items,
                           r->bins.count,
                           sizeof(odk_spine_calibration_bin)) != 0 ||
        gbuf_putc(&b, ']') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_putc(&b, '}') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    return gbuf_detach(&b);
}

odk_spine_status odk_spine_calibration_report_from_dom(const odk_json_value *v,
                                       odk_spine_calibration_report *r,
                                       odk_spine_error *err)
{
    static const char *const g_fields[] = {
        "report_id",
        "generated_from",
        "bins",
        NULL
    };
    const odk_json_value *fv;
    size_t i;
    size_t k;

    if (v == NULL || v->type != ODK_JSON_OBJECT) {
        gerr(err, "CalibrationReport: expected an object");
        return ODK_SPINE_ERR_SCHEMA;
    }
    for (k = 0; k < v->member_count; k++) {
        int known = 0;
        for (i = 0; g_fields[i] != NULL; i++) {
            if (strcmp(v->keys[k], g_fields[i]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            gerr(err, "CalibrationReport: unknown field '%s'", v->keys[k]);
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    fv = odk_json_object_get(v, "report_id");
    if (fv == NULL) {
        gerr(err, "CalibrationReport.report_id: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->report_id = g_strdup(fv->string);
    if (r->report_id == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "generated_from");
    if (fv == NULL) {
        gerr(err, "CalibrationReport.generated_from: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->generated_from = g_strdup(fv->string);
    if (r->generated_from == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "bins");
    if (fv == NULL) {
        gerr(err, "CalibrationReport.bins: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_ARRAY) {
        gerr(err, "CalibrationReport.bins: expected an array");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->item_count > 0) {
        r->bins.items = calloc(fv->item_count, sizeof(odk_spine_calibration_bin));
        if (r->bins.items == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    for (i = 0; i < fv->item_count; i++) {
        odk_spine_status st = odk_spine_calibration_bin_from_dom(fv->items[i],
                                    &r->bins.items[i], err);
        if (st != ODK_SPINE_OK) {
            return st;
        }
    }
    r->bins.count = fv->item_count;
    return ODK_SPINE_OK;
}

odk_spine_status odk_spine_calibration_report_from_json(const char *text,
                                        odk_spine_calibration_report *out,
                                        odk_spine_error *err)
{
    odk_json_value *root;
    odk_error jerr;
    odk_spine_status st;

    if (text == NULL || out == NULL) {
        gerr(err, "CalibrationReport: null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    odk_spine_calibration_report_init(out);
    root = odk_json_parse(text, strlen(text), &jerr);
    if (root == NULL) {
        gerr(err, "CalibrationReport: invalid JSON: %s", jerr.text);
        return ODK_SPINE_ERR_SCHEMA;
    }
    st = odk_spine_calibration_report_from_dom(root, out, err);
    odk_json_free(root);
    if (st != ODK_SPINE_OK) {
        odk_spine_calibration_report_release(out);
        return st;
    }
    return ODK_SPINE_OK;
}

void odk_spine_world_variable_init(odk_spine_world_variable *r)
{
    if (r != NULL) {
        memset(r, 0, sizeof(*r));
    }
}

void odk_spine_world_variable_release(odk_spine_world_variable *r)
{
    if (r == NULL) {
        return;
    }
    free(r->name);
    free(r->type);
    free(r->value);
    memset(r, 0, sizeof(*r));
}

char *odk_spine_world_variable_to_json(const odk_spine_world_variable *r)
{
    gbuf b;

    if (r == NULL || gbuf_init(&b) != 0) {
        return NULL;
    }
    if (gbuf_puts(&b, "{\"name\":") != 0 ||
        gbuf_put_json_string(&b, r->name) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"type\":") != 0 ||
        gbuf_put_json_string(&b, r->type) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"value\":") != 0 ||
        gbuf_put_json_string(&b, r->value) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_putc(&b, '}') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    return gbuf_detach(&b);
}

odk_spine_status odk_spine_world_variable_from_dom(const odk_json_value *v,
                                       odk_spine_world_variable *r,
                                       odk_spine_error *err)
{
    static const char *const g_fields[] = {
        "name",
        "type",
        "value",
        NULL
    };
    const odk_json_value *fv;
    size_t i;
    size_t k;

    if (v == NULL || v->type != ODK_JSON_OBJECT) {
        gerr(err, "WorldVariable: expected an object");
        return ODK_SPINE_ERR_SCHEMA;
    }
    for (k = 0; k < v->member_count; k++) {
        int known = 0;
        for (i = 0; g_fields[i] != NULL; i++) {
            if (strcmp(v->keys[k], g_fields[i]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            gerr(err, "WorldVariable: unknown field '%s'", v->keys[k]);
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    fv = odk_json_object_get(v, "name");
    if (fv == NULL) {
        gerr(err, "WorldVariable.name: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->name = g_strdup(fv->string);
    if (r->name == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "type");
    if (fv == NULL) {
        gerr(err, "WorldVariable.type: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->type = g_strdup(fv->string);
    if (r->type == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "value");
    if (fv == NULL) {
        gerr(err, "WorldVariable.value: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->value = g_strdup(fv->string);
    if (r->value == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    return ODK_SPINE_OK;
}

odk_spine_status odk_spine_world_variable_from_json(const char *text,
                                        odk_spine_world_variable *out,
                                        odk_spine_error *err)
{
    odk_json_value *root;
    odk_error jerr;
    odk_spine_status st;

    if (text == NULL || out == NULL) {
        gerr(err, "WorldVariable: null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    odk_spine_world_variable_init(out);
    root = odk_json_parse(text, strlen(text), &jerr);
    if (root == NULL) {
        gerr(err, "WorldVariable: invalid JSON: %s", jerr.text);
        return ODK_SPINE_ERR_SCHEMA;
    }
    st = odk_spine_world_variable_from_dom(root, out, err);
    odk_json_free(root);
    if (st != ODK_SPINE_OK) {
        odk_spine_world_variable_release(out);
        return st;
    }
    return ODK_SPINE_OK;
}

void odk_spine_world_state_context_init(odk_spine_world_state_context *r)
{
    if (r != NULL) {
        memset(r, 0, sizeof(*r));
    }
}

void odk_spine_world_state_context_release(odk_spine_world_state_context *r)
{
    size_t i;
    if (r == NULL) {
        return;
    }
    free(r->scope_json);
    for (i = 0; i < r->variables.count; i++) {
        odk_spine_world_variable_release(&r->variables.items[i]);
    }
    free(r->variables.items);
    memset(r, 0, sizeof(*r));
}

char *odk_spine_world_state_context_to_json(const odk_spine_world_state_context *r)
{
    gbuf b;

    if (r == NULL || gbuf_init(&b) != 0) {
        return NULL;
    }
    if (gbuf_puts(&b, "{\"t\":") != 0 ||
        gbuf_put_int(&b, r->t) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"scope_json\":") != 0 ||
        gbuf_put_json_string(&b, r->scope_json) != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_puts(&b, ",\"variables\":") != 0 ||
        gbuf_putc(&b, '[') != 0 ||
        gbuf_put_obj_list_s(&b, (gbuf_obj_to_json)odk_spine_world_variable_to_json,
                           r->variables.items,
                           r->variables.count,
                           sizeof(odk_spine_world_variable)) != 0 ||
        gbuf_putc(&b, ']') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    if (gbuf_putc(&b, '}') != 0) {
        gbuf_free(&b);
        return NULL;
    }
    return gbuf_detach(&b);
}

odk_spine_status odk_spine_world_state_context_from_dom(const odk_json_value *v,
                                       odk_spine_world_state_context *r,
                                       odk_spine_error *err)
{
    static const char *const g_fields[] = {
        "t",
        "scope_json",
        "variables",
        NULL
    };
    const odk_json_value *fv;
    size_t i;
    size_t k;

    if (v == NULL || v->type != ODK_JSON_OBJECT) {
        gerr(err, "WorldStateContext: expected an object");
        return ODK_SPINE_ERR_SCHEMA;
    }
    for (k = 0; k < v->member_count; k++) {
        int known = 0;
        for (i = 0; g_fields[i] != NULL; i++) {
            if (strcmp(v->keys[k], g_fields[i]) == 0) {
                known = 1;
                break;
            }
        }
        if (!known) {
            gerr(err, "WorldStateContext: unknown field '%s'", v->keys[k]);
            return ODK_SPINE_ERR_SCHEMA;
        }
    }
    fv = odk_json_object_get(v, "t");
    if (fv == NULL) {
        gerr(err, "WorldStateContext.t: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_NUMBER || !fv->is_integer) {
        gerr(err, "WorldStateContext.t: expected an integer");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->t = fv->integer;
    fv = odk_json_object_get(v, "scope_json");
    if (fv == NULL) {
        gerr(err, "WorldStateContext.scope_json: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    r->scope_json = g_strdup(fv->string);
    if (r->scope_json == NULL) {
        gerr(err, "out of memory");
        return ODK_SPINE_ERR_MEMORY;
    }
    fv = odk_json_object_get(v, "variables");
    if (fv == NULL) {
        gerr(err, "WorldStateContext.variables: missing required field");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->type != ODK_JSON_ARRAY) {
        gerr(err, "WorldStateContext.variables: expected an array");
        return ODK_SPINE_ERR_SCHEMA;
    }
    if (fv->item_count > 0) {
        r->variables.items = calloc(fv->item_count, sizeof(odk_spine_world_variable));
        if (r->variables.items == NULL) {
            gerr(err, "out of memory");
            return ODK_SPINE_ERR_MEMORY;
        }
    }
    for (i = 0; i < fv->item_count; i++) {
        odk_spine_status st = odk_spine_world_variable_from_dom(fv->items[i],
                                    &r->variables.items[i], err);
        if (st != ODK_SPINE_OK) {
            return st;
        }
    }
    r->variables.count = fv->item_count;
    return ODK_SPINE_OK;
}

odk_spine_status odk_spine_world_state_context_from_json(const char *text,
                                        odk_spine_world_state_context *out,
                                        odk_spine_error *err)
{
    odk_json_value *root;
    odk_error jerr;
    odk_spine_status st;

    if (text == NULL || out == NULL) {
        gerr(err, "WorldStateContext: null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    odk_spine_world_state_context_init(out);
    root = odk_json_parse(text, strlen(text), &jerr);
    if (root == NULL) {
        gerr(err, "WorldStateContext: invalid JSON: %s", jerr.text);
        return ODK_SPINE_ERR_SCHEMA;
    }
    st = odk_spine_world_state_context_from_dom(root, out, err);
    odk_json_free(root);
    if (st != ODK_SPINE_OK) {
        odk_spine_world_state_context_release(out);
        return st;
    }
    return ODK_SPINE_OK;
}

