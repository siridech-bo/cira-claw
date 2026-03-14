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
#include "signal_features.h"
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

/* Parse JSON string array (for signal feature names) */
static int json_get_string_array(const char* json, const char* key, char out[][128], int max_count) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char* pos = strstr(json, pattern);
    if (!pos) return 0;

    pos += strlen(pattern);
    while (*pos && (*pos == ' ' || *pos == ':' || *pos == '\t')) pos++;
    if (*pos != '[') return 0;
    pos++;

    int count = 0;
    while (*pos && *pos != ']' && count < max_count) {
        while (*pos && (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == ',')) pos++;
        if (*pos == '"') {
            pos++;
            int i = 0;
            while (*pos && *pos != '"' && i < 127) {
                out[count][i++] = *pos++;
            }
            out[count][i] = '\0';
            if (*pos == '"') pos++;
            if (out[count][0] != '\0') count++;
        }
    }
    return count;
}

/* Extract nested JSON object value as a malloc'd string */
static char* json_extract_object(const char* json, const char* key) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char* pos = strstr(json, pattern);
    if (!pos) return NULL;

    pos += strlen(pattern);
    while (*pos && (*pos == ' ' || *pos == ':' || *pos == '\t' || *pos == '\n')) pos++;
    if (*pos != '{') return NULL;

    /* Count braces to find matching close brace */
    const char* start = pos;
    int depth = 0;
    while (*pos) {
        if (*pos == '{') depth++;
        else if (*pos == '}') {
            depth--;
            if (depth == 0) {
                /* Found matching brace */
                int len = pos - start + 1;
                char* obj = (char*)malloc(len + 1);
                if (obj) {
                    memcpy(obj, start, len);
                    obj[len] = '\0';
                }
                return obj;
            }
        }
        pos++;
    }
    return NULL;
}

