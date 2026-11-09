/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_profiles.c - input-profile chunking + conservative auto-detector
 *.
 *
 * Chunking units per profile:
 *   PLAIN / HTML_TEXT  sentences (the kernel's classic chunker)
 *   MARKDOWN           heading + blank-line blocks
 *   CHAT / LOG         non-empty lines
 *   JSON / JSONL       top-level objects (brace-depth scan) / lines
 *   CSV / TSV          header row + each data row
 *   CODE               blank-line blocks
 *   EXTRACTED_PDF      blank-line blocks after de-hyphenation
 *
 * Everything is deterministic (pure function of the input); limits are
 * honored exactly like the classic chunker (chunk_source_max code
 * points / bytes, max_chunks cap, min 3 code points per chunk).
 *
 */

#include "odk_internal.h"
#include "odk_unicode_internal.h"

#include <stdio.h>
#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static int line_is_heading(const char *s, size_t len)
{
    size_t i = 0;
    size_t hashes = 0;
    while (i < len && s[i] == '#' && hashes < 6) {
        i++;
        hashes++;
    }
    return hashes >= 1 && i < len && (s[i] == ' ' || s[i] == '\t');
}

/* Push [start, end) after stripping; min length applies. */
static int push_stripped(odk_text_mode mode, const char *context,
                         size_t start, size_t end, str_list_t *out,
                         size_t *kept, size_t max_chunks)
{
    size_t lo;
    size_t hi;
    if (kept != NULL && *kept >= max_chunks) {
        return 0;
    }
    if (mode == ODK_TEXT_ASCII_COMPAT) {
        odk_strip_bounds(context + start, end - start, &lo, &hi);
        if (hi - lo >= 3) {
            if (str_list_push(out, context + start + lo, hi - lo) != 0) {
                str_list_free(out);
                return -1;
            }
            if (kept != NULL) {
                (*kept)++;
            }
        }
        return 0;
    }
    odk_u_strip_bounds(context + start, end - start, &lo, &hi);
    if (odk_utf8_codepoints(context + start + lo, hi - lo) >= 3) {
        if (str_list_push(out, context + start + lo, hi - lo) != 0) {
            str_list_free(out);
            return -1;
        }
        if (kept != NULL) {
            (*kept)++;
        }
    }
    return 0;
}

/* Line-based chunking: one chunk per non-empty line (CHAT, LOG, JSONL
 * fallback). */
static int chunks_by_lines(odk_text_mode mode, const char *context,
                           size_t len, str_list_t *out, size_t max_chunks)
{
    size_t start = 0;
    size_t i = 0;
    size_t kept = 0;
    while (i <= len) {
        if (i == len || context[i] == '\n') {
            if (push_stripped(mode, context, start, i, out, &kept,
                              max_chunks) != 0) {
                return -1;
            }
            start = i + 1;
        }
        i++;
    }
    return 0;
}

/* Blank-line (or heading) block chunking: MARKDOWN, CODE, PDF. */
static int chunks_by_blocks(odk_text_mode mode, const char *context,
                            size_t len, str_list_t *out, size_t max_chunks,
                            int split_on_headings)
{
    size_t start = 0;
    size_t i = 0;
    size_t kept = 0;
    while (i <= len) {
        int boundary = 0;
        if (i == len) {
            boundary = 1;
        } else if (context[i] == '\n') {
            /* a blank line (two consecutive newlines) ends the block; a
             * heading line starting right after the newline does too */
            if (i + 1 >= len || context[i + 1] == '\n') {
                boundary = 1;
            } else if (split_on_headings &&
                       line_is_heading(context + i + 1,
                                       strcspn(context + i + 1, "\n"))) {
                boundary = 1;
            }
        }
        if (boundary) {
            if (push_stripped(mode, context, start, i, out, &kept,
                              max_chunks) != 0) {
                return -1;
            }
            start = i + 1;
        }
        i++;
    }
    return 0;
}

/* CSV/TSV: the first non-empty line is the header; every data row chunk
 * repeats the header as context. */
static int chunks_by_rows(odk_text_mode mode, const char *context,
                          size_t len, char delimiter, str_list_t *out,
                          size_t max_chunks)
{
    size_t h_start = 0;
    size_t h_end = 0;
    int have_header = 0;
    size_t row_start = 0;
    size_t kept = 0;
    size_t i = 0;

    (void)delimiter;
    while (i <= len) {
        if (i == len || context[i] == '\n') {
            if (!have_header) {
                size_t lo;
                size_t hi;
                if (mode == ODK_TEXT_ASCII_COMPAT) {
                    odk_strip_bounds(context + h_start, i - h_start, &lo,
                                     &hi);
                } else {
                    odk_u_strip_bounds(context + h_start, i - h_start,
                                       &lo, &hi);
                }
                if (hi > lo) {
                    h_start += lo;
                    h_end = h_start + (hi - lo);
                    have_header = 1;
                }
            } else if (i > row_start) {
                if (kept >= max_chunks) {
                    return 0;
                }
                {
                    size_t lo;
                    size_t hi;
                    if (mode == ODK_TEXT_ASCII_COMPAT) {
                        odk_strip_bounds(context + row_start, i - row_start,
                                         &lo, &hi);
                    } else {
                        odk_u_strip_bounds(context + row_start, i - row_start,
                                           &lo, &hi);
                    }
                    if (hi > lo) {
                        sb_t sb;
                        if (sb_init(&sb) != 0) {
                            return -1;
                        }
                        if (sb_append(&sb, context + h_start,
                                      h_end - h_start) != 0 ||
                            sb_putc(&sb, '\n') != 0 ||
                            sb_append(&sb, context + row_start + lo,
                                      hi - lo) != 0) {
                            sb_free(&sb);
                            return -1;
                        }
                        if (str_list_push(out, sb.data, sb.len) != 0) {
                            sb_free(&sb);
                            str_list_free(out);
                            return -1;
                        }
                        sb_free(&sb);
                        kept++;
                    }
                }
            }
            row_start = i + 1;
        }
        i++;
    }
    return 0;
}

/* JSON: top-level array elements or objects, found with a brace/bracket
 * depth scan that respects string literals and escapes.  Falls back to
 * line chunking when the text is not array/object shaped. */
static int chunks_json(odk_text_mode mode, const char *context, size_t len,
                       str_list_t *out, size_t max_chunks)
{
    size_t i = 0;
    size_t kept = 0;
    size_t depth = 0;
    size_t obj_start = 0;
    int in_string = 0;
    int in_escape = 0;
    int any_object = 0;
    int top_bracket = 0;

    /* skip leading whitespace */
    while (i < len) {
        int span = odk_u_space_span(context + i, len - i);
        if (span > 0) {
            i += (size_t)span;
        } else {
            break;
        }
    }
    if (i >= len) {
        return 0;
    }
    if (context[i] == '[') {
        top_bracket = 1;
        i++;
    } else if (context[i] != '{') {
        return chunks_by_lines(mode, context, len, out, max_chunks);
    }

    while (i < len) {
        char c = context[i];
        if (in_string) {
            if (in_escape) {
                in_escape = 0;
            } else if (c == '\\') {
                in_escape = 1;
            } else if (c == '"') {
                in_string = 0;
            }
        } else if (c == '"') {
            in_string = 1;
        } else if (c == '{' || c == '[') {
            if (depth == 0) {
                obj_start = i;
            }
            depth++;
        } else if (c == '}' || c == ']') {
            if (depth > 0) {
                depth--;
                if (depth == 0) {
                    any_object = 1;
                    if (kept >= max_chunks) {
                        return 0;
                    }
                    if (str_list_push(out, context + obj_start,
                                      i + 1 - obj_start) != 0) {
                        str_list_free(out);
                        return -1;
                    }
                    kept++;
                }
            } else if (top_bracket && c == ']') {
                break; /* end of the top-level array */
            }
        }
        i++;
    }
    if (!any_object) {
        str_list_free(out);
        str_list_init(out);
        return chunks_by_lines(mode, context, len, out, max_chunks);
    }
    return 0;
}

/* EXTRACTED_PDF: rejoin hyphenated line breaks (word-\nnext -> wordnext)
 * then blank-line blocks.  The de-hyphenated copy is heap-allocated. */
static int chunks_pdf(odk_text_mode mode, const char *context, size_t len,
                      str_list_t *out, size_t max_chunks)
{
    sb_t sb;
    size_t i = 0;
    char *text;
    int rc;

    if (sb_init(&sb) != 0) {
        return -1;
    }
    while (i < len) {
        if (i + 1 < len && context[i] == '-' && context[i + 1] == '\n' &&
            i > 0) {
            /* only join [a-z]-\n[a-z] (lowercase letters) */
            unsigned char prev = (unsigned char)context[i - 1];
            unsigned char next = i + 2 < len
                                     ? (unsigned char)context[i + 2]
                                     : 0;
            if ((prev >= 'a' && prev <= 'z') &&
                (next >= 'a' && next <= 'z')) {
                i++; /* drop the hyphen and the newline */
                i++;
                continue;
            }
        }
        if (sb_putc(&sb, context[i]) != 0) {
            sb_free(&sb);
            return -1;
        }
        i++;
    }
    text = sb_detach(&sb);
    if (text == NULL) {
        return -1;
    }
    rc = chunks_by_blocks(mode, text, strlen(text), out, max_chunks, 0);
    free(text);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Public dispatcher                                                   */
/* ------------------------------------------------------------------ */

int odk_profile_chunks(odk_text_mode mode, odk_input_profile profile,
                       size_t chunk_source_max, size_t max_chunks,
                       const char *context, str_list_t *out)
{
    size_t len;

    str_list_init(out);
    if (context == NULL || context[0] == '\0') {
        return 0;
    }
    len = strlen(context);
    if (mode == ODK_TEXT_ASCII_COMPAT) {
        if (len > chunk_source_max) {
            len = chunk_source_max;
        }
    } else {
        len = odk_u_truncate_cp(context, len, chunk_source_max);
    }

    switch (profile) {
    case ODK_PROFILE_MARKDOWN:
        return chunks_by_blocks(mode, context, len, out, max_chunks, 1);
    case ODK_PROFILE_CHAT:
    case ODK_PROFILE_LOG:
    case ODK_PROFILE_JSONL:
        return chunks_by_lines(mode, context, len, out, max_chunks);
    case ODK_PROFILE_JSON:
        return chunks_json(mode, context, len, out, max_chunks);
    case ODK_PROFILE_CSV:
        return chunks_by_rows(mode, context, len, ',', out, max_chunks);
    case ODK_PROFILE_TSV:
        return chunks_by_rows(mode, context, len, '\t', out, max_chunks);
    case ODK_PROFILE_CODE:
        return chunks_by_blocks(mode, context, len, out, max_chunks, 0);
    case ODK_PROFILE_EXTRACTED_PDF:
        return chunks_pdf(mode, context, len, out, max_chunks);
    case ODK_PROFILE_PLAIN:
    case ODK_PROFILE_HTML_TEXT:
    case ODK_PROFILE_AUTO:
    default:
        /* classic sentence chunking lives in odk_kernel.c */
        return -2;
    }
}

/* ------------------------------------------------------------------ */
/* Conservative auto-detector                               */
/* ------------------------------------------------------------------ */

static size_t count_lines(const char *text, size_t len, size_t *non_empty)
{
    size_t lines = 0;
    size_t i = 0;
    size_t start = 0;
    *non_empty = 0;
    while (i <= len) {
        if (i == len || text[i] == '\n') {
            if (i > start) {
                (*non_empty)++;
            }
            lines++;
            start = i + 1;
        }
        i++;
    }
    return lines;
}

static int line_starts_with_timestamp(const char *s, size_t len)
{
    /* YYYY-MM-DD or YYYY/MM/DD at line start (>= 10 chars) */
    if (len < 10) {
        return 0;
    }
    if (s[4] != '-' && s[4] != '/') {
        return 0;
    }
    if (!(s[0] >= '0' && s[0] <= '9') || !(s[1] >= '0' && s[1] <= '9') ||
        !(s[2] >= '0' && s[2] <= '9') || !(s[3] >= '0' && s[3] <= '9')) {
        return 0;
    }
    {
        size_t k;
        for (k = 0; k < 10; k++) {
            if (k == 4 || k == 7) {
                continue;
            }
            if (!(s[k] >= '0' && s[k] <= '9')) {
                return 0;
            }
        }
    }
    return 1;
}

static int line_starts_with_level(const char *s, size_t len)
{
    static const char *const kLevels[] = {"DEBUG", "INFO", "WARN ",
                                          "ERROR", "FATAL", "TRACE"};
    size_t k;
    for (k = 0; k < sizeof(kLevels) / sizeof(kLevels[0]); k++) {
        size_t l = strlen(kLevels[k]);
        if (len >= l && strncmp(s, kLevels[k], l) == 0) {
            return 1;
        }
    }
    /* [HH:MM:SS] bracketed time at line start */
    if (len >= 10 && s[0] == '[' && s[3] == ':' && s[6] == ':' &&
        s[9] == ']') {
        size_t k;
        for (k = 1; k < 9; k++) {
            if (k == 3 || k == 6) {
                continue;
            }
            if (!(s[k] >= '0' && s[k] <= '9')) {
                return 0;
            }
        }
        return 1;
    }
    return 0;
}

/* TRUE when the line contains an HTML tag: '<' followed by
 * an optional '/' then a letter and a tag name, ending with '>' (for
 * example <p>, </p>, <h1>, <br/>.  Plain prose with angle brackets in
 * comparisons ("a < b") has no letter immediately after '<'. */
static int line_has_html_tag(const char *s, size_t len)
{
    size_t i;
    for (i = 0; i + 1 < len; i++) {
        if (s[i] != '<') {
            continue;
        }
        {
            size_t j = i + 1;
            if (j < len && s[j] == '/') {
                j++;
            }
            if (j < len && ((s[j] >= 'a' && s[j] <= 'z') ||
                            (s[j] >= 'A' && s[j] <= 'Z'))) {
                size_t k = j + 1;
                while (k < len && s[k] != '>' && s[k] != '<') {
                    k++;
                }
                if (k < len && s[k] == '>') {
                    return 1;
                }
            }
        }
    }
    return 0;
}

static int line_is_speaker_message(const char *s, size_t len)
{
    /* "speaker: message" - a short prefix without control chars, then a
     * colon+space early in the line (within the first 24 bytes).
     * a leading [bracketed] token (chat-export timestamps
     * like "[09:14] ") is skipped first - measured gap on the labeled
     * corpus, where every bracketed chat line read as plain. */
    if (len > 0 && s[0] == '[') {
        size_t close = 1;
        while (close < len && s[close] != ']') {
            if (s[close] == '\n') {
                close = len;
                break;
            }
            close++;
        }
        if (close < len && close + 2 < len && s[close + 1] == ' ') {
            s += close + 2;
            len -= close + 2;
        }
    }
    {
    size_t i = 0;
    size_t colons = 0;
    while (i < len && i < 24) {
        if (s[i] == ':') {
            colons = i;
            break;
        }
        if (s[i] == ' ' && i > 0) {
            return 0; /* spaces before the colon => prose, not a handle */
        }
        i++;
    }
    if (i >= 24 || i == 0 || colons == 0) {
        return 0;
    }
    if (colons + 1 >= len || s[colons + 1] != ' ') {
        return 0;
    }
    return 1;
    }
}

static size_t count_delimiter(const char *s, size_t len, char d)
{
    size_t i;
    size_t count = 0;
    for (i = 0; i < len; i++) {
        if (s[i] == d) {
            count++;
        }
    }
    return count;
}

/* PDF-extraction markers - "Section N" / "Table N"
 * captions, "Page N of M" footers, and lone page-number lines (1-3
 * digits, nothing else).  Case-sensitive capitals: prose does not
 * capitalise these shapes at line starts by accident often enough to
 * matter, and the rule additionally needs wrap evidence to fire. */
static int line_is_pdf_marker(const char *s, size_t len)
{
    static const char *const kWords[] = {"Section ", "Table "};
    size_t k;

    for (k = 0; k < sizeof(kWords) / sizeof(kWords[0]); k++) {
        size_t wl = strlen(kWords[k]);
        if (len > wl && strncmp(s, kWords[k], wl) == 0) {
            size_t j = wl;
            while (j < len && s[j] >= '0' && s[j] <= '9') {
                j++;
            }
            if (j == len) { /* digits to end of line, nothing else */
                return 1;
            }
        }
    }
    if (len > 5 && strncmp(s, "Page ", 5) == 0) {
        /* Page N of M */
        size_t j = 5;
        while (j < len && s[j] >= '0' && s[j] <= '9') {
            j++;
        }
        if (j > 5 && j + 4 <= len && s[j] == ' ' && s[j + 1] == 'o' &&
            s[j + 2] == 'f' && s[j + 3] == ' ') {
            j += 4;
            while (j < len && s[j] >= '0' && s[j] <= '9') {
                j++;
            }
            if (j == len) {
                return 1;
            }
        }
    }
    /* lone page number: 1-3 digits only */
    if (len >= 1 && len <= 3) {
        size_t j;
        for (j = 0; j < len; j++) {
            if (!(s[j] >= '0' && s[j] <= '9')) {
                return 0;
            }
        }
        return 1;
    }
    return 0;
}

/* The detector's scan state and per-concern helpers, used by
 * odk_detect_input_profile. */
struct profile_scan {
    size_t ts_lines;
    size_t md_score;
    size_t code_score;
    size_t chat_lines;
    size_t csv_consistent;
    size_t tsv_consistent;
    size_t csv_lines;
    size_t sentence_punct;
    size_t html_tag_lines;
    size_t first_commas;
    size_t first_tabs;
    size_t pdf_wraps;
    size_t pdf_hyphens;
    size_t pdf_markers;
    int csv_headerish;
    int have_first_line;
    size_t comment_lines;
    int data_headerish;
    int have_data_line;
    size_t last_start;
    size_t last_len;
    int prev_immediate;
};

/* Pdf signal: adjacent mid-sentence wrap (previous
 * non-empty line ends lowercase and this line starts lowercase, with
 * NO blank line between) and hyphen line-end breaks.  Blank lines
 * separate pdf paragraphs, so adjacency matters. */
static void scan_note_pdf_wrap(struct profile_scan *st,
                               const char *text,
                               const char *line,
                               size_t l,
                               size_t trimmed)
{
    if (st->last_start != (size_t)-1 && st->prev_immediate) {
        size_t last_end = st->last_len;
        while (last_end > 0 &&
               (text[st->last_start + last_end - 1] == '\r' ||
                text[st->last_start + last_end - 1] == ' ')) {
            last_end--;
        }
        if (last_end > 0) {
            char le = text[st->last_start + last_end - 1];
            char cs = line[trimmed];
            if (le >= 'a' && le <= 'z' && cs >= 'a' &&
                cs <= 'z') {
                st->pdf_wraps++;
            } else if (le == '-' && last_end >= 2 &&
                       text[st->last_start + last_end - 2] >= 'a' &&
                       text[st->last_start + last_end - 2] <= 'z' &&
                       cs >= 'a' && cs <= 'z') {
                st->pdf_hyphens++;
            }
        }
    }
    if (line_is_pdf_marker(line + trimmed, l - trimmed)) {
        st->pdf_markers++;
    }
    (void)l;
}

/* A space-free comma header (>= 3 columns).
 * Used for BOTH the first non-empty line (csv_headerish) and the
 * first non-comment data line (data_headerish) - the two call sites
 * carried identical blocks before. */
static void scan_note_header_shape(const char *line,
                                   size_t l,
                                   size_t trimmed,
                                   int *headerish)
{
    size_t commas = 0;
    int spaced = 0;
    size_t j;
    for (j = 0; j < l - trimmed; j++) {
        if (line[trimmed + j] == ',') {
            commas++;
        } else if (line[trimmed + j] == ' ' ||
                   line[trimmed + j] == '\t') {
            spaced = 1;
        }
    }
    if (commas >= 2 && !spaced) {
        *headerish = 1;
    }
}

/* per-line signals: timestamp/level lines, speaker lines, HTML tags,
 * markdown scores, code indentation/punctuation and end-of-line
 * sentence punctuation. */
static void scan_note_line_signals(struct profile_scan *st,
                                   const char *line,
                                   size_t l,
                                   size_t trimmed)
{
    if (line_starts_with_timestamp(line, l) ||
        line_starts_with_level(line, l)) {
        st->ts_lines++;
    }
    if (line_is_speaker_message(line, l)) {
        st->chat_lines++;
    }
    if (line_has_html_tag(line, l)) {
        st->html_tag_lines++;
    }
    if (line_is_heading(line, l)) {
        st->md_score += 2;
    }
    if (l >= 3 && (line[0] == '-' || line[0] == '*' ||
                   line[0] == '+') &&
        (line[1] == ' ')) {
        st->md_score++;
    }
    if (line[0] == '|' && l > 1 &&
        line[l - 1] == '|') {
        st->md_score++;
    }
    if (trimmed >= 4 || (trimmed > 0 && line[0] == '\t')) {
        st->code_score++;
    }
    {
        size_t k;
        for (k = 0; k < l; k++) {
            if (line[k] == '{' || line[k] == '}' ||
                line[k] == ';' || line[k] == '(') {
                st->code_score++;
                break;
            }
        }
    }
    {
        /* Count line-END sentence punctuation (prose
         * sentences), not every '.' character - attribute dots
         * (ctx.lag) broke CODE detection on the labeled corpus. */
        size_t last = l;
        while (last > 0 &&
               (line[last - 1] == ' ' || line[last - 1] == '\r')) {
            last--;
        }
        if (last > 0 && (line[last - 1] == '.' ||
                         line[last - 1] == '!' ||
                         line[last - 1] == '?')) {
            st->sentence_punct++;
        }
    }
}

/* CSV/TSV column-count consistency: the first line with >= 2
 * delimiters fixes the expected count; later lines must match. */
static void scan_note_delims(struct profile_scan *st,
                             const char *line,
                             size_t l)
{
    size_t commas = count_delimiter(line, l, ',');
    size_t tabs = count_delimiter(line, l, '\t');
    if (commas >= 2) {
        st->csv_lines++;
        if (st->first_commas == (size_t)-1) {
            st->first_commas = commas;
            st->csv_consistent = 1;
        } else if (commas == st->first_commas) {
            st->csv_consistent++;
        }
    }
    if (tabs >= 2) {
        if (st->first_tabs == (size_t)-1) {
            st->first_tabs = tabs;
            st->tsv_consistent = 1;
        } else if (tabs == st->first_tabs) {
            st->tsv_consistent++;
        }
    }
}

/* rule 2 rescan: count lines that parse as standalone JSON objects
 * or arrays. */
static size_t count_jsonl_object_lines(const char *text, size_t len)
{
    size_t json_lines = 0;
    size_t i = 0;
    size_t start = 0;
    while (i <= len) {
        if (i == len || text[i] == '\n') {
            if (i > start) {
                odk_json_value *v = odk_json_parse(text + start,
                                                   i - start, NULL);
                if (v != NULL) {
                    if (v->type == ODK_JSON_OBJECT ||
                        v->type == ODK_JSON_ARRAY) {
                        json_lines++;
                    }
                    odk_json_free(v);
                }
            }
            start = i + 1;
        }
        i++;
    }
    return json_lines;
}

/* rule 2b rescan: count lines starting with a FULL ISO-8601
 * timestamp (YYYY-MM-DDT...); bare dates stay prose and never
 * match. */
static size_t count_full_iso_lines(const char *text, size_t len)
{
    size_t full_ts = 0;
    size_t i = 0;
    size_t start = 0;
    while (i <= len) {
        if (i == len || text[i] == '\n') {
            if (i > start) {
                const char *line = text + start;
                size_t l = i - start;
                if (l > 11 && line_starts_with_timestamp(line, l) &&
                    line[10] == 'T') {
                    full_ts++;
                }
            }
            start = i + 1;
        }
        i++;
    }
    return full_ts;
}

/* rule 3 rescan: count lines whose tab count equals the expected
 * count. */
static size_t count_lines_with_tabs(const char *text, size_t len,
                                    size_t first_tabs)
{
    size_t tab_lines = 0;
    size_t i = 0;
    size_t start = 0;
    while (i <= len) {
        if (i == len || text[i] == '\n') {
            if (count_delimiter(text + start, i - start, '\t') ==
                first_tabs) {
                tab_lines++;
            }
            start = i + 1;
        }
        i++;
    }
    return tab_lines;
}

odk_input_profile odk_detect_input_profile(const char *text,
                                           char *reason_buf,
                                           size_t reason_cap)
{
    size_t len;
    size_t non_empty = 0;
    size_t i = 0;
    size_t start = 0;
    /* The scan state lives in struct profile_scan. */
    struct profile_scan st;
    memset(&st, 0, sizeof(st));
    st.first_commas = (size_t)-1;
    st.first_tabs = (size_t)-1;
    st.last_start = (size_t)-1;

    if (reason_buf != NULL && reason_cap > 0) {
        reason_buf[0] = '\0';
    }
    if (text == NULL || text[0] == '\0') {
        if (reason_buf != NULL) {
            snprintf(reason_buf, reason_cap, "empty input");
        }
        return ODK_PROFILE_PLAIN;
    }
    len = strlen(text);

    /* 1. JSON: whole text parses as one JSON value (depth-capped scan
     *    for structure, strict parse for the final say). */
    {
        odk_json_value *v = odk_json_parse(text, len, NULL);
        if (v != NULL) {
            odk_input_profile p = ODK_PROFILE_PLAIN;
            const char *why = "parses as a single JSON value";
            if (v->type == ODK_JSON_ARRAY) {
                p = ODK_PROFILE_JSON;
            } else if (v->type == ODK_JSON_OBJECT) {
                p = ODK_PROFILE_JSON;
            }
            odk_json_free(v);
            if (p == ODK_PROFILE_JSON) {
                if (reason_buf != NULL) {
                    snprintf(reason_buf, reason_cap, "%s", why);
                }
                return p;
            }
        }
    }

    (void)count_lines(text, len, &non_empty); /* only the non-empty count gates */
    if (non_empty < 2) {
        /* A single non-empty line is not enough evidence
         * for ANY structured profile - stays PLAIN (conservative gate;
         * the reviewer's short adversarial samples relied on it). */
        if (reason_buf != NULL) {
            snprintf(reason_buf, reason_cap,
                     "fewer than 2 non-empty lines");
        }
        return ODK_PROFILE_PLAIN;
    }

    /* line-wise statistics */
    while (i <= len) {
        if (i == len || text[i] == '\n') {
            size_t l = i - start;
            const char *line = text + start;
            size_t trimmed = 0;
            size_t k;
            for (k = 0; k < l; k++) {
                if (line[k] == ' ' || line[k] == '\t') {
                    trimmed++;
                } else {
                    break;
                }
            }
            if (l > trimmed) {
                /* The five per-line concerns live in the scan_note_*
                 * helpers (checked in a fixed order). */
                scan_note_pdf_wrap(&st, text, line, l, trimmed);
                /* First non-empty line is a space-free
                 * comma header (>= 3 columns) - enables the short CSV
                 * gate. */
                if (!st.have_first_line) {
                    st.have_first_line = 1;
                    scan_note_header_shape(line, l, trimmed,
                                           &st.csv_headerish);
                }
                /* Comment lines + first-data-line header */
                if (trimmed < l && line[trimmed] == '#') {
                    st.comment_lines++;
                } else if (!st.have_data_line) {
                    st.have_data_line = 1;
                    scan_note_header_shape(line, l, trimmed,
                                           &st.data_headerish);
                }
                scan_note_line_signals(&st, line, l, trimmed);
                scan_note_delims(&st, line, l);
            }
            st.prev_immediate = (l > trimmed);
            if (l > trimmed) {
                st.last_start = start;
                st.last_len = l;
            } else {
                st.last_start = (size_t)-1;
                st.last_len = 0;
            }
            start = i + 1;
        }
        i++;
    }

    /* 2. JSONL: >= 90% of non-empty lines parse as standalone objects.
     * this check now runs BEFORE CSV/TSV - comma-rich JSONL
     * objects (five-plus fields) satisfied the consistent-comma rule
     * first and every jsonl corpus sample read as CSV. */
    {
        size_t json_lines = count_jsonl_object_lines(text, len);
        if (json_lines * 10 >= non_empty * 9) {
            if (reason_buf != NULL) {
                snprintf(reason_buf, reason_cap,
                         "line-wise JSON objects");
            }
            return ODK_PROFILE_JSONL;
        }
    }

    /* 2b. TWO-line excerpts.  Real chat/log/csv excerpts
     * are routinely two lines; the old < 3-line gate returned PLAIN
     * for all of them.  Classify only under UNANIMOUS, unambiguous
     * signals (every line carries the pattern); anything weaker stays
     * PLAIN.  JSON/JSONL above already ran, so those shapes are safe. */
    if (non_empty == 2) {
        if (st.chat_lines == 2) {
            if (reason_buf != NULL) {
                snprintf(reason_buf, reason_cap,
                         "two-line unanimous speaker lines");
            }
            return ODK_PROFILE_CHAT;
        }
        {
            /* both lines start with FULL ISO-8601 timestamps
             * (YYYY-MM-DDT...): bare dates stay prose ("2024-10-08 was
             * the day we shipped") and never match. */
            size_t full_ts = count_full_iso_lines(text, len);
            if (full_ts == 2) {
                if (reason_buf != NULL) {
                    snprintf(reason_buf, reason_cap,
                             "two-line unanimous ISO timestamps");
                }
                return ODK_PROFILE_LOG;
            }
        }
        if (st.csv_headerish && st.csv_lines == 2 &&
            st.csv_consistent == 2) {
            if (reason_buf != NULL) {
                snprintf(reason_buf, reason_cap,
                         "two-line csv header + row");
            }
            return ODK_PROFILE_CSV;
        }
        if (reason_buf != NULL) {
            snprintf(reason_buf, reason_cap,
                     "two lines without a unanimous signal");
        }
        return ODK_PROFILE_PLAIN;
    }

    /* 3. CSV/TSV: >= 90% of non-empty lines share the delimiter count.
     * with a space-free comma header on the first line,
     * three lines (header + two rows) suffice - short exports are
     * common; without the header shape the >= 5-line gate stays. */
    if (non_empty >= 5 && st.csv_lines * 10 >= non_empty * 9 &&
        st.csv_consistent * 10 >= st.csv_lines * 9) {
        if (reason_buf != NULL) {
            snprintf(reason_buf, reason_cap,
                     "consistent comma column count");
        }
        return ODK_PROFILE_CSV;
    }
    if (non_empty >= 5 && st.tsv_consistent * 10 >= non_empty * 9 &&
        st.first_tabs >= 2) {
        size_t tab_lines = count_lines_with_tabs(text, len,
                                                 st.first_tabs);
        if (tab_lines * 10 >= non_empty * 9) {
            if (reason_buf != NULL) {
                snprintf(reason_buf, reason_cap,
                         "consistent tab column count");
            }
            return ODK_PROFILE_TSV;
        }
    }

    if (non_empty >= 3 && st.csv_headerish &&
        st.csv_lines == non_empty &&
        st.csv_consistent == st.csv_lines) {
        if (reason_buf != NULL) {
            snprintf(reason_buf, reason_cap,
                     "comma header + consistent rows (short file)");
        }
        return ODK_PROFILE_CSV;
    }

    /* 3c. comment-prefixed CSV exports.  Tool-generated
     * CSV often carries leading '#' comment lines (export metadata);
     * the comment line defeats rules 3/3b (its comma count is 0) and
     * feeds the MARKDOWN heading score, so such exports read as
     * MARKDOWN.  Conservative resolution toward CSV: comments are a
     * strict minority, EVERY remaining line is comma-consistent
     * (>= 2 commas), and the first non-comment line is a space-free
     * comma header.  A genuine markdown document never has a
     * comma-unanimous body (prose varies; tables use pipes), so the
     * signal cannot capture it (train corpus verified: no markdown
     * sample has this shape - see test_profiles_detector_412). */
    if (non_empty >= 4 && st.comment_lines > 0 &&
        st.comment_lines * 3 <= non_empty &&
        st.csv_lines == non_empty - st.comment_lines &&
        st.csv_consistent == st.csv_lines && st.csv_lines >= 3 &&
        st.data_headerish) {
        if (reason_buf != NULL) {
            snprintf(reason_buf, reason_cap,
                     "'#' comment lines + unanimous comma rows");
        }
        return ODK_PROFILE_CSV;
    }

    /* 4. LOG: >= 70% timestamped/level lines */
    if (st.ts_lines * 10 >= non_empty * 7) {
        if (reason_buf != NULL) {
            snprintf(reason_buf, reason_cap,
                     "timestamped/level line majority");
        }
        return ODK_PROFILE_LOG;
    }

    /* 5. CODE: indented majority + punctuation + low sentence density */
    if (st.code_score * 10 >= non_empty * 5 &&
        st.sentence_punct * 20 < non_empty) {
        if (reason_buf != NULL) {
            snprintf(reason_buf, reason_cap,
                     "indentation + code punctuation density");
        }
        return ODK_PROFILE_CODE;
    }

    /* 6. MARKDOWN: headings / list / table markers */
    if (st.md_score >= 2 || st.md_score * 10 >= non_empty * 2) {
        if (reason_buf != NULL) {
            snprintf(reason_buf, reason_cap,
                     "heading/list/table marker density");
        }
        return ODK_PROFILE_MARKDOWN;
    }

    /* 7. CHAT: speaker-prefixed lines */
    if (st.chat_lines * 10 >= non_empty * 7) {
        if (reason_buf != NULL) {
            snprintf(reason_buf, reason_cap,
                     "speaker-prefixed message lines");
        }
        return ODK_PROFILE_CHAT;
    }

    /* 8. HTML_TEXT : a majority of lines carrying an HTML
     * tag.  An earlier detector never claimed html_text; the labeled
     * corpus shows extracted-HTML prose is common enough to deserve
     * the signal (angle-bracket tags are unambiguous in the corpus). */
    if (st.html_tag_lines * 2 >= non_empty) {
        if (reason_buf != NULL) {
            snprintf(reason_buf, reason_cap,
                     "HTML tag line majority");
        }
        return ODK_PROFILE_HTML_TEXT;
    }

    /* 9. EXTRACTED_PDF : earlier detectors had NO rule
     * for this profile (such text always fell back to PLAIN).  PDF text extraction leaves two signatures the
     * other profiles do not produce together: hard-wrapped prose
     * (adjacent lines breaking MID-sentence, i.e. one line ends
     * lowercase and the next starts lowercase, or a hyphen break)
     * plus page artifacts ("Section N"/"Table N" captions, "Page N
     * of M", lone page-number lines).  Requiring BOTH keeps the rule
     * conservative: wrapped email prose without page artifacts stays
     * PLAIN, and every structured rule above already had its say, so
     * a mistake lands on PLAIN - never on another structured
     * profile. */
    if (st.pdf_wraps + st.pdf_hyphens >= 1 && st.pdf_markers >= 1) {
        if (reason_buf != NULL) {
            snprintf(reason_buf, reason_cap,
                     "mid-sentence wrapped prose + page/section "
                     "markers");
        }
        return ODK_PROFILE_EXTRACTED_PDF;
    }

    if (reason_buf != NULL) {
        snprintf(reason_buf, reason_cap, "no strong signal; plain text");
    }
    return ODK_PROFILE_PLAIN;
}
