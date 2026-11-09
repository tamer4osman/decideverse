/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_minifier.c - Markdown minifier.
 *
 * Each pattern is implemented as a hand-rolled scanner (per-function
 * notes give the exact regex being modelled).
 * `\s` is the ASCII whitespace subset, matching the byte semantics of
 * UTF-8 payloads.
 *
 */

#include "odk_internal.h"

#include <stdio.h>
#include <stdlib.h>

#define ODK_MIN_TABLE_PIPES 2

/* ------------------------------------------------------------------ */
/* Line scanners (regex equivalents)                                   */
/* ------------------------------------------------------------------ */

/* Mode-aware whitespace helpers (UNICODE mode uses the White_Space
 * property via odk_u_space_span; ASCII bytes behave identically in
 * both modes, so ASCII_COMPAT output stays byte-identical to the legacy output). */
static int mini_space_span(odk_text_mode mode, const char *s, size_t len)
{
    if (mode == ODK_TEXT_ASCII_COMPAT) {
        return odk_ascii_isspace((unsigned char)s[0]) ? 1 : 0;
    }
    return odk_u_space_span(s, len);
}

static void mini_strip_bounds(odk_text_mode mode, const char *s, size_t len,
                              size_t *lo, size_t *hi)
{
    if (mode == ODK_TEXT_ASCII_COMPAT) {
        odk_strip_bounds(s, len, lo, hi);
    } else {
        odk_u_strip_bounds(s, len, lo, hi);
    }
}

static size_t mini_trim_end(odk_text_mode mode, const char *s, size_t len)
{
    size_t lo = 0;
    size_t hi = len;
    mini_strip_bounds(mode, s, len, &lo, &hi);
    return hi;
}


/* _FENCE_PATTERN: ^\s{0,3}(```|~~~) */
static int line_is_fence(odk_text_mode mode, const char *line, size_t len)
{
    size_t i = 0;
    size_t ws_count = 0;
    while (i < len && ws_count < 3) {
        int span = mini_space_span(mode, line + i, len - i);
        if (span == 0) {
            break;
        }
        i += (size_t)span;
        ws_count++;
    }
    if (len - i < 3) {
        return 0;
    }
    if (line[i] == '`' && line[i + 1] == '`' && line[i + 2] == '`') {
        return 1;
    }
    if (line[i] == '~' && line[i + 1] == '~' && line[i + 2] == '~') {
        return 1;
    }
    return 0;
}

/* _HORIZONTAL_RULE_PATTERN: ^\s{0,3}(?:-{3,}|\*{3,}|_{3,})\s*$ */
static int line_is_horizontal_rule(odk_text_mode mode, const char *line,
                                   size_t len)
{
    size_t i = 0;
    size_t ws_count = 0;
    char marker;
    size_t run = 0;

    while (i < len && ws_count < 3) {
        int span = mini_space_span(mode, line + i, len - i);
        if (span == 0) {
            break;
        }
        i += (size_t)span;
        ws_count++;
    }
    if (len - i < 3) {
        return 0;
    }
    marker = line[i];
    if (marker != '-' && marker != '*' && marker != '_') {
        return 0;
    }
    while (i < len && line[i] == marker) {
        i++;
        run++;
    }
    if (run < 3) {
        return 0;
    }
    while (i < len) {
        int span = mini_space_span(mode, line + i, len - i);
        if (span == 0) {
            break;
        }
        i += (size_t)span;
    }
    return i == len;
}

