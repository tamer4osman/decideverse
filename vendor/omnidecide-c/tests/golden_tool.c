/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * golden_tool.c - golden corpus generator and checker (docs/GOLDEN.md).
 *
 *   golden_tool gen  [dir]   write <dir>/inputs.jsonl + <dir>/expected.jsonl
 *   golden_tool check [dir]  re-run every case, require byte-identical output
 *
 * Determinism: no clock, no RNG (one fixed-LCG byte stream documented in
 * the spec), no environment reads.  Exit 0 = pass, 1 = mismatch/error.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "omnidecide.h"
#include "golden_lang.h"

#define G_MIN_CASES 2000
#define G_MAXOPTS 64
#define G_MAXRUBRIC 16

/* ---------------- fixed LCG (docs/GOLDEN.md section 2) -------------- */
static uint64_t g_lcg = 0x243F6A8885A308D3ULL;
static unsigned char lcg_byte(void)
{
    g_lcg = g_lcg * 6364136223846793005ULL + 1442695040888963407ULL;
    unsigned char b = (unsigned char)(g_lcg >> 33);
    return b ? b : 0x41; /* no NUL bytes inside C-string contexts */
}

/* ---------------- growable string buffer --------------------------- */
typedef struct {
    char *s;
    size_t len;
    size_t cap;
} sb;

static int sb_grow(sb *b, size_t need)
{
    size_t cap;
    char *ns;
    if (b->len + need + 1 <= b->cap) {
        return 0;
    }
    cap = b->cap ? b->cap : 256;
    while (cap < b->len + need + 1) {
        cap *= 2;
    }
    ns = realloc(b->s, cap);
    if (ns == NULL) {
        return -1;
    }
    b->s = ns;
    b->cap = cap;
    return 0;
}
static int sb_putc(sb *b, char c)
{
    if (sb_grow(b, 1) != 0) {
        return -1;
    }
    b->s[b->len++] = c;
    b->s[b->len] = '\0';
    return 0;
}
static int sb_puts(sb *b, const char *s)
{
    size_t n = strlen(s);
    if (sb_grow(b, n) != 0) {
        return -1;
    }
    memcpy(b->s + b->len, s, n);
    b->len += n;
    b->s[b->len] = '\0';
    return 0;
}
static int sb_printf(sb *b, const char *fmt, ...)
{
    va_list ap;
    int n;
    int rc;
    va_start(ap, fmt);
    n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return -1;
    }
    if (sb_grow(b, (size_t)n) != 0) {
        return -1;
    }
    va_start(ap, fmt);
    rc = vsnprintf(b->s + b->len, (size_t)n + 1, fmt, ap);
    va_end(ap);
    if (rc != n) {
        return -1;
    }
    b->len += (size_t)n;
    return 0;
}
static void sb_free(sb *b)
{
    free(b->s);
    b->s = NULL;
    b->len = 0;
    b->cap = 0;
}

/* ---------------- base64 ------------------------------------------- */
static const char B64A[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64_encode(const unsigned char *src, size_t len)
{
    size_t out_len = ((len + 2) / 3) * 4;
    char *out = malloc(out_len + 1);
    size_t i;
    size_t o = 0;
    if (out == NULL) {
        return NULL;
    }
    for (i = 0; i < len; i += 3) {
        unsigned v = (unsigned)src[i] << 16;
        int pad = 0;
        if (i + 1 < len) {
            v |= (unsigned)src[i + 1] << 8;
        } else {
            pad++;
        }
        if (i + 2 < len) {
            v |= (unsigned)src[i + 2];
        } else {
            pad++;
        }
        out[o++] = B64A[(v >> 18) & 63];
        out[o++] = B64A[(v >> 12) & 63];
        out[o++] = pad >= 2 ? '=' : B64A[(v >> 6) & 63];
        out[o++] = pad >= 1 ? '=' : B64A[v & 63];
    }
    out[o] = '\0';
    return out;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

static unsigned char *b64_decode(const char *src, size_t *out_len)
{
    size_t slen = strlen(src);
    unsigned char *out;
    size_t o = 0;
    size_t i;
    unsigned v = 0;
    int bits = 0;
    if (slen % 4 != 0) {
        return NULL;
    }
    out = malloc(slen / 4 * 3 + 4);
    if (out == NULL) {
        return NULL;
    }
    for (i = 0; i < slen; i++) {
        int d = b64_val(src[i]);
        if (d < 0) {
            if (src[i] == '=' && i + 1 >= slen - 1) {
                continue; /* padding only at the end */
            }
            free(out);
            return NULL;
        }
        v = (v << 6) | (unsigned)d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char)((v >> bits) & 0xFF);
        }
    }
    *out_len = o;
    return out;
}

/* ---------------- JSON string escaping ------------------------------ */
static int json_escape_raw(sb *b, const char *s)
{
    size_t i;
    if (s == NULL) {
        return sb_puts(b, "null");
    }
    if (sb_putc(b, '"') != 0) {
        return -1;
    }
    for (i = 0; s[i] != '\0'; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\') {
            if (sb_putc(b, '\\') != 0 || sb_putc(b, (char)c) != 0) {
                return -1;
            }
        } else if (c < 0x20) {
            if (sb_printf(b, "\\u%04x", (unsigned)c) != 0) {
                return -1;
            }
        } else {
            if (sb_putc(b, (char)c) != 0) {
                return -1;
            }
        }
    }
    return sb_putc(b, '"');
}

/* Display form: invalid UTF-8 bytes become U+FFFD (library decoder). */
static int json_escape_preview(sb *b, const char *s, size_t len)
{
    size_t i = 0;
    if (sb_putc(b, '"') != 0) {
        return -1;
    }
    while (i < len && s[i] != '\0') {
        uint32_t cp = 0;
        int n = odk_utf8_decode_next(s + i, len - i, &cp);
        if (n <= 0) {
            if (sb_puts(b, "\xEF\xBF\xBD") != 0) {
                return -1;
            }
            i++;
        } else {
            size_t k;
            for (k = 0; k < (size_t)n; k++) {
                unsigned char c = (unsigned char)s[i + k];
                if (c == '"' || c == '\\') {
                    if (sb_putc(b, '\\') != 0 || sb_putc(b, (char)c) != 0) {
                        return -1;
                    }
                } else if (c < 0x20) {
                    if (sb_printf(b, "\\u%04x", (unsigned)c) != 0) {
                        return -1;
                    }
                } else if (sb_putc(b, (char)c) != 0) {
                    return -1;
                }
            }
            i += (size_t)n;
        }
    }
    return sb_putc(b, '"');
}

/* ---------------- case model ---------------------------------------- */
enum { K_BASE, K_KERN, K_CLI };
enum { P_CHOICE, P_SCORE, P_NOUL, P_RANK, P_COMPARE, P_EXTRACT };
static const char *const PRIM_NAMES[6] = {"choice", "score", "noul",
                                          "rank", "compare", "extract"};

typedef struct {
    char id[32];
    int kind; /* K_BASE / K_KERN / K_CLI                              */
    int prim; /* 0..5                                                  */
    const char *instruction;
    const char *statement;
    const char *options[G_MAXOPTS];
    size_t option_count;
    const char *rubric[G_MAXRUBRIC];
    size_t rubric_count;
    char *context; /* owned, NUL-terminated, may hold invalid UTF-8   */
    int mode;      /* 0 ascii, 1 unicode                              */
    int profile;   /* odk_input_profile                               */
    int lang;      /* odk_language (kernel config)                    */
    int over_limit;  /* odk_over_limit_strategy                       */
    long cfg_context_max;     /* -1 = default                          */
    long cfg_chunk_source_max;
    long cfg_max_chunks;
    long cfg_snippet_max;
    long cfg_max_ranked;
    long cfg_max_snippet_chars;
    long cfg_max_total_chars;
    long cfg_max_work;
    int call_lang;  /* -1 = no call struct; else odk_language value   */
    int call_morph; /* 0/1                                            */
    int cap_ranked;       /* -1 = no caps struct                      */
    int cap_snippet_chars;
    int cap_total_chars;
    int top_k;      /* extract                                        */
    int image;      /* 0 none, 1 gradient24, 2 half24, 3 solid64      */
    const char *cli_queries_json; /* K_CLI only                       */
} gcase;

/* ---------------- case vector --------------------------------------- */
static gcase *g_cases = NULL;
static size_t g_case_count = 0;
static size_t g_case_cap = 0;

