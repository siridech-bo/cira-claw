/**
 * CiRA Runtime - Streaming Server Test
 *
 * This test starts the HTTP streaming server and runs until interrupted.
 *
 * Usage:
 *   ./test_stream [port]
 *
 * Then visit:
 *   http://localhost:8080/health
 *   http://localhost:8080/api/results
 *
 * (c) CiRA Robotics / KMITL 2026
 */

#include "cira.h"
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>

#ifdef _WIN32
#include <windows.h>
#define sleep(x) Sleep((x) * 1000)
#else
#include <unistd.h>
#endif

static volatile int running = 1;

static void signal_handler(int sig) {
    (void)sig;
    running = 0;
    printf("\nReceived signal, shutting down...\n");
}

int main(int argc, char* argv[]) {
    printf("CiRA Runtime - Streaming Server Test\n");
    printf("Version: %s\n\n", cira_version());

    int port = 8080;
    const char* model_path = NULL;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            /* Skip flags */
        } else if (model_path == NULL) {
            /* First non-flag arg is model path */
            model_path = argv[i];
        } else {
            /* Second non-flag arg is port */
            port = atoi(argv[i]);
            if (port <= 0 || port > 65535) {
                fprintf(stderr, "Invalid port: %s\n", argv[i]);
                return 1;
            }
        }
    }

    /* Setup signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Create context */
    printf("Creating context...\n");
    cira_ctx* ctx = cira_create();
    if (!ctx) {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }

    /* Load model if specified */
    if (model_path) {
        printf("Loading model from: %s\n", model_path);
        int result = cira_load(ctx, model_path);
        if (result != CIRA_OK) {
            fprintf(stderr, "Warning: Failed to load model: %d\n", result);
            const char* err = cira_error(ctx);
            if (err) fprintf(stderr, "Error: %s\n", err);
            printf("Continuing without model (no detections)\n");
        } else {
            printf("Model loaded successfully!\n");
        }
    } else {
        printf("No model specified. Usage: %s [model_path] [port]\n", argv[0]);
        printf("Continuing without model (no detections)\n");
    }

    /* Start HTTP server */
    printf("Starting HTTP server on port %d...\n", port);
    int result = cira_start_server(ctx, port);
    if (result != CIRA_OK) {
        fprintf(stderr, "Failed to start server: %d\n", result);
        const char* err = cira_error(ctx);
        if (err) fprintf(stderr, "Error: %s\n", err);
        cira_destroy(ctx);
        return 1;
    }

    /* Start camera capture (device 0) */
    printf("Starting camera on device 0...\n");
    result = cira_start_camera(ctx, 0);
    if (result != CIRA_OK) {
        fprintf(stderr, "Warning: Failed to start camera: %d\n", result);
        fprintf(stderr, "Streaming will not have video frames.\n");
    } else {
        printf("Camera started successfully!\n");
    }

    printf("\nServer running. Press Ctrl+C to stop.\n\n");
    printf("Endpoints:\n");
    printf("  Health:  http://localhost:%d/health\n", port);
    printf("  Results: http://localhost:%d/api/results\n", port);
    printf("  Snapshot: http://localhost:%d/snapshot\n", port);
    printf("  Stream:  http://localhost:%d/stream/annotated\n", port);
    printf("  Raw:     http://localhost:%d/stream/raw\n", port);
    printf("\n");

    /* Run until interrupted */
    while (running) {
        sleep(1);

        /* Print status periodically */
        float fps = cira_get_fps(ctx);
        if (fps > 0) {
            printf("FPS: %.1f\n", fps);
        }
    }

    /* Cleanup */
    printf("Stopping camera...\n");
    cira_stop_camera(ctx);
    printf("Stopping server...\n");
    cira_stop_server(ctx);
    cira_destroy(ctx);

    printf("Test completed!\n");
    return 0;
}
