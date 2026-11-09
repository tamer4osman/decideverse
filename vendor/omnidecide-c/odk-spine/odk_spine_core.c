/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_spine_core.c - spine handle, status plumbing and deterministic
 * hashing (FNV-1a 64). No wall clock, no RNG, no locale dependence.
 */

#include "odk_spine_private.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void odk_spine_error_init(odk_spine_error *err)
{
    if (err != NULL) {
        err->text[0] = '\0';
    }
}

void odk_spine_set_err(odk_spine_error *err, const char *msg)
{
    if (err != NULL) {
        snprintf(err->text, sizeof(err->text), "%s", msg != NULL ? msg : "");
    }
}

void odk_spine_set_errf(odk_spine_error *err, const char *fmt, ...)
{
    va_list ap;

    if (err == NULL) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(err->text, sizeof(err->text), fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------------ */
/* FNV-1a 64 (deterministic content hashing)                           */
/* ------------------------------------------------------------------ */

void odk_spine_fnv1a64_init(uint64_t *h)
{
    *h = UINT64_C(0xcbf29ce484222325);
}

void odk_spine_fnv1a64_update(uint64_t *h, const void *data, size_t len)
{
    const unsigned char *p = data;
    size_t i;

    for (i = 0; i < len; i++) {
        *h ^= p[i];
        *h *= UINT64_C(0x100000001b3);
    }
}

void odk_spine_fnv1a64_hex(uint64_t h, char out[17])
{
    snprintf(out, 17, "%016llx", (unsigned long long)h);
}

void odk_spine_map_release_records(odk_spine_map *m)
{
    size_t i;

    if (m == NULL) {
        return;
    }
    for (i = 0; i < m->count; i++) {
        free(m->items[i].key);
        free(m->items[i].value);
    }
    free(m->items);
    m->items = NULL;
    m->count = 0;
}

void odk_spine_hash_hex(const void *data, size_t len, char out[17])
{
    uint64_t h;

    odk_spine_fnv1a64_init(&h);
    odk_spine_fnv1a64_update(&h, data, len);
    odk_spine_fnv1a64_hex(h, out);
}

/* ------------------------------------------------------------------ */
/* Spine handle                                                        */
/* ------------------------------------------------------------------ */

odk_spine *odk_spine_create(void)
{
    odk_spine *spine = calloc(1, sizeof(*spine));

    if (spine == NULL) {
        return NULL;
    }
    memcpy(spine->schema_fp, ODK_SPINE_SCHEMA_FINGERPRINT,
           sizeof(spine->schema_fp));
    return spine;
}

void odk_spine_free(odk_spine *spine)
{
    if (spine == NULL) {
        return;
    }
    odk_spine_registry_free(spine);
    free(spine);
}

const char *odk_spine_schema_fingerprint(const odk_spine *spine)
{
    return spine == NULL ? NULL : spine->schema_fp;
}

/* ------------------------------------------------------------------ */
/* worldState(variables, scope, t) contract (F6)                       */
/* ------------------------------------------------------------------ */

odk_spine_status odk_spine_worldstate_validate(const char *json,
                                               odk_spine_error *err)
{
    odk_spine_world_state_context ctx;
    odk_spine_status st;
    size_t i;
    int scope_ok;

    if (json == NULL) {
        set_err(err, "null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    st = odk_spine_world_state_context_from_json(json, &ctx, err);
    if (st != ODK_SPINE_OK) {
        return st;
    }
    /* scope is opaque: it must parse as JSON (object, array or string)
     * but its content is never interpreted by the kernel */
    if (ctx.scope_json[0] == '\0') {
        scope_ok = 1;
    } else {
        odk_json_value *scope;
        odk_error jerr;

        scope = odk_json_parse(ctx.scope_json, strlen(ctx.scope_json),
                               &jerr);
        scope_ok = scope != NULL &&
                   (scope->type == ODK_JSON_OBJECT ||
                    scope->type == ODK_JSON_ARRAY ||
                    scope->type == ODK_JSON_STRING);
        odk_json_free(scope);
    }
    for (i = 0; i < ctx.variables.count && scope_ok; i++) {
        if (ctx.variables.items[i].name[0] == '\0') {
            scope_ok = 0;
        }
    }
    odk_spine_world_state_context_release(&ctx);
    if (!scope_ok) {
        set_err(err,
                "worldState: scope must parse as JSON (object, array or "
                "string) and every variable needs a name");
        return ODK_SPINE_ERR_SCHEMA;
    }
    return ODK_SPINE_OK;
}
