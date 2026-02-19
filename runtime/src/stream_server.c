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
#include "cira_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

#ifdef CIRA_STREAMING_ENABLED

#include <microhttpd.h>
#include <pthread.h>

#ifdef _WIN32
#include <windows.h>
#define usleep(x) Sleep((x) / 1000)
#else
#include <unistd.h>
#endif

/* JPEG encoder functions (from jpeg_encoder.cpp) */
extern int jpeg_encode(const uint8_t* rgb_data, int width, int height,
                       int quality, uint8_t** out_data, size_t* out_size);
extern int jpeg_encode_annotated(cira_ctx* ctx, const uint8_t* rgb_data,
                                  int width, int height, int quality,
                                  uint8_t** out_data, size_t* out_size);

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

/* MJPEG streaming context */
typedef struct {
    cira_ctx* ctx;
    int annotated;          /* 1 for annotated, 0 for raw */
    int frame_sent;         /* Number of frames sent */
    int header_sent;        /* Boundary header sent for current frame */
    uint8_t* jpeg_data;     /* Current JPEG data */
    size_t jpeg_size;       /* Current JPEG size */
    size_t jpeg_offset;     /* Bytes sent from current frame */
} stream_ctx_t;

/* MJPEG stream callback for libmicrohttpd */
static ssize_t stream_callback(void* cls, uint64_t pos, char* buf, size_t max) {
    (void)pos;
    stream_ctx_t* sctx = (stream_ctx_t*)cls;

    if (!sctx || !sctx->ctx) {
        return MHD_CONTENT_READER_END_WITH_ERROR;
    }

    /* Check if server is still running */
    if (!g_server || !g_server->running) {
        return MHD_CONTENT_READER_END_OF_STREAM;
    }

    /* If we haven't sent a frame yet, or finished the current frame */
    if (sctx->jpeg_data == NULL || sctx->jpeg_offset >= sctx->jpeg_size) {
        /* Get new frame */
        int w, h;
        const uint8_t* frame = cira_get_frame(sctx->ctx, &w, &h);

        if (!frame || w <= 0 || h <= 0) {
            /* No frame available, wait a bit and return empty */
            usleep(10000);  /* 10ms */
            return 0;
        }

        /* Encode frame to JPEG */
        uint8_t* jpeg;
        size_t jpeg_size;
        int ret;

        if (sctx->annotated) {
            ret = jpeg_encode_annotated(sctx->ctx, frame, w, h, 80, &jpeg, &jpeg_size);
        } else {
            ret = jpeg_encode(frame, w, h, 80, &jpeg, &jpeg_size);
        }

        if (ret != CIRA_OK || !jpeg || jpeg_size == 0) {
            usleep(10000);
            return 0;
        }

        sctx->jpeg_data = jpeg;
        sctx->jpeg_size = jpeg_size;
        sctx->jpeg_offset = 0;
        sctx->header_sent = 0;
    }

    size_t written = 0;

    /* Send boundary header if not sent */
    if (!sctx->header_sent) {
        const char* boundary = "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ";
        char header[128];
        snprintf(header, sizeof(header), "%s%zu\r\n\r\n", boundary, sctx->jpeg_size);
        size_t hlen = strlen(header);

        if (hlen > max) {
            memcpy(buf, header, max);
            return (ssize_t)max;
        }

        memcpy(buf, header, hlen);
        written = hlen;
        sctx->header_sent = 1;
    }

    /* Send JPEG data */
    size_t remaining = sctx->jpeg_size - sctx->jpeg_offset;
    size_t space = max - written;
    size_t to_send = (remaining < space) ? remaining : space;

    if (to_send > 0) {
        memcpy(buf + written, sctx->jpeg_data + sctx->jpeg_offset, to_send);
        sctx->jpeg_offset += to_send;
        written += to_send;
    }

    /* Add trailing CRLF after frame */
    if (sctx->jpeg_offset >= sctx->jpeg_size && written + 2 <= max) {
        buf[written++] = '\r';
        buf[written++] = '\n';
        sctx->frame_sent++;
        sctx->jpeg_data = NULL;  /* Mark for next frame */
    }

    return (ssize_t)written;
}