static gcase *case_new(void)
{
    gcase c;
    memset(&c, 0, sizeof(c));
    c.cfg_context_max = -1;
    c.cfg_chunk_source_max = -1;
    c.cfg_max_chunks = -1;
    c.cfg_snippet_max = -1;
    c.cfg_max_ranked = -1;
    c.cfg_max_snippet_chars = -1;
    c.cfg_max_total_chars = -1;
    c.cfg_max_work = -1;
    c.call_lang = -1;
    c.cap_ranked = -1;
    c.cap_snippet_chars = -1;
    c.cap_total_chars = -1;
    c.top_k = 3;
    if (g_case_count == g_case_cap) {
        size_t ncap = g_case_cap ? g_case_cap * 2 : 1024;
        gcase *nc = realloc(g_cases, ncap * sizeof(gcase));
        if (nc == NULL) {
            fprintf(stderr, "fatal: out of memory growing case vector\n");
            exit(1);
        }
        g_cases = nc;
        g_case_cap = ncap;
    }
    g_cases[g_case_count] = c;
    return &g_cases[g_case_count++];
}

static char *dup_text(const char *s)
{
    size_t n = strlen(s);
    char *d = malloc(n + 1);
    if (d == NULL) {
        fprintf(stderr, "fatal: out of memory\n");
        exit(1);
    }
    memcpy(d, s, n + 1);
    return d;
}

/* ---------------- images -------------------------------------------- */
static unsigned char IMG_GRADIENT[24 * 24 * 3];
static unsigned char IMG_HALF[24 * 24 * 3];
static unsigned char IMG_SOLID[64 * 64 * 3];

static void images_init(void)
{
    int x;
    int y;
    for (y = 0; y < 24; y++) {
        for (x = 0; x < 24; x++) {
            size_t o = ((size_t)y * 24 + (size_t)x) * 3;
            IMG_GRADIENT[o] = (unsigned char)(x * 10);
            IMG_GRADIENT[o + 1] = (unsigned char)(y * 10);
            IMG_GRADIENT[o + 2] = (unsigned char)((x + y) * 5);
            IMG_HALF[o] = x < 12 ? 0xE0 : 0x10;
            IMG_HALF[o + 1] = 0x20;
            IMG_HALF[o + 2] = x < 12 ? 0x10 : 0xE0;
        }
    }
    for (x = 0; x < 64 * 64; x++) {
        IMG_SOLID[x * 3] = 0x10;
        IMG_SOLID[x * 3 + 1] = 0xC0;
        IMG_SOLID[x * 3 + 2] = 0x30;
    }
}

static const odk_image *case_image(const gcase *c)
{
    static odk_image im;
    if (c->image == 0) {
        return NULL;
    }
    if (c->image == 1) {
        im.width = 24;
        im.height = 24;
        im.rgb = IMG_GRADIENT;
    } else if (c->image == 2) {
        im.width = 24;
        im.height = 24;
        im.rgb = IMG_HALF;
    } else {
        im.width = 64;
        im.height = 64;
        im.rgb = IMG_SOLID;
    }
    return &im;
}

/* ---------------- context builders ---------------------------------- */
static const char *const PROFILE_NAMES[12] = {
    "plain", "markdown", "chat", "log", "json", "jsonl",
    "csv", "tsv", "code", "html", "pdf", "auto"};

/* Profile-shaped template with the language sentence embedded. */
static char *profile_text(int profile, const char *sentence)
{
    sb b;
    int i;
    memset(&b, 0, sizeof(b));
    switch (profile) {
    case ODK_PROFILE_PLAIN:
        sb_printf(&b, "Notes from the review meeting.\n%s\nAction items "
                      "follow next week.\n", sentence);
        break;
    case ODK_PROFILE_MARKDOWN:
        sb_printf(&b, "# Review\n\n## Summary\n\n%s\n\n- item one\n"
                      "- item two\n", sentence);
        break;
    case ODK_PROFILE_CHAT:
        sb_printf(&b, "user: what did the team decide?\nassistant: %s\n"
                      "user: thanks\n", sentence);
        break;
    case ODK_PROFILE_LOG:
        for (i = 0; i < 3; i++) {
            sb_printf(&b, "2026-10-%02d 10:%02d:00 INFO service healthy\n",
                      20 + i, 11 + i);
        }
        sb_printf(&b, "2026-10-23 10:14:00 INFO decision recorded: %s\n",
                  sentence);
        break;
    case ODK_PROFILE_JSON:
        sb_printf(&b, "{\"topic\":\"plans\",\"note\":\"%s\",\"count\":2}",
                  sentence);
        break;
    case ODK_PROFILE_JSONL:
        sb_printf(&b, "{\"k\":\"note\",\"v\":\"context\"}\n"
                      "{\"k\":\"decision\",\"v\":\"%s\"}\n", sentence);
        break;
    case ODK_PROFILE_CSV:
        sb_printf(&b, "id,note\n1,filler row\n2,%s\n", sentence);
        break;
    case ODK_PROFILE_TSV:
        sb_printf(&b, "id\tnote\n1\tfiller row\n2\t%s\n", sentence);
        break;
    case ODK_PROFILE_CODE:
        sb_printf(&b, "/* review comments */\nint main(void) {\n    /* "
                      "%s */\n    return 0;\n}\n", sentence);
        break;
    case ODK_PROFILE_HTML_TEXT:
        sb_printf(&b, "<html><body><p>report body</p><p>%s</p></body>"
                      "</html>", sentence);
        break;
    case ODK_PROFILE_EXTRACTED_PDF:
        sb_printf(&b, "--- page 1 ---\ncover text\n--- page 2 ---\n%s\n",
                  sentence);
        break;
    default: /* AUTO */
        sb_printf(&b, "Raw captured note. %s End of note.", sentence);
        break;
    }
    return b.s;
}

/* Long context: filler lines, optionally the decisive sentence at END. */
static char *long_text(size_t target, int decisive_at_end)
{
    sb b;
    size_t i = 0;
    memset(&b, 0, sizeof(b));
    while (b.len < target) {
        sb_printf(&b, "line %03zu: routine status entries continue here, "
                      "nothing decisive so far.\n", i % 1000);
        i++;
    }
    if (decisive_at_end) {
        sb_printf(&b, "Final line: the board approves the annual plan and "
                      "rejects the monthly plan.\n");
    }
    return b.s;
}

static const char *const INVALID_1 = "before \xFF after";
static const char *const INVALID_2 = "cut \xE5\x9C end";
static const char *const INVALID_3 = "\xC0\x80 overlong \xF4\x90\x80\x80 tail";

static char *blob_text(size_t target)
{
    sb b;
    size_t i;
    memset(&b, 0, sizeof(b));
    for (i = 0; i < target; i++) {
        sb_putc(&b, (char)lcg_byte());
    }
    return b.s;
}

/* ---------------- enumeration ---------------------------------------- */
static void set_lang_fields(gcase *c, const glang *L, int is_auto)
{
    c->instruction = is_auto ? G_AUTO_INSTRUCTION : L->instruction;
    c->options[0] = is_auto ? G_AUTO_OPTS[0] : L->opt0;
    c->options[1] = is_auto ? G_AUTO_OPTS[1] : L->opt1;
    c->option_count = 2;
    c->statement = is_auto ? G_AUTO_SENTENCE : L->statement;
}

