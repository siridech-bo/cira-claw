/**
 * CiRA Runtime - Image Annotator
 *
 * This file implements drawing bounding boxes, labels, and confidence
 * scores on frames. It uses simple pixel-level operations to avoid
 * external dependencies.
 *
 * (c) CiRA Robotics / KMITL 2026
 */

#include "cira.h"
#include "cira_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#ifdef CIRA_STREAMING_ENABLED

/* Color palette for different classes (BGR format) */
static const uint8_t class_colors[][3] = {
    {0, 255, 0},     /* Green */
    {255, 0, 0},     /* Blue */
    {0, 0, 255},     /* Red */
    {255, 255, 0},   /* Cyan */
    {255, 0, 255},   /* Magenta */
    {0, 255, 255},   /* Yellow */
    {128, 0, 255},   /* Orange */
    {255, 128, 0},   /* Light blue */
    {0, 128, 255},   /* Light orange */
    {128, 255, 0},   /* Light green */
};
#define NUM_COLORS (sizeof(class_colors) / sizeof(class_colors[0]))

/* Simple 5x7 font for labels */
/* Each character is a 5-wide, 7-tall bitmap */
static const uint8_t font_5x7[][7] = {
    /* Space (32) */
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    /* 0-9 (48-57) */
    {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}, /* 0 */
    {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}, /* 1 */
    {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}, /* 2 */
    {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}, /* 3 */
    {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}, /* 4 */
    {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}, /* 5 */
    {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}, /* 6 */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}, /* 7 */
    {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}, /* 8 */
    {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}, /* 9 */
    /* A-Z (65-90) */
    {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, /* A */
    {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}, /* B */
    {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}, /* C */
    {0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C}, /* D */
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}, /* E */
    {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}, /* F */
    {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}, /* G */
    {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}, /* H */
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}, /* I */
    {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C}, /* J */
    {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}, /* K */
    {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}, /* L */
    {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}, /* M */
    {0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11}, /* N */
    {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, /* O */
    {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}, /* P */
    {0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D}, /* Q */
    {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}, /* R */
    {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}, /* S */
    {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}, /* T */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}, /* U */
    {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}, /* V */
    {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}, /* W */
    {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}, /* X */
    {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}, /* Y */
    {0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F}, /* Z */
};

/**
 * Get font bitmap for a character.
 */
static const uint8_t* get_char_bitmap(char c) {
    if (c == ' ') return font_5x7[0];
    if (c >= '0' && c <= '9') return font_5x7[1 + (c - '0')];
    if (c >= 'A' && c <= 'Z') return font_5x7[11 + (c - 'A')];
    if (c >= 'a' && c <= 'z') return font_5x7[11 + (c - 'a')];  /* Use uppercase */
    return font_5x7[0];  /* Default to space */
}

/**
 * Draw a pixel on RGB image.
 */
static void draw_pixel(uint8_t* img, int w, int h, int x, int y,
                        uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || x >= w || y < 0 || y >= h) return;
    int idx = (y * w + x) * 3;
    img[idx + 0] = r;
    img[idx + 1] = g;
    img[idx + 2] = b;
}

/**
 * Draw a horizontal line.
 */
static void draw_hline(uint8_t* img, int w, int h, int x1, int x2, int y,
                        int thickness, uint8_t r, uint8_t g, uint8_t b) {
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    for (int dy = -thickness/2; dy <= thickness/2; dy++) {
        for (int x = x1; x <= x2; x++) {
            draw_pixel(img, w, h, x, y + dy, r, g, b);
        }
    }
}

/**
 * Draw a vertical line.
 */
static void draw_vline(uint8_t* img, int w, int h, int x, int y1, int y2,
                        int thickness, uint8_t r, uint8_t g, uint8_t b) {
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    for (int dx = -thickness/2; dx <= thickness/2; dx++) {
        for (int y = y1; y <= y2; y++) {
            draw_pixel(img, w, h, x + dx, y, r, g, b);
        }
    }
}

/**
 * Draw a rectangle.
 */
static void draw_rect(uint8_t* img, int w, int h,
                       int x, int y, int rect_w, int rect_h,
                       int thickness, uint8_t r, uint8_t g, uint8_t b) {
    draw_hline(img, w, h, x, x + rect_w, y, thickness, r, g, b);
    draw_hline(img, w, h, x, x + rect_w, y + rect_h, thickness, r, g, b);
    draw_vline(img, w, h, x, y, y + rect_h, thickness, r, g, b);
    draw_vline(img, w, h, x + rect_w, y, y + rect_h, thickness, r, g, b);
}

/**
 * Draw a filled rectangle (for label background).
 */
static void draw_filled_rect(uint8_t* img, int w, int h,
                              int x, int y, int rect_w, int rect_h,
                              uint8_t r, uint8_t g, uint8_t b) {
    for (int dy = 0; dy < rect_h; dy++) {
        for (int dx = 0; dx < rect_w; dx++) {
            draw_pixel(img, w, h, x + dx, y + dy, r, g, b);
        }
    }
}

