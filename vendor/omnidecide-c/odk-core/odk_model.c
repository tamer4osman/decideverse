/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * odk_model.c - model file v1 loader (docs/MODEL.md).
 *
 * Dependency-free SHA-256 (FIPS 180-4) + the strict 128-byte loader.
 * Every rejection leaves the kernel's model unchanged.
 */
#include "odk_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- SHA-256 (FIPS 180-4) ------------------------------ */
typedef struct {
    uint32_t h[8];
    uint64_t len;
    unsigned char buf[64];
    size_t buf_len;
} odk_sha256;

static uint32_t rotr(uint32_t x, int n)
{
    return (x >> n) | (x << (32 - n));
}

static void sha256_init(odk_sha256 *c)
{
    c->h[0] = 0x6a09e667u;
    c->h[1] = 0xbb67ae85u;
    c->h[2] = 0x3c6ef372u;
    c->h[3] = 0xa54ff53au;
    c->h[4] = 0x510e527fu;
    c->h[5] = 0x9b05688cu;
    c->h[6] = 0x1f83d9abu;
    c->h[7] = 0x5be0cd19u;
    c->len = 0;
    c->buf_len = 0;
}

static const uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
    0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
    0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
    0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
    0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
    0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
    0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
    0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

static void sha256_block(odk_sha256 *c, const unsigned char *p)
{
    uint32_t w[64];
    uint32_t a;
    uint32_t b;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    uint32_t t1;
    uint32_t t2;
    int i;

    for (i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
               ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
    }
    for (i = 16; i < 64; i++) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^
                      (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^
                      (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = c->h[0];
    b = c->h[1];
    d = c->h[3]; /* c->h[2] carried in local below */
    {
        uint32_t cc = c->h[2];
        e = c->h[4];
        f = c->h[5];
        g = c->h[6];
        h = c->h[7];
        for (i = 0; i < 64; i++) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            t1 = h + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
            t2 = S0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = cc;
            cc = b;
            b = a;
            a = t1 + t2;
        }
        c->h[0] += a;
        c->h[1] += b;
        c->h[2] += cc;
        c->h[3] += d;
        c->h[4] += e;
        c->h[5] += f;
        c->h[6] += g;
        c->h[7] += h;
    }
}

static void sha256_update(odk_sha256 *c, const unsigned char *data,
                          size_t len)
{
    c->len += len;
    while (len > 0) {
        size_t take = 64 - c->buf_len;
        if (take > len) {
            take = len;
        }
        memcpy(c->buf + c->buf_len, data, take);
        c->buf_len += take;
        data += take;
        len -= take;
        if (c->buf_len == 64) {
            sha256_block(c, c->buf);
            c->buf_len = 0;
        }
    }
}

static void sha256_final(odk_sha256 *c, unsigned char out[32])
{
    uint64_t bits = c->len * 8;
    unsigned char pad = 0x80;
    unsigned char lenb[8];
    int i;

    sha256_update(c, &pad, 1);
    pad = 0x00;
    while (c->buf_len != 56) {
        sha256_update(c, &pad, 1);
    }
    for (i = 0; i < 8; i++) {
        lenb[i] = (unsigned char)(bits >> (56 - 8 * i));
    }
    sha256_update(c, lenb, 8);
    for (i = 0; i < 8; i++) {
        out[i * 4] = (unsigned char)(c->h[i] >> 24);
        out[i * 4 + 1] = (unsigned char)(c->h[i] >> 16);
        out[i * 4 + 2] = (unsigned char)(c->h[i] >> 8);
        out[i * 4 + 3] = (unsigned char)c->h[i];
    }
}

void odk_sha256_hex(const unsigned char *data, size_t len, char out[65])
{
    odk_sha256 c;
    unsigned char dig[32];
    static const char HEX[] = "0123456789abcdef";
    int i;

    sha256_init(&c);
    sha256_update(&c, data, len);
    sha256_final(&c, dig);
    for (i = 0; i < 32; i++) {
        out[i * 2] = HEX[dig[i] >> 4];
        out[i * 2 + 1] = HEX[dig[i] & 15];
    }
    out[64] = '\0';
}

/* ---------------- model file v1 loader ------------------------------ */
#define ODK_MODEL_FILE_SIZE 128
#define ODK_MODEL_WEIGHT_OFF 20
#define ODK_MODEL_WEIGHTS 9

static const char ODK_MODEL_MAGIC[8] = {'O', 'D', 'K', 'M',
                                        'O', 'D', 'L', '1'};

static int read_double_le(const unsigned char *p, double *out)
{
    uint64_t bits = 0;
    int i;
    double d;
    for (i = 0; i < 8; i++) {
        bits |= ((uint64_t)p[i]) << (8 * i);
    }
    memcpy(&d, &bits, sizeof(d));
    *out = d;
    return 0;
}

static int bad_number(double d)
{
    return d != d || isinf(d); /* NaN or +-inf */
}

odk_status odk_model_parse_file(const char *path, odk_model *out,
                                odk_error *err)
{
    /* v1 = 128 bytes; v2 = 128 + 8*extra_count (cap 8) => at most 192 */
    unsigned char buf[ODK_MODEL_FILE_SIZE + ODK_MODEL_EXTRA_MAX * 8];
    unsigned char hash[32];
    odk_sha256 c;
    FILE *f;
    size_t got;
    uint32_t version;
    uint32_t fsid;
    uint32_t extra_count = 0;
    size_t hash_len;
    size_t i;
    int weight_i;

    if (path == NULL || out == NULL) {
        odk_set_error(err, "load model: NULL path or out");
        return ODK_ERR_ARGUMENT;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        odk_set_error(err, "load model: cannot open '%s'", path);
        return ODK_ERR_INFERENCE;
    }
    got = fread(buf, 1, sizeof(buf), f);
    {
        int extra = fgetc(f);
        fclose(f);
        if (extra != EOF) {
            odk_set_error(err, "load model: file larger than %d bytes",
                          ODK_MODEL_FILE_SIZE + ODK_MODEL_EXTRA_MAX * 8);
            return ODK_ERR_ARGUMENT;
        }
    }
    if (got < ODK_MODEL_FILE_SIZE) {
        odk_set_error(err, "load model: expected at least %d bytes, got "
                           "%zu",
                      ODK_MODEL_FILE_SIZE, got);
        return ODK_ERR_ARGUMENT;
    }
    if (memcmp(buf, ODK_MODEL_MAGIC, 7) != 0) {
        odk_set_error(err, "load model: bad magic");
        return ODK_ERR_ARGUMENT;
    }
    version = (uint32_t)buf[8] | ((uint32_t)buf[9] << 8) |
              ((uint32_t)buf[10] << 16) | ((uint32_t)buf[11] << 24);
    /* magic[7] is the format digit and must agree with the version */
    if (buf[7] != (unsigned char)('0' + version)) {
        odk_set_error(err, "load model: magic/version disagreement");
        return ODK_ERR_ARGUMENT;
    }
    if (version != 1 && version != 2) {
        odk_set_error(err, "load model: unsupported format version %lu",
                      (unsigned long)version);
        return ODK_ERR_ARGUMENT;
    }
    if (version == 2) {
        extra_count = (uint32_t)buf[16] | ((uint32_t)buf[17] << 8) |
                      ((uint32_t)buf[18] << 16) | ((uint32_t)buf[19] << 24);
        if (extra_count > ODK_MODEL_EXTRA_MAX) {
            odk_set_error(err, "load model: extra count %lu exceeds cap",
                          (unsigned long)extra_count);
            return ODK_ERR_ARGUMENT;
        }
    }
    /* Layout: 0..95 payload, then v2 extra weights (M*8 at offset 96),
     * then the 32-byte hash of everything before it.  v1: total 128.
     * v2: total 128 + 8*M with the hash at 96 + 8*M. */
    hash_len = 96 + (size_t)extra_count * 8;
    if (got != hash_len + 32) {
        odk_set_error(err, "load model: size mismatch (want %zu, got %zu)",
                      hash_len + 32, got);
        return ODK_ERR_ARGUMENT;
    }
    sha256_init(&c);
    sha256_update(&c, buf, hash_len);
    sha256_final(&c, hash);
    if (memcmp(hash, buf + hash_len, 32) != 0) {
        odk_set_error(err, "load model: hash mismatch (corrupt file)");
        return ODK_ERR_ARGUMENT;
    }
    fsid = (uint32_t)buf[12] | ((uint32_t)buf[13] << 8) |
           ((uint32_t)buf[14] << 16) | ((uint32_t)buf[15] << 24);
    if (fsid != 0 && fsid != 1) {
        odk_set_error(err, "load model: unsupported feature set id %lu",
                      (unsigned long)fsid);
        return ODK_ERR_ARGUMENT;
    }
    if (version == 2 && fsid != 1) {
        odk_set_error(err, "load model: v2 requires feature set 1");
        return ODK_ERR_ARGUMENT;
    }
    if (version == 1 && (fsid == 1 || extra_count != 0)) {
        odk_set_error(err, "load model: v1 carries no extra weights");
        return ODK_ERR_ARGUMENT;
    }
    if (version == 1) {
        /* the reserved bytes are part of the hashed
         * payload and must be zero in v1 (docs/MODEL.md section 1) -
         * the old check for extra_count != 0 was dead code. */
        size_t r;
        for (r = 16; r < 20; r++) {
            if (buf[r] != 0) {
                odk_set_error(err, "load model: v1 reserved bytes at "
                                   "16..19 must be zero");
                return ODK_ERR_ARGUMENT;
            }
        }
        for (r = 92; r < 96; r++) {
            if (buf[r] != 0) {
                odk_set_error(err, "load model: v1 reserved bytes at "
                                   "92..95 must be zero");
                return ODK_ERR_ARGUMENT;
            }
        }
    }
    if (version == 2) {
        /* bytes 92..95 are reserved in v2 too (bytes 16..19 hold the extra
         * count); a nonzero value could mean a newer format. */
        size_t r;
        for (r = 92; r < 96; r++) {
            if (buf[r] != 0) {
                odk_set_error(err, "load model: v2 reserved bytes at "
                                   "92..95 must be zero");
                return ODK_ERR_ARGUMENT;
            }
        }
    }
    memset(out, 0, sizeof(*out));
    out->feature_set_id = fsid;
    for (weight_i = 0; weight_i < ODK_MODEL_WEIGHTS; weight_i++) {
        double d;
        read_double_le(buf + ODK_MODEL_WEIGHT_OFF + 8 * weight_i, &d);
        if (bad_number(d)) {
            odk_set_error(err, "load model: weight %d is NaN or infinite",
                          weight_i);
            return ODK_ERR_ARGUMENT;
        }
        if (weight_i >= 7 && d < 0.0) { /* calibration scales >= 0 */
            odk_set_error(err, "load model: calibration scale %d is "
                               "negative",
                          weight_i);
            return ODK_ERR_ARGUMENT;
        }
    }
    read_double_le(buf + 20, &out->w_cos);
    read_double_le(buf + 28, &out->w_jaccard);
    read_double_le(buf + 36, &out->w_trigram);
    read_double_le(buf + 44, &out->w_whole);
    read_double_le(buf + 52, &out->w_best);
    read_double_le(buf + 60, &out->w_ctx);
    read_double_le(buf + 68, &out->w_dir);
    read_double_le(buf + 76, &out->logit_scale);
    read_double_le(buf + 84, &out->noul_logit_scale);
    if (version == 2) {
        out->extra_count = (int)extra_count;
        for (i = 0; i < extra_count; i++) {
            double d;
            read_double_le(buf + 96 + i * 8, &d);
            if (bad_number(d)) {
                odk_set_error(err, "load model: extra weight %zu is NaN "
                                   "or infinite",
                              i);
                return ODK_ERR_ARGUMENT;
            }
            out->extra[i] = d;
        }
    }
    return ODK_OK;
}