static void enumerate(void)
{
    size_t li;
    int mode;
    int prim;
    int profile;
    size_t idn = 0;
    size_t kid = 0;
    size_t cid = 0;

    /* 1. core cross: base kind, 12 languages x 2 modes x 6 prims x 12
     *    profiles = 1728 */
    for (li = 0; li < G_LANG_COUNT + 1; li++) {
        int is_auto = (li == G_LANG_COUNT);
        const glang *L = is_auto ? &G_LANGS[0] : &G_LANGS[li];
        for (mode = 0; mode <= 1; mode++) {
            for (prim = 0; prim < 6; prim++) {
                for (profile = 0; profile < 12; profile++) {
                    gcase *c = case_new();
                    snprintf(c->id, sizeof(c->id), "base-%06zu", ++idn);
                    c->kind = K_BASE;
                    c->prim = prim;
                    c->mode = mode;
                    c->profile = profile;
                    c->lang = is_auto ? ODK_LANG_AUTO : ODK_LANG_UND + (int)li;
                    set_lang_fields(c, L, is_auto);
                    if (prim == P_SCORE) {
                        c->rubric[0] = "low";
                        c->rubric[1] = "medium";
                        c->rubric[2] = "high";
                        c->rubric_count = 3;
                    }
                    if (prim == P_NOUL) {
                        c->instruction = "";
                    }
                    c->context = profile_text(profile, is_auto
                                                          ? G_AUTO_SENTENCE
                                                          : L->sentence);
                }
            }
        }
    }

    /* 2. length ladder: 8 sizes x 2 modes x 6 prims (kern) */
    {
        static const size_t SIZES[] = {0, 10, 100, 1024, 8192, 20000, 24000,
                                       90000};
        size_t si;
        for (si = 0; si < sizeof(SIZES) / sizeof(SIZES[0]); si++) {
            for (mode = 0; mode <= 1; mode++) {
                for (prim = 0; prim < 6; prim++) {
                    gcase *c = case_new();
                    snprintf(c->id, sizeof(c->id), "kern-%06zu", ++kid);
                    c->kind = K_KERN;
                    c->prim = prim;
                    c->mode = mode;
                    c->profile = ODK_PROFILE_PLAIN;
                    c->lang = ODK_LANG_EN;
                    c->instruction = "Which plan did the board approve?";
                    c->options[0] = "annual plan";
                    c->options[1] = "monthly plan";
                    c->option_count = 2;
                    c->statement =
                        "The board approves the annual plan and rejects the "
                        "monthly plan.";
                    if (prim == P_SCORE) {
                        c->rubric[0] = "reject";
                        c->rubric[1] = "unsure";
                        c->rubric[2] = "approve";
                        c->rubric_count = 3;
                    }
                    c->context = SIZES[si] == 0
                                     ? dup_text("")
                                     : long_text(SIZES[si], 1);
                }
            }
        }
    }

    /* 3. over-limit strategies on the 90 KB decisive-at-end context */
    {
        int strat;
        for (strat = 0; strat <= 3; strat++) {
            for (mode = 0; mode <= 1; mode++) {
                int prims2[3] = {P_CHOICE, P_RANK, P_NOUL};
                int k;
                for (k = 0; k < 3; k++) {
                    gcase *c = case_new();
                    snprintf(c->id, sizeof(c->id), "kern-%06zu", ++kid);
                    c->kind = K_KERN;
                    c->prim = prims2[k];
                    c->mode = mode;
                    c->profile = ODK_PROFILE_PLAIN;
                    c->lang = ODK_LANG_EN;
                    c->over_limit = strat;
                    c->instruction = "Which plan did the board approve?";
                    c->options[0] = "annual plan";
                    c->options[1] = "monthly plan";
                    c->option_count = 2;
                    c->statement =
                        "The board approves the annual plan and rejects the "
                        "monthly plan.";
                    c->context = long_text(90000, 1);
                }
            }
        }
    }

    /* 4. invalid UTF-8 + binary blob */
    {
        const char *const INV[3] = {INVALID_1, INVALID_2, INVALID_3};
        int v;
        for (v = 0; v < 3; v++) {
            for (mode = 0; mode <= 1; mode++) {
                int prims2[2] = {P_CHOICE, P_NOUL};
                int k;
                for (k = 0; k < 2; k++) {
                    gcase *c = case_new();
                    snprintf(c->id, sizeof(c->id), "kern-%06zu", ++kid);
                    c->kind = K_KERN;
                    c->prim = prims2[k];
                    c->mode = mode;
                    c->profile = ODK_PROFILE_PLAIN;
                    c->lang = ODK_LANG_UND;
                    c->instruction = "Which plan?";
                    c->options[0] = "annual plan";
                    c->options[1] = "monthly plan";
                    c->option_count = 2;
                    c->statement = "The annual plan saves twenty percent.";
                    c->context = dup_text(INV[v]);
                }
            }
        }
        for (mode = 0; mode <= 1; mode++) {
            for (prim = 0; prim < 6; prim++) {
                gcase *c = case_new();
                snprintf(c->id, sizeof(c->id), "kern-%06zu", ++kid);
                c->kind = K_KERN;
                c->prim = prim;
                c->mode = mode;
                c->profile = ODK_PROFILE_PLAIN;
                c->lang = ODK_LANG_UND;
                c->instruction = "Which plan?";
                c->options[0] = "annual plan";
                c->options[1] = "monthly plan";
                c->option_count = 2;
                c->statement = "The annual plan saves twenty percent.";
                c->context = blob_text(4096); /* fixed LCG stream */
            }
        }
    }

    /* 5. images: 3 images x 6 prims x 2 modes */
    for (mode = 0; mode <= 1; mode++) {
        for (prim = 0; prim < 6; prim++) {
            int img;
            for (img = 1; img <= 3; img++) {
                gcase *c = case_new();
                snprintf(c->id, sizeof(c->id), "kern-%06zu", ++kid);
                c->kind = K_KERN;
                c->prim = prim;
                c->mode = mode;
                c->profile = ODK_PROFILE_PLAIN;
                c->lang = ODK_LANG_EN;
                c->image = img;
                c->instruction = "Which plan did the board approve?";
                c->options[0] = "annual plan";
                c->options[1] = "monthly plan";
                c->option_count = 2;
                c->statement = "The annual plan saves twenty percent.";
                if (prim == P_SCORE) {
                    c->rubric[0] = "low";
                    c->rubric[1] = "high";
                    c->rubric_count = 2;
                }
                c->context = profile_text(ODK_PROFILE_PLAIN,
                                          "The board approved the annual "
                                          "plan.");
            }
        }
    }

    /* 6. per-call language overrides + morph_light */
    for (mode = 0; mode <= 1; mode++) {
        for (prim = 0; prim < 6; prim++) {
            int cl;
            for (cl = 0; cl < 3; cl++) {
                gcase *c = case_new();
                static const int CALLS[3] = {ODK_LANG_AUTO, ODK_LANG_EN,
                                             ODK_LANG_ZH};
                snprintf(c->id, sizeof(c->id), "kern-%06zu", ++kid);
                c->kind = K_KERN;
                c->prim = prim;
                c->mode = mode;
                c->profile = ODK_PROFILE_PLAIN;
                c->lang = ODK_LANG_UND;
                c->call_lang = CALLS[cl];
                c->instruction = G_AUTO_INSTRUCTION;
                c->options[0] = G_AUTO_OPTS[0];
                c->options[1] = G_AUTO_OPTS[1];
                c->option_count = 2;
                c->statement = G_AUTO_SENTENCE;
                c->context = dup_text(G_AUTO_SENTENCE);
            }
            {
                gcase *c = case_new();
                snprintf(c->id, sizeof(c->id), "kern-%06zu", ++kid);
                c->kind = K_KERN;
                c->prim = prim;
                c->mode = mode;
                c->profile = ODK_PROFILE_PLAIN;
                c->lang = ODK_LANG_UND;
                c->call_lang = ODK_LANG_UND;
                c->call_morph = 1;
                c->instruction = "Which plan did the board approve?";
                c->options[0] = "annual plans";
                c->options[1] = "monthly plans";
                c->option_count = 2;
                c->statement = "The board approves the annual plan.";
                c->context = dup_text(
                    "The board approves the annual plan. Plans were "
                    "compared. Approving plans is their job.");
            }
        }
    }

    /* 7. output caps */
    for (mode = 0; mode <= 1; mode++) {
        int capset;
        for (capset = 0; capset < 3; capset++) {
            int prims2[2] = {P_RANK, P_EXTRACT};
            int k;
            for (k = 0; k < 2; k++) {
                gcase *c = case_new();
                snprintf(c->id, sizeof(c->id), "kern-%06zu", ++kid);
                c->kind = K_KERN;
                c->prim = prims2[k];
                c->mode = mode;
                c->profile = ODK_PROFILE_PLAIN;
                c->lang = ODK_LANG_EN;
                c->instruction = "Which plan did the board approve?";
                c->options[0] = "annual plan";
                c->options[1] = "monthly plan";
                c->options[2] = "trial plan";
                c->option_count = 3;
                if (capset == 0) {
                    c->cap_ranked = 1;
                } else if (capset == 1) {
                    c->cap_snippet_chars = 20;
                    c->cap_total_chars = 40;
                } else {
                    c->cap_ranked = 2;
                    c->cap_snippet_chars = 30;
                    c->cap_total_chars = 50;
                }
                c->top_k = 3;
                c->context = long_text(2000, 1);
            }
        }
    }

    /* 8. config variants that force truncation / work limits */
    for (mode = 0; mode <= 1; mode++) {
        struct {
            int prim;
            long ctx_max;
            long chunk_src;
            long max_chunks;
            long snippet;
            long work;
        } V[6] = {{P_CHOICE, 100, -1, -1, -1, -1},
                  {P_RANK, 100, -1, -1, -1, -1},
                  {P_EXTRACT, 100, -1, -1, -1, -1},
                  {P_CHOICE, -1, 100, -1, -1, -1},
                  {P_CHOICE, -1, -1, 2, -1, -1},
                  {P_CHOICE, -1, -1, -1, -1, 500}};
        int v;
        for (v = 0; v < 6; v++) {
            gcase *c = case_new();
            snprintf(c->id, sizeof(c->id), "kern-%06zu", ++kid);
            c->kind = K_KERN;
            c->prim = V[v].prim;
            c->mode = mode;
            c->profile = ODK_PROFILE_PLAIN;
            c->lang = ODK_LANG_EN;
            c->cfg_context_max = V[v].ctx_max;
            c->cfg_chunk_source_max = V[v].chunk_src;
            c->cfg_max_chunks = V[v].max_chunks;
            c->cfg_max_work = V[v].work;
            c->instruction = "Which plan did the board approve?";
            c->options[0] = "annual plan";
            c->options[1] = "monthly plan";
            c->option_count = 2;
            c->top_k = 3;
            c->context = long_text(2000, 1);
        }
    }

    /* 9. client pipeline scenarios x 2 modes */
    for (mode = 0; mode <= 1; mode++) {
        static const char *const Q_SIMPLE =
            "{\"q1\":{\"primitive\":\"choice\",\"instruction\":\"Which "
            "plan?\",\"options\":[\"annual plan\",\"monthly plan\"]}}";
        static const char *const Q_MULTI =
            "{\"a\":{\"primitive\":\"choice\",\"instruction\":\"Which "
            "plan?\",\"options\":[\"annual plan\",\"monthly plan\"]},"
            "\"b\":{\"primitive\":\"score\",\"instruction\":\"How "
            "supported is approval?\",\"rubric\":[\"low\",\"high\"]},"
            "\"c\":{\"primitive\":\"noul\",\"statement\":\"The annual plan "
            "saves twenty percent.\"}}";
        static const char *const Q_BADPRIM =
            "{\"q1\":{\"primitive\":\"nope\",\"instruction\":\"x\","
            "\"options\":[\"a\",\"b\"]}}";
        static const char *const Q_ZH =
            "{\"q1\":{\"primitive\":\"choice\",\"instruction\":\"应该选择"
            "哪个方案？\",\"options\":[\"年度方案\",\"月度方案\"]}}";
        static const char *const SCEN_TEXT[4] = {
            "Meeting notes: the board approves the annual plan and rejects "
            "the monthly plan.",
            "# Report\n\n## Plans\n\nThe annual plan saves twenty "
            "percent.\n\n- item one\n- item two\n",
            "[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[["
             "]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]]"
             "]]",
            "年度方案节省百分之二十。团队选择了年度方案。"};
        int scen;
        for (scen = 0; scen < 8; scen++) {
            gcase *c = case_new();
            snprintf(c->id, sizeof(c->id), "cli-%06zu", ++cid);
            c->kind = K_CLI;
            c->mode = mode;
            c->profile = ODK_PROFILE_AUTO;
            c->lang = ODK_LANG_UND;
            c->top_k = 3;
            if (scen < 4) {
                c->context = dup_text(SCEN_TEXT[scen]);
                c->cli_queries_json =
                    scen == 0 ? Q_SIMPLE
                              : scen == 1 ? Q_MULTI
                                          : scen == 2 ? Q_BADPRIM : Q_ZH;
            } else if (scen == 4) {
                c->context = dup_text(SCEN_TEXT[0]);
                c->cli_queries_json = "{}";
            } else if (scen == 5) {
                c->context = dup_text(SCEN_TEXT[3]);
                c->cli_queries_json = Q_ZH;
            } else if (scen == 6) {
                c->context = blob_text(2048);
                c->cli_queries_json = Q_SIMPLE;
            } else {
                c->context = dup_text("");
                c->cli_queries_json = Q_SIMPLE;
            }
        }
    }

    /* 10. edge and error paths */
    {
        {
            gcase *c = case_new(); /* rank with 1 option: error */
            snprintf(c->id, sizeof(c->id), "kern-%06zu", ++kid);
            c->kind = K_KERN;
            c->prim = P_RANK;
            c->lang = ODK_LANG_EN;
            c->instruction = "Rank the plans";
            c->options[0] = "annual plan";
            c->option_count = 1;
            c->context = dup_text("The board approved the annual plan.");
        }
        {
            gcase *c = case_new(); /* compare with 3 options: error */
            snprintf(c->id, sizeof(c->id), "kern-%06zu", ++kid);
            c->kind = K_KERN;
            c->prim = P_COMPARE;
            c->lang = ODK_LANG_EN;
            c->instruction = "Compare";
            c->options[0] = "a";
            c->options[1] = "b";
            c->options[2] = "c";
            c->option_count = 3;
            c->context = dup_text("context text");
        }
        {
            gcase *c = case_new(); /* choice with 0 options: error */
            snprintf(c->id, sizeof(c->id), "kern-%06zu", ++kid);
            c->kind = K_KERN;
            c->prim = P_CHOICE;
            c->lang = ODK_LANG_EN;
            c->instruction = "Choose";
            c->context = dup_text("context text");
        }
        {
            gcase *c = case_new(); /* whitespace-only context */
            snprintf(c->id, sizeof(c->id), "kern-%06zu", ++kid);
            c->kind = K_KERN;
            c->prim = P_CHOICE;
            c->lang = ODK_LANG_EN;
            c->instruction = "Choose";
            c->options[0] = "annual plan";
            c->options[1] = "monthly plan";
            c->option_count = 2;
            c->context = dup_text("   \n\t  ");
        }
        {
            gcase *c = case_new(); /* empty instruction, empty options text */
            snprintf(c->id, sizeof(c->id), "kern-%06zu", ++kid);
            c->kind = K_KERN;
            c->prim = P_CHOICE;
            c->lang = ODK_LANG_EN;
            c->instruction = "";
            c->options[0] = "";
            c->options[1] = "x";
            c->option_count = 2;
            c->context = dup_text("The board approved the annual plan.");
        }
        {
            static char big_opt[5001]; /* 5,000-char option */
            gcase *c;
            memset(big_opt, 'w', sizeof(big_opt) - 1);
            big_opt[sizeof(big_opt) - 1] = '\0';
            c = case_new();
            snprintf(c->id, sizeof(c->id), "kern-%06zu", ++kid);
            c->kind = K_KERN;
            c->prim = P_RANK;
            c->lang = ODK_LANG_EN;
            c->instruction = "Rank";
            c->options[0] = big_opt;
            c->options[1] = "short";
            c->option_count = 2;
            c->context = dup_text("www www www short long option check");
        }
        {
            gcase *c; /* 60 options */
            int i;
            c = case_new();
            snprintf(c->id, sizeof(c->id), "kern-%06zu", ++kid);
            c->kind = K_KERN;
            c->prim = P_CHOICE;
            c->lang = ODK_LANG_EN;
            c->instruction = "Pick one";
            for (i = 0; i < 60; i++) {
                static char optbuf[60][8];
                snprintf(optbuf[i], sizeof(optbuf[i]), "opt-%02d", i);
                c->options[i] = optbuf[i];
            }
            c->option_count = 60;
            c->context = dup_text("choose opt-07 as the answer here");
        }
        {
            gcase *c; /* duplicate options */
            c = case_new();
            snprintf(c->id, sizeof(c->id), "kern-%06zu", ++kid);
            c->kind = K_KERN;
            c->prim = P_RANK;
            c->lang = ODK_LANG_EN;
            c->instruction = "Rank";
            c->options[0] = "annual plan";
            c->options[1] = "annual plan";
            c->options[2] = "monthly plan";
            c->option_count = 3;
            c->context = dup_text("The annual plan was chosen.");
        }
        {
            int tk[4] = {0, 1, 10, 99}; /* top_k clamps */
            int i;
            for (i = 0; i < 4; i++) {
                gcase *c = case_new();
                snprintf(c->id, sizeof(c->id), "kern-%06zu", ++kid);
                c->kind = K_KERN;
                c->prim = P_EXTRACT;
                c->lang = ODK_LANG_EN;
                c->instruction = "approval of the annual plan";
                c->top_k = tk[i];
                c->context = long_text(3000, 1);
            }
        }
        {
            gcase *c; /* 1-level rubric */
            c = case_new();
            snprintf(c->id, sizeof(c->id), "kern-%06zu", ++kid);
            c->kind = K_KERN;
            c->prim = P_SCORE;
            c->lang = ODK_LANG_EN;
            c->instruction = "Rate the support";
            c->rubric[0] = "only-level";
            c->rubric_count = 1;
            c->context = dup_text("support is present in the text");
        }
        {
            gcase *c; /* 9-level rubric */
            int i;
            c = case_new();
            snprintf(c->id, sizeof(c->id), "kern-%06zu", ++kid);
            c->kind = K_KERN;
            c->prim = P_SCORE;
            c->lang = ODK_LANG_EN;
            c->instruction = "Rate the support";
            for (i = 0; i < 9; i++) {
                static char lvbuf[9][8];
                snprintf(lvbuf[i], sizeof(lvbuf[i]), "level-%d", i + 1);
                c->rubric[i] = lvbuf[i];
            }
            c->rubric_count = 9;
            c->context = dup_text("evidence matches level-4 wording");
        }
        {
            gcase *c; /* noul empty statement: error */
            c = case_new();
            snprintf(c->id, sizeof(c->id), "kern-%06zu", ++kid);
            c->kind = K_KERN;
            c->prim = P_NOUL;
            c->lang = ODK_LANG_EN;
            c->statement = "   ";
            c->context = dup_text("context");
        }
        {
            int i;
            for (i = 0; i < 2; i++) { /* unicode select on 24 KB */
                gcase *c = case_new();
                snprintf(c->id, sizeof(c->id), "kern-%06zu", ++kid);
                c->kind = K_KERN;
                c->prim = P_CHOICE;
                c->mode = i;
                c->profile = ODK_PROFILE_PLAIN;
                c->lang = ODK_LANG_UND;
                c->over_limit = 3; /* select */
                c->instruction = "Which plan did the board approve?";
                c->options[0] = "annual plan";
                c->options[1] = "monthly plan";
                c->option_count = 2;
                c->context = long_text(24000, 1);
            }
        }
    }
}

