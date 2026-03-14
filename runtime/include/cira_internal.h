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
#include "yolo_decoder.h"
#include "signal_buffer.h"
#include <pthread.h>
#include <time.h>

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

/* Model format types (ordered by priority) */
typedef enum {
    CIRA_FORMAT_UNKNOWN = 0,
    CIRA_FORMAT_DARKNET,
    CIRA_FORMAT_NCNN,       /* Primary non-CUDA path */
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

/* Context structure (internal) */
struct cira_ctx {
    /* Status */
    int status;
    char error_msg[CIRA_MAX_ERROR_LEN];

    /* Model slots (dual-model support) */
    cira_format_t formats[MODEL_SLOT_COUNT];
    char model_paths[MODEL_SLOT_COUNT][1024];
    char model_names[MODEL_SLOT_COUNT][256];
    void* model_handles[MODEL_SLOT_COUNT];  /* Format-specific model data */
    pthread_mutex_t model_slot_mutexes[MODEL_SLOT_COUNT];  /* Per-slot locking */

    /* Legacy single-model fields (for backward compatibility) */
    /* These map to image slot (MODEL_SLOT_IMAGE) */
    cira_format_t format;           /* Alias for formats[MODEL_SLOT_IMAGE] */
    char model_path[1024];          /* Alias for model_paths[MODEL_SLOT_IMAGE] */
    char model_name[256];           /* Alias for model_names[MODEL_SLOT_IMAGE] */
    void* model_handle;             /* Alias for model_handles[MODEL_SLOT_IMAGE] */

    /* Labels */
    char labels[CIRA_MAX_LABELS][CIRA_MAX_LABEL_LEN];
    int num_labels;

    /* Model input size */
    int input_w;
    int input_h;

    /* Inference settings */
    float confidence_threshold;
    float nms_threshold;
    yolo_version_t yolo_version;    /* YOLO version (from manifest or auto-detect) */

    /* Signal ingestion (Spec C) */
    signal_buffer_t*  signal_buffer;                     /* NULL for image-only models */
    char   signal_selected_features[256][128];           /* selected feature names */
    int    signal_num_selected;                          /* count of selected features */
    char   signal_output_format[32];                     /* label_prob|softmax|reconstruction|anomaly_score */
    float  signal_anomaly_threshold;                     /* for reconstruction format */

    /* Results */
    cira_detection_t detections[CIRA_MAX_DETECTIONS];
    int num_detections;
    char result_json[CIRA_MAX_JSON_LEN];

    /* Detection persistence (for smooth annotations) */
    cira_detection_t prev_detections[CIRA_MAX_DETECTIONS];
    int prev_num_detections;
    uint64_t prev_detection_frame;  /* Frame number when prev_detections was set */

    /* Streaming state */
    int camera_running;
    int current_camera;     /* Currently active camera device ID (-1 if none) */
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

    /* Cumulative statistics (for /api/stats endpoint) */
    uint64_t total_detections;                      /* Total detections since startup */
    uint64_t detections_by_label[CIRA_MAX_LABELS];  /* Detections per label */
    uint64_t total_frames;                          /* Total frames processed */
    time_t start_time;                              /* Startup timestamp */

    /* Model swap synchronization (prevents NCNN pool allocator errors) */
    volatile int model_swapping;                    /* Flag: model is being swapped */
    pthread_mutex_t model_mutex;                    /* Mutex for model access */

    /* File-based frame transfer (cross-platform alternative to MJPEG) */
    char frame_file_path[512];                      /* Path to current frame file */
    uint64_t frame_sequence;                        /* Frame sequence number */
    pthread_mutex_t frame_file_mutex;               /* Mutex for file access */
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

/**
 * Synchronize legacy single-model fields with model slot arrays.
 * Call this after modifying slot arrays to update backward-compatible pointers.
 *
 * @param ctx Context handle
 */
void cira_sync_legacy_fields(cira_ctx* ctx);

/* Helper macros for slot access */
#define SLOT_FORMAT(ctx, slot) ((ctx)->formats[(slot)])
#define SLOT_HANDLE(ctx, slot) ((ctx)->model_handles[(slot)])
#define SLOT_PATH(ctx, slot) ((ctx)->model_paths[(slot)])
#define SLOT_NAME(ctx, slot) ((ctx)->model_names[(slot)])
#define SLOT_MUTEX(ctx, slot) ((ctx)->model_slot_mutexes[(slot)])

#ifdef __cplusplus
}
#endif

#endif /* CIRA_INTERNAL_H */
