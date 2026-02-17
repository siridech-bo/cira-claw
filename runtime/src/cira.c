/**
 * CiRA Runtime - Main API Implementation
 *
 * This file implements the public API defined in cira.h.
 * It manages the context lifecycle and dispatches to format-specific loaders.
 *
 * (c) CiRA Robotics / KMITL 2026
 */

#include "cira.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>

/* Version string */
#define CIRA_VERSION_STRING "1.0.0"

/* Maximum detections per frame */
#define CIRA_MAX_DETECTIONS 256

/* Maximum labels */
#define CIRA_MAX_LABELS 256

/* Maximum label length */
#define CIRA_MAX_LABEL_LEN 64

/* Maximum error message length */
#define CIRA_MAX_ERROR_LEN 512

/* Maximum JSON result length */
#define CIRA_MAX_JSON_LEN 65536

/* Model format types */
typedef enum {
    CIRA_FORMAT_UNKNOWN = 0,
    CIRA_FORMAT_DARKNET,
    CIRA_FORMAT_ONNX,
    CIRA_FORMAT_TENSORRT,
    CIRA_FORMAT_SKLEARN
} cira_format_t;

/* Detection result */
typedef struct {
    float x, y, w, h;       /* Bounding box (normalized 0-1) */
    float confidence;       /* Detection confidence */
    int label_id;           /* Label index */
} cira_detection_t;

/* Forward declarations for loader functions */
#ifdef CIRA_DARKNET_ENABLED
extern int darknet_load(cira_ctx* ctx, const char* model_dir);
extern void darknet_unload(cira_ctx* ctx);
extern int darknet_predict(cira_ctx* ctx, const uint8_t* data, int w, int h, int channels);
#endif

#ifdef CIRA_ONNX_ENABLED
extern int onnx_load(cira_ctx* ctx, const char* model_path);
extern void onnx_unload(cira_ctx* ctx);
extern int onnx_predict(cira_ctx* ctx, const uint8_t* data, int w, int h, int channels);
#endif

#ifdef CIRA_TRT_ENABLED
extern int trt_load(cira_ctx* ctx, const char* model_path);
extern void trt_unload(cira_ctx* ctx);
extern int trt_predict(cira_ctx* ctx, const uint8_t* data, int w, int h, int channels);
#endif

#ifdef CIRA_STREAMING_ENABLED
extern int camera_start(cira_ctx* ctx, int device_id);
extern int camera_stop(cira_ctx* ctx);
extern int server_start(cira_ctx* ctx, int port);
extern int server_stop(cira_ctx* ctx);
#endif

/* Context structure (internal) */
struct cira_ctx {
    /* Status */
    int status;
    char error_msg[CIRA_MAX_ERROR_LEN];

    /* Model info */
    cira_format_t format;
    char model_path[1024];
    void* model_handle;             /* Format-specific model data */

    /* Labels */
    char labels[CIRA_MAX_LABELS][CIRA_MAX_LABEL_LEN];
    int num_labels;

    /* Model input size */
    int input_w;
    int input_h;

    /* Inference settings */
    float confidence_threshold;
    float nms_threshold;

    /* Results */
    cira_detection_t detections[CIRA_MAX_DETECTIONS];
    int num_detections;
    char result_json[CIRA_MAX_JSON_LEN];

    /* Streaming state */
    int camera_running;
    int server_running;
    int server_port;
    pthread_t camera_thread;
    pthread_mutex_t result_mutex;
    float current_fps;

    /* Frame buffer for streaming */
    uint8_t* frame_buffer;
    int frame_w;
    int frame_h;
    pthread_mutex_t frame_mutex;
};

/* Helper: Set error message */
static void set_error(cira_ctx* ctx, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(ctx->error_msg, CIRA_MAX_ERROR_LEN, fmt, args);
    va_end(args);
    ctx->status = CIRA_STATUS_ERROR;
}

/* Helper: Check if path is a directory */
static int is_directory(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

/* Helper: Check if file exists */
static int file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Helper: Find file with extension in directory */
static int find_file_with_ext(const char* dir, const char* ext, char* out, size_t out_size) {
    DIR* d = opendir(dir);
    if (!d) return 0;

    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
        const char* name = entry->d_name;
        size_t len = strlen(name);
        size_t ext_len = strlen(ext);

        if (len > ext_len && strcmp(name + len - ext_len, ext) == 0) {
            snprintf(out, out_size, "%s/%s", dir, name);
            closedir(d);
            return 1;
        }
    }

    closedir(d);
    return 0;
}