/* ---------------- runner --------------------------------------------- */
static odk_kernel *build_kernel(const gcase *c)
{
    odk_kernel_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.text_mode = c->mode ? ODK_TEXT_UNICODE : ODK_TEXT_ASCII_COMPAT;
    cfg.profile = (odk_input_profile)c->profile;
    cfg.language = (odk_language)c->lang;
    cfg.over_limit = (odk_over_limit_strategy)c->over_limit;
    if (c->cfg_context_max >= 0) {
        cfg.context_max = (size_t)c->cfg_context_max;
    }
    if (c->cfg_chunk_source_max >= 0) {
        cfg.chunk_source_max = (size_t)c->cfg_chunk_source_max;
    }
    if (c->cfg_max_chunks >= 0) {
        cfg.max_chunks = (size_t)c->cfg_max_chunks;
    }
    if (c->cfg_snippet_max >= 0) {
        cfg.snippet_max = (size_t)c->cfg_snippet_max;
    }
    if (c->cfg_max_ranked >= 0) {
        cfg.max_ranked = (size_t)c->cfg_max_ranked;
    }
    if (c->cfg_max_snippet_chars >= 0) {
        cfg.max_snippet_chars = (size_t)c->cfg_max_snippet_chars;
    }
    if (c->cfg_max_total_chars >= 0) {
        cfg.max_total_chars = (size_t)c->cfg_max_total_chars;
    }
    if (c->cfg_max_work >= 0) {
        cfg.max_work = (size_t)c->cfg_max_work;
    }
    return odk_kernel_create_ex(&cfg);
}