/* Cleanup callback for stream context */
static void stream_free_callback(void* cls) {
    stream_ctx_t* sctx = (stream_ctx_t*)cls;
    if (sctx) {
        free(sctx);
    }
}

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

/* Helper: Get CPU usage percentage */
static float get_cpu_usage(void) {
#ifdef _WIN32
    return 0;  /* TODO: Implement for Windows */
#else
    static long prev_total = 0, prev_idle = 0;
    FILE* f = fopen("/proc/stat", "r");
    if (!f) return 0;

    char cpu[16];
    long user, nice, system, idle, iowait, irq, softirq;
    if (fscanf(f, "%s %ld %ld %ld %ld %ld %ld %ld",
               cpu, &user, &nice, &system, &idle, &iowait, &irq, &softirq) != 8) {
        fclose(f);
        return 0;
    }
    fclose(f);

    long total = user + nice + system + idle + iowait + irq + softirq;
    long diff_total = total - prev_total;
    long diff_idle = idle - prev_idle;

    float usage = diff_total > 0 ? 100.0f * (1.0f - (float)diff_idle / diff_total) : 0;

    prev_total = total;
    prev_idle = idle;

    return usage;
#endif
}

/* Helper: Get memory usage percentage */
static float get_memory_usage(void) {
#ifdef _WIN32
    return 0;  /* TODO: Implement for Windows */
#else
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return 0;

    long mem_total = 0, mem_available = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "MemTotal:", 9) == 0) {
            sscanf(line + 9, "%ld", &mem_total);
        } else if (strncmp(line, "MemAvailable:", 13) == 0) {
            sscanf(line + 13, "%ld", &mem_available);
        }
    }
    fclose(f);

    if (mem_total > 0) {
        return 100.0f * (1.0f - (float)mem_available / mem_total);
    }
    return 0;
#endif
}

/**
 * Handle /health endpoint.
 * Returns fields compatible with CiRA Edge dashboard.
 */
