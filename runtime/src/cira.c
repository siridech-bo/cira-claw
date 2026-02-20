/**
 * CiRA Runtime - Main API Implementation
 *
 * This file implements the public API defined in cira.h.
 * It manages the context lifecycle and dispatches to format-specific loaders.
 *
 * (c) CiRA Robotics / KMITL 2026
 */

#include "cira.h"
#include "cira_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

/* Version string */
#define CIRA_VERSION_STRING "1.0.0"

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

#ifdef CIRA_NCNN_ENABLED
extern int ncnn_load(cira_ctx* ctx, const char* model_path);
extern void ncnn_unload(cira_ctx* ctx);
extern int ncnn_predict(cira_ctx* ctx, const uint8_t* data, int w, int h, int channels);
#endif

#ifdef CIRA_STREAMING_ENABLED
extern int camera_start(cira_ctx* ctx, int device_id);
extern int camera_stop(cira_ctx* ctx);
extern int server_start(cira_ctx* ctx, int port);
extern int server_stop(cira_ctx* ctx);
#endif

/* === Internal Helper Functions (exported via cira_internal.h) === */

void cira_set_error(cira_ctx* ctx, const char* fmt, ...) {
    if (!ctx) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(ctx->error_msg, CIRA_MAX_ERROR_LEN, fmt, args);
    va_end(args);
    ctx->status = CIRA_STATUS_ERROR;
}

int cira_add_detection(cira_ctx* ctx, float x, float y, float w, float h,
                        float confidence, int label_id) {
    if (!ctx || ctx->num_detections >= CIRA_MAX_DETECTIONS) return 0;

    cira_detection_t* det = &ctx->detections[ctx->num_detections];
    det->x = x;
    det->y = y;
    det->w = w;
    det->h = h;
    det->confidence = confidence;
    det->label_id = label_id;
    ctx->num_detections++;

    /* Update cumulative statistics */
    ctx->total_detections++;
    if (label_id >= 0 && label_id < CIRA_MAX_LABELS) {
        ctx->detections_by_label[label_id]++;
    }

    return 1;
}

void cira_clear_detections(cira_ctx* ctx) {
    if (!ctx) return;
    ctx->num_detections = 0;
}

const char* cira_get_label(cira_ctx* ctx, int label_id) {
    if (!ctx || label_id < 0 || label_id >= ctx->num_labels) {
        return "unknown";
    }
    return ctx->labels[label_id];
}

void cira_store_frame(cira_ctx* ctx, const uint8_t* data, int w, int h) {
    if (!ctx || !data) return;

    pthread_mutex_lock(&ctx->frame_mutex);

    int size = w * h * 3;

    /* Reallocate if size changed */
    if (ctx->frame_size != size) {
        if (ctx->frame_buffer) {
            free(ctx->frame_buffer);
        }
        ctx->frame_buffer = (uint8_t*)malloc(size);
        ctx->frame_size = size;
    }

    if (ctx->frame_buffer) {
        memcpy(ctx->frame_buffer, data, size);
        ctx->frame_w = w;
        ctx->frame_h = h;
    }

    pthread_mutex_unlock(&ctx->frame_mutex);
}

const uint8_t* cira_get_frame(cira_ctx* ctx, int* w, int* h) {
    if (!ctx || !ctx->frame_buffer) {
        if (w) *w = 0;
        if (h) *h = 0;
        return NULL;
    }

    if (w) *w = ctx->frame_w;
    if (h) *h = ctx->frame_h;
    return ctx->frame_buffer;
}

/* === Private Helper Functions === */

/* Check if path is a directory */
static int is_directory(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

/* Check if file exists */
static int file_exists(const char* path) {
    return access(path, F_OK) == 0;
}

/* Find file with extension in directory */
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

/* Load labels from file */
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
            /* Use memcpy since we already know the length (avoids strncpy truncation warning) */
            size_t copy_len = len < CIRA_MAX_LABEL_LEN - 1 ? len : CIRA_MAX_LABEL_LEN - 1;
            memcpy(ctx->labels[ctx->num_labels], line, copy_len);
            ctx->labels[ctx->num_labels][copy_len] = '\0';
            ctx->num_labels++;
        }
    }

    fclose(f);
    return ctx->num_labels;
}