static odk_status run_case(const gcase *c, odk_result *out,
                           odk_result_meta *meta, odk_error *err)
{
    odk_kernel *k = build_kernel(c);
    const odk_image *img = case_image(c);
    const odk_call_config *call_ptr = NULL;
    odk_call_config call;
    odk_output_caps caps;
    const odk_output_caps *caps_ptr = NULL;
    odk_status st;

    memset(out, 0, sizeof(*out));
    if (meta != NULL) {
        memset(meta, 0, sizeof(*meta));
    }
    memset(err, 0, sizeof(*err));
    if (k == NULL) {
        return ODK_ERR_MEMORY;
    }
    if (c->call_lang >= 0) {
        memset(&call, 0, sizeof(call));
        call.language = (odk_language)c->call_lang;
        call.morph_light = c->call_morph;
        call_ptr = &call;
    }
    if (c->cap_ranked >= 0 || c->cap_snippet_chars >= 0 ||
        c->cap_total_chars >= 0) {
        memset(&caps, 0, sizeof(caps));
        caps.max_ranked = c->cap_ranked >= 0 ? (size_t)c->cap_ranked : 0;
        caps.max_snippet_chars =
            c->cap_snippet_chars >= 0 ? (size_t)c->cap_snippet_chars : 0;
        caps.max_total_chars =
            c->cap_total_chars >= 0 ? (size_t)c->cap_total_chars : 0;
        caps_ptr = &caps;
    }
    switch (c->prim) {
    case P_CHOICE:
        if (c->kind == K_BASE) {
            st = odk_evaluate_choice(k, c->instruction, c->options,
                                     c->option_count, c->context, img, out,
                                     err);
        } else {
            st = odk_evaluate_choice_call(k, c->instruction, c->options,
                                          c->option_count, c->context, img,
                                          call_ptr, caps_ptr, out, meta,
                                          err);
        }
        break;
    case P_SCORE:
        if (c->kind == K_BASE) {
            st = odk_evaluate_score(k, c->instruction, c->rubric,
                                    c->rubric_count, c->context, img, out,
                                    err);
        } else {
            st = odk_evaluate_score_call(k, c->instruction, c->rubric,
                                         c->rubric_count, c->context, img,
                                         call_ptr, caps_ptr, out, meta,
                                         err);
        }
        break;
    case P_NOUL:
        if (c->kind == K_BASE) {
            st = odk_evaluate_noul(k, c->statement, c->context, img, out,
                                   err);
        } else {
            st = odk_evaluate_noul_call(k, c->statement, c->context, img,
                                        call_ptr, caps_ptr, out, meta,
                                        err);
        }
        break;
    case P_RANK:
        if (c->kind == K_BASE) {
            st = odk_evaluate_rank(k, c->instruction, c->options,
                                   c->option_count, c->context, img, out,
                                   err);
        } else {
            st = odk_evaluate_rank_call(k, c->instruction, c->options,
                                        c->option_count, c->context, img,
                                        call_ptr, caps_ptr, out, meta,
                                        err);
        }
        break;
    case P_COMPARE:
        if (c->kind == K_BASE) {
            st = odk_evaluate_compare(k, c->instruction, c->options,
                                      c->option_count, c->context, img, out,
                                      err);
        } else {
            st = odk_evaluate_compare_call(k, c->instruction,
                                           c->option_count > 0 ? c->options[0]
                                                               : NULL,
                                           c->option_count > 1 ? c->options[1]
                                                               : NULL,
                                           c->context, img, call_ptr,
                                           caps_ptr, out, meta, err);
        }
        break;
    case P_EXTRACT:
        if (c->kind == K_BASE) {
            st = odk_evaluate_extract(k, c->instruction, c->context, img,
                                      c->top_k, out, err);
        } else {
            st = odk_evaluate_extract_call(k, c->instruction, c->statement,
                                           c->context, img, c->top_k,
                                           call_ptr, caps_ptr, out, meta,
                                           err);
        }
        break;
    default:
        st = ODK_ERR_ARGUMENT;
        break;
    }
    odk_kernel_free(k);
    return st;
}

/* ---------------- serializers ---------------------------------------- */
static const char *const MODE_NAMES[2] = {"ascii", "unicode"};

static void lang_code_json(sb *b, int lang)
{
    const char *code = odk_lang_to_code((odk_language)lang);
    json_escape_raw(b, code ? code : "?");
}

