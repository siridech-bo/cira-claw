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
#include <unistd.h>

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
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Invalid port: %s\n", argv[1]);
            return 1;
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

    printf("\nServer running. Press Ctrl+C to stop.\n\n");
    printf("Endpoints:\n");
    printf("  Health:  http://localhost:%d/health\n", port);
    printf("  Results: http://localhost:%d/api/results\n", port);
    printf("  Stream:  http://localhost:%d/stream/annotated\n", port);
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
    printf("Stopping server...\n");
    cira_stop_server(ctx);
    cira_destroy(ctx);

    printf("Test completed!\n");
    return 0;
}
