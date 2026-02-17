/**
 * CiRA Runtime - Internal Header
 *
 * This header exposes internal structures and functions to loader modules.
 * NOT part of the public API - only for internal use by loader implementations.
 *
 * (c) CiRA Robotics / KMITL 2026
 */

#ifndef CIRA_INTERNAL_H
#define CIRA_INTERNAL_H

#include "cira.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

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
    CIRA_FORMAT_NCNN,
    CIRA_FORMAT_SKLEARN
} cira_format_t;

/* Detection result */
typedef struct {
    float x, y, w, h;       /* Bounding box (normalized 0-1) */
    float confidence;       /* Detection confidence */
    int label_id;           /* Label index */
} cira_detection_t;

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
    int frame_size;
    pthread_mutex_t frame_mutex;
};

/* Internal helper functions (defined in cira.c) */

/**
 * Set error message on context.
 */
void cira_set_error(cira_ctx* ctx, const char* fmt, ...);

/**
 * Add a detection result to the context.
 * Coordinates should be normalized (0-1).
 *
 * @param ctx Context handle
 * @param x Bounding box x (top-left, normalized)
 * @param y Bounding box y (top-left, normalized)
 * @param w Bounding box width (normalized)
 * @param h Bounding box height (normalized)
 * @param confidence Detection confidence (0-1)
 * @param label_id Class label index
 * @return 1 if added, 0 if full
 */
int cira_add_detection(cira_ctx* ctx, float x, float y, float w, float h,
                        float confidence, int label_id);

/**
 * Clear all detections from context.
 */
void cira_clear_detections(cira_ctx* ctx);

/**
 * Get the label string for a label ID.
 */
const char* cira_get_label(cira_ctx* ctx, int label_id);

/**
 * Store frame data in context (for streaming).
 * Makes a copy of the data.
 *
 * @param ctx Context handle
 * @param data Frame data (RGB)
 * @param w Width
 * @param h Height
 */
void cira_store_frame(cira_ctx* ctx, const uint8_t* data, int w, int h);

/**
 * Get latest frame data (for streaming).
 * Returns pointer to internal buffer - do not free.
 */
const uint8_t* cira_get_frame(cira_ctx* ctx, int* w, int* h);

#ifdef __cplusplus
}
#endif

#endif /* CIRA_INTERNAL_H */