static void serialize_input_line(sb *b, const gcase *c)
{
    size_t i;
    char *b64;
    sb_printf(b, "{\"id\":\"%s\",\"kind\":\"%s\",\"primitive\":\"%s\",",
              c->id,
              c->kind == K_BASE ? "base" : c->kind == K_KERN ? "kern" : "cli",
              PRIM_NAMES[c->prim]);
    sb_puts(b, "\"instruction\":");
    json_escape_raw(b, c->instruction ? c->instruction : "");
    sb_puts(b, ",\"statement\":");
    json_escape_raw(b, c->statement ? c->statement : "");
    sb_puts(b, ",\"options\":[");
    for (i = 0; i < c->option_count; i++) {
        if (i) {
            sb_putc(b, ',');
        }
        json_escape_raw(b, c->options[i]);
    }
    sb_puts(b, "],\"rubric\":[");
    for (i = 0; i < c->rubric_count; i++) {
        if (i) {
            sb_putc(b, ',');
        }
        json_escape_raw(b, c->rubric[i]);
    }
    sb_puts(b, "],\"context_b64\":");
    b64 = b64_encode((const unsigned char *)(c->context ? c->context : ""),
                     c->context ? strlen(c->context) : 0);
    json_escape_raw(b, b64 ? b64 : "");
    free(b64);
    sb_puts(b, ",\"context_preview\":");
    json_escape_preview(b, c->context ? c->context : "",
                        c->context ? strlen(c->context) : 0);
    sb_printf(b, ",\"text_mode\":\"%s\",\"profile\":\"%s\",\"language\":",
              MODE_NAMES[c->mode], PROFILE_NAMES[c->profile]);
    lang_code_json(b, c->lang);
    sb_printf(b, ",\"over_limit\":\"%s\",\"config\":{",
              c->over_limit == 0 ? "profile_default"
                                 : c->over_limit == 1 ? "head"
                                                      : c->over_limit == 2
                                                            ? "head_tail"
                                                            : "select");
    {
        int cf = 0;
        if (c->cfg_context_max >= 0) {
            sb_printf(b, "%s\"context_max\":%ld", cf ? "," : "",
                      c->cfg_context_max);
            cf = 1;
        }
        if (c->cfg_chunk_source_max >= 0) {
            sb_printf(b, "%s\"chunk_source_max\":%ld", cf ? "," : "",
                      c->cfg_chunk_source_max);
            cf = 1;
        }
        if (c->cfg_max_chunks >= 0) {
            sb_printf(b, "%s\"max_chunks\":%ld", cf ? "," : "",
                      c->cfg_max_chunks);
            cf = 1;
        }
        if (c->cfg_snippet_max >= 0) {
            sb_printf(b, "%s\"snippet_max\":%ld", cf ? "," : "",
                      c->cfg_snippet_max);
            cf = 1;
        }
        if (c->cfg_max_ranked >= 0) {
            sb_printf(b, "%s\"max_ranked\":%ld", cf ? "," : "",
                      c->cfg_max_ranked);
            cf = 1;
        }
        if (c->cfg_max_snippet_chars >= 0) {
            sb_printf(b, "%s\"max_snippet_chars\":%ld", cf ? "," : "",
                      c->cfg_max_snippet_chars);
            cf = 1;
        }
        if (c->cfg_max_total_chars >= 0) {
            sb_printf(b, "%s\"max_total_chars\":%ld", cf ? "," : "",
                      c->cfg_max_total_chars);
            cf = 1;
        }
        if (c->cfg_max_work >= 0) {
            sb_printf(b, "%s\"max_work\":%ld", cf ? "," : "",
                      c->cfg_max_work);
            cf = 1;
        }
    }
    sb_puts(b, "},\"call\":{");
    if (c->call_lang >= 0) {
        sb_puts(b, "\"language\":");
        lang_code_json(b, c->call_lang);
        sb_printf(b, ",\"morph_light\":%d", c->call_morph);
    } else {
        sb_puts(b, "\"language\":\"none\",\"morph_light\":0");
    }
    sb_puts(b, "},\"caps\":{");
    {
        int cf = 0;
        if (c->cap_ranked >= 0) {
            sb_printf(b, "%s\"max_ranked\":%d", cf ? "," : "",
                      c->cap_ranked);
            cf = 1;
        }
        if (c->cap_snippet_chars >= 0) {
            sb_printf(b, "%s\"max_snippet_chars\":%d", cf ? "," : "",
                      c->cap_snippet_chars);
            cf = 1;
        }
        if (c->cap_total_chars >= 0) {
            sb_printf(b, "%s\"max_total_chars\":%d", cf ? "," : "",
                      c->cap_total_chars);
            cf = 1;
        }
    }
    sb_printf(b, "},\"top_k\":%d,\"image\":\"%s\"",
              c->top_k,
              c->image == 0 ? "none"
                            : c->image == 1 ? "gradient24"
                                            : c->image == 2 ? "half24"
                                                            : "solid64");
    if (c->kind == K_CLI) {
        sb_puts(b, ",\"queries_json\":");
        json_escape_raw(b, c->cli_queries_json ? c->cli_queries_json : "{}");
    }
    sb_putc(b, '}');
}

static void serialize_meta(sb *b, const odk_result_meta *m)
{
    /* docs/GOLDEN.md section 3: EVERY odk_result_meta field is frozen,
     * including the truncation report (context_truncated /
     * context_units_dropped) - the independent review caught their
     * omission as a spec violation. */
    sb_printf(b,
              "{\"total_options\":%zu,\"truncated\":%d,\"emitted\":%zu,"
              "\"dropped\":%zu,\"chunks_kept\":%zu,\"chunks_dropped\":%zu,"
              "\"work_used\":%zu,\"work_budget\":%zu,\"work_exceeded\":%d,"
              "\"context_truncated\":%d,\"context_units_dropped\":%zu,"
              "\"language\":",
              m->total_options, m->truncated, m->emitted, m->dropped,
              m->chunks_kept, m->chunks_dropped, m->work_used, m->work_budget,
              m->work_exceeded, m->context_truncated,
              m->context_units_dropped);
    lang_code_json(b, (int)m->language);
    sb_puts(b, ",\"detected\":");
    lang_code_json(b, (int)m->detected);
    sb_printf(b, ",\"detected_confidence\":%.17g,\"detected_signal\":",
              m->detected_confidence);
    json_escape_raw(b, m->detected_signal);
    sb_putc(b, '}');
}

static void serialize_expected_line(sb *b, const gcase *c,
                                    const odk_result *result,
                                    const odk_result_meta *meta,
                                    odk_status status, const char *err_text)
{
    char *rj;
    sb_printf(b, "{\"id\":\"%s\",\"status\":%d,\"result\":", c->id,
              (int)status);
    rj = status == ODK_OK && result != NULL ? odk_result_to_json(result) : NULL;
    if (rj != NULL) {
        sb_puts(b, rj);
        free(rj);
    } else {
        sb_puts(b, "null");
    }
    sb_puts(b, ",\"meta\":");
    if (c->kind == K_KERN && meta != NULL) {
        serialize_meta(b, meta);
    } else {
        sb_puts(b, "null");
    }
    sb_puts(b, ",\"error\":");
    json_escape_raw(b, err_text ? err_text : "");
    sb_putc(b, '}');
}

/* Strip the volatile wall-clock metadata key from client output JSON. */
static char *strip_execution_time(const char *json)
{
    const char *key = "\"execution_time_ms\":";
    const char *p = json;
    const char *k;
    sb b;
    memset(&b, 0, sizeof(b));
    while ((k = strstr(p, key)) != NULL) {
        const char *v = k + strlen(key);
        const char *end = v;
        while (*end && *end != ',' && *end != '}') {
            end++;
        }
        if (k > json && *(k - 1) == ',') {
            k--; /* remove the leading comma too */
        }
        if (sb_grow(&b, (size_t)(k - p)) != 0) {
            free(b.s);
            return NULL;
        }
        memcpy(b.s + b.len, p, (size_t)(k - p));
        b.len += (size_t)(k - p);
        b.s[b.len] = '\0';
        p = end;
    }
    sb_puts(&b, p);
    return b.s;
}

/* Replace the string value that follows `key` (which must end with the
 * opening quote) with `placeholder`.  The library version is masked so a
 * release does not force a golden update; every other byte stays checked. */
