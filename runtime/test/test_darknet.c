/**
 * CiRA Runtime - Darknet Loader Test
 *
 * This test loads a Darknet model and runs inference on a test image.
 * It verifies backward compatibility with CiRA CORE exported models.
 *
 * Usage:
 *   ./test_darknet <model_dir> [test_image.jpg]
 *
 * Example:
 *   ./test_darknet ~/.cira/workspace/models/scratch_v3
 *
 * (c) CiRA Robotics / KMITL 2026
 */

#include "cira.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Simple PPM image loader for testing */
static uint8_t* load_ppm(const char* path, int* w, int* h) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Failed to open image: %s\n", path);
        return NULL;
    }

    /* Read PPM header */
    char magic[3];
    if (fscanf(f, "%2s", magic) != 1 || strcmp(magic, "P6") != 0) {
        fprintf(stderr, "Not a P6 PPM file\n");
        fclose(f);
        return NULL;
    }

    /* Skip comments */
    int c;
    while ((c = fgetc(f)) == '#') {
        while ((c = fgetc(f)) != '\n' && c != EOF);
    }
    ungetc(c, f);

    /* Read dimensions */
    int width, height, maxval;
    if (fscanf(f, "%d %d %d", &width, &height, &maxval) != 3) {
        fprintf(stderr, "Failed to read PPM header\n");
        fclose(f);
        return NULL;
    }
    fgetc(f);  /* Skip whitespace after header */

    *w = width;
    *h = height;

    /* Allocate and read pixel data */
    size_t size = width * height * 3;
    uint8_t* data = (uint8_t*)malloc(size);
    if (!data) {
        fclose(f);
        return NULL;
    }

    if (fread(data, 1, size, f) != size) {
        fprintf(stderr, "Failed to read pixel data\n");
        free(data);
        fclose(f);
        return NULL;
    }

    fclose(f);
    return data;
}

/* Create a test image (red gradient) */
static uint8_t* create_test_image(int w, int h) {
    uint8_t* data = (uint8_t*)malloc(w * h * 3);
    if (!data) return NULL;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = (y * w + x) * 3;
            data[idx + 0] = (uint8_t)(x * 255 / w);     /* R */
            data[idx + 1] = (uint8_t)(y * 255 / h);     /* G */
            data[idx + 2] = 128;                         /* B */
        }
    }

    return data;
}

int main(int argc, char* argv[]) {
    printf("CiRA Runtime - Darknet Test\n");
    printf("Version: %s\n\n", cira_version());

    if (argc < 2) {
        printf("Usage: %s <model_dir> [test_image.ppm]\n", argv[0]);
        printf("\nExample:\n");
        printf("  %s ~/.cira/workspace/models/scratch_v3\n", argv[0]);
        return 1;
    }

    const char* model_path = argv[1];
    const char* image_path = argc > 2 ? argv[2] : NULL;

    /* Create context */
    printf("Creating context...\n");
    cira_ctx* ctx = cira_create();
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }

    /* Load model */
    printf("Loading model: %s\n", model_path);
    int result = cira_load(ctx, model_path);
    if (result != CIRA_OK) {
        fprintf(stderr, "Failed to load model: %d\n", result);
        const char* err = cira_error(ctx);
        if (err) fprintf(stderr, "Error: %s\n", err);
        cira_destroy(ctx);
        return 1;
    }
    printf("Model loaded successfully\n\n");

    /* Load or create test image */
    int w = 416, h = 416;
    uint8_t* image = NULL;

    if (image_path) {
        printf("Loading image: %s\n", image_path);
        image = load_ppm(image_path, &w, &h);
        if (!image) {
            fprintf(stderr, "Failed to load image, using test pattern\n");
        }
    }

    if (!image) {
        printf("Creating test image (%dx%d)...\n", w, h);
        image = create_test_image(w, h);
    }

    if (!image) {
        fprintf(stderr, "Failed to create test image\n");
        cira_destroy(ctx);
        return 1;
    }

    /* Run inference */
    printf("Running inference...\n");
    result = cira_predict_image(ctx, image, w, h, 3);
    if (result != CIRA_OK) {
        fprintf(stderr, "Inference failed: %d\n", result);
        const char* err = cira_error(ctx);
        if (err) fprintf(stderr, "Error: %s\n", err);
        free(image);
        cira_destroy(ctx);
        return 1;
    }

    /* Print results */
    int count = cira_result_count(ctx);
    printf("\nDetections: %d\n", count);

    for (int i = 0; i < count; i++) {
        float x, y, bw, bh;
        cira_result_bbox(ctx, i, &x, &y, &bw, &bh);
        float score = cira_result_score(ctx, i);
        const char* label = cira_result_label(ctx, i);

        printf("  [%d] %s: %.1f%% at (%.1f, %.1f, %.1f, %.1f)\n",
               i, label ? label : "?", score * 100, x, y, bw, bh);
    }

    /* Print JSON result */
    printf("\nJSON Result:\n%s\n", cira_result_json(ctx));

    /* Cleanup */
    free(image);
    cira_destroy(ctx);

    printf("\nTest completed successfully!\n");
    return 0;
}