/* Helper: Load labels from file */
static int load_labels(cira_ctx* ctx, const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;

    ctx->num_labels = 0;
    char line[CIRA_MAX_LABEL_LEN];

    while (fgets(line, sizeof(line), f) && ctx->num_labels < CIRA_MAX_LABELS) {
        /* Trim newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) {
            line[--len] = '\0';
        }

        if (len > 0) {
            strncpy(ctx->labels[ctx->num_labels], line, CIRA_MAX_LABEL_LEN - 1);
            ctx->labels[ctx->num_labels][CIRA_MAX_LABEL_LEN - 1] = '\0';
            ctx->num_labels++;
        }
    }

    fclose(f);
    return ctx->num_labels;
}

/* Helper: Detect model format from path */
static cira_format_t detect_format(const char* path) {
    if (is_directory(path)) {
        /* Check for Darknet files in directory */
        char buf[1024];
        if (find_file_with_ext(path, ".weights", buf, sizeof(buf)) &&
            find_file_with_ext(path, ".cfg", buf, sizeof(buf))) {
            return CIRA_FORMAT_DARKNET;
        }
        /* Check for ONNX file */
        if (find_file_with_ext(path, ".onnx", buf, sizeof(buf))) {
            return CIRA_FORMAT_ONNX;
        }
        /* Check for TensorRT engine */
        if (find_file_with_ext(path, ".engine", buf, sizeof(buf)) ||
            find_file_with_ext(path, ".trt", buf, sizeof(buf))) {
            return CIRA_FORMAT_TENSORRT;
        }
    } else {
        /* Check file extension */
        const char* ext = strrchr(path, '.');
        if (ext) {
            if (strcmp(ext, ".weights") == 0 || strcmp(ext, ".cfg") == 0) {
                return CIRA_FORMAT_DARKNET;
            }
            if (strcmp(ext, ".onnx") == 0) {
                return CIRA_FORMAT_ONNX;
            }
            if (strcmp(ext, ".engine") == 0 || strcmp(ext, ".trt") == 0) {
                return CIRA_FORMAT_TENSORRT;
            }
            if (strcmp(ext, ".pkl") == 0 || strcmp(ext, ".joblib") == 0) {
                return CIRA_FORMAT_SKLEARN;
            }
        }
    }

    return CIRA_FORMAT_UNKNOWN;
}

/* Helper: Build JSON result string */
static void build_result_json(cira_ctx* ctx, int img_w, int img_h) {
    char* p = ctx->result_json;
    char* end = ctx->result_json + CIRA_MAX_JSON_LEN;

    p += snprintf(p, end - p, "{\"detections\":[");

    for (int i = 0; i < ctx->num_detections && p < end - 256; i++) {
        cira_detection_t* det = &ctx->detections[i];

        /* Convert normalized coords to pixel coords */
        int px = (int)(det->x * img_w);
        int py = (int)(det->y * img_h);
        int pw = (int)(det->w * img_w);
        int ph = (int)(det->h * img_h);

        const char* label = (det->label_id >= 0 && det->label_id < ctx->num_labels)
            ? ctx->labels[det->label_id]
            : "unknown";

        if (i > 0) p += snprintf(p, end - p, ",");
        p += snprintf(p, end - p,
            "{\"label\":\"%s\",\"confidence\":%.3f,\"bbox\":[%d,%d,%d,%d]}",
            label, det->confidence, px, py, pw, ph);
    }

    p += snprintf(p, end - p, "],\"count\":%d}", ctx->num_detections);
}

/* === Public API Implementation === */

const char* cira_version(void) {
    return CIRA_VERSION_STRING;
}

cira_ctx* cira_create(void) {
    cira_ctx* ctx = (cira_ctx*)calloc(1, sizeof(cira_ctx));
    if (!ctx) return NULL;

    ctx->status = CIRA_STATUS_READY;
    ctx->format = CIRA_FORMAT_UNKNOWN;
    ctx->confidence_threshold = 0.5f;
    ctx->nms_threshold = 0.4f;
    ctx->input_w = 416;
    ctx->input_h = 416;

    pthread_mutex_init(&ctx->result_mutex, NULL);
    pthread_mutex_init(&ctx->frame_mutex, NULL);

    return ctx;
}

void cira_destroy(cira_ctx* ctx) {
    if (!ctx) return;

    /* Stop streaming if running */
    if (ctx->camera_running) {
#ifdef CIRA_STREAMING_ENABLED
        camera_stop(ctx);
#endif
    }
    if (ctx->server_running) {
#ifdef CIRA_STREAMING_ENABLED
        server_stop(ctx);
#endif
    }

    /* Unload model */
    switch (ctx->format) {
#ifdef CIRA_DARKNET_ENABLED
        case CIRA_FORMAT_DARKNET:
            darknet_unload(ctx);
            break;
#endif
#ifdef CIRA_ONNX_ENABLED
        case CIRA_FORMAT_ONNX:
            onnx_unload(ctx);
            break;
#endif
#ifdef CIRA_TRT_ENABLED
        case CIRA_FORMAT_TENSORRT:
            trt_unload(ctx);
            break;
#endif
        default:
            break;
    }

    if (ctx->frame_buffer) {
        free(ctx->frame_buffer);
    }

    pthread_mutex_destroy(&ctx->result_mutex);
    pthread_mutex_destroy(&ctx->frame_mutex);

    free(ctx);
}

int cira_load(cira_ctx* ctx, const char* config_path) {
    if (!ctx || !config_path) return CIRA_ERROR_INPUT;

    ctx->status = CIRA_STATUS_LOADING;

    /* If already loaded, unload first */
    if (ctx->format != CIRA_FORMAT_UNKNOWN) {
        switch (ctx->format) {
#ifdef CIRA_DARKNET_ENABLED
            case CIRA_FORMAT_DARKNET:
                darknet_unload(ctx);
                break;
#endif
#ifdef CIRA_ONNX_ENABLED
            case CIRA_FORMAT_ONNX:
                onnx_unload(ctx);
                break;
#endif
#ifdef CIRA_TRT_ENABLED
            case CIRA_FORMAT_TENSORRT:
                trt_unload(ctx);
                break;
#endif
            default:
                break;
        }
        ctx->format = CIRA_FORMAT_UNKNOWN;
    }

    /* Detect model format */
    cira_format_t format = detect_format(config_path);
    if (format == CIRA_FORMAT_UNKNOWN) {
        set_error(ctx, "Unknown model format: %s", config_path);
        return CIRA_ERROR_MODEL;
    }

    strncpy(ctx->model_path, config_path, sizeof(ctx->model_path) - 1);

    /* Try to load labels */
    if (is_directory(config_path)) {
        char label_path[1024];
        snprintf(label_path, sizeof(label_path), "%s/obj.names", config_path);
        if (!file_exists(label_path)) {
            snprintf(label_path, sizeof(label_path), "%s/labels.txt", config_path);
        }
        if (file_exists(label_path)) {
            load_labels(ctx, label_path);
        }
    }

    /* Dispatch to format-specific loader */
    int result;
    switch (format) {
#ifdef CIRA_DARKNET_ENABLED
        case CIRA_FORMAT_DARKNET:
            result = darknet_load(ctx, config_path);
            break;
#endif
#ifdef CIRA_ONNX_ENABLED
        case CIRA_FORMAT_ONNX:
            result = onnx_load(ctx, config_path);
            break;
#endif
#ifdef CIRA_TRT_ENABLED
        case CIRA_FORMAT_TENSORRT:
            result = trt_load(ctx, config_path);
            break;
#endif
        default:
            set_error(ctx, "Model format not supported in this build");
            return CIRA_ERROR_MODEL;
    }

    if (result == CIRA_OK) {
        ctx->format = format;
        ctx->status = CIRA_STATUS_READY;
    }

    return result;
}

int cira_predict_image(cira_ctx* ctx, const uint8_t* data, int w, int h, int channels) {
    if (!ctx || !data) return CIRA_ERROR_INPUT;
    if (ctx->status != CIRA_STATUS_READY) return CIRA_ERROR;
    if (channels != 3) {
        set_error(ctx, "Only 3-channel images supported");
        return CIRA_ERROR_INPUT;
    }

    pthread_mutex_lock(&ctx->result_mutex);

    ctx->num_detections = 0;

    int result;
    switch (ctx->format) {
#ifdef CIRA_DARKNET_ENABLED
        case CIRA_FORMAT_DARKNET:
            result = darknet_predict(ctx, data, w, h, channels);
            break;
#endif
#ifdef CIRA_ONNX_ENABLED
        case CIRA_FORMAT_ONNX:
            result = onnx_predict(ctx, data, w, h, channels);
            break;
#endif
#ifdef CIRA_TRT_ENABLED
        case CIRA_FORMAT_TENSORRT:
            result = trt_predict(ctx, data, w, h, channels);
            break;
#endif
        default:
            result = CIRA_ERROR_MODEL;
            break;
    }

    if (result == CIRA_OK) {
        build_result_json(ctx, w, h);
    }

    pthread_mutex_unlock(&ctx->result_mutex);
    return result;
}

int cira_predict_sensor(cira_ctx* ctx, const float* values, int count) {
    if (!ctx || !values || count <= 0) return CIRA_ERROR_INPUT;

    /* Sensor prediction is only supported for sklearn models */
    if (ctx->format != CIRA_FORMAT_SKLEARN) {
        set_error(ctx, "Sensor prediction requires sklearn model");
        return CIRA_ERROR_MODEL;
    }

    /* TODO: Implement sklearn prediction */
    set_error(ctx, "Sklearn prediction not yet implemented");
    return CIRA_ERROR;
}

int cira_predict_batch(cira_ctx* ctx, const uint8_t** images, int count, int w, int h, int channels) {
    if (!ctx || !images || count <= 0) return CIRA_ERROR_INPUT;

    /* For now, just process images one by one */
    /* TODO: Implement true batch processing for supported backends */
    int total_detections = 0;

    for (int i = 0; i < count; i++) {
        int result = cira_predict_image(ctx, images[i], w, h, channels);
        if (result != CIRA_OK) return result;
        total_detections += ctx->num_detections;
    }

    return CIRA_OK;
}

const char* cira_result_json(cira_ctx* ctx) {
    if (!ctx) return "{}";
    return ctx->result_json;
}

int cira_result_count(cira_ctx* ctx) {
    if (!ctx) return 0;
    return ctx->num_detections;
}

int cira_result_bbox(cira_ctx* ctx, int index, float* x, float* y, float* w, float* h) {
    if (!ctx || index < 0 || index >= ctx->num_detections) return CIRA_ERROR;

    cira_detection_t* det = &ctx->detections[index];
    if (x) *x = det->x;
    if (y) *y = det->y;
    if (w) *w = det->w;
    if (h) *h = det->h;

    return CIRA_OK;
}

float cira_result_score(cira_ctx* ctx, int index) {
    if (!ctx || index < 0 || index >= ctx->num_detections) return -1.0f;
    return ctx->detections[index].confidence;
}

const char* cira_result_label(cira_ctx* ctx, int index) {
    if (!ctx || index < 0 || index >= ctx->num_detections) return NULL;

    int label_id = ctx->detections[index].label_id;
    if (label_id >= 0 && label_id < ctx->num_labels) {
        return ctx->labels[label_id];
    }
    return "unknown";
}

int cira_status(cira_ctx* ctx) {
    if (!ctx) return CIRA_STATUS_ERROR;
    return ctx->status;
}

const char* cira_error(cira_ctx* ctx) {
    if (!ctx) return NULL;
    if (ctx->error_msg[0] == '\0') return NULL;
    return ctx->error_msg;
}

/* === Streaming API === */

int cira_start_camera(cira_ctx* ctx, int device_id) {
#ifdef CIRA_STREAMING_ENABLED
    if (!ctx) return CIRA_ERROR_INPUT;
    if (ctx->camera_running) return CIRA_OK;
    return camera_start(ctx, device_id);
#else
    (void)ctx;
    (void)device_id;
    return CIRA_ERROR;
#endif
}

int cira_stop_camera(cira_ctx* ctx) {
#ifdef CIRA_STREAMING_ENABLED
    if (!ctx) return CIRA_ERROR_INPUT;
    if (!ctx->camera_running) return CIRA_OK;
    return camera_stop(ctx);
#else
    (void)ctx;
    return CIRA_ERROR;
#endif
}

int cira_start_server(cira_ctx* ctx, int port) {
#ifdef CIRA_STREAMING_ENABLED
    if (!ctx) return CIRA_ERROR_INPUT;
    if (ctx->server_running) return CIRA_OK;
    return server_start(ctx, port);
#else
    (void)ctx;
    (void)port;
    return CIRA_ERROR;
#endif
}

int cira_stop_server(cira_ctx* ctx) {
#ifdef CIRA_STREAMING_ENABLED
    if (!ctx) return CIRA_ERROR_INPUT;
    if (!ctx->server_running) return CIRA_OK;
    return server_stop(ctx);
#else
    (void)ctx;
    return CIRA_ERROR;
#endif
}

float cira_get_fps(cira_ctx* ctx) {
    if (!ctx) return 0.0f;
    return ctx->current_fps;
}
