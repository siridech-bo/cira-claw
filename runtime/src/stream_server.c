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
#include <dirent.h>
#include <sys/stat.h>

#ifdef CIRA_STREAMING_ENABLED

#include <microhttpd.h>
#include <pthread.h>

#ifdef _WIN32
#include <windows.h>
#define usleep(x) Sleep((x) / 1000)
#define strcasecmp _stricmp
#else
#include <unistd.h>
#include <strings.h>
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

/* Models directory for hot-swapping */
static char g_models_dir[1024] = "";

/* Temp directory for frame files */
static char g_temp_dir[512] = "";

/**
 * Get cross-platform temp directory.
 */
static const char* get_temp_dir(void) {
    if (g_temp_dir[0] != '\0') return g_temp_dir;

#ifdef _WIN32
    /* Windows: use %TEMP% or %TMP% */
    const char* temp = getenv("TEMP");
    if (!temp) temp = getenv("TMP");
    if (!temp) temp = "C:\\Temp";
    strncpy(g_temp_dir, temp, sizeof(g_temp_dir) - 1);
#else
    /* Linux/macOS: use /tmp */
    strncpy(g_temp_dir, "/tmp", sizeof(g_temp_dir) - 1);
#endif

    return g_temp_dir;
}

/**
 * Write current frame to temp file atomically.
 * Uses write-to-temp + rename pattern for atomic updates.
 *
 * @param ctx Context with frame data
 * @param annotated 1 for annotated frame, 0 for raw
 * @return CIRA_OK on success
 */
int cira_write_frame_file(cira_ctx* ctx, int annotated) {
    if (!ctx) return CIRA_ERROR_INPUT;

    /* Get frame data */
    int w, h;
    const uint8_t* frame = cira_get_frame(ctx, &w, &h);
    if (!frame || w <= 0 || h <= 0) {
        return CIRA_ERROR;  /* No frame available */
    }

    /* Encode to JPEG */
    uint8_t* jpeg;
    size_t jpeg_size;
    int ret;

    if (annotated) {
        ret = jpeg_encode_annotated(ctx, frame, w, h, 85, &jpeg, &jpeg_size);
    } else {
        ret = jpeg_encode(frame, w, h, 85, &jpeg, &jpeg_size);
    }

    if (ret != CIRA_OK || !jpeg || jpeg_size == 0) {
        return CIRA_ERROR;
    }

    /* Build temp file path */
    char temp_path[512];
    char final_path[512];
    snprintf(temp_path, sizeof(temp_path), "%s/cira_frame_%p.tmp",
             get_temp_dir(), (void*)ctx);
    snprintf(final_path, sizeof(final_path), "%s/cira_frame_%p.jpg",
             get_temp_dir(), (void*)ctx);

    pthread_mutex_lock(&ctx->frame_file_mutex);

    /* Write to temp file */
    FILE* f = fopen(temp_path, "wb");
    if (!f) {
        pthread_mutex_unlock(&ctx->frame_file_mutex);
        return CIRA_ERROR_FILE;
    }

    size_t written = fwrite(jpeg, 1, jpeg_size, f);
    fclose(f);

    if (written != jpeg_size) {
        unlink(temp_path);
        pthread_mutex_unlock(&ctx->frame_file_mutex);
        return CIRA_ERROR_FILE;
    }

    /* Atomic rename (overwrites existing file) */
#ifdef _WIN32
    /* Windows: remove destination first, then rename */
    unlink(final_path);
#endif
    if (rename(temp_path, final_path) != 0) {
        unlink(temp_path);
        pthread_mutex_unlock(&ctx->frame_file_mutex);
        return CIRA_ERROR_FILE;
    }

    /* Update context with file path and sequence */
    strncpy(ctx->frame_file_path, final_path, sizeof(ctx->frame_file_path) - 1);
    ctx->frame_sequence++;

    pthread_mutex_unlock(&ctx->frame_file_mutex);

    return CIRA_OK;
}

/* Set models directory for model listing */
void server_set_models_dir(const char* dir) {
    if (dir) {
        strncpy(g_models_dir, dir, sizeof(g_models_dir) - 1);
    }
}

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
        /* Free previous frame data if any */
        if (sctx->jpeg_data) {
            free(sctx->jpeg_data);
            sctx->jpeg_data = NULL;
        }

        /* Get new frame */
        int w, h;
        const uint8_t* frame = cira_get_frame(sctx->ctx, &w, &h);

        if (!frame || w <= 0 || h <= 0) {
            /* No frame available, wait a bit and return empty */
            usleep(10000);  /* 10ms */
            return 0;
        }

        /* Encode frame to JPEG (returns pointer to shared internal buffer) */
        uint8_t* jpeg_ptr;
        size_t jpeg_size;
        int ret;

        if (sctx->annotated) {
            ret = jpeg_encode_annotated(sctx->ctx, frame, w, h, 80, &jpeg_ptr, &jpeg_size);
            /* Fallback to raw encoding if annotated fails */
            if (ret != CIRA_OK || !jpeg_ptr || jpeg_size == 0) {
                ret = jpeg_encode(frame, w, h, 80, &jpeg_ptr, &jpeg_size);
            }
        } else {
            ret = jpeg_encode(frame, w, h, 80, &jpeg_ptr, &jpeg_size);
        }

        if (ret != CIRA_OK || !jpeg_ptr || jpeg_size == 0) {
            usleep(10000);
            return 0;
        }

        /* Copy JPEG data to our own buffer (encoder uses shared buffer) */
        sctx->jpeg_data = (uint8_t*)malloc(jpeg_size);
        if (!sctx->jpeg_data) {
            usleep(10000);
            return 0;
        }
        memcpy(sctx->jpeg_data, jpeg_ptr, jpeg_size);
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
        /* Free current frame and mark for next frame */
        free(sctx->jpeg_data);
        sctx->jpeg_data = NULL;
    }

    return (ssize_t)written;
}

