/**
 * CiRA Runtime - HTTP Streaming Server
 *
 * This file implements an HTTP server using libmicrohttpd that provides:
 * - GET /health - JSON status endpoint
 * - GET /snapshot - Single JPEG image
 * - GET /stream/raw - Raw MJPEG stream
 * - GET /stream/annotated - MJPEG stream with annotations
 * - GET /api/results - Latest inference results as JSON
 *
 * (c) CiRA Robotics / KMITL 2026
 */

#include "cira.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#ifdef CIRA_STREAMING_ENABLED

#include <microhttpd.h>
#include <pthread.h>

/* Content types */
#define CT_JSON "application/json"
#define CT_JPEG "image/jpeg"
#define CT_MJPEG "multipart/x-mixed-replace; boundary=frame"
#define CT_TEXT "text/plain"

/* MJPEG boundary */
#define MJPEG_BOUNDARY "--frame\r\nContent-Type: image/jpeg\r\n\r\n"

/* Maximum response buffer size */
#define MAX_RESPONSE_SIZE 65536

/* Server state */
typedef struct {
    struct MHD_Daemon* daemon;
    cira_ctx* ctx;
    int port;
    int running;
} server_state_t;

/* Global server state (one per context) */
static server_state_t* g_server = NULL;

/* Helper: Get current timestamp as string */
static void get_timestamp(char* buf, size_t size) {
    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    strftime(buf, size, "%Y-%m-%dT%H:%M:%S", tm);
}

/* Helper: Get system uptime in seconds */
static long get_uptime(void) {
    FILE* f = fopen("/proc/uptime", "r");
    if (!f) return 0;

    double uptime;
    if (fscanf(f, "%lf", &uptime) != 1) uptime = 0;
    fclose(f);

    return (long)uptime;
}

/* Helper: Get CPU temperature (Linux/Jetson) */
static float get_temperature(void) {
    FILE* f = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!f) return 0;

    int temp;
    if (fscanf(f, "%d", &temp) != 1) temp = 0;
    fclose(f);

    return temp / 1000.0f;  /* Convert from millidegrees */
}

/**
 * Handle /health endpoint.
 */
static int handle_health(struct MHD_Connection* conn, cira_ctx* ctx) {
    char response[MAX_RESPONSE_SIZE];
    char timestamp[32];
    get_timestamp(timestamp, sizeof(timestamp));

    snprintf(response, sizeof(response),
        "{"
        "\"status\":\"ok\","
        "\"version\":\"%s\","
        "\"uptime\":%ld,"
        "\"timestamp\":\"%s\","
        "\"fps\":%.1f,"
        "\"temperature\":%.1f,"
        "\"model_loaded\":%s,"
        "\"camera_running\":%s,"
        "\"detections\":%d"
        "}",
        cira_version(),
        get_uptime(),
        timestamp,
        cira_get_fps(ctx),
        get_temperature(),
        cira_status(ctx) == CIRA_STATUS_READY ? "true" : "false",
        "false",  /* TODO: Check ctx->camera_running */
        cira_result_count(ctx)
    );

    struct MHD_Response* mhd_response = MHD_create_response_from_buffer(
        strlen(response), response, MHD_RESPMEM_MUST_COPY);

    MHD_add_response_header(mhd_response, "Content-Type", CT_JSON);
    MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");

    int ret = MHD_queue_response(conn, MHD_HTTP_OK, mhd_response);
    MHD_destroy_response(mhd_response);

    return ret;
}

/**
 * Handle /api/results endpoint.
 */
static int handle_results(struct MHD_Connection* conn, cira_ctx* ctx) {
    const char* json = cira_result_json(ctx);
    size_t len = strlen(json);

    struct MHD_Response* response = MHD_create_response_from_buffer(
        len, (void*)json, MHD_RESPMEM_MUST_COPY);

    MHD_add_response_header(response, "Content-Type", CT_JSON);
    MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");

    int ret = MHD_queue_response(conn, MHD_HTTP_OK, response);
    MHD_destroy_response(response);

    return ret;
}

/**
 * Handle /snapshot endpoint.
 */
static int handle_snapshot(struct MHD_Connection* conn, cira_ctx* ctx) {
    (void)ctx;

    /* TODO: Get frame from ctx and encode as JPEG */
    /* For now, return a placeholder */

    const char* error = "{\"error\":\"No frame available\"}";

    struct MHD_Response* response = MHD_create_response_from_buffer(
        strlen(error), (void*)error, MHD_RESPMEM_MUST_COPY);

    MHD_add_response_header(response, "Content-Type", CT_JSON);
    MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");

    int ret = MHD_queue_response(conn, MHD_HTTP_SERVICE_UNAVAILABLE, response);
    MHD_destroy_response(response);

    return ret;
}

/**
 * Handle /stream/annotated endpoint (MJPEG).
 */
