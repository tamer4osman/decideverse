/* SPDX-License-Identifier: Apache-2.0 */
/* SPDX-FileCopyrightText: 2026 Tamer Osman */

/*
 * odk_vision.c - deterministic image fingerprint.
 *
 * Source pixels are area-averaged directly onto a 24x24 grid:
 * deterministic and dependency-free.  The brightness/colour/aspect
 * buckets are coarse thresholds, so they are robust to small
 * resampling differences.
 *
 * Never fails: any degenerate input degrades to the bare marker "image".
 *
 */

#include "odk_internal.h"

#include <stdio.h>

#define ODK_SIGNATURE_GRID 24
#define ODK_LUMA_DARK_THRESHOLD 85.0
#define ODK_LUMA_BRIGHT_THRESHOLD 170.0
#define ODK_NEUTRAL_SPREAD_THRESHOLD 24.0
#define ODK_ASPECT_TOLERANCE 0.05

static const char *vision_aspect_label(int width, int height)
{
    double bigger;
    if (width <= 0 || height <= 0) {
        return "unknown";
    }
    bigger = (double)(width > height ? width : height);
    if (fabs((double)(width - height)) <= ODK_ASPECT_TOLERANCE * bigger) {
        return "square";
    }
    if (height > width) {
        return "portrait";
    }
    return "landscape";
}

static const char *vision_color_label(double avg_r, double avg_g, double avg_b)
{
    double channel_max = avg_r;
    double channel_min = avg_r;
    double spread;
    if (avg_g > channel_max) {
        channel_max = avg_g;
    }
    if (avg_b > channel_max) {
        channel_max = avg_b;
    }
    if (avg_g < channel_min) {
        channel_min = avg_g;
    }
    if (avg_b < channel_min) {
        channel_min = avg_b;
    }
    spread = channel_max - channel_min;
    if (spread < ODK_NEUTRAL_SPREAD_THRESHOLD) {
        return "neutral";
    }
    if (channel_max == avg_r) {
        return "reddish";
    }
    if (channel_max == avg_g) {
        return "greenish";
    }
    return "bluish";
}

odk_status odk_describe_image(const odk_image *image, char *buf, size_t cap,
                              odk_error *err)
{
    double avg_r = 0.0;
    double avg_g = 0.0;
    double avg_b = 0.0;
    double luma;
    const char *brightness;
    const char *color;
    const char *aspect;
    int cell;
    int usable;
    int width;
    int height;
    const unsigned char *rgb;

    if (buf == NULL || cap == 0) {
        if (err != NULL) {
            snprintf(err->text, sizeof(err->text), "signature buffer missing");
        }
        return ODK_ERR_ARGUMENT;
    }
    buf[0] = '\0';

    usable = image != NULL && image->rgb != NULL && image->width > 0 &&
             image->height > 0;
    if (usable) {
        uint64_t area = (uint64_t)image->width * (uint64_t)image->height;
        if (area > (uint64_t)262144) { /* bounded work: 512x512 megapixels guard */
            usable = 0;
        }
    }
    if (!usable) {
        if (snprintf(buf, cap, "image") >= (int)cap) {
            buf[0] = '\0';
            if (err != NULL) {
                snprintf(err->text, sizeof(err->text),
                         "signature buffer too small");
            }
            return ODK_ERR_ARGUMENT;
        }
        return ODK_OK;
    }

    width = image->width;
    height = image->height;
    rgb = image->rgb;

    for (cell = 0; cell < ODK_SIGNATURE_GRID * ODK_SIGNATURE_GRID; cell++) {
        int cx = cell % ODK_SIGNATURE_GRID;
        int cy = cell / ODK_SIGNATURE_GRID;
        long x0 = (long)cx * width / ODK_SIGNATURE_GRID;
        long x1 = (long)(cx + 1) * width / ODK_SIGNATURE_GRID;
        long y0 = (long)cy * height / ODK_SIGNATURE_GRID;
        long y1 = (long)(cy + 1) * height / ODK_SIGNATURE_GRID;
        long x;
        long y;
        double sum_r = 0.0;
        double sum_g = 0.0;
        double sum_b = 0.0;
        long pixels;
        if (x1 <= x0) {
            x1 = x0 + 1;
        }
        if (y1 <= y0) {
            y1 = y0 + 1;
        }
        if (x1 > width) {
            x1 = width;
        }
        if (y1 > height) {
            y1 = height;
        }
        pixels = (x1 - x0) * (y1 - y0);
        if (pixels <= 0) {
            continue;
        }
        for (y = y0; y < y1; y++) {
            const unsigned char *row = rgb + ((size_t)y * (size_t)width) * 3;
            for (x = x0; x < x1; x++) {
                sum_r += row[(size_t)x * 3 + 0];
                sum_g += row[(size_t)x * 3 + 1];
                sum_b += row[(size_t)x * 3 + 2];
            }
        }
        avg_r += sum_r / (double)pixels;
        avg_g += sum_g / (double)pixels;
        avg_b += sum_b / (double)pixels;
    }
    avg_r /= (double)(ODK_SIGNATURE_GRID * ODK_SIGNATURE_GRID);
    avg_g /= (double)(ODK_SIGNATURE_GRID * ODK_SIGNATURE_GRID);
    avg_b /= (double)(ODK_SIGNATURE_GRID * ODK_SIGNATURE_GRID);

    luma = 0.299 * avg_r + 0.587 * avg_g + 0.114 * avg_b;
    if (luma < ODK_LUMA_DARK_THRESHOLD) {
        brightness = "dark";
    } else if (luma < ODK_LUMA_BRIGHT_THRESHOLD) {
        brightness = "midtone";
    } else {
        brightness = "bright";
    }
    color = vision_color_label(avg_r, avg_g, avg_b);
    aspect = vision_aspect_label(width, height);

    if (snprintf(buf, cap, "image %s %s %s %dx%d", brightness, color, aspect,
                 width, height) >= (int)cap) {
        buf[0] = '\0';
        if (err != NULL) {
            snprintf(err->text, sizeof(err->text),
                     "signature buffer too small");
        }
        return ODK_ERR_ARGUMENT;
    }
    return ODK_OK;
}