/* Cleanup callback for stream context */
static void stream_free_callback(void* cls) {
    stream_ctx_t* sctx = (stream_ctx_t*)cls;
    if (sctx) {
        /* Free the copied JPEG data if any */
        if (sctx->jpeg_data) {
            free(sctx->jpeg_data);
            sctx->jpeg_data = NULL;
        }
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

    /* Get model name */
    const char* model_name = "none";
    if (ctx->format == CIRA_FORMAT_ONNX) model_name = "ONNX";
    else if (ctx->format == CIRA_FORMAT_NCNN) model_name = "NCNN";
    else if (ctx->format == CIRA_FORMAT_DARKNET) model_name = "Darknet";
    else if (ctx->format == CIRA_FORMAT_TENSORRT) model_name = "TensorRT";

    /* Build full response */
    snprintf(response, sizeof(response),
        "{"
        "\"total_detections\":%llu,"
        "\"total_frames\":%llu,"
        "\"by_label\":%s,"
        "\"fps\":%.1f,"
        "\"uptime_sec\":%ld,"
        "\"timestamp\":\"%s\","
        "\"model_loaded\":%s,"
        "\"model_name\":\"%s\","
        "\"model_path\":\"%s\""
        "}",
        (unsigned long long)ctx->total_detections,
        (unsigned long long)ctx->total_frames,
        by_label,
        cira_get_fps(ctx),
        uptime_sec,
        timestamp,
        ctx->format != CIRA_FORMAT_UNKNOWN ? "true" : "false",
        model_name,
        ctx->model_path
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

/* HTML template for web UI - built incrementally to avoid overlength string warnings */
static char g_html_template[16384];
static int g_html_initialized = 0;

static void init_html_template(void) {
    if (g_html_initialized) return;

    char* p = g_html_template;
    char* end = g_html_template + sizeof(g_html_template);

    /* Part 1: Head and styles */
    p += snprintf(p, end - p,
        "<!DOCTYPE html><html><head><meta charset=\"UTF-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>CiRA Runtime</title><style>"
        "*{box-sizing:border-box;margin:0;padding:0}"
        "body{font-family:system-ui;background:#1a1a2e;color:#eee;min-height:100vh}"
        ".hdr{background:#16213e;padding:1rem 2rem;display:flex;justify-content:space-between}"
        ".hdr h1{font-size:1.5rem;color:#0df}.st{display:flex;gap:1rem;align-items:center}"
        ".dot{width:12px;height:12px;border-radius:50%%;background:#4ade80}"
        ".dot.off{background:#f87171}"
        ".cnt{display:flex;gap:1rem;padding:1rem;max-width:1400px;margin:0 auto}");

    /* Part 2: More styles */
    p += snprintf(p, end - p,
        ".vp{flex:2}.sp{flex:1;display:flex;flex-direction:column;gap:1rem}"
        ".cd{background:#16213e;border-radius:8px;padding:1rem}"
        ".cd h2{font-size:1rem;color:#0df;margin-bottom:.5rem}"
        ".vc{background:#000;border-radius:8px;overflow:hidden;aspect-ratio:4/3}"
        ".vc img{width:100%%;height:100%%;object-fit:contain}"
        ".sg{display:grid;grid-template-columns:1fr 1fr;gap:.5rem}"
        ".s{background:#1a1a2e;padding:.75rem;border-radius:4px;text-align:center}"
        ".sv{font-size:1.5rem;font-weight:bold;color:#0df}.sl{font-size:.75rem;color:#888}"
        ".dl{max-height:200px;overflow-y:auto}");

    /* Part 3: Form styles */
    p += snprintf(p, end - p,
        ".di{display:flex;justify-content:space-between;padding:.5rem;background:#1a1a2e;"
        "margin-bottom:.25rem;border-radius:4px}.lb{color:#4ade80}.cf{color:#fbbf24}"
        "select{width:100%%;padding:.5rem;background:#1a1a2e;color:#eee;border:1px solid #333;"
        "border-radius:4px;font-size:.9rem;margin-bottom:.5rem}"
        "select:focus{outline:none;border-color:#0df}"
        "button{padding:.5rem 1rem;background:#0df;color:#000;border:none;border-radius:4px;"
        "cursor:pointer;font-weight:bold;width:100%%}button:hover{background:#0be}"
        "button:disabled{background:#555;cursor:not-allowed}");

    /* Part 4: Input and message styles */
    p += snprintf(p, end - p,
        ".mi{display:flex;gap:.5rem;margin-bottom:.5rem}"
        ".mi input{flex:1;padding:.5rem;background:#1a1a2e;color:#eee;border:1px solid #333;"
        "border-radius:4px;font-size:.9rem}.mi input:focus{outline:none;border-color:#0df}"
        ".msg{padding:.5rem;margin-top:.5rem;border-radius:4px;font-size:.85rem}"
        ".msg.ok{background:#166534;color:#4ade80}.msg.err{background:#7f1d1d;color:#f87171}"
        "</style></head><body>");

    /* Part 5: Header and main layout */
    p += snprintf(p, end - p,
        "<div class=\"hdr\"><h1>CiRA Runtime</h1><div class=\"st\">"
        "<span id=\"fps\">-- FPS</span><div class=\"dot\" id=\"dot\"></div></div></div>"
        "<div class=\"cnt\"><div class=\"vp\"><div class=\"cd\"><h2>Live Stream</h2>"
        "<div class=\"vc\"><img id=\"vid\" src=\"/stream/annotated\"></div></div></div>"
        "<div class=\"sp\"><div class=\"cd\"><h2>Stats</h2><div class=\"sg\">"
        "<div class=\"s\"><div class=\"sv\" id=\"dc\">0</div><div class=\"sl\">Detections</div></div>"
        "<div class=\"s\"><div class=\"sv\" id=\"fv\">0</div><div class=\"sl\">FPS</div></div>"
        "<div class=\"s\"><div class=\"sv\" id=\"td\">0</div><div class=\"sl\">Total</div></div>"
        "<div class=\"s\"><div class=\"sv\" id=\"ut\">0s</div><div class=\"sl\">Uptime</div></div>"
        "</div></div>");

    /* Part 6: Model selector panel */
    p += snprintf(p, end - p,
        "<div class=\"cd\"><h2>Model</h2>"
        "<p style=\"margin-bottom:.5rem\">Current: <span id=\"mn\">-</span></p>"
        "<select id=\"msel\"><option value=\"\">Select a model...</option></select>"
        "<div class=\"mi\"><input type=\"text\" id=\"mpath\" placeholder=\"Or enter model path...\"></div>"
        "<button id=\"mbtn\" onclick=\"loadModel()\">Load Model</button>"
        "<div id=\"mmsg\"></div></div>"
        "<div class=\"cd\"><h2>Detections</h2><div class=\"dl\" id=\"det\"></div></div>"
        "</div></div>");

    /* Part 7: JavaScript - loadModels function */
    p += snprintf(p, end - p,
        "<script>let models=[];"
        "async function loadModels(){"
        "try{const r=await fetch('/api/models').then(x=>x.json());"
        "models=r.models||[];const sel=document.getElementById('msel');"
        "sel.innerHTML='<option value=\"\">Select a model...</option>';"
        "models.forEach(m=>{"
        "const opt=document.createElement('option');opt.value=m.path;"
        "opt.textContent=m.name+(m.loaded?' (current)':'');sel.appendChild(opt);});"
        "}catch(e){console.error(e);}}");

    /* Part 8: JavaScript - loadModel function */
    p += snprintf(p, end - p,
        "async function loadModel(){"
        "const sel=document.getElementById('msel');const inp=document.getElementById('mpath');"
        "const path=inp.value.trim()||sel.value;if(!path){alert('Select or enter a model path');return;}"
        "const btn=document.getElementById('mbtn');const msg=document.getElementById('mmsg');"
        "btn.disabled=true;btn.textContent='Loading...';"
        "try{const r=await fetch('/api/model',{method:'POST',headers:{'Content-Type':'application/json'},"
        "body:JSON.stringify({path:path})}).then(x=>x.json());"
        "if(r.success){msg.className='msg ok';msg.textContent='Loaded: '+r.format;loadModels();}"
        "else{msg.className='msg err';msg.textContent=r.error||'Failed';}"
        "}catch(e){msg.className='msg err';msg.textContent='Error: '+e.message;}"
        "btn.disabled=false;btn.textContent='Load Model';setTimeout(()=>msg.textContent='',5000);}");

    /* Part 9: JavaScript - update function */
    p += snprintf(p, end - p,
        "async function u(){try{const[r,s]=await Promise.all(["
        "fetch('/api/results').then(x=>x.json()),"
        "fetch('/api/stats').then(x=>x.json())]);"
        "document.getElementById('dc').textContent=r.count||0;"
        "document.getElementById('fv').textContent=s.fps?s.fps.toFixed(1):'0';"
        "document.getElementById('fps').textContent=(s.fps?s.fps.toFixed(1):'0')+' FPS';"
        "document.getElementById('td').textContent=s.total_detections||0;"
        "document.getElementById('ut').textContent=s.uptime_sec+'s';"
        "document.getElementById('dot').className='dot'+(s.model_loaded?'':' off');"
        "document.getElementById('mn').textContent=s.model_loaded?(s.model_name||'Loaded'):'Not loaded';");

    /* Part 10: JavaScript - detection list and init */
    p += snprintf(p, end - p,
        "var l=document.getElementById('det');"
        "if(r.detections&&r.detections.length>0){"
        "l.innerHTML=r.detections.slice(0,10).map(d=>"
        "'<div class=\"di\"><span class=\"lb\">'+d.label+'</span>'+"
        "'<span class=\"cf\">'+(d.confidence*100).toFixed(1)+'%%</span></div>').join('');"
        "}else{l.innerHTML='<p style=\"color:#666;text-align:center\">No detections</p>';}}"
        "catch(e){}}"
        "loadModels();setInterval(u,500);u();</script></body></html>");

    g_html_initialized = 1;
}

/**
 * Handle GET /api/models - List available models.
 */
static int handle_models_list(struct MHD_Connection* conn, cira_ctx* ctx) {
    (void)ctx;
    char response[MAX_RESPONSE_SIZE];
    char* p = response;
    char* end = response + sizeof(response) - 256;

    p += snprintf(p, end - p, "{\"models\":[");

    int count = 0;

    /* If models directory is set, scan it */
    if (g_models_dir[0] != '\0') {
        DIR* d = opendir(g_models_dir);
        if (d) {
            struct dirent* entry;
            while ((entry = readdir(d)) != NULL && p < end) {
                if (entry->d_name[0] == '.') continue;

                /* Check if it's a directory (potential model folder) */
                char full_path[2048];
                snprintf(full_path, sizeof(full_path), "%s/%s", g_models_dir, entry->d_name);

                struct stat st;
                if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                    /* Check for model files inside */
                    int has_onnx = 0, has_ncnn = 0;
                    DIR* model_d = opendir(full_path);
                    if (model_d) {
                        struct dirent* model_entry;
                        while ((model_entry = readdir(model_d)) != NULL) {
                            const char* name = model_entry->d_name;
                            size_t len = strlen(name);
                            if (len > 5 && strcmp(name + len - 5, ".onnx") == 0) has_onnx = 1;
                            if (len > 6 && strcmp(name + len - 6, ".param") == 0) has_ncnn = 1;
                        }
                        closedir(model_d);
                    }

                    if (has_onnx || has_ncnn) {
                        if (count > 0) p += snprintf(p, end - p, ",");
                        p += snprintf(p, end - p, "{\"name\":\"%s\",\"path\":\"%s\",\"type\":\"%s\"}",
                                     entry->d_name, full_path, has_onnx ? "onnx" : "ncnn");
                        count++;
                    }
                }
            }
            closedir(d);
        }
    }

    /* Also add currently loaded model path if any */
    if (ctx->model_path[0] != '\0') {
        /* Check if already in list */
        int already_listed = (strstr(response, ctx->model_path) != NULL);
        if (!already_listed && p < end) {
            if (count > 0) p += snprintf(p, end - p, ",");
            const char* type = "unknown";
            if (ctx->format == CIRA_FORMAT_ONNX) type = "onnx";
            else if (ctx->format == CIRA_FORMAT_NCNN) type = "ncnn";
            p += snprintf(p, end - p, "{\"name\":\"current\",\"path\":\"%s\",\"type\":\"%s\",\"loaded\":true}",
                         ctx->model_path, type);
            count++;
        }
    }

    p += snprintf(p, end - p, "],\"count\":%d,\"models_dir\":\"%s\"}", count, g_models_dir);

    struct MHD_Response* mhd_response = MHD_create_response_from_buffer(
        strlen(response), response, MHD_RESPMEM_MUST_COPY);

    MHD_add_response_header(mhd_response, "Content-Type", CT_JSON);
    MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");

    int ret = MHD_queue_response(conn, MHD_HTTP_OK, mhd_response);
    MHD_destroy_response(mhd_response);

    return ret;
}

/**
 * Handle POST /api/model - Load a new model.
 */
static int handle_model_load(struct MHD_Connection* conn, cira_ctx* ctx,
                             const char* upload_data, size_t upload_size) {
    char response[2048];

    /* Parse model path from POST data (simple JSON: {"path":"..."}) */
    char model_path[512] = "";

    if (upload_data && upload_size > 0) {
        /* Find "path" in JSON */
        const char* path_key = strstr(upload_data, "\"path\"");
        if (path_key) {
            path_key = strchr(path_key + 6, '"');
            if (path_key) {
                path_key++;
                const char* path_end = strchr(path_key, '"');
                if (path_end) {
                    size_t len = path_end - path_key;
                    if (len >= sizeof(model_path)) len = sizeof(model_path) - 1;
                    memcpy(model_path, path_key, len);
                    model_path[len] = '\0';
                }
            }
        }
    }

    if (model_path[0] == '\0') {
        snprintf(response, sizeof(response),
                "{\"success\":false,\"error\":\"Missing model path\"}");

        struct MHD_Response* mhd_response = MHD_create_response_from_buffer(
            strlen(response), response, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(mhd_response, "Content-Type", CT_JSON);
        MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");
        int ret = MHD_queue_response(conn, MHD_HTTP_BAD_REQUEST, mhd_response);
        MHD_destroy_response(mhd_response);
        return ret;
    }

    fprintf(stderr, "Loading model: %s\n", model_path);

    /* Load the new model */
    int result = cira_load(ctx, model_path);

    if (result == CIRA_OK) {
        const char* fmt = ctx->format == CIRA_FORMAT_ONNX ? "onnx" :
                          ctx->format == CIRA_FORMAT_NCNN ? "ncnn" : "unknown";
        snprintf(response, sizeof(response),
                "{\"success\":true,\"model\":\"%.500s\",\"format\":\"%s\"}",
                model_path, fmt);
    } else {
        const char* err = cira_error(ctx);
        snprintf(response, sizeof(response),
                "{\"success\":false,\"error\":\"%.500s\"}",
                err ? err : "Failed to load model");
    }

    struct MHD_Response* mhd_response = MHD_create_response_from_buffer(
        strlen(response), response, MHD_RESPMEM_MUST_COPY);

    MHD_add_response_header(mhd_response, "Content-Type", CT_JSON);
    MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");

    int ret = MHD_queue_response(conn, result == CIRA_OK ? MHD_HTTP_OK : MHD_HTTP_BAD_REQUEST, mhd_response);
    MHD_destroy_response(mhd_response);

    return ret;
}

/**
 * Handle /frame/latest endpoint - serve latest frame from file.
 * This is a file-based alternative to MJPEG streaming for better cross-platform stability.
 */
static int handle_frame_latest(struct MHD_Connection* conn, cira_ctx* ctx) {
    pthread_mutex_lock(&ctx->frame_file_mutex);

    /* Check if we have a frame file */
    if (ctx->frame_file_path[0] == '\0') {
        pthread_mutex_unlock(&ctx->frame_file_mutex);

        /* No frame file yet - try to generate one */
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

        /* Generate frame file */
        if (cira_write_frame_file(ctx, 1) != CIRA_OK) {
            const char* error = "{\"error\":\"Failed to generate frame\"}";
            struct MHD_Response* response = MHD_create_response_from_buffer(
                strlen(error), (void*)error, MHD_RESPMEM_MUST_COPY);
            MHD_add_response_header(response, "Content-Type", CT_JSON);
            MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
            int ret = MHD_queue_response(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
            MHD_destroy_response(response);
            return ret;
        }

        pthread_mutex_lock(&ctx->frame_file_mutex);
    }

    /* Read frame file */
    FILE* f = fopen(ctx->frame_file_path, "rb");
    if (!f) {
        pthread_mutex_unlock(&ctx->frame_file_mutex);
        const char* error = "{\"error\":\"Frame file not found\"}";
        struct MHD_Response* response = MHD_create_response_from_buffer(
            strlen(error), (void*)error, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(response, "Content-Type", CT_JSON);
        MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
        int ret = MHD_queue_response(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
        MHD_destroy_response(response);
        return ret;
    }

    /* Get file size */
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 10 * 1024 * 1024) {  /* 10MB max */
        fclose(f);
        pthread_mutex_unlock(&ctx->frame_file_mutex);
        const char* error = "{\"error\":\"Invalid frame file\"}";
        struct MHD_Response* response = MHD_create_response_from_buffer(
            strlen(error), (void*)error, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(response, "Content-Type", CT_JSON);
        int ret = MHD_queue_response(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
        MHD_destroy_response(response);
        return ret;
    }

    /* Read file content */
    uint8_t* data = (uint8_t*)malloc(file_size);
    if (!data) {
        fclose(f);
        pthread_mutex_unlock(&ctx->frame_file_mutex);
        const char* error = "{\"error\":\"Memory allocation failed\"}";
        struct MHD_Response* response = MHD_create_response_from_buffer(
            strlen(error), (void*)error, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(response, "Content-Type", CT_JSON);
        int ret = MHD_queue_response(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
        MHD_destroy_response(response);
        return ret;
    }

    size_t read_size = fread(data, 1, file_size, f);
    fclose(f);

    uint64_t seq = ctx->frame_sequence;
    pthread_mutex_unlock(&ctx->frame_file_mutex);

    if (read_size != (size_t)file_size) {
        free(data);
        const char* error = "{\"error\":\"Failed to read frame file\"}";
        struct MHD_Response* response = MHD_create_response_from_buffer(
            strlen(error), (void*)error, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(response, "Content-Type", CT_JSON);
        int ret = MHD_queue_response(conn, MHD_HTTP_INTERNAL_SERVER_ERROR, response);
        MHD_destroy_response(response);
        return ret;
    }

    /* Return JPEG */
    struct MHD_Response* response = MHD_create_response_from_buffer(
        read_size, data, MHD_RESPMEM_MUST_FREE);

    MHD_add_response_header(response, "Content-Type", CT_JPEG);
    MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
    MHD_add_response_header(response, "Access-Control-Expose-Headers", "X-Frame-Sequence");
    MHD_add_response_header(response, "Cache-Control", "no-cache, no-store");

    /* Add sequence number header for client-side change detection */
    char seq_str[32];
    snprintf(seq_str, sizeof(seq_str), "%llu", (unsigned long long)seq);
    MHD_add_response_header(response, "X-Frame-Sequence", seq_str);

    int ret = MHD_queue_response(conn, MHD_HTTP_OK, response);
    MHD_destroy_response(response);

    return ret;
}

/**
 * Handle GET /api/cameras - enumerate available camera devices.
 */
static int handle_cameras_list(struct MHD_Connection* conn, cira_ctx* ctx) {
    char response[MAX_RESPONSE_SIZE];
    char* p = response;
    char* end = response + sizeof(response) - 256;

    p += snprintf(p, end - p, "{\"cameras\":[");

    int count = 0;

#ifdef _WIN32
    /* Windows: Check DirectShow devices via OpenCV device indices */
    /* We probe device indices 0-9 (OpenCV style) */
    for (int i = 0; i < 10 && p < end - 128; i++) {
        char dev_path[64];
        snprintf(dev_path, sizeof(dev_path), "%d", i);
        /* On Windows we can't easily enumerate without opening, so just list indices */
        if (i < 4) {  /* List first 4 potential cameras */
            if (count > 0) p += snprintf(p, end - p, ",");
            p += snprintf(p, end - p, "{\"id\":%d,\"name\":\"Camera %d\",\"path\":\"%d\"}",
                         i, i, i);
            count++;
        }
    }
#else
    /* Linux: Enumerate /dev/video* devices */
    DIR* d = opendir("/dev");
    if (d) {
        struct dirent* entry;
        while ((entry = readdir(d)) != NULL && p < end - 128) {
            if (strncmp(entry->d_name, "video", 5) == 0) {
                int dev_num = atoi(entry->d_name + 5);
                char dev_path[64];
                snprintf(dev_path, sizeof(dev_path), "/dev/%s", entry->d_name);

                /* Check if device is readable */
                struct stat st;
                if (stat(dev_path, &st) == 0 && S_ISCHR(st.st_mode)) {
                    if (count > 0) p += snprintf(p, end - p, ",");
                    p += snprintf(p, end - p, "{\"id\":%d,\"name\":\"%s\",\"path\":\"%s\"}",
                                 dev_num, entry->d_name, dev_path);
                    count++;
                }
            }
        }
        closedir(d);
    }
#endif

    /* Add info about current camera */
    int current_camera = -1;
    int camera_running = 0;
    if (ctx) {
        camera_running = ctx->camera_running;
        current_camera = ctx->current_camera;
    }

    p += snprintf(p, end - p, "],\"count\":%d,\"current\":%d,\"running\":%s}",
                 count, current_camera, camera_running ? "true" : "false");

    struct MHD_Response* mhd_response = MHD_create_response_from_buffer(
        strlen(response), response, MHD_RESPMEM_MUST_COPY);

    MHD_add_response_header(mhd_response, "Content-Type", CT_JSON);
    MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");

    int ret = MHD_queue_response(conn, MHD_HTTP_OK, mhd_response);
    MHD_destroy_response(mhd_response);

    return ret;
}

/**
 * Handle GET /api/nodes/:id - return detailed node information.
 * For standalone mode, only "local" is a valid node ID.
 */
static int handle_node_detail(struct MHD_Connection* conn, cira_ctx* ctx, const char* node_id) {
    char response[4096];
    char timestamp[32];
    get_timestamp(timestamp, sizeof(timestamp));

    /* Only support "local" node ID in standalone mode */
    if (strcmp(node_id, "local") != 0) {
        const char* error = "{\"error\":\"Node not found\"}";
        struct MHD_Response* mhd_response = MHD_create_response_from_buffer(
            strlen(error), (void*)error, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(mhd_response, "Content-Type", CT_JSON);
        MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");
        int ret = MHD_queue_response(conn, MHD_HTTP_NOT_FOUND, mhd_response);
        MHD_destroy_response(mhd_response);
        return ret;
    }

    /* Get the port from server state */
    int port = 8080;
    if (g_server) {
        port = g_server->port;
    }

    /* Calculate uptime */
    time_t now = time(NULL);
    long uptime_sec = ctx ? (long)(now - ctx->start_time) : 0;

    /* Get model name */
    const char* model_name = "none";
    if (ctx && ctx->format == CIRA_FORMAT_ONNX) model_name = "ONNX Model";
    else if (ctx && ctx->format == CIRA_FORMAT_NCNN) model_name = "NCNN Model";
    else if (ctx && ctx->format == CIRA_FORMAT_DARKNET) model_name = "Darknet Model";
    else if (ctx && ctx->format == CIRA_FORMAT_TENSORRT) model_name = "TensorRT Model";

    /* Build detailed node info */
    snprintf(response, sizeof(response),
        "{"
        "\"id\":\"local\","
        "\"name\":\"Local Runtime\","
        "\"type\":\"edge\","
        "\"host\":\"localhost\","
        "\"status\":\"online\","
        "\"lastSeen\":\"%s\","
        "\"runtime\":{"
            "\"port\":%d,"
            "\"config\":\"standalone\""
        "},"
        "\"metrics\":{"
            "\"fps\":%.1f,"
            "\"temperature\":%.1f,"
            "\"cpuUsage\":%.1f,"
            "\"memoryUsage\":%.1f,"
            "\"uptime\":%ld"
        "},"
        "\"inference\":{"
            "\"modelName\":\"%s\","
            "\"defectsTotal\":%llu,"
            "\"defectsPerHour\":%.1f,"
            "\"lastDefect\":null,"
            "\"running\":%s"
        "},"
        "\"location\":\"Local Machine\""
        "}",
        timestamp,
        port,
        ctx ? ctx->current_fps : 0.0f,
        get_temperature(),
        get_cpu_usage(),
        get_memory_usage(),
        uptime_sec,
        (ctx && ctx->model_handle) ? model_name : "None",
        ctx ? (unsigned long long)ctx->total_detections : 0ULL,
        uptime_sec > 0 && ctx ? (float)ctx->total_detections * 3600.0f / uptime_sec : 0.0f,
        (ctx && ctx->camera_running) ? "true" : "false"
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
 * Handle GET /api/nodes - return this runtime as a single node.
 * This allows the dashboard to work in standalone mode without the coordinator.
 */
static int handle_nodes_list(struct MHD_Connection* conn, cira_ctx* ctx) {
    char response[2048];

    /* Get hostname */
    char hostname[256] = "localhost";
#ifdef _WIN32
    DWORD size = sizeof(hostname);
    GetComputerNameA(hostname, &size);
#else
    gethostname(hostname, sizeof(hostname));
#endif

    /* Get the port from server state */
    int port = 8080;
    if (g_server) {
        port = g_server->port;
    }

    /* Build node info */
    snprintf(response, sizeof(response),
        "{"
        "\"nodes\":["
            "{"
                "\"id\":\"local\","
                "\"name\":\"Local Runtime\","
                "\"type\":\"edge\","
                "\"host\":\"localhost\","
                "\"status\":\"online\","
                "\"runtime\":{\"port\":%d},"
                "\"lastSeen\":\"%s\","
                "\"metrics\":{"
                    "\"fps\":%.1f,"
                    "\"inferenceTime\":%.1f"
                "},"
                "\"inference\":{"
                    "\"modelName\":\"%s\","
                    "\"running\":%s"
                "}"
            "}"
        "],"
        "\"summary\":{\"total\":1,\"online\":1,\"offline\":0}"
        "}",
        port,
        "now",
        ctx ? ctx->current_fps : 0.0f,
        0.0f,  /* inference time not tracked in ctx */
        (ctx && ctx->model_handle) ? "loaded" : "none",
        (ctx && ctx->camera_running) ? "true" : "false"
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
 * Handle GET /api/files - browse directory on edge device.
 * Query param: path (directory path to list)
 */
static int handle_files_list(struct MHD_Connection* conn, cira_ctx* ctx) {
    (void)ctx;
    char response[MAX_RESPONSE_SIZE];
    char* p = response;
    char* end = response + sizeof(response) - 256;

    /* Get path parameter */
    const char* path = MHD_lookup_connection_value(conn, MHD_GET_ARGUMENT_KIND, "path");
    if (!path || path[0] == '\0') {
#ifdef _WIN32
        path = "C:\\";
#else
        path = "/home";
#endif
    }

    /* Security: Prevent path traversal attacks */
    if (strstr(path, "..") != NULL) {
        const char* error = "{\"error\":\"Invalid path\"}";
        struct MHD_Response* mhd_response = MHD_create_response_from_buffer(
            strlen(error), (void*)error, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(mhd_response, "Content-Type", CT_JSON);
        MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");
        int ret = MHD_queue_response(conn, MHD_HTTP_BAD_REQUEST, mhd_response);
        MHD_destroy_response(mhd_response);
        return ret;
    }

    /* Escape the path for JSON */
    char escaped_path[1024];
    const char* src = path;
    char* dst = escaped_path;
    char* dst_end = escaped_path + sizeof(escaped_path) - 2;
    while (*src && dst < dst_end) {
        if (*src == '\\') {
            *dst++ = '\\';
            *dst++ = '\\';
        } else if (*src == '"') {
            *dst++ = '\\';
            *dst++ = '"';
        } else {
            *dst++ = *src;
        }
        src++;
    }
    *dst = '\0';

    p += snprintf(p, end - p, "{\"path\":\"%s\",\"entries\":[", escaped_path);

    int count = 0;
    int file_count = 0;
    int dir_count = 0;

    DIR* d = opendir(path);
    if (d) {
        struct dirent* entry;
        while ((entry = readdir(d)) != NULL && p < end - 256) {
            /* Skip hidden files and . / .. */
            if (entry->d_name[0] == '.') continue;

            char full_path[2048];
            snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

            struct stat st;
            if (stat(full_path, &st) != 0) continue;

            int is_dir = S_ISDIR(st.st_mode);
            int is_image = 0;

            /* Check if file is an image (for image tester) */
            if (!is_dir) {
                const char* name = entry->d_name;
                size_t len = strlen(name);
                if (len > 4) {
                    const char* ext = name + len - 4;
                    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".png") == 0 ||
                        strcasecmp(ext, ".bmp") == 0) {
                        is_image = 1;
                    }
                    if (len > 5) {
                        ext = name + len - 5;
                        if (strcasecmp(ext, ".jpeg") == 0) {
                            is_image = 1;
                        }
                    }
                }
            }

            /* Escape filename for JSON */
            char escaped_name[512];
            const char* s = entry->d_name;
            char* d_ptr = escaped_name;
            char* d_end = escaped_name + sizeof(escaped_name) - 2;
            while (*s && d_ptr < d_end) {
                if (*s == '\\') {
                    *d_ptr++ = '\\';
                    *d_ptr++ = '\\';
                } else if (*s == '"') {
                    *d_ptr++ = '\\';
                    *d_ptr++ = '"';
                } else {
                    *d_ptr++ = *s;
                }
                s++;
            }
            *d_ptr = '\0';

            if (count > 0) p += snprintf(p, end - p, ",");
            p += snprintf(p, end - p,
                         "{\"name\":\"%s\",\"is_dir\":%s,\"is_image\":%s,\"size\":%ld}",
                         escaped_name,
                         is_dir ? "true" : "false",
                         is_image ? "true" : "false",
                         (long)st.st_size);
            count++;

            if (is_dir) dir_count++;
            else file_count++;

            /* Limit entries to prevent huge responses */
            if (count >= 500) break;
        }
        closedir(d);
    } else {
        /* Directory not found or not accessible */
        snprintf(response, sizeof(response),
                "{\"error\":\"Cannot access directory\",\"path\":\"%s\"}", escaped_path);

        struct MHD_Response* mhd_response = MHD_create_response_from_buffer(
            strlen(response), response, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(mhd_response, "Content-Type", CT_JSON);
        MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");
        int ret = MHD_queue_response(conn, MHD_HTTP_NOT_FOUND, mhd_response);
        MHD_destroy_response(mhd_response);
        return ret;
    }

    p += snprintf(p, end - p, "],\"count\":%d,\"dirs\":%d,\"files\":%d}",
                 count, dir_count, file_count);

    struct MHD_Response* mhd_response = MHD_create_response_from_buffer(
        strlen(response), response, MHD_RESPMEM_MUST_COPY);

    MHD_add_response_header(mhd_response, "Content-Type", CT_JSON);
    MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");

    int ret = MHD_queue_response(conn, MHD_HTTP_OK, mhd_response);
    MHD_destroy_response(mhd_response);

    return ret;
}

/**
 * Handle /frame/info endpoint - return frame file info without the image data.
 */
static int handle_frame_info(struct MHD_Connection* conn, cira_ctx* ctx) {
    char response[1024];

    pthread_mutex_lock(&ctx->frame_file_mutex);
    snprintf(response, sizeof(response),
        "{"
        "\"sequence\":%llu,"
        "\"path\":\"%s\","
        "\"available\":%s"
        "}",
        (unsigned long long)ctx->frame_sequence,
        ctx->frame_file_path,
        ctx->frame_file_path[0] != '\0' ? "true" : "false"
    );
    pthread_mutex_unlock(&ctx->frame_file_mutex);

    struct MHD_Response* mhd_response = MHD_create_response_from_buffer(
        strlen(response), response, MHD_RESPMEM_MUST_COPY);

    MHD_add_response_header(mhd_response, "Content-Type", CT_JSON);
    MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");

    int ret = MHD_queue_response(conn, MHD_HTTP_OK, mhd_response);
    MHD_destroy_response(mhd_response);

    return ret;
}

/* Forward declarations for camera control */
extern int camera_start(cira_ctx* ctx, int device_id);
extern int camera_stop(cira_ctx* ctx);

/**
 * Handle POST /api/camera/start - start camera capture.
 */
static int handle_camera_start(struct MHD_Connection* conn, cira_ctx* ctx,
                               const char* upload_data, size_t upload_size) {
    char response[1024];

    /* Parse device_id from POST data (simple JSON: {"device_id": 0}) */
    int device_id = 0;

    if (upload_data && upload_size > 0) {
        const char* dev_key = strstr(upload_data, "\"device_id\"");
        if (dev_key) {
            dev_key = strchr(dev_key + 11, ':');
            if (dev_key) {
                device_id = atoi(dev_key + 1);
            }
        }
    }

    fprintf(stderr, "Starting camera %d...\n", device_id);

    int result = camera_start(ctx, device_id);

    if (result == CIRA_OK) {
        snprintf(response, sizeof(response),
                "{\"success\":true,\"device_id\":%d,\"message\":\"Camera started\"}",
                device_id);
    } else {
        snprintf(response, sizeof(response),
                "{\"success\":false,\"error\":\"Failed to start camera %d\"}", device_id);
    }

    struct MHD_Response* mhd_response = MHD_create_response_from_buffer(
        strlen(response), response, MHD_RESPMEM_MUST_COPY);

    MHD_add_response_header(mhd_response, "Content-Type", CT_JSON);
    MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");

    int ret = MHD_queue_response(conn, result == CIRA_OK ? MHD_HTTP_OK : MHD_HTTP_BAD_REQUEST, mhd_response);
    MHD_destroy_response(mhd_response);

    return ret;
}

/**
 * Handle POST /api/camera/stop - stop camera capture.
 */
static int handle_camera_stop(struct MHD_Connection* conn, cira_ctx* ctx) {
    char response[512];

    fprintf(stderr, "Stopping camera...\n");

    int result = camera_stop(ctx);

    if (result == CIRA_OK) {
        snprintf(response, sizeof(response),
                "{\"success\":true,\"message\":\"Camera stopped\"}");
    } else {
        snprintf(response, sizeof(response),
                "{\"success\":false,\"error\":\"Failed to stop camera\"}");
    }

    struct MHD_Response* mhd_response = MHD_create_response_from_buffer(
        strlen(response), response, MHD_RESPMEM_MUST_COPY);

    MHD_add_response_header(mhd_response, "Content-Type", CT_JSON);
    MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");

    int ret = MHD_queue_response(conn, MHD_HTTP_OK, mhd_response);
    MHD_destroy_response(mhd_response);

    return ret;
}

/**
 * Handle POST /api/inference/image - run inference on a single image.
 * Accepts either:
 * - {"path": "/path/to/image.jpg"} for device-local image
 * - Multipart form data with image upload (future)
 */
static int handle_inference_image(struct MHD_Connection* conn, cira_ctx* ctx,
                                  const char* upload_data, size_t upload_size) {
    char response[MAX_RESPONSE_SIZE];

    /* Check if model is loaded */
    if (ctx->format == CIRA_FORMAT_UNKNOWN || ctx->model_handle == NULL) {
        snprintf(response, sizeof(response),
                "{\"success\":false,\"error\":\"No model loaded\"}");

        struct MHD_Response* mhd_response = MHD_create_response_from_buffer(
            strlen(response), response, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(mhd_response, "Content-Type", CT_JSON);
        MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");
        int ret = MHD_queue_response(conn, MHD_HTTP_BAD_REQUEST, mhd_response);
        MHD_destroy_response(mhd_response);
        return ret;
    }

    /* Parse image path from POST data */
    char image_path[512] = "";

    if (upload_data && upload_size > 0) {
        const char* path_key = strstr(upload_data, "\"path\"");
        if (path_key) {
            path_key = strchr(path_key + 6, '"');
            if (path_key) {
                path_key++;
                const char* path_end = strchr(path_key, '"');
                if (path_end) {
                    size_t len = path_end - path_key;
                    if (len >= sizeof(image_path)) len = sizeof(image_path) - 1;
                    memcpy(image_path, path_key, len);
                    image_path[len] = '\0';
                }
            }
        }
    }

    if (image_path[0] == '\0') {
        snprintf(response, sizeof(response),
                "{\"success\":false,\"error\":\"Missing image path\"}");

        struct MHD_Response* mhd_response = MHD_create_response_from_buffer(
            strlen(response), response, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(mhd_response, "Content-Type", CT_JSON);
        MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");
        int ret = MHD_queue_response(conn, MHD_HTTP_BAD_REQUEST, mhd_response);
        MHD_destroy_response(mhd_response);
        return ret;
    }

    /* Security: Prevent path traversal */
    if (strstr(image_path, "..") != NULL) {
        snprintf(response, sizeof(response),
                "{\"success\":false,\"error\":\"Invalid path\"}");

        struct MHD_Response* mhd_response = MHD_create_response_from_buffer(
            strlen(response), response, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(mhd_response, "Content-Type", CT_JSON);
        MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");
        int ret = MHD_queue_response(conn, MHD_HTTP_BAD_REQUEST, mhd_response);
        MHD_destroy_response(mhd_response);
        return ret;
    }

    /* Check if file exists */
    struct stat st;
    if (stat(image_path, &st) != 0) {
        snprintf(response, sizeof(response),
                "{\"success\":false,\"error\":\"Image file not found\"}");

        struct MHD_Response* mhd_response = MHD_create_response_from_buffer(
            strlen(response), response, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(mhd_response, "Content-Type", CT_JSON);
        MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");
        int ret = MHD_queue_response(conn, MHD_HTTP_NOT_FOUND, mhd_response);
        MHD_destroy_response(mhd_response);
        return ret;
    }

    fprintf(stderr, "Running inference on image: %s\n", image_path);

    /* Load and decode image using stb_image (include from jpeg_encoder.cpp) */
    /* For now, we'll use OpenCV if available, or return an error */
    /* TODO: Add direct image loading support */

    /* Use cira_predict_image API - but we need to load the image first */
    /* This requires OpenCV or stb_image integration */

    /* For now, store the path and let the frontend know it needs to upload */
    /* In a full implementation, we'd load the image here and run inference */

    /* Placeholder response - indicates image inference is available but needs image data */
    snprintf(response, sizeof(response),
            "{\"success\":false,\"error\":\"Image loading not yet implemented. Use camera stream or upload via dashboard.\",\"path\":\"%s\"}",
            image_path);

    struct MHD_Response* mhd_response = MHD_create_response_from_buffer(
        strlen(response), response, MHD_RESPMEM_MUST_COPY);

    MHD_add_response_header(mhd_response, "Content-Type", CT_JSON);
    MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");

    int ret = MHD_queue_response(conn, MHD_HTTP_NOT_IMPLEMENTED, mhd_response);
    MHD_destroy_response(mhd_response);

    return ret;
}

/**
 * Handle OPTIONS for CORS preflight.
 */
static int handle_cors_preflight(struct MHD_Connection* conn) {
    struct MHD_Response* response = MHD_create_response_from_buffer(0, "", MHD_RESPMEM_PERSISTENT);

    MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
    MHD_add_response_header(response, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    MHD_add_response_header(response, "Access-Control-Allow-Headers", "Content-Type, Cache-Control");
    MHD_add_response_header(response, "Access-Control-Max-Age", "86400");

    int ret = MHD_queue_response(conn, MHD_HTTP_OK, response);
    MHD_destroy_response(response);

    return ret;
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

/* Connection context for POST data accumulation */
typedef struct {
    char* data;
    size_t size;
    size_t capacity;
} post_ctx_t;

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

    cira_ctx* ctx = (cira_ctx*)cls;

    /* Handle CORS preflight */
    if (strcmp(method, "OPTIONS") == 0) {
        return handle_cors_preflight(conn);
    }

    /* Handle POST requests */
    if (strcmp(method, "POST") == 0) {
        /* First call - allocate context */
        if (*con_cls == NULL) {
            post_ctx_t* pctx = (post_ctx_t*)calloc(1, sizeof(post_ctx_t));
            if (!pctx) return MHD_NO;
            pctx->capacity = 4096;
            pctx->data = (char*)malloc(pctx->capacity);
            if (!pctx->data) { free(pctx); return MHD_NO; }
            pctx->size = 0;
            *con_cls = pctx;
            return MHD_YES;
        }

        post_ctx_t* pctx = (post_ctx_t*)*con_cls;

        /* Accumulate upload data */
        if (*upload_data_size > 0) {
            if (pctx->size + *upload_data_size >= pctx->capacity) {
                pctx->capacity = pctx->size + *upload_data_size + 1024;
                char* new_data = (char*)realloc(pctx->data, pctx->capacity);
                if (!new_data) return MHD_NO;
                pctx->data = new_data;
            }
            memcpy(pctx->data + pctx->size, upload_data, *upload_data_size);
            pctx->size += *upload_data_size;
            pctx->data[pctx->size] = '\0';
            *upload_data_size = 0;
            return MHD_YES;
        }

        /* All data received - process request */
        int ret = MHD_NO;
        if (strcmp(url, "/api/model") == 0) {
            ret = handle_model_load(conn, ctx, pctx->data, pctx->size);
        } else if (strncmp(url, "/api/nodes/", 11) == 0 && strstr(url, "/model") != NULL) {
            /* Handle /api/nodes/:id/model - same as /api/model for standalone mode */
            ret = handle_model_load(conn, ctx, pctx->data, pctx->size);
        } else if (strcmp(url, "/api/camera/start") == 0) {
            ret = handle_camera_start(conn, ctx, pctx->data, pctx->size);
        } else if (strcmp(url, "/api/camera/stop") == 0) {
            ret = handle_camera_stop(conn, ctx);
        } else if (strcmp(url, "/api/inference/image") == 0) {
            ret = handle_inference_image(conn, ctx, pctx->data, pctx->size);
        } else {
            ret = handle_not_found(conn);
        }

        /* Clean up */
        free(pctx->data);
        free(pctx);
        *con_cls = NULL;

        return ret;
    }

    /* Handle GET requests */
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
    if (strcmp(url, "/api/models") == 0) {
        return handle_models_list(conn, ctx);
    }
    if (strcmp(url, "/api/nodes") == 0) {
        return handle_nodes_list(conn, ctx);
    }
    /* Handle /api/nodes/:id/models - return available models for node */
    if (strncmp(url, "/api/nodes/", 11) == 0 && strstr(url, "/models") != NULL) {
        /* For standalone mode, return models in format expected by DeviceDetail.vue */
        /* DeviceDetail expects { available: [...] } format */
        char response[MAX_RESPONSE_SIZE];
        char* p = response;
        char* end = response + sizeof(response) - 256;

        p += snprintf(p, end - p, "{\"available\":[");

        int count = 0;

        /* Scan models directory if set */
        if (g_models_dir[0] != '\0') {
            DIR* d = opendir(g_models_dir);
            if (d) {
                struct dirent* entry;
                while ((entry = readdir(d)) != NULL && p < end - 256) {
                    if (entry->d_name[0] == '.') continue;

                    char full_path[2048];
                    snprintf(full_path, sizeof(full_path), "%s/%s", g_models_dir, entry->d_name);

                    struct stat st;
                    if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
                        /* Check for model files inside */
                        int has_onnx = 0, has_ncnn = 0;
                        DIR* model_d = opendir(full_path);
                        if (model_d) {
                            struct dirent* model_entry;
                            while ((model_entry = readdir(model_d)) != NULL) {
                                const char* name = model_entry->d_name;
                                size_t len = strlen(name);
                                if (len > 5 && strcmp(name + len - 5, ".onnx") == 0) has_onnx = 1;
                                if (len > 6 && strcmp(name + len - 6, ".param") == 0) has_ncnn = 1;
                            }
                            closedir(model_d);
                        }

                        if (has_onnx || has_ncnn) {
                            int is_loaded = (ctx && ctx->model_path[0] != '\0' &&
                                           strstr(ctx->model_path, entry->d_name) != NULL);
                            if (count > 0) p += snprintf(p, end - p, ",");
                            p += snprintf(p, end - p, "{\"name\":\"%s\",\"path\":\"%s\",\"type\":\"%s\",\"loaded\":%s}",
                                         entry->d_name, full_path, has_onnx ? "onnx" : "ncnn",
                                         is_loaded ? "true" : "false");
                            count++;
                        }
                    }
                }
                closedir(d);
            }
        }

        /* Add currently loaded model if not already in list */
        if (ctx && ctx->model_path[0] != '\0' && count == 0) {
            const char* type = "unknown";
            if (ctx->format == CIRA_FORMAT_ONNX) type = "onnx";
            else if (ctx->format == CIRA_FORMAT_NCNN) type = "ncnn";

            if (count > 0) p += snprintf(p, end - p, ",");
            p += snprintf(p, end - p, "{\"name\":\"Current Model\",\"path\":\"%s\",\"type\":\"%s\",\"loaded\":true}",
                         ctx->model_path, type);
            count++;
        }

        p += snprintf(p, end - p, "]}");

        struct MHD_Response* mhd_response = MHD_create_response_from_buffer(
            strlen(response), response, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(mhd_response, "Content-Type", CT_JSON);
        MHD_add_response_header(mhd_response, "Access-Control-Allow-Origin", "*");
        int ret = MHD_queue_response(conn, MHD_HTTP_OK, mhd_response);
        MHD_destroy_response(mhd_response);
        return ret;
    }
    /* Handle /api/nodes/:id for individual node details */
    if (strncmp(url, "/api/nodes/", 11) == 0 && strlen(url) > 11) {
        const char* node_id = url + 11;
        /* Skip if it contains another slash (like /models) - already handled above */
        if (strchr(node_id, '/') == NULL) {
            return handle_node_detail(conn, ctx, node_id);
        }
    }
    if (strcmp(url, "/api/cameras") == 0) {
        return handle_cameras_list(conn, ctx);
    }
    if (strcmp(url, "/api/files") == 0) {
        return handle_files_list(conn, ctx);
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
    /* File-based frame transfer endpoints (cross-platform alternative to MJPEG) */
    if (strcmp(url, "/frame/latest") == 0) {
        return handle_frame_latest(conn, ctx);
    }
    if (strcmp(url, "/frame/info") == 0) {
        return handle_frame_info(conn, ctx);
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
    fprintf(stderr, "  Frame:     http://localhost:%d/frame/latest (file-based)\n", port);
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