static char *mask_string_value(const char *json, const char *key,
                               const char *placeholder)
{
    const char *p = json;
    const char *k;
    sb b;
    memset(&b, 0, sizeof(b));
    while ((k = strstr(p, key)) != NULL) {
        const char *v = k + strlen(key);
        const char *end = strchr(v, '"');
        if (end == NULL) {
            break;
        }
        sb_puts(&b, "");
        if (sb_grow(&b, (size_t)(v - p)) != 0) {
            free(b.s);
            return NULL;
        }
        memcpy(b.s + b.len, p, (size_t)(v - p));
        b.len += (size_t)(v - p);
        b.s[b.len] = '\0';
        sb_puts(&b, placeholder);
        p = end;
    }
    sb_puts(&b, p);
    return b.s;
}

static void serialize_client_line(sb *b, const gcase *c, odk_status status,
                                  const char *out_json, const char *err_text)
{
    char *timed = out_json ? strip_execution_time(out_json) : NULL;
    char *stripped =
        timed ? mask_string_value(timed, "\"library_version\":\"", "<version>")
              : NULL;
    free(timed);
    sb_printf(b, "{\"id\":\"%s\",\"status\":%d,\"output\":", c->id,
              (int)status);
    if (stripped != NULL) {
        sb_puts(b, stripped);
        free(stripped);
    } else {
        sb_puts(b, "null");
    }
    sb_puts(b, ",\"error\":");
    json_escape_raw(b, err_text ? err_text : "");
    sb_putc(b, '}');
}

static int run_client_case(const gcase *c, sb *line)
{
    odk_client *client = odk_client_create("cpu");
    odk_error err;
    odk_status st;
    char *contract = NULL;

    memset(&err, 0, sizeof(err));
    if (client == NULL) {
        serialize_client_line(line, c, ODK_ERR_MEMORY, NULL, "client alloc");
        return 0;
    }
    st = odk_client_evaluate_json_ex(client, c->context ? c->context : "",
                                     c->cli_queries_json ? c->cli_queries_json
                                                         : "{}",
                                     "hybrid", NULL, &contract, &err);
    serialize_client_line(line, c, st, contract,
                          err.text[0] ? err.text : NULL);
    free(contract);
    odk_client_free(client);
    return 0;
}

/* ---------------- input parsing (check mode) ------------------------- */
static void free_case(gcase *c)
{
    size_t i;
    free(c->context);
    free((char *)c->instruction);
    free((char *)c->statement);
    for (i = 0; i < c->option_count; i++) {
        free((char *)c->options[i]);
    }
    for (i = 0; i < c->rubric_count; i++) {
        free((char *)c->rubric[i]);
    }
    free((char *)c->cli_queries_json);
    memset(c, 0, sizeof(*c));
}

static char *dup_json_string(const odk_json_value *v)
{
    if (v == NULL || v->type != ODK_JSON_STRING) {
        return dup_text("");
    }
    return dup_text(v->string);
}

static long json_get_long(const odk_json_value *obj, const char *key,
                          long dflt)
{
    const odk_json_value *v = odk_json_object_get(obj, key);
    if (v == NULL || v->type != ODK_JSON_NUMBER) {
        return dflt;
    }
    return (long)v->number;
}

static const char *json_get_string(const odk_json_value *obj, const char *key,
                                   const char *dflt)
{
    const odk_json_value *v = odk_json_object_get(obj, key);
    if (v == NULL || v->type != ODK_JSON_STRING) {
        return dflt;
    }
    return v->string;
}

static int lang_from_code_or(const char *code, int dflt)
{
    int ok = 0;
    odk_language l;
    if (code == NULL) {
        return dflt;
    }
    if (strcmp(code, "none") == 0) {
        return -1;
    }
    if (strcmp(code, "default") == 0) {
        return ODK_LANG_DEFAULT;
    }
    l = odk_lang_from_code(code, &ok);
    return ok ? (int)l : dflt;
}

static int parse_input_line(const char *line, gcase *out, char **err_msg)
{
    odk_json_value *root;
    const odk_json_value *v;
    const odk_json_value *arr;
    const char *kind;
    const char *prim;
    const char *code;
    size_t i;
    char *b64;
    unsigned char *ctx;
    size_t ctx_len = 0;

    memset(out, 0, sizeof(*out));
    out->cfg_context_max = -1;
    out->cfg_chunk_source_max = -1;
    out->cfg_max_chunks = -1;
    out->cfg_snippet_max = -1;
    out->cfg_max_ranked = -1;
    out->cfg_max_snippet_chars = -1;
    out->cfg_max_total_chars = -1;
    out->cfg_max_work = -1;
    out->call_lang = -1;
    out->cap_ranked = -1;
    out->cap_snippet_chars = -1;
    out->cap_total_chars = -1;
    out->top_k = 3;
    root = odk_json_parse(line, strlen(line), NULL);
    if (root == NULL || root->type != ODK_JSON_OBJECT) {
        *err_msg = dup_text("not a JSON object");
        odk_json_free(root);
        return 1;
    }
    code = json_get_string(root, "id", "");
    snprintf(out->id, sizeof(out->id), "%s", code);
    kind = json_get_string(root, "kind", "base");
    out->kind = strcmp(kind, "kern") == 0
                    ? K_KERN
                    : strcmp(kind, "cli") == 0 ? K_CLI : K_BASE;
    prim = json_get_string(root, "primitive", "choice");
    out->prim = -1;
    for (i = 0; i < 6; i++) {
        if (strcmp(prim, PRIM_NAMES[i]) == 0) {
            out->prim = (int)i;
        }
    }
    if (out->prim < 0) {
        *err_msg = dup_text("unknown primitive");
        goto bad;
    }
    out->instruction = dup_json_string(odk_json_object_get(root, "instruction"));
    out->statement = dup_json_string(odk_json_object_get(root, "statement"));
    arr = odk_json_object_get(root, "options");
    if (arr != NULL && arr->type == ODK_JSON_ARRAY) {
        if (arr->item_count > G_MAXOPTS) {
            *err_msg = dup_text("too many options");
            goto bad;
        }
        for (i = 0; i < arr->item_count; i++) {
            out->options[i] = dup_json_string(arr->items[i]);
        }
        out->option_count = arr->item_count;
    }
    arr = odk_json_object_get(root, "rubric");
    if (arr != NULL && arr->type == ODK_JSON_ARRAY) {
        if (arr->item_count > G_MAXRUBRIC) {
            *err_msg = dup_text("too many rubric levels");
            goto bad;
        }
        for (i = 0; i < arr->item_count; i++) {
            out->rubric[i] = dup_json_string(arr->items[i]);
        }
        out->rubric_count = arr->item_count;
    }
    b64 = dup_json_string(odk_json_object_get(root, "context_b64"));
    ctx = b64_decode(b64, &ctx_len);
    free(b64);
    if (ctx == NULL) {
        *err_msg = dup_text("bad base64 context");
        goto bad;
    }
    ctx = realloc(ctx, ctx_len + 1);
    if (ctx == NULL) {
        *err_msg = dup_text("oom");
        goto bad;
    }
    ctx[ctx_len] = '\0';
    out->context = (char *)ctx;
    out->mode = strcmp(json_get_string(root, "text_mode", "ascii"),
                       "unicode") == 0
                    ? 1
                    : 0;
    {
        const char *pn = json_get_string(root, "profile", "plain");
        out->profile = 0;
        for (i = 0; i < 12; i++) {
            if (strcmp(pn, PROFILE_NAMES[i]) == 0) {
                out->profile = (int)i;
            }
        }
    }
    out->lang = lang_from_code_or(
        json_get_string(root, "language", "und"), ODK_LANG_UND);
    {
        const char *on = json_get_string(root, "over_limit",
                                         "profile_default");
        out->over_limit =
            strcmp(on, "head") == 0
                ? 1
                : strcmp(on, "head_tail") == 0
                      ? 2
                      : strcmp(on, "select") == 0 ? 3 : 0;
    }
    v = odk_json_object_get(root, "config");
    if (v != NULL && v->type == ODK_JSON_OBJECT) {
        out->cfg_context_max = json_get_long(v, "context_max", -1);
        out->cfg_chunk_source_max =
            json_get_long(v, "chunk_source_max", -1);
        out->cfg_max_chunks = json_get_long(v, "max_chunks", -1);
        out->cfg_snippet_max = json_get_long(v, "snippet_max", -1);
        out->cfg_max_ranked = json_get_long(v, "max_ranked", -1);
        out->cfg_max_snippet_chars =
            json_get_long(v, "max_snippet_chars", -1);
        out->cfg_max_total_chars =
            json_get_long(v, "max_total_chars", -1);
        out->cfg_max_work = json_get_long(v, "max_work", -1);
    }
    v = odk_json_object_get(root, "call");
    if (v != NULL && v->type == ODK_JSON_OBJECT) {
        out->call_lang = lang_from_code_or(
            json_get_string(v, "language", "none"), -1);
        out->call_morph =
            (int)json_get_long(v, "morph_light", 0) ? 1 : 0;
    }
    v = odk_json_object_get(root, "caps");
    if (v != NULL && v->type == ODK_JSON_OBJECT) {
        out->cap_ranked = (int)json_get_long(v, "max_ranked", -1);
        out->cap_snippet_chars =
            (int)json_get_long(v, "max_snippet_chars", -1);
        out->cap_total_chars =
            (int)json_get_long(v, "max_total_chars", -1);
    }
    out->top_k = (int)json_get_long(root, "top_k", 3);
    {
        const char *im = json_get_string(root, "image", "none");
        out->image = strcmp(im, "gradient24") == 0
                         ? 1
                         : strcmp(im, "half24") == 0
                               ? 2
                               : strcmp(im, "solid64") == 0 ? 3 : 0;
    }
    if (out->kind == K_CLI) {
        out->cli_queries_json = dup_json_string(
            odk_json_object_get(root, "queries_json"));
    }
    odk_json_free(root);
    return 0;
bad:
    odk_json_free(root);
    return 1;
}