static int handle_health(struct MHD_Connection* conn, cira_ctx* ctx) {
    char response[MAX_RESPONSE_SIZE];
    char timestamp[32];
    get_timestamp(timestamp, sizeof(timestamp));

    /* Calculate defects per hour */
    long uptime = get_uptime();
    float defects_per_hour = 0;
    if (uptime > 0) {
        defects_per_hour = (float)ctx->total_detections * 3600.0f / uptime;
    }

    /* Get model name - use format name or "unknown" */
    const char* model_name = "unknown";
    if (ctx->format == CIRA_FORMAT_ONNX) {
        model_name = "ONNX";
    } else if (ctx->format == CIRA_FORMAT_DARKNET) {
        model_name = "Darknet";
    } else if (ctx->format == CIRA_FORMAT_NCNN) {
        model_name = "NCNN";
    } else if (ctx->format == CIRA_FORMAT_TENSORRT) {
        model_name = "TensorRT";
    }

    snprintf(response, sizeof(response),
        "{"
        "\"status\":\"ok\","
        "\"version\":\"%s\","
        "\"uptime\":%ld,"
        "\"timestamp\":\"%s\","
        "\"fps\":%.1f,"
        "\"temperature\":%.1f,"
        "\"cpu_usage\":%.1f,"
        "\"memory_usage\":%.1f,"
        "\"model_loaded\":%s,"
        "\"model_name\":\"%s\","
        "\"camera_running\":%s,"
        "\"detections\":%d,"
        "\"defects_total\":%llu,"
        "\"defects_per_hour\":%.1f"
        "}",
        cira_version(),
        uptime,
        timestamp,
        cira_get_fps(ctx),
        get_temperature(),
        get_cpu_usage(),
        get_memory_usage(),
        cira_status(ctx) == CIRA_STATUS_READY ? "true" : "false",
        model_name,
        g_server && g_server->ctx ? "true" : "false",
        cira_result_count(ctx),
        (unsigned long long)ctx->total_detections,
        defects_per_hour
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
 * Handle /api/stats endpoint - cumulative statistics since startup.
 */
static int handle_stats(struct MHD_Connection* conn, cira_ctx* ctx) {
    char response[MAX_RESPONSE_SIZE];
    char timestamp[32];
    get_timestamp(timestamp, sizeof(timestamp));

    /* Calculate uptime in seconds */
    time_t now = time(NULL);
    long uptime_sec = (long)(now - ctx->start_time);

    /* Build by_label JSON object */
    char by_label[8192] = "{";
    char* p = by_label + 1;
    char* end = by_label + sizeof(by_label) - 2;
    int first = 1;

    for (int i = 0; i < ctx->num_labels && p < end - 128; i++) {
        if (ctx->detections_by_label[i] > 0) {
            if (!first) {
                *p++ = ',';
            }
            p += snprintf(p, end - p, "\"%s\":%llu",
                         ctx->labels[i],
                         (unsigned long long)ctx->detections_by_label[i]);
            first = 0;
        }
    }
    *p++ = '}';
    *p = '\0';

    /* Build full response */
    snprintf(response, sizeof(response),
        "{"
        "\"total_detections\":%llu,"
        "\"total_frames\":%llu,"
        "\"by_label\":%s,"
        "\"fps\":%.1f,"
        "\"uptime_sec\":%ld,"
        "\"timestamp\":\"%s\","
        "\"model_loaded\":%s"
        "}",
        (unsigned long long)ctx->total_detections,
        (unsigned long long)ctx->total_frames,
        by_label,
        cira_get_fps(ctx),
        uptime_sec,
        timestamp,
        ctx->format != CIRA_FORMAT_UNKNOWN ? "true" : "false"
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
 * Handle /snapshot endpoint.
 */
static int handle_snapshot(struct MHD_Connection* conn, cira_ctx* ctx) {
    /* Get latest frame */
    int w, h;
    const uint8_t* frame = cira_get_frame(ctx, &w, &h);

    if (!frame || w <= 0 || h <= 0) {
        const char* error = "{\"error\":\"No frame available\"}";
        struct MHD_Response* response = MHD_create_response_from_buffer(
            strlen(error), (void*)error, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(response, "Content-Type", CT_JSON);
        MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
        int ret = MHD_queue_response(conn, MHD_HTTP_SERVICE_UNAVAILABLE, response);
        MHD_destroy_response(response);
        return ret;
    }

    /* Encode to JPEG with annotations */
    uint8_t* jpeg;
    size_t jpeg_size;
    int enc_ret = jpeg_encode_annotated(ctx, frame, w, h, 90, &jpeg, &jpeg_size);

    if (enc_ret != CIRA_OK || !jpeg || jpeg_size == 0) {
        const char* error = "{\"error\":\"JPEG encoding failed\"}";
        struct MHD_Response* response = MHD_create_response_from_buffer(
            strlen(error), (void*)error, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(response, "Content-Type", CT_JSON);
        int ret = MHD_queue_response(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
        MHD_destroy_response(response);
        return ret;
    }

    /* Return JPEG image */
    struct MHD_Response* response = MHD_create_response_from_buffer(
        jpeg_size, jpeg, MHD_RESPMEM_MUST_COPY);

    MHD_add_response_header(response, "Content-Type", CT_JPEG);
    MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
    MHD_add_response_header(response, "Cache-Control", "no-cache, no-store");

    int ret = MHD_queue_response(conn, MHD_HTTP_OK, response);
    MHD_destroy_response(response);

    return ret;
}

/**
 * Handle /stream/annotated endpoint (MJPEG).
 */
static int handle_stream(struct MHD_Connection* conn, cira_ctx* ctx, int annotated) {
    /* Allocate streaming context */
    stream_ctx_t* sctx = (stream_ctx_t*)calloc(1, sizeof(stream_ctx_t));
    if (!sctx) {
        const char* error = "{\"error\":\"Memory allocation failed\"}";
        struct MHD_Response* response = MHD_create_response_from_buffer(
            strlen(error), (void*)error, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(response, "Content-Type", CT_JSON);
        int ret = MHD_queue_response(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
        MHD_destroy_response(response);
        return ret;
    }

    sctx->ctx = ctx;
    sctx->annotated = annotated;
    sctx->frame_sent = 0;
    sctx->header_sent = 0;
    sctx->jpeg_data = NULL;
    sctx->jpeg_size = 0;
    sctx->jpeg_offset = 0;

    /* Create streaming response using callback */
    struct MHD_Response* response = MHD_create_response_from_callback(
        MHD_SIZE_UNKNOWN,           /* Unknown total size (streaming) */
        32768,                      /* Block size */
        stream_callback,            /* Callback function */
        sctx,                       /* Callback context */
        stream_free_callback        /* Free callback */
    );

    if (!response) {
        free(sctx);
        const char* error = "{\"error\":\"Failed to create response\"}";
        struct MHD_Response* err_response = MHD_create_response_from_buffer(
            strlen(error), (void*)error, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(err_response, "Content-Type", CT_JSON);
        int ret = MHD_queue_response(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, err_response);
        MHD_destroy_response(err_response);
        return ret;
    }

    /* Set MJPEG headers */
    MHD_add_response_header(response, "Content-Type", CT_MJPEG);
    MHD_add_response_header(response, "Cache-Control", "no-cache, no-store, must-revalidate");
    MHD_add_response_header(response, "Pragma", "no-cache");
    MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
    MHD_add_response_header(response, "Connection", "close");

    int ret = MHD_queue_response(conn, MHD_HTTP_OK, response);
    MHD_destroy_response(response);

    return ret;
}

/* HTML template for web UI - stored as static buffer */
static char g_html_template[8192];
static int g_html_initialized = 0;

static void init_html_template(void) {
    if (g_html_initialized) return;
    snprintf(g_html_template, sizeof(g_html_template),
        "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>CiRA Runtime</title><style>"
        "*{box-sizing:border-box;margin:0;padding:0}"
        "body{font-family:system-ui;background:#1a1a2e;color:#eee;min-height:100vh}"
        ".hdr{background:#16213e;padding:1rem 2rem;display:flex;justify-content:space-between}"
        ".hdr h1{font-size:1.5rem;color:#0df}.st{display:flex;gap:1rem;align-items:center}"
        ".dot{width:12px;height:12px;border-radius:50%%;background:#4ade80}"
        ".dot.off{background:#f87171}.cnt{display:flex;gap:1rem;padding:1rem;max-width:1400px;margin:0 auto}"
        ".vp{flex:2}.sp{flex:1;display:flex;flex-direction:column;gap:1rem}"
        ".cd{background:#16213e;border-radius:8px;padding:1rem}"
        ".cd h2{font-size:1rem;color:#0df;margin-bottom:.5rem}"
        ".vc{background:#000;border-radius:8px;overflow:hidden;aspect-ratio:4/3}"
        ".vc img{width:100%%;height:100%%;object-fit:contain}"
        ".sg{display:grid;grid-template-columns:1fr 1fr;gap:.5rem}"
        ".s{background:#1a1a2e;padding:.75rem;border-radius:4px;text-align:center}"
        ".sv{font-size:1.5rem;font-weight:bold;color:#0df}.sl{font-size:.75rem;color:#888}"
        ".dl{max-height:300px;overflow-y:auto}"
        ".di{display:flex;justify-content:space-between;padding:.5rem;background:#1a1a2e;"
        "margin-bottom:.25rem;border-radius:4px}.lb{color:#4ade80}.cf{color:#fbbf24}"
        "</style></head><body>"
        "<div class=\"hdr\"><h1>CiRA Runtime</h1><div class=\"st\">"
        "<span id=\"fps\">-- FPS</span><div class=\"dot\" id=\"dot\"></div></div></div>"
        "<div class=\"cnt\"><div class=\"vp\"><div class=\"cd\"><h2>Live Stream</h2>"
        "<div class=\"vc\"><img id=\"vid\" src=\"/stream/annotated\"></div></div></div>"
        "<div class=\"sp\"><div class=\"cd\"><h2>Stats</h2><div class=\"sg\">"
        "<div class=\"s\"><div class=\"sv\" id=\"dc\">0</div><div class=\"sl\">Detections</div></div>"
        "<div class=\"s\"><div class=\"sv\" id=\"fv\">0</div><div class=\"sl\">FPS</div></div>"
        "<div class=\"s\"><div class=\"sv\" id=\"td\">0</div><div class=\"sl\">Total</div></div>"
        "<div class=\"s\"><div class=\"sv\" id=\"ut\">0s</div><div class=\"sl\">Uptime</div></div>"
        "</div></div><div class=\"cd\"><h2>Model</h2>"
        "<p>Status: <span id=\"ms\">-</span></p></div>"
        "<div class=\"cd\"><h2>Detections</h2><div class=\"dl\" id=\"det\"></div></div>"
        "</div></div><script>"
        "async function u(){try{const[r,s]=await Promise.all(["
        "fetch('/api/results').then(x=>x.json()),"
        "fetch('/api/stats').then(x=>x.json())]);"
        "document.getElementById('dc').textContent=r.count||0;"
        "document.getElementById('fv').textContent=s.fps?s.fps.toFixed(1):'0';"
        "document.getElementById('fps').textContent=(s.fps?s.fps.toFixed(1):'0')+' FPS';"
        "document.getElementById('td').textContent=s.total_detections||0;"
        "document.getElementById('ut').textContent=s.uptime_sec+'s';"
        "document.getElementById('dot').className='dot'+(s.model_loaded?'':' off');"
        "document.getElementById('ms').textContent=s.model_loaded?'Loaded':'Not loaded';"
        "var l=document.getElementById('det');"
        "if(r.detections&&r.detections.length>0){"
        "l.innerHTML=r.detections.slice(0,10).map(d=>"
        "'<div class=\"di\"><span class=\"lb\">'+d.label+'</span>'+"
        "'<span class=\"cf\">'+(d.confidence*100).toFixed(1)+'%%</span></div>').join('');"
        "}else{l.innerHTML='<p style=\"color:#666;text-align:center\">No detections</p>';}}"
        "catch(e){}}setInterval(u,500);u();</script></body></html>"
    );
    g_html_initialized = 1;
}

/**
 * Handle / endpoint - Web UI.
 */
static int handle_index(struct MHD_Connection* conn, cira_ctx* ctx) {
    (void)ctx;
    init_html_template();

    struct MHD_Response* response = MHD_create_response_from_buffer(
        strlen(g_html_template), g_html_template, MHD_RESPMEM_PERSISTENT);

    MHD_add_response_header(response, "Content-Type", "text/html; charset=utf-8");
    MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");

    int ret = MHD_queue_response(conn, MHD_HTTP_OK, response);
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
    if (strcmp(url, "/") == 0 || strcmp(url, "/index.html") == 0) {
        return handle_index(conn, ctx);
    }
    if (strcmp(url, "/health") == 0) {
        return handle_health(conn, ctx);
    }
    if (strcmp(url, "/api/results") == 0) {
        return handle_results(conn, ctx);
    }
    if (strcmp(url, "/api/stats") == 0) {
        return handle_stats(conn, ctx);
    }
    if (strcmp(url, "/snapshot") == 0) {
        return handle_snapshot(conn, ctx);
    }
    if (strcmp(url, "/stream/annotated") == 0 || strcmp(url, "/stream") == 0) {
        return handle_stream(conn, ctx, 1);  /* Annotated */
    }
    if (strcmp(url, "/stream/raw") == 0) {
        return handle_stream(conn, ctx, 0);  /* Raw */
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
    fprintf(stderr, "  Web UI:    http://localhost:%d/\n", port);
    fprintf(stderr, "  Health:    http://localhost:%d/health\n", port);
    fprintf(stderr, "  Snapshot:  http://localhost:%d/snapshot\n", port);
    fprintf(stderr, "  Stream:    http://localhost:%d/stream/annotated\n", port);
    fprintf(stderr, "  Results:   http://localhost:%d/api/results\n", port);
    fprintf(stderr, "  Stats:     http://localhost:%d/api/stats\n", port);

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
