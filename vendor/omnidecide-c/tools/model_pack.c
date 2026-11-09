/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */
/*
 * model_pack.c - write a model file v1 (docs/MODEL.md).
 *
 *   model_pack OUT.odkmodel [w_cos w_jac w_trig w_whole w_best w_ctx w_dir
 *                            logit_scale noul_scale]
 * With no weights the built-in default constants are written.
 */
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "odk_internal.h"

int main(int argc, char **argv)
{
    unsigned char buf[128 + ODK_MODEL_EXTRA_MAX * 8];
    odk_model m = odk_default_model;
    double vals[9];
    double extras[ODK_MODEL_EXTRA_MAX];
    int extra_count = 0;
    int version = 1;
    size_t payload_len;
    char hash[65];
    FILE *f;
    int i;

    memset(extras, 0, sizeof(extras));
    if (argc >= 2 && argc <= 12) {
        /* v1: OUT [9 weights] */
        if (argc == 11) {
            for (i = 0; i < 9; i++) {
                vals[i] = strtod(argv[i + 2], NULL);
            }
            m.w_cos = vals[0];
            m.w_jaccard = vals[1];
            m.w_trigram = vals[2];
            m.w_whole = vals[3];
            m.w_best = vals[4];
            m.w_ctx = vals[5];
            m.w_dir = vals[6];
            m.logit_scale = vals[7];
            m.noul_logit_scale = vals[8];
            m.feature_set_id = 0;
        } else if (argc == 2) {
            /* default constants */
        } else {
            fprintf(stderr, "usage: %s OUT.odkmodel [9 weights]\n"
                            "       %s OUT.odkmodel --fs1 W1..W9 E1..E6\n",
                    argv[0], argv[0]);
            return 1;
        }
    } else if (argc >= 15 && argc <= 20 && strcmp(argv[2], "--fs1") == 0) {
        /* v2: OUT --fs1 W1..W9 E1..EM  (M = argc - 12, cap 8) */
        version = 2;
        for (i = 0; i < 9; i++) {
            vals[i] = strtod(argv[i + 3], NULL);
        }
        extra_count = argc - 12;
        if (extra_count < 0 || extra_count > ODK_MODEL_EXTRA_MAX) {
            fprintf(stderr, "extras must be 0..%d\n", ODK_MODEL_EXTRA_MAX);
            return 1;
        }
        for (i = 0; i < extra_count; i++) {
            extras[i] = strtod(argv[12 + i], NULL);
        }
        m.w_cos = vals[0];
        m.w_jaccard = vals[1];
        m.w_trigram = vals[2];
        m.w_whole = vals[3];
        m.w_best = vals[4];
        m.w_ctx = vals[5];
        m.w_dir = vals[6];
        m.logit_scale = vals[7];
        m.noul_logit_scale = vals[8];
        m.feature_set_id = 1;
        m.extra_count = extra_count;
        for (i = 0; i < extra_count; i++) {
            m.extra[i] = extras[i];
        }
    } else {
        fprintf(stderr, "usage: %s OUT.odkmodel [9 weights]\n"
                        "       %s OUT.odkmodel --fs1 W1..W9 E1..E6\n",
                argv[0], argv[0]);
        return 1;
    }
    memcpy(buf, version == 2 ? "ODKMODL2" : "ODKMODL1", 8);
    buf[8] = (unsigned char)version;
    buf[9] = buf[10] = buf[11] = 0;
    buf[12] = (unsigned char)(m.feature_set_id & 0xFF);
    buf[13] = (unsigned char)((m.feature_set_id >> 8) & 0xFF);
    buf[14] = (unsigned char)((m.feature_set_id >> 16) & 0xFF);
    buf[15] = (unsigned char)((m.feature_set_id >> 24) & 0xFF);
    buf[16] = (unsigned char)(m.extra_count & 0xFF);
    buf[17] = buf[18] = buf[19] = 0;
    {
        const double w[9] = {m.w_cos,   m.w_jaccard, m.w_trigram,
                             m.w_whole, m.w_best,    m.w_ctx,
                             m.w_dir,   m.logit_scale,
                             m.noul_logit_scale};
        for (i = 0; i < 9; i++) {
            unsigned char *p = buf + 20 + 8 * i;
            uint64_t bits;
            int k;
            memcpy(&bits, &w[i], 8);
            for (k = 0; k < 8; k++) {
                p[k] = (unsigned char)((bits >> (8 * k)) & 0xFF);
            }
        }
    }
    buf[92] = buf[93] = buf[94] = buf[95] = 0;
    for (i = 0; i < m.extra_count; i++) {
        unsigned char *p = buf + 96 + 8 * i;
        uint64_t bits;
        int k;
        memcpy(&bits, &m.extra[i], 8);
        for (k = 0; k < 8; k++) {
            p[k] = (unsigned char)((bits >> (8 * k)) & 0xFF);
        }
    }
    payload_len = 96 + (size_t)m.extra_count * 8;
    odk_sha256_hex(buf, payload_len, hash);
    for (i = 0; i < 32; i++) {
        unsigned int byte;
        sscanf(hash + 2 * i, "%2x", &byte);
        buf[payload_len + i] = (unsigned char)byte;
    }
    f = fopen(argv[1], "wb");
    if (f == NULL) {
        fprintf(stderr, "cannot write %s\n", argv[1]);
        return 1;
    }
    fwrite(buf, 1, payload_len + 32, f);
    fclose(f);
    printf("wrote %s (%d bytes, sha256 %s)\n", argv[1],
           (int)(payload_len + 32), hash);
    return 0;
}