/* Parse float array from JSON */
static int json_get_float_array(const char* json, const char* key, float* out, int max_count) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char* pos = strstr(json, pattern);
    if (!pos) return 0;

    pos += strlen(pattern);
    while (*pos && (*pos == ' ' || *pos == ':' || *pos == '\t')) pos++;
    if (*pos != '[') return 0;
    pos++;

    int count = 0;
    while (*pos && *pos != ']' && count < max_count) {
        while (*pos && (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == ',')) pos++;
        if (*pos == '-' || (*pos >= '0' && *pos <= '9')) {
            out[count++] = (float)atof(pos);
            while (*pos && *pos != ',' && *pos != ']') pos++;
        }
    }
    return count;
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

    /* Model name */
    if (json_get_string(json, "name", ctx->model_name, sizeof(ctx->model_name))) {
        fprintf(stderr, "Manifest: name=%s\n", ctx->model_name);
    }

    /* Input type (image or signal) */
    char input_type[32] = "image";
    if (json_get_string(json, "input_type", input_type, sizeof(input_type))) {
        fprintf(stderr, "Manifest: input_type=%s\n", input_type);
    }

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

    /* === Signal ingestion fields (Spec C v4) === */

    if (strcmp(input_type, "signal") == 0) {
        /* Parse nested signal model manifest */

        /* --- Parse normalization section --- */
        char* norm_obj = json_extract_object(json, "normalization");
        if (!norm_obj) {
            cira_set_error(ctx, "Signal model missing 'normalization' section");
            free(json);
            return CIRA_ERROR_INPUT;
        }

        char sensor_columns[16][128];
        int num_channels = json_get_string_array(norm_obj, "sensor_columns", sensor_columns, 16);
        if (num_channels == 0) {
            cira_set_error(ctx, "Signal model missing 'sensor_columns'");
            free(norm_obj);
            free(json);
            return CIRA_ERROR_INPUT;
        }

        float channel_min[16];
        float channel_max[16];
        int min_count = json_get_float_array(norm_obj, "channel_min", channel_min, 16);
        int max_count = json_get_float_array(norm_obj, "channel_max", channel_max, 16);

        if (min_count != num_channels || max_count != num_channels) {
            cira_set_error(ctx, "Signal model channel_min/max count mismatch");
            free(norm_obj);
            free(json);
            return CIRA_ERROR_INPUT;
        }
        free(norm_obj);

        /* --- Parse windowing section --- */
        char* window_obj = json_extract_object(json, "windowing");
        int window_size = 128;  /* default */
        int stride = 64;        /* default */

        if (window_obj) {
            json_get_int(window_obj, "window_size", &window_size);
            json_get_int(window_obj, "stride", &stride);
            free(window_obj);
        }

        /* --- Parse feature_extraction section --- */
        char* feat_extract_obj = json_extract_object(json, "feature_extraction");
        float sample_rate = 100.0f;  /* default */

        if (feat_extract_obj) {
            json_get_float(feat_extract_obj, "sampling_rate_hz", &sample_rate);
            free(feat_extract_obj);
        }

        /* --- Parse feature_selection section --- */
        char* feat_select_obj = json_extract_object(json, "feature_selection");
        ctx->signal_num_selected = 0;

        if (feat_select_obj) {
            ctx->signal_num_selected = json_get_string_array(feat_select_obj, "selected_features",
                                                              ctx->signal_selected_features, 256);
            free(feat_select_obj);
        }

        if (ctx->signal_num_selected == 0) {
            cira_set_error(ctx, "Signal model missing 'selected_features'");
            free(json);
            return CIRA_ERROR_INPUT;
        }

        /* --- Parse output section --- */
        char* output_obj = json_extract_object(json, "output");
        strcpy(ctx->signal_output_format, "label_prob");  /* default */
        ctx->signal_anomaly_threshold = 0.5f;              /* default */

        if (output_obj) {
            json_get_string(output_obj, "format", ctx->signal_output_format,
                           sizeof(ctx->signal_output_format));
            json_get_float(output_obj, "anomaly_threshold", &ctx->signal_anomaly_threshold);
            free(output_obj);
        }

        /* --- Parse signal class labels --- */
        ctx->signal_num_labels = json_get_string_array(json, "labels",
                                                        ctx->signal_labels, CIRA_MAX_LABELS);
        if (ctx->signal_num_labels > 0) {
            fprintf(stderr, "Signal labels loaded: %d classes\n", ctx->signal_num_labels);
        }

        /* --- Create signal buffer --- */
        const char* channel_names[16];
        for (int i = 0; i < num_channels; i++) {
            channel_names[i] = sensor_columns[i];
        }

        ctx->signal_buffer = signal_buffer_create(
            num_channels, channel_names,
            channel_min, channel_max,
            window_size, stride, sample_rate
        );

        if (!ctx->signal_buffer) {
            cira_set_error(ctx, "Failed to allocate signal buffer");
            free(json);
            return CIRA_ERROR_MEMORY;
        }

        fprintf(stderr, "Signal buffer created: %d channels, window=%d, stride=%d, sr=%.1f Hz\n",
                num_channels, window_size, stride, sample_rate);
        fprintf(stderr, "  Features selected: %d\n", ctx->signal_num_selected);
        fprintf(stderr, "  Output format: %s\n", ctx->signal_output_format);
        if (strcmp(ctx->signal_output_format, "reconstruction") == 0 ||
            strcmp(ctx->signal_output_format, "anomaly_score") == 0) {
            fprintf(stderr, "  Anomaly threshold: %.3f\n", ctx->signal_anomaly_threshold);
        }

    } else {
        /* Image model - only parse signal fields if no signal config exists yet */
        /* This preserves signal config when loading image model after signal model */
        if (ctx->signal_num_selected == 0) {
            ctx->signal_num_selected = json_get_string_array(json, "signal_selected_features",
                                                              ctx->signal_selected_features, 256);

            if (json_get_string(json, "signal_output_format", ctx->signal_output_format,
                                sizeof(ctx->signal_output_format))) {
                /* parsed */
            } else {
                strcpy(ctx->signal_output_format, "label_prob");
            }

            float anomaly_thresh = 0;
            if (json_get_float(json, "signal_anomaly_threshold", &anomaly_thresh) && anomaly_thresh > 0) {
                ctx->signal_anomaly_threshold = anomaly_thresh;
            } else {
                ctx->signal_anomaly_threshold = 0.5f;
            }
        }
    }

    free(json);
    return 1;
}

