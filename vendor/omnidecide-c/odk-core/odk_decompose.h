/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_decompose.h - internal API of the Tier A decomposition engine
 * (F3). Not installed; the spine orchestrates it.
 *
 * Deterministic and replayable like everything else in Tier A.
 */

#ifndef ODK_DECOMPOSE_H
#define ODK_DECOMPOSE_H

#include "odk_spine_types.h"

/* Resolve the caller's params_json (an object) against the template's
 * declared parameters: unknown names and missing required parameters are
 * errors; defaults are filled in; values are stringified
 * deterministically into *out (caller releases the map). */
odk_spine_status odk_decompose_resolve_params(
    const odk_spine_decomposition_template *tmpl, const char *params_json,
    odk_spine_map *out, odk_spine_error *err);

/* Run the extraction rules of `tmpl` over `item_text` with the resolved
 * parameters already stringified in `params_used`. The output document
 * is the contract between the perception layer and the decision layer. */
odk_spine_status odk_decompose_run(const odk_spine_decomposition_template *tmpl,
                                   const char *item_text,
                                   const odk_spine_map *params_used,
                                   odk_spine_components *out,
                                   odk_spine_error *err);

#endif /* ODK_DECOMPOSE_H */
