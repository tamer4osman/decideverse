/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * languages.c - non-English text with the UNICODE text mode.
 *
 *   make examples && ./build/languages
 *
 * The default text mode (ODK_TEXT_ASCII_COMPAT) only tokenizes ASCII
 * words, so it cannot see words in Arabic, Chinese, Hindi, ...  Use
 * ODK_TEXT_UNICODE for any non-ASCII text.  Here the same decision is
 * asked in Spanish, Arabic and Chinese, with the language set
 * explicitly (a pack) and with the auto-detector.
 */
#include <stdio.h>
#include <string.h>

#include "omnidecide.h"

typedef struct {
    const char *code;
    const char *question;
    const char *options[2];
    const char *context;
} sample;

static const sample samples[] = {
    {"es",
     "\xC2\xBF" "D\xC3\xB3nde deber\xC3\xAD" "a alojarse el servidor de respaldo?",
     {"centro de datos de Berl\xC3\xAD" "n", "oficina remota"},
     "El servidor de respaldo debe estar en el centro de datos de "
     "Berl\xC3\xAD" "n porque tiene energ\xC3\xAD" "a redundante."},
    {"ar",
     "\xD8\xA3\xD9\x8A\xD9\x86 \xD9\x8A\xD8\xAC\xD8\xA8 \xD8\xA7\xD8\xB3\xD8\xAA"
     "\xD8\xB6\xD8\xA7\xD9\x81\xD8\xA9 \xD8\xAE\xD8\xA7\xD8\xAF\xD9\x85 "
     "\xD8\xA7\xD9\x84\xD9\x86\xD8\xB3\xD8\xAE "
     "\xD8\xA7\xD9\x84\xD8\xA7\xD8\xAD\xD8\xAA\xD9\x8A\xD8\xA7\xD8\xB7\xD9\x8A\xD8\x9F",
     {"\xD9\x85\xD8\xB1\xD9\x83\xD8\xB2 \xD8\xA8\xD9\x8A\xD8\xA7\xD9\x86\xD8\xA7\xD8\xAA "
      "\xD8\xA8\xD8\xB1\xD9\x84\xD9\x8A\xD9\x86",
      "\xD9\x85\xD9\x83\xD8\xAA\xD8\xA8 \xD8\xA7\xD9\x84\xD9\x81\xD8\xB1\xD8\xB9 "
      "\xD8\xA7\xD9\x84\xD8\xA8\xD8\xB9\xD9\x8A\xD8\xAF"},
     "\xD9\x8A\xD8\xAC\xD8\xA8 \xD8\xA3\xD9\x86 \xD9\x8A\xD9\x83\xD9\x88\xD9\x86 "
     "\xD8\xAE\xD8\xA7\xD8\xAF\xD9\x85 \xD8\xA7\xD9\x84\xD9\x86\xD8\xB3\xD8\xAE "
     "\xD8\xA7\xD9\x84\xD8\xA7\xD8\xAD\xD8\xAA\xD9\x8A\xD8\xA7\xD8\xB7\xD9\x8A "
     "\xD9\x81\xD9\x8A \xD9\x85\xD8\xB1\xD9\x83\xD8\xB2 \xD8\xA8\xD9\x8A\xD8\xA7"
     "\xD9\x86\xD8\xA7\xD8\xAA \xD8\xA8\xD8\xB1\xD9\x84\xD9\x8A\xD9\x86."},
    {"zh",
     "\xE5\xA4\x87\xE4\xBB\xBD\xE6\x9C\x8D\xE5\x8A\xA1\xE5\x99\xA8\xE5\xBA\x94"
     "\xE8\xAF\xA5\xE6\x94\xBE\xE5\x9C\xA8\xE5\x93\xAA\xE9\x87\x8C\xEF\xBC\x9F",
     {"\xE6\x9F\x8F\xE6\x9E\x97\xE6\x95\xB0\xE6\x8D\xAE\xE4\xB8\xAD\xE5\xBF\x83",
      "\xE8\xBF\x9C\xE7\xA8\x8B\xE5\x88\x86\xE5\x85\xAC\xE5\x8F\xB8"},
     "\xE5\xA4\x87\xE4\xBB\xBD\xE6\x9C\x8D\xE5\x8A\xA1\xE5\x99\xA8\xE5\xBA\x94"
     "\xE8\xAF\xA5\xE6\x94\xBE\xE5\x9C\xA8\xE6\x9F\x8F\xE6\x9E\x97\xE6\x95\xB0"
     "\xE6\x8D\xAE\xE4\xB8\xAD\xE5\xBF\x83\xEF\xBC\x8C\xE5\x9B\xA0\xE4\xB8\xBA"
     "\xE9\x82\xA3\xE9\x87\x8C\xE6\x9C\x89\xE5\x86\x97\xE4\xBD\x99\xE7\x94\xB5"
     "\xE6\xBA\x90\xE3\x80\x82"},
};

static void ask(const sample *s, odk_language lang, const char *label)
{
    odk_kernel_config cfg;
    odk_kernel *kernel;
    odk_result result;
    odk_error err;

    memset(&cfg, 0, sizeof(cfg));   /* zero = documented defaults       */
    cfg.text_mode = ODK_TEXT_UNICODE;
    cfg.language = lang;
    kernel = odk_kernel_create_ex(&cfg);
    if (kernel == NULL) {
        return;
    }
    if (odk_evaluate_choice(kernel, s->question, s->options, 2, s->context,
                            NULL, &result, &err) == ODK_OK) {
        printf("  %-12s -> %s  (confidence %.2f)\n", label, result.string,
               result.confidence);
        odk_result_release(&result);
    } else {
        printf("  %-12s -> error: %s\n", label, err.text);
    }
    odk_kernel_free(kernel);
}

int main(void)
{
    size_t i;

    for (i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        const sample *s = &samples[i];
        int ok = 0;
        odk_language lang = odk_lang_from_code(s->code, &ok);
        double confidence = 0.0;
        odk_language detected =
            odk_lang_detect_text(s->context, &confidence, NULL, 0);

        printf("%s (%s); detector says %s (%.2f)\n", odk_lang_display_name(lang),
               s->code, odk_lang_to_code(detected), confidence);
        ask(s, lang, "pack");
        ask(s, ODK_LANG_AUTO, "auto-detect");
        ask(s, ODK_LANG_UND, "generic");
    }
    return 0;
}