/* Detect input_type from manifest file */
static int detect_input_type_from_manifest(const char* config_path, char* input_type, size_t input_type_size) {
    if (!is_directory(config_path)) {
        /* Not a directory, default to image */
        strncpy(input_type, "image", input_type_size - 1);
        input_type[input_type_size - 1] = '\0';
        return 1;
    }

    char manifest_path[1024];
    snprintf(manifest_path, sizeof(manifest_path), "%s/cira_model.json", config_path);

    if (!file_exists(manifest_path)) {
        /* No manifest, default to image */
        strncpy(input_type, "image", input_type_size - 1);
        input_type[input_type_size - 1] = '\0';
        return 1;
    }

    /* Read manifest file */
    FILE* f = fopen(manifest_path, "r");
    if (!f) {
        strncpy(input_type, "image", input_type_size - 1);
        input_type[input_type_size - 1] = '\0';
        return 0;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* json = (char*)malloc(size + 1);
    if (!json) {
        fclose(f);
        strncpy(input_type, "image", input_type_size - 1);
        input_type[input_type_size - 1] = '\0';
        return 0;
    }

    fread(json, 1, size, f);
    json[size] = '\0';
    fclose(f);

    /* Extract input_type */
    strncpy(input_type, "image", input_type_size - 1);
    input_type[input_type_size - 1] = '\0';
    json_get_string(json, "input_type", input_type, input_type_size);

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

    /* Initialize model slots */
    for (int i = 0; i < MODEL_SLOT_COUNT; i++) {
        ctx->formats[i] = CIRA_FORMAT_UNKNOWN;
        ctx->model_handles[i] = NULL;
        ctx->model_paths[i][0] = '\0';
        ctx->model_names[i][0] = '\0';
        pthread_mutex_init(&ctx->model_slot_mutexes[i], NULL);
    }

    /* Initialize legacy fields (backward compatibility - map to image slot) */
    ctx->format = CIRA_FORMAT_UNKNOWN;
    ctx->model_handle = NULL;
    ctx->model_path[0] = '\0';
    ctx->model_name[0] = '\0';

    ctx->confidence_threshold = 0.5f;
    ctx->nms_threshold = 0.4f;
    ctx->yolo_version = YOLO_VERSION_AUTO;
    ctx->input_w = 416;
    ctx->input_h = 416;

    pthread_mutex_init(&ctx->result_mutex, NULL);
    pthread_mutex_init(&ctx->frame_mutex, NULL);
    pthread_mutex_init(&ctx->model_mutex, NULL);
    pthread_mutex_init(&ctx->frame_file_mutex, NULL);
    ctx->model_swapping = 0;
    ctx->current_camera = -1;  /* No camera active initially */
    ctx->frame_sequence = 0;
    ctx->frame_file_path[0] = '\0';

    /* Initialize empty result JSON */
    strcpy(ctx->result_json, "{\"detections\":[],\"count\":0}");
    strcpy(ctx->signal_result_json, "{\"status\":\"no_prediction_yet\"}");
    ctx->signal_result_valid = 0;

    /* Initialize signal labels (separate from image labels) */
    ctx->signal_num_labels = 0;
    ctx->signal_labels[0][0] = '\0';

    /* Initialize cumulative statistics */
    ctx->total_detections = 0;
    ctx->total_frames = 0;
    ctx->start_time = time(NULL);
    memset(ctx->detections_by_label, 0, sizeof(ctx->detections_by_label));

    /* Initialize signal ingestion fields (Spec C) */
    ctx->signal_buffer = NULL;
    ctx->signal_num_selected = 0;
    strcpy(ctx->signal_output_format, "label_prob");
    ctx->signal_anomaly_threshold = 0.5f;

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

    /* Unload all model slots */
    for (int slot = 0; slot < MODEL_SLOT_COUNT; slot++) {
        switch (ctx->formats[slot]) {
#ifdef CIRA_DARKNET_ENABLED
            case CIRA_FORMAT_DARKNET:
                /* Note: Darknet unload would need slot parameter */
                break;
#endif
#ifdef CIRA_ONNX_ENABLED
            case CIRA_FORMAT_ONNX:
                /* Note: ONNX unload would need slot parameter */
                break;
#endif
#ifdef CIRA_TRT_ENABLED
            case CIRA_FORMAT_TENSORRT:
                /* Note: TensorRT unload would need slot parameter */
                break;
#endif
#ifdef CIRA_NCNN_ENABLED
            case CIRA_FORMAT_NCNN:
                /* Note: NCNN unload would need slot parameter */
                break;
#endif
            default:
                break;
        }
    }

    /* Also unload legacy model slot for backward compatibility */
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

    /* Destroy signal buffer (Spec C) */
    if (ctx->signal_buffer) {
        signal_buffer_destroy(ctx->signal_buffer);
        ctx->signal_buffer = NULL;
    }

    /* Destroy per-slot mutexes */
    for (int i = 0; i < MODEL_SLOT_COUNT; i++) {
        pthread_mutex_destroy(&ctx->model_slot_mutexes[i]);
    }

    pthread_mutex_destroy(&ctx->result_mutex);
    pthread_mutex_destroy(&ctx->frame_mutex);
    pthread_mutex_destroy(&ctx->model_mutex);
    pthread_mutex_destroy(&ctx->frame_file_mutex);

    /* Clean up temp frame file */
    if (ctx->frame_file_path[0] != '\0') {
        unlink(ctx->frame_file_path);
    }

    free(ctx);
}

void cira_sync_legacy_fields(cira_ctx* ctx) {
    if (!ctx) return;

    /* Sync legacy fields to image slot (MODEL_SLOT_IMAGE = 0) */
    ctx->format = ctx->formats[MODEL_SLOT_IMAGE];
    ctx->model_handle = ctx->model_handles[MODEL_SLOT_IMAGE];

    /* Copy strings */
    strncpy(ctx->model_path, ctx->model_paths[MODEL_SLOT_IMAGE], sizeof(ctx->model_path) - 1);
    ctx->model_path[sizeof(ctx->model_path) - 1] = '\0';

    strncpy(ctx->model_name, ctx->model_names[MODEL_SLOT_IMAGE], sizeof(ctx->model_name) - 1);
    ctx->model_name[sizeof(ctx->model_name) - 1] = '\0';
}

int cira_load_slot(cira_ctx* ctx, const char* config_path, model_slot_t slot) {
    if (!ctx || !config_path) return CIRA_ERROR_INPUT;
    if (slot < 0 || slot >= MODEL_SLOT_COUNT) {
        cira_set_error(ctx, "Invalid model slot: %d", slot);
        return CIRA_ERROR_INPUT;
    }

    fprintf(stderr, "Loading model into slot %d (%s)...\n", slot,
            slot == MODEL_SLOT_IMAGE ? "image" : "signal");

    ctx->status = CIRA_STATUS_LOADING;

    /* Signal that model swap is in progress (camera thread will pause inference) */
    ctx->model_swapping = 1;

    /* Lock global model mutex to prevent conflicts */
    pthread_mutex_lock(&ctx->model_mutex);

    /* Lock the specific slot mutex */
    pthread_mutex_lock(&ctx->model_slot_mutexes[slot]);

    /* If slot already has a model, unload it first */
    if (ctx->formats[slot] != CIRA_FORMAT_UNKNOWN && ctx->model_handles[slot] != NULL) {
        cira_format_t old_format = ctx->formats[slot];
        void* old_handle = ctx->model_handles[slot];

        fprintf(stderr, "Unloading previous model from slot %d (format=%d)...\n", slot, old_format);

        /* Temporarily point legacy fields at slot's model for unload functions */
        ctx->format = old_format;
        ctx->model_handle = old_handle;

        /* Call appropriate unload function */
        switch (old_format) {
#ifdef CIRA_ONNX_ENABLED
            case CIRA_FORMAT_ONNX:
                onnx_unload(ctx);
                break;
#endif
#ifdef CIRA_NCNN_ENABLED
            case CIRA_FORMAT_NCNN:
                ncnn_unload(ctx);
                break;
#endif
#ifdef CIRA_DARKNET_ENABLED
            case CIRA_FORMAT_DARKNET:
                darknet_unload(ctx);
                break;
#endif
            default:
                /* TensorRT or unknown - just clear */
                break;
        }

        /* Clear the slot */
        ctx->formats[slot] = CIRA_FORMAT_UNKNOWN;
        ctx->model_handles[slot] = NULL;
        ctx->model_paths[slot][0] = '\0';
        ctx->model_names[slot][0] = '\0';

        fprintf(stderr, "Unloaded previous model from slot %d\n", slot);
    }

    /* Unlock slot mutex - we'll re-lock it after loading */
    pthread_mutex_unlock(&ctx->model_slot_mutexes[slot]);

    /* Detect model format */
    cira_format_t format = detect_format(config_path);
    if (format == CIRA_FORMAT_UNKNOWN) {
        pthread_mutex_unlock(&ctx->model_mutex);
        ctx->model_swapping = 0;
        cira_set_error(ctx, "Unknown model format: %s", config_path);
        return CIRA_ERROR_MODEL;
    }

    /* Store path in slot */
    strncpy(ctx->model_paths[slot], config_path, sizeof(ctx->model_paths[slot]) - 1);
    ctx->model_paths[slot][sizeof(ctx->model_paths[slot]) - 1] = '\0';
    ctx->model_names[slot][0] = '\0';  /* Reset model name before loading manifest */

    /* Try to load manifest and labels */
    if (is_directory(config_path)) {
        /* Load manifest first (sets yolo_version, input size, thresholds) */
        /* Note: load_model_manifest currently uses ctx->model_path and ctx->model_name */
        /* For now, temporarily sync to legacy fields for manifest loading */
        strncpy(ctx->model_path, config_path, sizeof(ctx->model_path) - 1);
        load_model_manifest(ctx, config_path);

        /* Copy model name back to slot */
        strncpy(ctx->model_names[slot], ctx->model_name, sizeof(ctx->model_names[slot]) - 1);

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
    /* Note: Current loaders use ctx->model_handle and ctx->format */
    /* We need to temporarily set these for the loader, then restore */
    cira_format_t saved_format = ctx->format;
    void* saved_handle = ctx->model_handle;
    char saved_path[1024];
    strncpy(saved_path, ctx->model_path, sizeof(saved_path) - 1);

    ctx->format = CIRA_FORMAT_UNKNOWN;
    ctx->model_handle = NULL;
    strncpy(ctx->model_path, config_path, sizeof(ctx->model_path) - 1);

    int result;
    switch (format) {
#ifdef CIRA_ONNX_ENABLED
        case CIRA_FORMAT_ONNX:
            result = onnx_load(ctx, config_path);
            break;
#endif
#ifdef CIRA_DARKNET_ENABLED
        case CIRA_FORMAT_DARKNET:
            result = darknet_load(ctx, config_path);
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
            pthread_mutex_unlock(&ctx->model_mutex);
            ctx->model_swapping = 0;
            cira_set_error(ctx, "Model format not supported in this build");
            return CIRA_ERROR_MODEL;
    }

    /* Lock slot mutex to update slot */
    pthread_mutex_lock(&ctx->model_slot_mutexes[slot]);

    if (result == CIRA_OK) {
        /* Save loaded model to slot */
        fprintf(stderr, "[DEBUG] Before slot write: ctx->format=%d, ctx->model_handle=%p\n", ctx->format, ctx->model_handle);

        ctx->formats[slot] = ctx->format;
        ctx->model_handles[slot] = ctx->model_handle;
        strncpy(ctx->model_paths[slot], ctx->model_path, sizeof(ctx->model_paths[slot]) - 1);
        strncpy(ctx->model_names[slot], ctx->model_name, sizeof(ctx->model_names[slot]) - 1);

        fprintf(stderr, "[DEBUG] After slot write: slot=%d, formats[%d]=%d, handles[%d]=%p\n",
                slot, slot, ctx->formats[slot], slot, ctx->model_handles[slot]);
        fprintf(stderr, "Model loaded successfully into slot %d\n", slot);

        /* For IMAGE slot, keep legacy fields in sync (backward compatibility) */
        /* For SIGNAL slot, restore legacy fields to previous values */
        if (slot != MODEL_SLOT_IMAGE) {
            ctx->format = saved_format;
            ctx->model_handle = saved_handle;
            strncpy(ctx->model_path, saved_path, sizeof(ctx->model_path) - 1);
        }
        /* Note: If slot == IMAGE, legacy fields already point to the loaded model */

        /* Update status */
        ctx->status = CIRA_STATUS_READY;
    } else {
        /* Restore legacy fields on failure */
        ctx->format = saved_format;
        ctx->model_handle = saved_handle;
        strncpy(ctx->model_path, saved_path, sizeof(ctx->model_path) - 1);
    }

    pthread_mutex_unlock(&ctx->model_slot_mutexes[slot]);

    /* Unlock and clear swapping flag (inference can resume) */
    pthread_mutex_unlock(&ctx->model_mutex);
    ctx->model_swapping = 0;

    fprintf(stderr, "Model swap complete, inference can resume\n");
    return result;
}

int cira_load(cira_ctx* ctx, const char* config_path) {
    if (!ctx || !config_path) return CIRA_ERROR_INPUT;

    /* Auto-detect slot based on input_type from manifest */
    char input_type[32];
    detect_input_type_from_manifest(config_path, input_type, sizeof(input_type));

    model_slot_t slot;
    if (strcmp(input_type, "signal") == 0) {
        slot = MODEL_SLOT_SIGNAL;
        fprintf(stderr, "Auto-detected signal model, loading into signal slot\n");
    } else {
        slot = MODEL_SLOT_IMAGE;
        fprintf(stderr, "Auto-detected image model, loading into image slot\n");
    }

    /* Load into the appropriate slot */
    int result = cira_load_slot(ctx, config_path, slot);

    /* For backward compatibility, also update legacy fields if loading image model */
    if (result == CIRA_OK && slot == MODEL_SLOT_IMAGE) {
        cira_sync_legacy_fields(ctx);
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

/* === Signal API (Spec C) === */

int cira_feed_signal(cira_ctx* ctx, int channel_idx, float value) {
    if (!ctx || !ctx->signal_buffer) return CIRA_ERROR_INPUT;
    return signal_buffer_feed(ctx->signal_buffer, channel_idx, value);
}

int cira_feed_signal_batch(cira_ctx* ctx, int channel_idx,
                           const float* values, int count) {
    if (!ctx || !ctx->signal_buffer || !values) return CIRA_ERROR_INPUT;

    for (int i = 0; i < count; i++) {
        int ret = signal_buffer_feed(ctx->signal_buffer, channel_idx, values[i]);
        if (ret != 0) return CIRA_ERROR;
    }

    return CIRA_OK;
}

int cira_signal_ready(cira_ctx* ctx) {
    if (!ctx || !ctx->signal_buffer) return -1;
    return signal_buffer_ready(ctx->signal_buffer) ? 1 : 0;
}

int cira_predict_signal(cira_ctx* ctx) {
    if (!ctx || !ctx->signal_buffer) return CIRA_ERROR_INPUT;
    if (ctx->status != CIRA_STATUS_READY) return CIRA_ERROR;

    /* Clear any stale error from other threads (e.g., camera thread) */
    ctx->error_msg[0] = '\0';

    /* Debug: Log slot status */
    fprintf(stderr, "[SIGNAL] Checking slot: format=%d, handle=%p\n",
            SLOT_FORMAT(ctx, MODEL_SLOT_SIGNAL), SLOT_HANDLE(ctx, MODEL_SLOT_SIGNAL));

    /* Check SIGNAL slot for signal prediction */
    if (SLOT_FORMAT(ctx, MODEL_SLOT_SIGNAL) == CIRA_FORMAT_UNKNOWN ||
        SLOT_HANDLE(ctx, MODEL_SLOT_SIGNAL) == NULL) {
        cira_set_error(ctx, "No signal model loaded in SIGNAL slot");
        return CIRA_ERROR_MODEL;
    }

    /* Only ONNX models support signal prediction currently */
    if (SLOT_FORMAT(ctx, MODEL_SLOT_SIGNAL) != CIRA_FORMAT_ONNX) {
        cira_set_error(ctx, "Signal prediction only supported for ONNX models (got format=%d)",
                       SLOT_FORMAT(ctx, MODEL_SLOT_SIGNAL));
        return CIRA_ERROR_MODEL;
    }

    /* Check if buffer is ready */
    if (!signal_buffer_ready(ctx->signal_buffer)) {
        cira_set_error(ctx, "Signal buffer not ready for inference");
        return CIRA_ERROR_INPUT;
    }

    pthread_mutex_lock(&ctx->result_mutex);

    /* Forward to ONNX loader's signal prediction */
    #ifdef CIRA_ONNX_ENABLED
    extern int onnx_predict_tensor(cira_ctx* ctx, const float* tensor, int size);

    /* Get feature vector from signal buffer */
    int num_channels = ctx->signal_buffer->num_channels;
    int window_size = ctx->signal_buffer->window_size;
    int num_selected = ctx->signal_num_selected;

    /* Allocate feature vector: num_channels * num_selected_features */
    int feature_size = num_channels * num_selected;
    float* features = (float*)malloc(feature_size * sizeof(float));
    if (!features) {
        pthread_mutex_unlock(&ctx->result_mutex);
        cira_set_error(ctx, "Failed to allocate feature buffer");
        return CIRA_ERROR_MEMORY;
    }

    /* Extract features for each channel */
    for (int ch = 0; ch < num_channels; ch++) {
        const float* window = signal_buffer_get_channel_window(ctx->signal_buffer, ch);
        if (!window) {
            free(features);
            pthread_mutex_unlock(&ctx->result_mutex);
            cira_set_error(ctx, "Failed to get signal window");
            return CIRA_ERROR;
        }

        /* Compute all 46 features */
        float all_features[46];
        signal_compute_features(window, window_size,
                                ctx->signal_buffer->sample_rate_hz, all_features);

        /* Select only the requested features */
        for (int i = 0; i < num_selected; i++) {
            int feature_idx = signal_feature_index(ctx->signal_selected_features[i]);
            if (feature_idx >= 0 && feature_idx < 46) {
                features[ch * num_selected + i] = all_features[feature_idx];
            } else {
                features[ch * num_selected + i] = 0.0f;
            }
        }
    }

    /* Run inference */
    fprintf(stderr, "[SIGNAL] Calling onnx_predict_tensor with feature_size=%d\n", feature_size);
    int result = onnx_predict_tensor(ctx, features, feature_size);
    fprintf(stderr, "[SIGNAL] onnx_predict_tensor returned: %d, error_msg='%s'\n",
            result, ctx->error_msg[0] ? ctx->error_msg : "(none)");

    free(features);

    /* Advance buffer by stride */
    signal_buffer_advance(ctx->signal_buffer);

    pthread_mutex_unlock(&ctx->result_mutex);

    return result;
    #else
    pthread_mutex_unlock(&ctx->result_mutex);
    cira_set_error(ctx, "ONNX support not enabled");
    return CIRA_ERROR_MODEL;
    #endif
}

int cira_signal_stats(cira_ctx* ctx, int channel_idx,
                      float* out_rms, float* out_peak, float* out_mean) {
    if (!ctx || !ctx->signal_buffer) return CIRA_ERROR_INPUT;

    float rms = signal_buffer_rms(ctx->signal_buffer, channel_idx);
    float peak = signal_buffer_peak(ctx->signal_buffer, channel_idx);
    float mean = signal_buffer_mean(ctx->signal_buffer, channel_idx);

    if (out_rms) *out_rms = rms;
    if (out_peak) *out_peak = peak;
    if (out_mean) *out_mean = mean;

    return CIRA_OK;
}