static int handle_stream(struct MHD_Connection* conn, cira_ctx* ctx) {
    (void)ctx;

    /* TODO: Implement MJPEG streaming */
    /*
     * MJPEG streaming works by:
     * 1. Sending Content-Type: multipart/x-mixed-replace; boundary=frame
     * 2. For each frame:
     *    - Send: --frame\r\n
     *    - Send: Content-Type: image/jpeg\r\n\r\n
     *    - Send: <JPEG data>
     *    - Send: \r\n
     * 3. Repeat until client disconnects
     *
     * MHD needs a callback-based approach for streaming.
     */

    const char* error = "{\"error\":\"Streaming not implemented\"}";

    struct MHD_Response* response = MHD_create_response_from_buffer(
        strlen(error), (void*)error, MHD_RESPMEM_MUST_COPY);

    MHD_add_response_header(response, "Content-Type", CT_JSON);

    int ret = MHD_queue_response(conn, MHD_HTTP_NOT_IMPLEMENTED, response);
    MHD_destroy_response(response);

    return ret;
}

/**
 * Handle 404 Not Found.
 */
static int handle_not_found(struct MHD_Connection* conn) {
    const char* error = "{\"error\":\"Not found\"}";

    struct MHD_Response* response = MHD_create_response_from_buffer(
        strlen(error), (void*)error, MHD_RESPMEM_MUST_COPY);

    MHD_add_response_header(response, "Content-Type", CT_JSON);

    int ret = MHD_queue_response(conn, MHD_HTTP_NOT_FOUND, response);
    MHD_destroy_response(response);

    return ret;
}

/**
 * Main request handler callback for MHD.
 */
static enum MHD_Result request_handler(
    void* cls,
    struct MHD_Connection* conn,
    const char* url,
    const char* method,
    const char* version,
    const char* upload_data,
    size_t* upload_data_size,
    void** con_cls
) {
    (void)version;
    (void)upload_data;
    (void)upload_data_size;
    (void)con_cls;

    cira_ctx* ctx = (cira_ctx*)cls;

    /* Only handle GET requests */
    if (strcmp(method, "GET") != 0) {
        return handle_not_found(conn);
    }

    /* Route requests */
    if (strcmp(url, "/health") == 0) {
        return handle_health(conn, ctx);
    }
    if (strcmp(url, "/api/results") == 0 || strcmp(url, "/api/stats") == 0) {
        return handle_results(conn, ctx);
    }
    if (strcmp(url, "/snapshot") == 0) {
        return handle_snapshot(conn, ctx);
    }
    if (strcmp(url, "/stream/annotated") == 0 ||
        strcmp(url, "/stream/raw") == 0 ||
        strcmp(url, "/stream") == 0) {
        return handle_stream(conn, ctx);
    }

    return handle_not_found(conn);
}

/**
 * Start HTTP streaming server.
 *
 * @param ctx Context handle
 * @param port HTTP port (e.g., 8080)
 * @return CIRA_OK on success
 */
int server_start(cira_ctx* ctx, int port) {
    if (!ctx) return CIRA_ERROR_INPUT;
    if (g_server && g_server->running) return CIRA_OK;

    /* Allocate server state */
    g_server = (server_state_t*)calloc(1, sizeof(server_state_t));
    if (!g_server) return CIRA_ERROR_MEMORY;

    g_server->ctx = ctx;
    g_server->port = port;

    /* Start MHD daemon */
    g_server->daemon = MHD_start_daemon(
        MHD_USE_SELECT_INTERNALLY | MHD_USE_THREAD_PER_CONNECTION,
        port,
        NULL, NULL,                         /* Accept policy */
        &request_handler, ctx,              /* Request handler */
        MHD_OPTION_END
    );

    if (!g_server->daemon) {
        fprintf(stderr, "Failed to start HTTP server on port %d\n", port);
        free(g_server);
        g_server = NULL;
        return CIRA_ERROR;
    }

    g_server->running = 1;

    fprintf(stderr, "HTTP server started on port %d\n", port);
    fprintf(stderr, "  Health:    http://localhost:%d/health\n", port);
    fprintf(stderr, "  Snapshot:  http://localhost:%d/snapshot\n", port);
    fprintf(stderr, "  Stream:    http://localhost:%d/stream/annotated\n", port);
    fprintf(stderr, "  Results:   http://localhost:%d/api/results\n", port);

    return CIRA_OK;
}

/**
 * Stop HTTP streaming server.
 */
int server_stop(cira_ctx* ctx) {
    (void)ctx;

    if (!g_server || !g_server->running) return CIRA_OK;

    MHD_stop_daemon(g_server->daemon);
    g_server->daemon = NULL;
    g_server->running = 0;

    fprintf(stderr, "HTTP server stopped\n");

    free(g_server);
    g_server = NULL;

    return CIRA_OK;
}

#else /* CIRA_STREAMING_ENABLED */

/* Stubs when streaming is not enabled */
int server_start(cira_ctx* ctx, int port) {
    (void)ctx;
    (void)port;
    fprintf(stderr, "Streaming not enabled in this build\n");
    return CIRA_ERROR;
}

int server_stop(cira_ctx* ctx) {
    (void)ctx;
    return CIRA_ERROR;
}

#endif /* CIRA_STREAMING_ENABLED */
