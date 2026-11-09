/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_spine_decompose.c - F3 orchestration: resolve the decomposition
 * template as-of t (exact version or newest valid), resolve parameters
 * against the template's declared contract, run the Tier A extraction
 * engine, and return the Components document — the contract between the
 * perception layer and the decision layer.
 */

#include "odk_spine_private.h"

#include "odk_decompose.h"

#include <stdlib.h>
#include <string.h>

odk_spine_status odk_spine_decompose(odk_spine *spine,
                                     const char *template_id,
                                     int64_t template_version,
                                     const char *item_text,
                                     const char *params_json, int64_t t,
                                     odk_spine_components *out,
                                     odk_spine_error *err)
{
    const odk_spine_decomposition_template *tmpl = NULL;
    odk_spine_map params_used;
    odk_spine_status st;

    if (spine == NULL || template_id == NULL || out == NULL) {
        set_err(err, "null argument");
        return ODK_SPINE_ERR_ARGUMENT;
    }
    st = odk_spine_template_at(spine, template_id, template_version, t,
                               &tmpl, err);
    if (st != ODK_SPINE_OK) {
        return st;
    }
    st = odk_decompose_resolve_params(tmpl, params_json, &params_used, err);
    if (st != ODK_SPINE_OK) {
        return st;
    }
    st = odk_decompose_run(tmpl, item_text, &params_used, out, err);
    odk_spine_map_release_records(&params_used);
    return st;
}
