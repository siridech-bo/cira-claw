/**
 * Signal Ingestion Test Suite (Spec C v4)
 *
 * Build and run sequence:
 *   cd runtime/build
 *   cmake -DCIRA_ENABLE_ONNX=ON -DCIRA_BUILD_TESTS=ON ..
 *   cmake --build .
 *
 *   # Phase 1: Unit tests (no model)
 *   ./test_signal.exe
 *
 *   # Phase 2: Generate test model
 *   cd ../..
 *   python tools/make_test_signal_model.py
 *
 *   # Phase 3: Full pipeline test
 *   cd runtime/build
 *   ./test_signal.exe ../../test_signal_model
 */

#include "../include/cira.h"
#include "../include/signal_buffer.h"
#include "../include/signal_features.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define PASS() printf("  [PASS] %s\n", __func__)
#define FAIL(msg) do { printf("  [FAIL] %s: %s\n", __func__, msg); return 1; } while(0)
#define ASSERT(cond, msg) if (!(cond)) FAIL(msg)
#define ASSERT_NEAR(val, expected, tol, name) do { \
    float _v = (val); float _e = (expected); float _t = (tol); \
    printf("    %s: %.4f (expected %.4f ± %.4f)\n", name, _v, _e, _t); \
    if (fabsf(_v - _e) > _t) FAIL(name); \
} while(0)

/* Test 1: Buffer lifecycle */
static int test_buffer_lifecycle(void) {
    const char* names[] = {"ch0", "ch1", "ch2"};
    float mins[] = {-10.0f, -5.0f, 0.0f};
    float maxs[] = {10.0f, 5.0f, 100.0f};

    signal_buffer_t* buf = signal_buffer_create(3, names, mins, maxs, 128, 64, 1000.0f);
    ASSERT(buf != NULL, "signal_buffer_create failed");
    ASSERT(buf->num_channels == 3, "num_channels mismatch");
    ASSERT(buf->window_size == 128, "window_size mismatch");
    ASSERT(buf->stride == 64, "stride mismatch");
    ASSERT(fabsf(buf->sample_rate_hz - 1000.0f) < 0.01f, "sample_rate_hz mismatch");

    signal_buffer_destroy(buf);
    PASS();
    return 0;
}

/* Test 2: Feed and readiness */
static int test_feed_and_readiness(void) {
    const char* names[] = {"x"};
    float mins[] = {0.0f};
    float maxs[] = {1.0f};

    signal_buffer_t* buf = signal_buffer_create(1, names, mins, maxs, 10, 5, 100.0f);
    ASSERT(buf != NULL, "buffer create failed");

    /* Feed 9 samples - not ready */
    for (int i = 0; i < 9; i++) {
        signal_buffer_feed(buf, 0, 0.5f);
    }
    ASSERT(signal_buffer_ready(buf) == 0, "buffer should not be ready at 9 samples");

    /* Feed 10th sample - now ready */
    signal_buffer_feed(buf, 0, 0.5f);
    ASSERT(signal_buffer_ready(buf) == 1, "buffer should be ready at 10 samples");

    signal_buffer_destroy(buf);
    PASS();
    return 0;
}

/* Test 3a: DC signal features */
static int test_features_dc_signal(void) {
    const int N = 128;
    float window[128];
    float features[46];

    /* DC signal: all 5.0 */
    for (int i = 0; i < N; i++) {
        window[i] = 5.0f;
    }

    signal_compute_features(window, N, 1000.0f, features);

    ASSERT_NEAR(features[0], 5.0f, 0.01f, "mean");
    ASSERT_NEAR(features[1], 0.0f, 0.01f, "std");
    ASSERT_NEAR(features[25], 5.0f, 0.01f, "rms");
    ASSERT_NEAR(features[26], 0.0f, 0.01f, "peak_to_peak");

    PASS();
    return 0;
}

/* Test 3b: Sine wave features */
static int test_features_sine_wave(void) {
    const int N = 128;
    const float SR = 1000.0f;
    const float FREQ = SR / 4.0f;  /* 250 Hz */
    float window[128];
    float features[46];

    /* Generate sine wave */
    for (int i = 0; i < N; i++) {
        window[i] = sinf(2.0f * M_PI * FREQ * i / SR);
    }

    signal_compute_features(window, N, SR, features);

    ASSERT_NEAR(features[0], 0.0f, 0.1f, "mean (sine)");
    ASSERT_NEAR(features[25], 0.707f, 0.1f, "rms (sine)");

    /* Spectral features */
    float centroid = features[35];
    float peak_freq = features[40];
    printf("    spectral_centroid: %.2f Hz (expected ~%.2f Hz)\n", centroid, FREQ);
    printf("    peak_frequency: %.2f Hz (expected ~%.2f Hz)\n", peak_freq, FREQ);

    /* Allow wider tolerance for spectral features due to FFT windowing */
    ASSERT(fabsf(centroid - FREQ) < 50.0f, "spectral_centroid out of range");
    ASSERT(fabsf(peak_freq - FREQ) < 50.0f, "peak_frequency out of range");

    PASS();
    return 0;
}