/* _REFERENCE_LINK_PATTERN: ^\s{0,3}\[[^\]]+\]:\s*\S+\s*$ */
static int line_is_reference_link(odk_text_mode mode, const char *line,
                                  size_t len)
{
    size_t i = 0;
    size_t ws_count = 0;
    size_t label_chars = 0;
    size_t ws = 0;
    size_t nonspace = 0;

    while (i < len && ws_count < 3) {
        int span_ = mini_space_span(mode, line + i, len - i);
        if (span_ == 0) {
            break;
        }
        i += (size_t)span_;
        ws_count++;
    }
    if (i >= len || line[i] != '[') {
        return 0;
    }
    i++;
    while (i < len && line[i] != ']') {
        i++;
        label_chars++;
    }
    if (label_chars == 0 || i >= len) {
        return 0;
    }
    i++; /* ']' */
    if (i >= len || line[i] != ':') {
        return 0;
    }
    i++;
    while (i < len) {
        int span_ = mini_space_span(mode, line + i, len - i);
        if (span_ == 0) {
            break;
        }
        i += (size_t)span_;
        ws++;
    }
    if (ws == 0) {
        return 0;
    }
    while (i < len && mini_space_span(mode, line + i, len - i) == 0) {
        i++;
        nonspace++;
    }
    if (nonspace == 0) {
        return 0;
    }
    while (i < len) {
        int span_ = mini_space_span(mode, line + i, len - i);
        if (span_ == 0) {
            break;
        }
        i += (size_t)span_;
    }
    return i == len;
}

/* _BLOCKQUOTE_PATTERN: ^\s{0,3}(?:>[ \t]?)+  ->  returns the match length. */
static size_t blockquote_prefix_len(odk_text_mode mode, const char *line,
                                        size_t len)
{
    size_t i = 0;
    size_t ws_count = 0;
    size_t quotes = 0;
    size_t last = 0;

    while (i < len && ws_count < 3) {
        int span_ = mini_space_span(mode, line + i, len - i);
        if (span_ == 0) {
            break;
        }
        i += (size_t)span_;
        ws_count++;
    }
    if (i >= len || line[i] != '>') {
        return 0;
    }
    last = i;
    while (i < len && line[i] == '>') {
        i++;
        quotes++;
        last = i;
        if (i < len && (line[i] == ' ' || line[i] == '\t')) {
            i++;
            last = i;
        }
    }
    return quotes > 0 ? last : 0;
}

/* _DIVIDER_CELL_PATTERN fullmatch: :?-+:? */
static int cell_is_divider(const char *cell, size_t len)
{
    size_t i = 0;
    size_t dashes = 0;
    if (i < len && cell[i] == ':') {
        i++;
    }
    while (i < len && cell[i] == '-') {
        i++;
        dashes++;
    }
    if (dashes == 0) {
        return 0;
    }
    if (i < len && cell[i] == ':') {
        i++;
    }
    return i == len;
}

/* Compress a table row: trim the row and its outer "|", trim every cell,
 * detect a divider row (all non-empty cells match :?-+:?) and re-join
 * the cells as "|cell|cell|".
 */
