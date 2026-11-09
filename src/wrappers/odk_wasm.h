/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * odk_wasm.h - C interoperability wrapper for the OmniDecide kernel.
 *
 * This is the single integration layer compiled for BOTH hosts:
 *   - natively (parity runner, native tests), and
 *   - as WebAssembly via Emscripten (browser and Node.js).
 *
 * Because both hosts run the same C code, byte-identical envelopes are
 * a property of the wrapper, not something the JavaScript layer has to
 * re-derive.
 *
 * Surface (deliberately minimal - no additional decision primitives are
 * introduced here; everything below is a thin marshaling layer over the
 * public API in omnidecide.h):
 *
 *   odkw_kernel_create / odkw_kernel_free  kernel lifecycle
 *   odkw_evaluate                          one decision, JSON envelope out
 *   odkw_version / odkw_backend            identity strings
 *   odkw_last_status / odkw_last_error     diagnostics
 *   odkw_kernel_count                      live-handle bookkeeping
 *   odkw_free                              release returned envelopes
 *
 * The envelope contract:
 *   success -> the upstream five-key decision payload produced by
 *              odk_result_to_json(), optionally followed by a "meta"
 *              object (ODWK_FLAG_META) with truncation/language/work
 *              accounting.
 *   failure -> {"error":{"status":<odk_status>,"message":"..."}}.
 *   odkw_last_status() reports ODK_OK for a success envelope and the
 *   kernel status for an error envelope.  Envelopes never contain
 *   wall-clock or environment-dependent values, so they are comparable
 *   byte-for-byte across hosts.
 */

#ifndef ODK_WASM_H
#define ODK_WASM_H

#include <stddef.h>
#include <stdint.h>

#include "omnidecide.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Flags for odkw_evaluate(). */
#define ODWK_FLAG_META 0x1u /* append the "meta" accounting object */

/* Kernel text modes accepted by odkw_kernel_create(). */
#define ODWK_MODE_ASCII "ascii"     /* ODK_TEXT_ASCII_COMPAT (default) */
#define ODWK_MODE_UNICODE "unicode" /* ODK_TEXT_UNICODE */

/* Create a kernel.  mode: "ascii" (default) or "unicode".
 * language: a language code accepted by odk_lang_from_code()
 * ("en", "zh", ..., "auto", "und"; "default" is per-call only and is
 * rejected here).  NULL selects the documented defaults.
 *
 * Returns a nonzero opaque handle on success, 0 on failure with
 * odkw_last_status()/odkw_last_error() set.  Handles live in a small
 * internal registry so 32-bit (WASM) and 64-bit (native) callers can
 * pass them as integers. */
uint32_t odkw_kernel_create(const char *mode, const char *language);

/* Release a kernel handle.  Passing 0 or an already-released handle is
 * a no-op. */
void odkw_kernel_free(uint32_t handle);

/* Number of currently live kernel handles (leak diagnostics). */
uint32_t odkw_kernel_count(void);

/* Run one decision.  Options/rubric are passed as a caller-built array
 * of C strings (the JS layer builds it in WASM heap memory).
 *
 *   primitive: "choice" | "score" | "noul" | "rank" | "compare" |
 *              "extract" (exactly these; no other primitive is exposed)
 *   instruction: choice/score/rank/compare/extract instruction; for
 *              "noul" this is the statement under test
 *   options:   option strings (choice/rank/compare), rubric items
 *              (score); ignored by noul/extract (may be NULL)
 *   option_count: number of entries in options
 *   context:   evidence text (NULL is treated as "")
 *   top_k:     extract only; number of snippets (>= 1)
 *   max_ranked / max_snippet_chars / max_total_chars:
 *              output caps, 0 selects the kernel default (caps never
 *              change a numeric decision, per omnidecide.h)
 *   flags:     ODWK_FLAG_META to append the meta accounting object
 *
 * Returns a malloc'd envelope string (release with odkw_free) or NULL
 * only when the envelope itself could not be allocated (status
 * ODK_ERR_MEMORY).  Always inspect odkw_last_status(). */
char *odkw_evaluate(uint32_t handle, const char *primitive,
                    const char *instruction, const char *const *options,
                    size_t option_count, const char *context, int32_t top_k,
                    int32_t max_ranked, int32_t max_snippet_chars,
                    int32_t max_total_chars, uint32_t flags);

/* Identity strings (never NULL). */
const char *odkw_version(void); /* kernel version, e.g. "2.0.0"     */
const char *odkw_backend(void); /* always "embedded-fallback"        */

/* Diagnostics for the most recent fallible call (the module is
 * single-threaded). */
int32_t odkw_last_status(void);    /* ODK_OK, or the failing status  */
const char *odkw_last_error(void); /* message, "" when none          */
void odkw_clear_error(void);       /* reset both (used by tests)     */

/* Release a string returned by this wrapper. */
void odkw_free(void *ptr);

/* Compose the error envelope for the most recent failed call: returns a
 * malloc'd {"error":{"status":n,"message":"..."}} string built from
 * odkw_last_status()/odkw_last_error().  Kernel-creation failures have
 * no request context to evaluate, so callers (native runner, JS layer)
 * use this to emit the identical envelope shape on both hosts. */
char *odkw_last_error_envelope(void);

#ifdef __cplusplus
}
#endif

#endif /* ODK_WASM_H */