/**
 * Draw a character at position.
 */
static void draw_char(uint8_t* img, int w, int h,
                       int x, int y, char c, int scale,
                       uint8_t r, uint8_t g, uint8_t b) {
    const uint8_t* bitmap = get_char_bitmap(c);

    for (int row = 0; row < 7; row++) {
        for (int col = 0; col < 5; col++) {
            if (bitmap[row] & (1 << (4 - col))) {
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        draw_pixel(img, w, h,
                                   x + col * scale + sx,
                                   y + row * scale + sy,
                                   r, g, b);
                    }
                }
            }
        }
    }
}

/**
 * Draw text string.
 */
static void draw_text(uint8_t* img, int w, int h,
                       int x, int y, const char* text, int scale,
                       uint8_t r, uint8_t g, uint8_t b) {
    int char_w = 6 * scale;  /* 5 pixels + 1 space */

    while (*text) {
        draw_char(img, w, h, x, y, *text, scale, r, g, b);
        x += char_w;
        text++;
    }
}

/**
 * Annotate an image with detection results.
 *
 * @param img RGB image data (will be modified)
 * @param w Image width
 * @param h Image height
 * @param ctx Context with detection results
 * @param thickness Line thickness (default 2)
 * @param show_label Whether to show labels
 * @param show_confidence Whether to show confidence scores
 */
void annotate_detections(uint8_t* img, int w, int h, cira_ctx* ctx,
                          int thickness, int show_label, int show_confidence) {
    if (!img || !ctx) return;

    int count = cira_result_count(ctx);

    for (int i = 0; i < count; i++) {
        float bx, by, bw, bh;
        if (cira_result_bbox(ctx, i, &bx, &by, &bw, &bh) != CIRA_OK) continue;

        float score = cira_result_score(ctx, i);
        const char* label = cira_result_label(ctx, i);

        /* Convert normalized coords to pixels */
        int px = (int)(bx * w);
        int py = (int)(by * h);
        int pw = (int)(bw * w);
        int ph = (int)(bh * h);

        /* Get color for this detection */
        int color_idx = i % NUM_COLORS;
        uint8_t r = class_colors[color_idx][2];  /* Convert BGR to RGB */
        uint8_t g = class_colors[color_idx][1];
        uint8_t b = class_colors[color_idx][0];

        /* Draw bounding box */
        draw_rect(img, w, h, px, py, pw, ph, thickness, r, g, b);

        /* Draw label */
        if (show_label || show_confidence) {
            char label_text[64];
            if (show_label && show_confidence) {
                snprintf(label_text, sizeof(label_text), "%s %.0f%%",
                         label ? label : "?", score * 100);
            } else if (show_label) {
                snprintf(label_text, sizeof(label_text), "%s",
                         label ? label : "?");
            } else {
                snprintf(label_text, sizeof(label_text), "%.0f%%", score * 100);
            }

            int text_len = strlen(label_text);
            int text_w = text_len * 6 + 4;  /* 5 pixels per char + padding */
            int text_h = 10;

            /* Draw label background */
            int label_y = py - text_h - 2;
            if (label_y < 0) label_y = py + ph + 2;

            draw_filled_rect(img, w, h, px, label_y, text_w, text_h, r, g, b);

            /* Draw label text in white */
            draw_text(img, w, h, px + 2, label_y + 1, label_text, 1,
                      255, 255, 255);
        }
    }
}

/**
 * Draw FPS counter on image.
 */
void annotate_fps(uint8_t* img, int w, int h, float fps) {
    char text[32];
    snprintf(text, sizeof(text), "FPS: %.1f", fps);

    /* Draw background */
    draw_filled_rect(img, w, h, 5, 5, 60, 12, 0, 0, 0);

    /* Draw text */
    draw_text(img, w, h, 8, 7, text, 1, 0, 255, 0);
}

/**
 * Draw timestamp on image.
 */
void annotate_timestamp(uint8_t* img, int w, int h, const char* timestamp) {
    if (!timestamp) return;

    int text_len = strlen(timestamp);
    int text_w = text_len * 6 + 4;

    /* Draw at bottom-right */
    int x = w - text_w - 5;
    int y = h - 15;

    /* Draw background */
    draw_filled_rect(img, w, h, x, y, text_w, 12, 0, 0, 0);

    /* Draw text */
    draw_text(img, w, h, x + 2, y + 2, timestamp, 1, 255, 255, 255);
}

#else /* CIRA_STREAMING_ENABLED */

void annotate_detections(uint8_t* img, int w, int h, cira_ctx* ctx,
                          int thickness, int show_label, int show_confidence) {
    (void)img; (void)w; (void)h; (void)ctx;
    (void)thickness; (void)show_label; (void)show_confidence;
}

void annotate_fps(uint8_t* img, int w, int h, float fps) {
    (void)img; (void)w; (void)h; (void)fps;
}

void annotate_timestamp(uint8_t* img, int w, int h, const char* timestamp) {
    (void)img; (void)w; (void)h; (void)timestamp;
}

#endif /* CIRA_STREAMING_ENABLED */