static char *compress_table_row(odk_text_mode mode, const char *row,
                                size_t len)
{
    size_t lo;
    size_t hi;
    size_t core_start;
    size_t core_end;
    size_t separators = 0;
    size_t cell_count;
    size_t *starts = NULL;
    size_t *ends = NULL;
    size_t i;
    size_t idx;
    size_t non_empty = 0;
    int is_divider;
    sb_t out;

    mini_strip_bounds(mode, row, len, &lo, &hi);
    core_start = lo;
    core_end = hi;
    while (core_start < core_end && row[core_start] == '|') {
        core_start++;
    }
    while (core_end > core_start && row[core_end - 1] == '|') {
        core_end--;
    }
    for (i = core_start; i < core_end; i++) {
        if (row[i] == '|') {
            separators++;
        }
    }
    cell_count = separators + 1;

    starts = malloc(cell_count * sizeof(size_t));
    ends = malloc(cell_count * sizeof(size_t));
    if (starts == NULL || ends == NULL) {
        free(starts);
        free(ends);
        return NULL;
    }
    idx = 0;
    {
        size_t seg_start = core_start;
        for (i = core_start; i <= core_end; i++) {
            if (i == core_end || row[i] == '|') {
                starts[idx] = seg_start;
                ends[idx] = i;
                idx++;
                seg_start = i + 1;
                if (i == core_end) {
                    break;
                }
            }
        }
        cell_count = idx;
    }
    non_empty = 0;
    for (i = 0; i < cell_count; i++) {
        size_t clen = ends[i] > starts[i] ? ends[i] - starts[i] : 0;
        const char *cell = row + starts[i];
        size_t clo;
        size_t chi;
        mini_strip_bounds(mode, cell, clen, &clo, &chi);
        if (chi > clo) {
            non_empty++;
        }
    }
    is_divider = 0;
    if (non_empty > 0) {
        int all_dividers = 1;
        for (i = 0; i < cell_count; i++) {
            size_t clen = ends[i] > starts[i] ? ends[i] - starts[i] : 0;
            const char *cell = row + starts[i];
            size_t clo;
            size_t chi;
            mini_strip_bounds(mode, cell, clen, &clo, &chi);
            if (chi > clo && !cell_is_divider(cell + clo, chi - clo)) {
                all_dividers = 0;
                break;
            }
        }
        is_divider = all_dividers;
    }

    if (sb_init(&out) != 0) {
        free(starts);
        free(ends);
        return NULL;
    }
    if (sb_putc(&out, '|') != 0) {
        goto fail;
    }
    for (i = 0; i < cell_count; i++) {
        size_t clen = ends[i] > starts[i] ? ends[i] - starts[i] : 0;
        const char *cell = row + starts[i];
        size_t clo;
        size_t chi;
        if (i > 0 && sb_putc(&out, '|') != 0) {
            goto fail;
        }
        mini_strip_bounds(mode, cell, clen, &clo, &chi);
        if (is_divider) {
            if (sb_puts(&out, "---") != 0) {
                goto fail;
            }
        } else if (chi > clo) {
            if (sb_append(&out, cell + clo, chi - clo) != 0) {
                goto fail;
            }
        }
    }
    if (sb_putc(&out, '|') != 0) {
        goto fail;
    }
    free(starts);
    free(ends);
    return sb_detach(&out);

fail:
    free(starts);
    free(ends);
    sb_free(&out);
    return NULL;
}

/* _INLINE_LINK_PATTERN: !?\[([^\]]*)\]\([^)]*\)  ->  group(1), globally.
 * The optional leading '!' is part of the match, so an immediately
 * preceding '!' is dropped from the output when a link matches. */
