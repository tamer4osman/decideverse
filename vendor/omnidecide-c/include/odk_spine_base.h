/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_spine_base.h - shared base declarations for the OmniDecide spine.
 *
 * This header is hand-written and deliberately tiny: status codes, the
 * error carrier and hash helpers. Every record type is GENERATED from
 * schema/omnidecide.schema (odk-spine/odk_spine_types.h) and must not be
 * redeclared by hand.
 */

#ifndef ODK_SPINE_BASE_H
#define ODK_SPINE_BASE_H

#include "omnidecide.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Status codes for spine operations                                   */
/* ------------------------------------------------------------------ */

typedef enum odk_spine_status {
    ODK_SPINE_OK = 0,
    ODK_SPINE_ERR_ARGUMENT = 1,   /* API misuse: NULL, bad JSON, bad name   */
    ODK_SPINE_ERR_MEMORY = 2,     /* allocation failure                     */
    ODK_SPINE_ERR_SCHEMA = 3,     /* record failed schema validation        */
    ODK_SPINE_ERR_REFERENTIAL = 4,/* dangling reference in registered data  */
    ODK_SPINE_ERR_NOT_FOUND = 5,  /* unknown id, version or time interval   */
    ODK_SPINE_ERR_STATE = 6,      /* illegal lifecycle transition           */
    ODK_SPINE_ERR_RANGE = 7       /* numeric value outside the exact range  */
} odk_spine_status;

/* Every spine call reports through this structure (kernel style). */
typedef struct {
    char text[256];
} odk_spine_error;

void odk_spine_error_init(odk_spine_error *err);

/* ------------------------------------------------------------------ */
/* Schema fingerprint (F5)                                             */
/* ------------------------------------------------------------------ */

/* The fingerprint of the schema this build was generated from: FNV-1a 64
 * over the exact bytes of schema/omnidecide.schema, lowercase hex. The
 * value macro is defined by the GENERATED odk_spine_types.h; the fallback
 * only guards standalone inclusion. */
#ifndef ODK_SPINE_SCHEMA_FP_VALUE
#define ODK_SPINE_SCHEMA_FP_VALUE "unknown"
#endif
#define ODK_SPINE_SCHEMA_FINGERPRINT ODK_SPINE_SCHEMA_FP_VALUE

/* Compute the fingerprint of arbitrary bytes (same algorithm). */
void odk_spine_fnv1a64_init(uint64_t *h);
void odk_spine_fnv1a64_update(uint64_t *h, const void *data, size_t len);
void odk_spine_fnv1a64_hex(uint64_t h, char out[17]);

/* ------------------------------------------------------------------ */
/* Deterministic identifier helpers (kernel-style, no RNG)             */
/* ------------------------------------------------------------------ */

/* content_hash / context_hash: FNV-1a 64 over the exact bytes, hex. */
void odk_spine_hash_hex(const void *data, size_t len, char out[17]);

#ifdef __cplusplus
}
#endif

#endif /* ODK_SPINE_BASE_H */