/* Simple JSON value extraction (no external deps) */
static int json_get_string(const char* json, const char* key, char* out, size_t out_size) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char* pos = strstr(json, pattern);
    if (!pos) return 0;

    pos += strlen(pattern);
    while (*pos && (*pos == ' ' || *pos == ':' || *pos == '\t')) pos++;
    if (*pos != '"') return 0;
    pos++;

    size_t i = 0;
    while (*pos && *pos != '"' && i < out_size - 1) {
        out[i++] = *pos++;
    }
    out[i] = '\0';
    return 1;
}

static int json_get_int(const char* json, const char* key, int* out) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char* pos = strstr(json, pattern);
    if (!pos) return 0;

    pos += strlen(pattern);
    while (*pos && (*pos == ' ' || *pos == ':' || *pos == '\t')) pos++;

    *out = atoi(pos);
    return 1;
}

static int json_get_float(const char* json, const char* key, float* out) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char* pos = strstr(json, pattern);
    if (!pos) return 0;

    pos += strlen(pattern);
    while (*pos && (*pos == ' ' || *pos == ':' || *pos == '\t')) pos++;

    *out = (float)atof(pos);
    return 1;
}

/* Load model manifest (cira_model.json) */
static int load_model_manifest(cira_ctx* ctx, const char* model_dir) {
    char manifest_path[1024];
    snprintf(manifest_path, sizeof(manifest_path), "%s/cira_model.json", model_dir);

    FILE* f = fopen(manifest_path, "r");
    if (!f) {
        /* No manifest - use auto-detection (not an error) */
        ctx->yolo_version = YOLO_VERSION_AUTO;
        return 0;
    }

    /* Read entire file */
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size <= 0 || size > 65536) {
        fclose(f);
        return 0;
    }

    char* json = (char*)malloc(size + 1);
    if (!json) {
        fclose(f);
        return 0;
    }

    size_t read_size = fread(json, 1, size, f);
    fclose(f);
    json[read_size] = '\0';

    /* Parse manifest fields */
    char version_str[32] = {0};
    if (json_get_string(json, "yolo_version", version_str, sizeof(version_str))) {
        ctx->yolo_version = yolo_parse_version(version_str);
        fprintf(stderr, "Manifest: yolo_version=%s (%s)\n",
                version_str, yolo_version_name(ctx->yolo_version));
    }

    int input_size = 0;
    if (json_get_int(json, "input_size", &input_size) && input_size > 0) {
        ctx->input_w = input_size;
        ctx->input_h = input_size;
        fprintf(stderr, "Manifest: input_size=%d\n", input_size);
    }

    int input_w = 0, input_h = 0;
    if (json_get_int(json, "input_width", &input_w) && input_w > 0) {
        ctx->input_w = input_w;
    }
    if (json_get_int(json, "input_height", &input_h) && input_h > 0) {
        ctx->input_h = input_h;
    }

    float conf = 0;
    if (json_get_float(json, "confidence_threshold", &conf) && conf > 0) {
        ctx->confidence_threshold = conf;
        fprintf(stderr, "Manifest: confidence_threshold=%.2f\n", conf);
    }

    float nms = 0;
    if (json_get_float(json, "nms_threshold", &nms) && nms > 0) {
        ctx->nms_threshold = nms;
        fprintf(stderr, "Manifest: nms_threshold=%.2f\n", nms);
    }

    int num_classes = 0;
    if (json_get_int(json, "num_classes", &num_classes) && num_classes > 0) {
        fprintf(stderr, "Manifest: num_classes=%d\n", num_classes);
        /* num_classes is used by loaders, not stored in ctx directly */
    }

    free(json);
    return 1;
}