static int append_inline_links_processed(sb_t *out, const char *line,
                                         size_t len)
{
    size_t i = 0;
    size_t copy_start = 0;

    while (i < len) {
        if (line[i] == '[') {
            size_t close_bracket = i + 1;
            while (close_bracket < len && line[close_bracket] != ']') {
                close_bracket++;
            }
            if (close_bracket < len) {
                size_t j = close_bracket + 1;
                if (j < len && line[j] == '(') {
                    size_t close_paren = j + 1;
                    while (close_paren < len && line[close_paren] != ')') {
                        close_paren++;
                    }
                    if (close_paren < len) {
                        size_t text_end = i;
                        if (text_end > copy_start &&
                            line[text_end - 1] == '!') {
                            text_end--;
                        }
                        if (sb_append(out, line + copy_start,
                                      text_end - copy_start) != 0) {
                            return -1;
                        }
                        if (sb_append(out, line + i + 1,
                                      close_bracket - i - 1) != 0) {
                            return -1;
                        }
                        copy_start = close_paren + 1;
                        i = copy_start;
                        continue;
                    }
                }
            }
        }
        i++;
    }
    if (copy_start < len) {
        if (sb_append(out, line + copy_start, len - copy_start) != 0) {
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Front-matter                                                        */
/* ------------------------------------------------------------------ */

/* _FRONTMATTER_PATTERN.match: \A---[ \t]*\n(.*?)\n---[ \t]*  (DOTALL).
 * On success returns the match end (body start) and the group(0) end. */
static int frontmatter_match(const char *text, size_t len, size_t *match_end,
                             size_t *g0_end)
{
    size_t pos = 3;
    size_t search;

    if (len < 3 || text[0] != '-' || text[1] != '-' || text[2] != '-') {
        return 0;
    }
    while (pos < len && (text[pos] == ' ' || text[pos] == '\t')) {
        pos++;
    }
    if (pos >= len || text[pos] != '\n') {
        return 0;
    }
    pos++;
    search = pos;
    while (search + 4 <= len) {
        if (text[search] == '\n' && text[search + 1] == '-' &&
            text[search + 2] == '-' && text[search + 3] == '-') {
            size_t after = search + 4;
            while (after < len && (text[after] == ' ' || text[after] == '\t')) {
                after++;
            }
            *match_end = after;
            *g0_end = after;
            return 1;
        }
        search++;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

char *odk_minify_ex(const char *text, odk_text_mode mode)
{
    sb_t output_lines;
    sb_t compressed;
    sb_t frontmatter;
    char *result;
    const char *body;
    size_t body_len;
    size_t line_start;
    int in_fence = 0;

    if (text == NULL || text[0] == '\0') {
        char *empty = malloc(1);
        if (empty != NULL) {
            empty[0] = '\0';
        }
        return empty;
    }
    if (sb_init(&output_lines) != 0) {
        return NULL;
    }
    if (sb_init(&frontmatter) != 0) {
        sb_free(&output_lines);
        return NULL;
    }

    body = text;
    body_len = strlen(text);
    {
        size_t match_end;
        size_t g0_end;
        if (frontmatter_match(text, body_len, &match_end, &g0_end)) {
            size_t lo;
            size_t hi;
            mini_strip_bounds(mode, text, g0_end, &lo, &hi);
            if (sb_append(&frontmatter, text + lo, hi - lo) != 0 ||
                sb_putc(&frontmatter, '\n') != 0) {
                goto oom;
            }
            body = text + match_end;
            body_len -= match_end;
        }
    }

    line_start = 0;
    while (line_start <= body_len) {
        size_t line_end;
        const char *line;
        size_t len;

        if (line_start == body_len && body_len > 0) {
            /* split() trailing element after a final newline: the join
             * separator model already emitted it. */
            break;
        }
        line_end = line_start;
        while (line_end < body_len && body[line_end] != '\n') {
            line_end++;
        }
        line = body + line_start;
        len = line_end - line_start;

        if (line_is_fence(mode, line, len)) {
            in_fence = !in_fence;
            {
                size_t lo;
                size_t hi;
                mini_strip_bounds(mode, line, len, &lo, &hi);
                if (sb_append(&output_lines, line + lo, hi - lo) != 0 ||
                    sb_putc(&output_lines, '\n') != 0) {
                    goto oom;
                }
            }
        } else if (in_fence) {
            size_t hi = mini_trim_end(mode, line, len);
            if (sb_append(&output_lines, line, hi) != 0 ||
                sb_putc(&output_lines, '\n') != 0) {
                goto oom;
            }
        } else if (line_is_horizontal_rule(mode, line, len) ||
                   line_is_reference_link(mode, line, len)) {
            /* dropped entirely */
        } else {
            size_t bq = blockquote_prefix_len(mode, line, len);
            const char *processed = line + bq;
            size_t plen = len - bq;
            size_t stripped_start;
            size_t stripped_end;
            char *table_row = NULL;
            sb_t link_processed;

            mini_strip_bounds(mode, processed, plen, &stripped_start,
                             &stripped_end);
            if (stripped_end > stripped_start &&
                processed[stripped_start] == '|') {
                size_t pipes = 0;
                size_t k;
                for (k = stripped_start; k < stripped_end; k++) {
                    if (processed[k] == '|') {
                        pipes++;
                    }
                }
                if (pipes >= ODK_MIN_TABLE_PIPES) {
                    table_row = compress_table_row(mode, processed + stripped_start,
                                                   stripped_end - stripped_start);
                    if (table_row == NULL) {
                        goto oom;
                    }
                    processed = table_row;
                    plen = strlen(table_row);
                }
            }
            if (sb_init(&link_processed) != 0) {
                free(table_row);
                goto oom;
            }
            if (append_inline_links_processed(&link_processed, processed,
                                              plen) != 0) {
                sb_free(&link_processed);
                free(table_row);
                goto oom;
            }
            free(table_row);
            {
                size_t hi = mini_trim_end(mode, link_processed.data,
                                          link_processed.len);
                if (sb_append(&output_lines, link_processed.data, hi) != 0 ||
                    sb_putc(&output_lines, '\n') != 0) {
                    sb_free(&link_processed);
                    goto oom;
                }
            }
            sb_free(&link_processed);
        }
        if (line_end == body_len) {
            break;
        }
        line_start = line_end + 1;
    }

    /* Collapse \n{2,} -> \n. */
    if (sb_init(&compressed) != 0) {
        goto oom;
    }
    {
        size_t i = 0;
        while (i < output_lines.len) {
            char c = output_lines.data[i];
            if (c == '\n') {
                if (sb_putc(&compressed, '\n') != 0) {
                    sb_free(&compressed);
                    goto oom;
                }
                while (i < output_lines.len && output_lines.data[i] == '\n') {
                    i++;
                }
            } else {
                if (sb_putc(&compressed, c) != 0) {
                    sb_free(&compressed);
                    goto oom;
                }
                i++;
            }
        }
    }
    sb_free(&output_lines);
    output_lines.data = NULL;
    output_lines.len = 0;
    output_lines.cap = 0;

    /* result = frontmatter + compressed body with surrounding newlines
     * trimmed (plus the trailing newline rule) */
    {
        size_t lo = 0;
        size_t hi = compressed.len;
        size_t total;
        while (lo < hi && compressed.data[lo] == '\n') {
            lo++;
        }
        while (hi > lo && compressed.data[hi - 1] == '\n') {
            hi--;
        }
        total = frontmatter.len + (hi - lo) + 2;
        result = malloc(total);
        if (result == NULL) {
            sb_free(&compressed);
            sb_free(&frontmatter);
            return NULL;
        }
        memcpy(result, frontmatter.data, frontmatter.len);
        memcpy(result + frontmatter.len, compressed.data + lo, hi - lo);
        result[frontmatter.len + (hi - lo)] = '\0';
    }
    sb_free(&compressed);
    sb_free(&frontmatter);
    {
        size_t total = strlen(result);
        if (total > 0 && result[total - 1] != '\n') {
            char *grown = realloc(result, total + 2);
            if (grown == NULL) {
                free(result);
                return NULL;
            }
            result = grown;
            result[total] = '\n';
            result[total + 1] = '\0';
        }
    }
    return result;

oom:
    sb_free(&output_lines);
    sb_free(&frontmatter);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Token estimation                                                    */
/* ------------------------------------------------------------------ */

int odk_estimate_tokens_ex(const char *text, odk_text_mode mode)
{
    size_t len;
    size_t characters = 0;
    size_t words = 0;
    int in_word = 0;
    double char_estimate;
    double word_estimate;
    double estimate;
    double rounded;

    if (text == NULL || text[0] == '\0') {
        return 0;
    }
    len = strlen(text);
    characters = odk_utf8_codepoints(text, len);
    {
        size_t i = 0;
        while (i < len) {
            int span = mini_space_span(mode, text + i, len - i);
            if (span > 0) {
                in_word = 0;
                i += (size_t)span;
            } else {
                if (!in_word) {
                    in_word = 1;
                    words++;
                }
                i++;
            }
        }
    }
    char_estimate = (double)characters / 4.0;
    word_estimate = (double)words * 1.3;
    estimate = char_estimate > word_estimate ? char_estimate : word_estimate;
    rounded = rint(estimate);
    if (rounded < 1.0) {
        return 1;
    }
    return (int)rounded;
}

char *odk_minify(const char *text)
{
    return odk_minify_ex(text, ODK_TEXT_ASCII_COMPAT);
}

int odk_estimate_tokens(const char *text)
{
    return odk_estimate_tokens_ex(text, ODK_TEXT_ASCII_COMPAT);
}

void odk_token_report(const char *original, const char *minified,
                      odk_token_report_t *out)
{
    int raw;
    int compressed;

    if (out == NULL) {
        return;
    }
    raw = odk_estimate_tokens(original);
    compressed = odk_estimate_tokens(minified);
    out->raw_tokens = raw;
    out->compressed_tokens = compressed;
    out->reduction_pct =
        raw != 0
            ? odk_round2(100.0 * (1.0 - (double)compressed / (double)raw))
            : 0.0;
}
