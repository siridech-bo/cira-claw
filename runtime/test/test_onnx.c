/**
 * CiRA Runtime - ONNX Loader Test
 *
 * This test loads an ONNX model and runs inference.
 *
 * Usage:
 *   ./test_onnx <model.onnx>
 *
 * (c) CiRA Robotics / KMITL 2026
 */

#include "cira.h"
#include <stdio.h>
#include <stdlib.h>

/* Create a test image */
static uint8_t* create_test_image(int w, int h) {
    uint8_t* data = (uint8_t*)malloc(w * h * 3);
    if (!data) return NULL;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = (y * w + x) * 3;
            data[idx + 0] = (uint8_t)(x * 255 / w);
            data[idx + 1] = (uint8_t)(y * 255 / h);
            data[idx + 2] = 128;
        }
    }

    return data;
}

int main(int argc, char* argv[]) {
    printf("CiRA Runtime - ONNX Test\n");
    printf("Version: %s\n\n", cira_version());

    if (argc < 2) {
        printf("Usage: %s <model.onnx>\n", argv[0]);
        return 1;
    }

    const char* model_path = argv[1];

    /* Create context */
    printf("Creating context...\n");
    cira_ctx* ctx = cira_create();
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }

    /* Load model */
    printf("Loading ONNX model: %s\n", model_path);
    int result = cira_load(ctx, model_path);
    if (result != CIRA_OK) {
        fprintf(stderr, "Failed to load model: %d\n", result);
        const char* err = cira_error(ctx);
        if (err) fprintf(stderr, "Error: %s\n", err);
        cira_destroy(ctx);
        return 1;
    }
    printf("Model loaded successfully\n\n");

    /* Create test image */
    int w = 224, h = 224;  /* Common ONNX input size */
    printf("Creating test image (%dx%d)...\n", w, h);
    uint8_t* image = create_test_image(w, h);
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

        printf("  [%d] %s: %.1f%%\n", i, label ? label : "?", score * 100);
    }

    printf("\nJSON Result:\n%s\n", cira_result_json(ctx));

    /* Cleanup */
    free(image);
    cira_destroy(ctx);

    printf("\nTest completed!\n");
    return 0;
}