/* ---------------- gen / check mains ---------------------------------- */
static int gen_main(const char *dir)
{
    char path[512];
    FILE *fi;
    FILE *fe;
    size_t i;
    int rc = 0;

    snprintf(path, sizeof(path), "%s", dir);
    /* mkdir via system mkdir(2) through stdio-independent call */
    {
        char cmd[560];
        snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", dir);
        if (system(cmd) != 0) {
            fprintf(stderr, "fatal: cannot create %s\n", dir);
            return 1;
        }
    }
    snprintf(path, sizeof(path), "%s/inputs.jsonl", dir);
    fi = fopen(path, "w");
    snprintf(path, sizeof(path), "%s/expected.jsonl", dir);
    fe = fopen(path, "w");
    if (fi == NULL || fe == NULL) {
        fprintf(stderr, "fatal: cannot open corpus files in %s\n", dir);
        return 1;
    }
    if (g_case_count < G_MIN_CASES) {
        fprintf(stderr, "fatal: only %zu cases (< %d)\n", g_case_count,
                G_MIN_CASES);
        return 1;
    }
    for (i = 0; i < g_case_count && rc == 0; i++) {
        const gcase *c = &g_cases[i];
        sb in;
        sb ex;
        memset(&in, 0, sizeof(in));
        memset(&ex, 0, sizeof(ex));
        serialize_input_line(&in, c);
        if (c->kind == K_CLI) {
            run_client_case(c, &ex);
        } else {
            odk_result result;
            odk_result_meta meta;
            odk_error err;
            odk_status st;
            memset(&meta, 0, sizeof(meta));
            st = run_case(c, &result, c->kind == K_KERN ? &meta : NULL, &err);
            serialize_expected_line(&ex, c, &result,
                                    c->kind == K_KERN ? &meta : NULL, st,
                                    st == ODK_OK ? NULL : err.text);
            odk_result_release(&result);
        }
        fputs(in.s, fi);
        fputc('\n', fi);
        fputs(ex.s, fe);
        fputc('\n', fe);
        sb_free(&in);
        sb_free(&ex);
    }
    fclose(fi);
    fclose(fe);
    {
        size_t nb = 0;
        size_t nk = 0;
        size_t nc = 0;
        for (i = 0; i < g_case_count; i++) {
            if (g_cases[i].kind == K_BASE) {
                nb++;
            } else if (g_cases[i].kind == K_KERN) {
                nk++;
            } else {
                nc++;
            }
        }
        printf("golden: wrote %zu cases to %s (base=%zu kern=%zu cli=%zu)\n",
               g_case_count, dir, nb, nk, nc);
    }
    return rc;
}

static int check_main(const char *dir)
{
    char path[512];
    FILE *fi;
    FILE *fe;
    char *line_in = NULL;
    char *line_ex = NULL;
    size_t cap_in = 0;
    size_t cap_ex = 0;
    size_t n = 0;
    size_t mismatches = 0;
    const size_t MAX_REPORT = 10;
    int rc = 0;

    snprintf(path, sizeof(path), "%s/inputs.jsonl", dir);
    fi = fopen(path, "r");
    snprintf(path, sizeof(path), "%s/expected.jsonl", dir);
    fe = fopen(path, "r");
    if (fi == NULL || fe == NULL) {
        fprintf(stderr, "fatal: missing corpus files in %s\n", dir);
        return 1;
    }
    while (getline(&line_in, &cap_in, fi) >= 0) {
        sb actual;
        if (getline(&line_ex, &cap_ex, fe) < 0) {
            fprintf(stderr, "fatal: expected.jsonl shorter than inputs.jsonl "
                            "at case %zu\n", n);
            rc = 1;
            break;
        }
        {
            gcase c;
            char *err_msg = NULL;
            size_t ll = strlen(line_in);
            while (ll > 0 && (line_in[ll - 1] == '\n' ||
                              line_in[ll - 1] == '\r')) {
                line_in[--ll] = '\0';
            }
            if (parse_input_line(line_in, &c, &err_msg) != 0) {
                fprintf(stderr, "fatal: cannot parse inputs.jsonl line %zu: "
                                "%s\n", n + 1, err_msg ? err_msg : "?");
                free(err_msg);
                rc = 1;
                break;
            }
            memset(&actual, 0, sizeof(actual));
            if (c.kind == K_CLI) {
                run_client_case(&c, &actual);
            } else {
                odk_result result;
                odk_result_meta meta;
                odk_error err;
                odk_status st;
                memset(&meta, 0, sizeof(meta));
                st = run_case(&c, &result, c.kind == K_KERN ? &meta : NULL,
                              &err);
                serialize_expected_line(&actual, &c, &result,
                                        c.kind == K_KERN ? &meta : NULL, st,
                                        st == ODK_OK ? NULL : err.text);
                odk_result_release(&result);
            }
            free_case(&c);
        }
        {
            size_t la = strlen(actual.s ? actual.s : "");
            size_t le = strlen(line_ex);
            while (le > 0 && (line_ex[le - 1] == '\n' ||
                              line_ex[le - 1] == '\r')) {
                line_ex[--le] = '\0';
            }
            if (actual.s == NULL || la != le ||
                memcmp(actual.s ? actual.s : "", line_ex, le) != 0) {
                mismatches++;
                if (mismatches <= MAX_REPORT) {
                    printf("MISMATCH at line %zu\n  expected: %s\n  "
                           "actual:   %s\n",
                           n + 1, line_ex, actual.s ? actual.s : "(null)");
                }
            }
        }
        sb_free(&actual);
        n++;
    }
    if (getline(&line_ex, &cap_ex, fe) >= 0 && rc == 0) {
        fprintf(stderr, "fatal: expected.jsonl has extra lines after %zu\n",
                n);
        rc = 1;
    }
    free(line_in);
    free(line_ex);
    fclose(fi);
    fclose(fe);
    if (rc == 0) {
        if (n < G_MIN_CASES) {
            fprintf(stderr, "fatal: only %zu cases checked (< %d)\n", n,
                    G_MIN_CASES);
            return 1;
        }
        if (mismatches == 0) {
            printf("golden-check: %zu cases, all byte-identical\n", n);
            return 0;
        }
        printf("golden-check: FAILED, %zu/%zu cases differ\n", mismatches, n);
        return 1;
    }
    return rc;
}

int main(int argc, char **argv)
{
    const char *mode;
    const char *dir = "golden";
    images_init();
    if (argc < 2) {
        fprintf(stderr, "usage: %s gen|check [dir]\n", argv[0]);
        return 1;
    }
    mode = argv[1];
    if (argc >= 3) {
        dir = argv[2];
    }
    if (strcmp(mode, "gen") == 0) {
        enumerate();
        return gen_main(dir);
    }
    if (strcmp(mode, "check") == 0) {
        return check_main(dir);
    }
    fprintf(stderr, "unknown mode: %s\n", mode);
    return 1;
}