/* Detect model format from path */
static cira_format_t detect_format(const char* path) {
    if (is_directory(path)) {
        /* Check for Darknet files in directory */
        char buf[1024];
        if (find_file_with_ext(path, ".weights", buf, sizeof(buf)) &&
            find_file_with_ext(path, ".cfg", buf, sizeof(buf))) {
            return CIRA_FORMAT_DARKNET;
        }
        /* Check for NCNN files (primary non-CUDA path) */
        if (find_file_with_ext(path, ".param", buf, sizeof(buf)) &&
            find_file_with_ext(path, ".bin", buf, sizeof(buf))) {
            return CIRA_FORMAT_NCNN;
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
            if (strcmp(ext, ".param") == 0 || strcmp(ext, ".bin") == 0) {
                return CIRA_FORMAT_NCNN;
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

/* Build JSON result string */
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
    ctx->yolo_version = YOLO_VERSION_AUTO;
    ctx->input_w = 416;
    ctx->input_h = 416;

    pthread_mutex_init(&ctx->result_mutex, NULL);
    pthread_mutex_init(&ctx->frame_mutex, NULL);

    /* Initialize empty result JSON */
    strcpy(ctx->result_json, "{\"detections\":[],\"count\":0}");

    /* Initialize cumulative statistics */
    ctx->total_detections = 0;
    ctx->total_frames = 0;
    ctx->start_time = time(NULL);
    memset(ctx->detections_by_label, 0, sizeof(ctx->detections_by_label));

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
#ifdef CIRA_NCNN_ENABLED
        case CIRA_FORMAT_NCNN:
            ncnn_unload(ctx);
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
#ifdef CIRA_NCNN_ENABLED
            case CIRA_FORMAT_NCNN:
                ncnn_unload(ctx);
                break;
#endif
            default:
                break;
        }
        ctx->format = CIRA_FORMAT_UNKNOWN;
        ctx->model_handle = NULL;
    }

    /* Detect model format */
    cira_format_t format = detect_format(config_path);
    if (format == CIRA_FORMAT_UNKNOWN) {
        cira_set_error(ctx, "Unknown model format: %s", config_path);
        return CIRA_ERROR_MODEL;
    }

    strncpy(ctx->model_path, config_path, sizeof(ctx->model_path) - 1);

    /* Initialize YOLO version to auto-detect */
    ctx->yolo_version = YOLO_VERSION_AUTO;

    /* Try to load manifest and labels */
    if (is_directory(config_path)) {
        /* Load manifest first (sets yolo_version, input size, thresholds) */
        load_model_manifest(ctx, config_path);
        char label_path[1024];
        snprintf(label_path, sizeof(label_path), "%s/obj.names", config_path);
        if (!file_exists(label_path)) {
            snprintf(label_path, sizeof(label_path), "%s/labels.txt", config_path);
        }
        if (file_exists(label_path)) {
            int n = load_labels(ctx, label_path);
            fprintf(stderr, "Loaded %d labels from %s\n", n, label_path);
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
#ifdef CIRA_NCNN_ENABLED
        case CIRA_FORMAT_NCNN:
            result = ncnn_load(ctx, config_path);
            break;
#endif
        default:
            cira_set_error(ctx, "Model format not supported in this build");
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
        cira_set_error(ctx, "Only 3-channel images supported");
        return CIRA_ERROR_INPUT;
    }
    if (ctx->format == CIRA_FORMAT_UNKNOWN || !ctx->model_handle) {
        cira_set_error(ctx, "No model loaded");
        return CIRA_ERROR_MODEL;
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
#ifdef CIRA_NCNN_ENABLED
        case CIRA_FORMAT_NCNN:
            result = ncnn_predict(ctx, data, w, h, channels);
            break;
#endif
        default:
            result = CIRA_ERROR_MODEL;
            break;
    }

    if (result == CIRA_OK) {
        build_result_json(ctx, w, h);
        ctx->total_frames++;
    }

    pthread_mutex_unlock(&ctx->result_mutex);
    return result;
}

int cira_predict_sensor(cira_ctx* ctx, const float* values, int count) {
    if (!ctx || !values || count <= 0) return CIRA_ERROR_INPUT;

    /* Sensor prediction is only supported for sklearn models */
    if (ctx->format != CIRA_FORMAT_SKLEARN) {
        cira_set_error(ctx, "Sensor prediction requires sklearn model");
        return CIRA_ERROR_MODEL;
    }

    /* TODO: Implement sklearn prediction */
    cira_set_error(ctx, "Sklearn prediction not yet implemented");
    return CIRA_ERROR;
}

int cira_predict_batch(cira_ctx* ctx, const uint8_t** images, int count, int w, int h, int channels) {
    if (!ctx || !images || count <= 0) return CIRA_ERROR_INPUT;

    /* For now, just process images one by one */
    /* TODO: Implement true batch processing for supported backends */
    for (int i = 0; i < count; i++) {
        int result = cira_predict_image(ctx, images[i], w, h, channels);
        if (result != CIRA_OK) return result;
    }

    return CIRA_OK;
}

const char* cira_result_json(cira_ctx* ctx) {
    if (!ctx) return "{\"detections\":[],\"count\":0}";
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