/* Test 3c: Band power boundaries (critical regression check) */
static int test_features_band_power(void) {
    const int N = 128;
    const float SR = 1000.0f;
    const float FREQ = SR * 0.25f;  /* 250 Hz = sr/4, which is > sr/6 and < sr/3 */
    float window[128];
    float features[46];

    /* Generate tone at sr/4 (should fall in mid band) */
    for (int i = 0; i < N; i++) {
        window[i] = sinf(2.0f * M_PI * FREQ * i / SR);
    }

    signal_compute_features(window, N, SR, features);

    float band_low = features[43];   /* [0, sr/6) = [0, 166.7) Hz */
    float band_mid = features[44];   /* [sr/6, sr/3) = [166.7, 333.3) Hz */
    float band_high = features[45];  /* [sr/3, sr/2] = [333.3, 500] Hz */

    printf("    Tone at %.1f Hz (sr/4)\n", FREQ);
    printf("    band_power_low [0, %.1f): %.6f\n", SR/6, band_low);
    printf("    band_power_mid [%.1f, %.1f): %.6f\n", SR/6, SR/3, band_mid);
    printf("    band_power_high [%.1f, %.1f]: %.6f\n", SR/3, SR/2, band_high);

    /* Critical assertion: mid band must dominate */
    ASSERT(band_mid > band_low, "band_mid should be > band_low for tone at sr/4");
    ASSERT(band_mid > band_high, "band_mid should be > band_high for tone at sr/4");
    ASSERT(band_mid > (band_low + band_high) * 2.0f, "band_mid should strongly dominate");

    PASS();
    return 0;
}

/* Test 4: Manifest parsing */
static int test_manifest_parsing(const char* model_dir) {
    if (!model_dir) {
        printf("  [SKIP] %s (no model_dir provided)\n", __func__);
        return 0;
    }

    cira_ctx* ctx = cira_create();
    ASSERT(ctx != NULL, "cira_create failed");

    int ret = cira_load(ctx, model_dir);
    if (ret != CIRA_OK) {
        printf("  [FAIL] cira_load returned %d: %s\n", ret, cira_error(ctx));
        cira_destroy(ctx);
        return 1;
    }

    /* Check signal buffer was created */
    int ready = cira_signal_ready(ctx);
    ASSERT(ready != -1, "signal buffer not initialized (ready returned -1)");
    printf("    signal_ready: %d (expected 0 or 1, not -1)\n", ready);

    cira_destroy(ctx);
    PASS();
    return 0;
}

/* Test 5: Feed + stats */
static int test_feed_and_stats(const char* model_dir) {
    if (!model_dir) {
        printf("  [SKIP] %s (no model_dir provided)\n", __func__);
        return 0;
    }

    cira_ctx* ctx = cira_create();
    ASSERT(ctx != NULL, "cira_create failed");

    int ret = cira_load(ctx, model_dir);
    ASSERT(ret == CIRA_OK, "cira_load failed");

    /* Feed constant samples on channel 0 */
    for (int i = 0; i < 200; i++) {
        ret = cira_feed_signal(ctx, 0, 5.0f);
        ASSERT(ret == CIRA_OK, "cira_feed_signal failed");
    }

    /* Get stats */
    float rms, peak, mean;
    ret = cira_signal_stats(ctx, 0, &rms, &peak, &mean);
    ASSERT(ret == CIRA_OK, "cira_signal_stats failed");

    printf("    Channel 0 stats: rms=%.4f, peak=%.4f, mean=%.4f\n", rms, peak, mean);

    /* Stats should return raw values (normalization happens during feature extraction) */
    ASSERT(mean > 4.9f && mean < 5.1f, "mean should be ~5.0 (raw value)");

    cira_destroy(ctx);
    PASS();
    return 0;
}

int main(int argc, char** argv) {
    const char* model_dir = (argc > 1) ? argv[1] : NULL;
    int failures = 0;

    printf("=== CiRA Signal Ingestion Test Suite (Spec C v4) ===\n\n");

    printf("Test Group 1: Buffer Lifecycle\n");
    failures += test_buffer_lifecycle();

    printf("\nTest Group 2: Feed and Readiness\n");
    failures += test_feed_and_readiness();

    printf("\nTest Group 3: Known-Value Feature Assertions\n");
    printf("  3a: DC Signal\n");
    failures += test_features_dc_signal();
    printf("  3b: Sine Wave\n");
    failures += test_features_sine_wave();
    printf("  3c: Band Power Boundaries (CRITICAL)\n");
    failures += test_features_band_power();

    printf("\nTest Group 4: Manifest Parsing\n");
    failures += test_manifest_parsing(model_dir);

    printf("\nTest Group 5: Feed + Stats\n");
    failures += test_feed_and_stats(model_dir);

    printf("\n=== Summary ===\n");
    if (failures == 0) {
        printf("All tests passed!\n");
        return 0;
    } else {
        printf("%d test(s) failed.\n", failures);
        return 1;
    }
}
