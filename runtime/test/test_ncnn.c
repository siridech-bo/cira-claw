/**
 * CiRA Runtime - NCNN Loader Test
 *
 * This test verifies the NCNN model loading and inference functionality.
 *
 * (c) CiRA Robotics / KMITL 2026
 */

#include "cira.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test image dimensions */
#define TEST_WIDTH 640
#define TEST_HEIGHT 480
#define TEST_CHANNELS 3

/* Create a simple test image (gradient pattern) */
static uint8_t* create_test_image(int w, int h, int c) {
    uint8_t* img = (uint8_t*)malloc(w * h * c);
    if (!img) return NULL;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int idx = (y * w + x) * c;
            img[idx + 0] = (uint8_t)(x * 255 / w);     /* R: horizontal gradient */
            img[idx + 1] = (uint8_t)(y * 255 / h);     /* G: vertical gradient */
            img[idx + 2] = 128;                         /* B: constant */
        }
    }

    return img;
}

int main(int argc, char** argv) {
    const char* model_path = NULL;
    int verbose = 0;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = 1;
        } else if (!model_path) {
            model_path = argv[i];
        }
    }

    printf("=== CiRA NCNN Loader Test ===\n\n");
    printf("CiRA Runtime Version: %s\n", cira_version());

    /* Create context */
    printf("\n[1] Creating context...\n");
    cira_ctx* ctx = cira_create();
    if (!ctx) {
        fprintf(stderr, "FAIL: Failed to create context\n");
        return 1;
    }
    printf("    OK: Context created\n");

    /* Test without model (should fail gracefully) */
    printf("\n[2] Testing prediction without model...\n");
    uint8_t* test_img = create_test_image(TEST_WIDTH, TEST_HEIGHT, TEST_CHANNELS);
    if (!test_img) {
        fprintf(stderr, "FAIL: Failed to allocate test image\n");
        cira_destroy(ctx);
        return 1;
    }

    int result = cira_predict_image(ctx, test_img, TEST_WIDTH, TEST_HEIGHT, TEST_CHANNELS);
    if (result == CIRA_ERROR_MODEL) {
        printf("    OK: Correctly failed with CIRA_ERROR_MODEL\n");
    } else {
        printf("    WARN: Expected CIRA_ERROR_MODEL, got %d\n", result);
    }

    /* Test with model if path provided */
    if (model_path) {
        printf("\n[3] Loading NCNN model: %s\n", model_path);

        result = cira_load(ctx, model_path);
        if (result != CIRA_OK) {
            fprintf(stderr, "    FAIL: Failed to load model (error %d)\n", result);
            const char* err = cira_error(ctx);
            if (err) fprintf(stderr, "    Error: %s\n", err);
            free(test_img);
            cira_destroy(ctx);
            return 1;
        }
        printf("    OK: Model loaded\n");

        /* Run inference */
        printf("\n[4] Running inference...\n");
        result = cira_predict_image(ctx, test_img, TEST_WIDTH, TEST_HEIGHT, TEST_CHANNELS);
        if (result != CIRA_OK) {
            fprintf(stderr, "    FAIL: Inference failed (error %d)\n", result);
            const char* err = cira_error(ctx);
            if (err) fprintf(stderr, "    Error: %s\n", err);
            free(test_img);
            cira_destroy(ctx);
            return 1;
        }
        printf("    OK: Inference completed\n");

        /* Check results */
        int count = cira_result_count(ctx);
        printf("\n[5] Results: %d detections\n", count);

        if (verbose) {
            for (int i = 0; i < count; i++) {
                float x, y, w, h;
                cira_result_bbox(ctx, i, &x, &y, &w, &h);
                float score = cira_result_score(ctx, i);
                const char* label = cira_result_label(ctx, i);

                printf("    [%d] %s (%.1f%%) at [%.3f, %.3f, %.3f, %.3f]\n",
                       i, label ? label : "?", score * 100, x, y, w, h);
            }
        }

        /* Print JSON result */
        printf("\n[6] JSON output:\n");
        const char* json = cira_result_json(ctx);
        printf("    %s\n", json);
    } else {
        printf("\n[3] Skipping model test (no model path provided)\n");
        printf("    Usage: %s <model_directory> [-v]\n", argv[0]);
    }

    /* Cleanup */
    printf("\n[7] Cleanup...\n");
    free(test_img);
    cira_destroy(ctx);
    printf("    OK: Resources freed\n");

    printf("\n=== Test Complete ===\n");
    return 0;
}
