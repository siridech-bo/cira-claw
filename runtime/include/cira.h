/**
 * CiRA Runtime - Lightweight Edge AI Inference Engine
 *
 * A sealed black-box inference runtime. Load a model, feed it data, get predictions.
 * No user access to internals. Like an industrial sensor module.
 *
 * (c) CiRA Robotics / KMITL 2026
 */

#ifndef CIRA_H
#define CIRA_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* === Opaque context type === */
typedef struct cira_ctx cira_ctx;

/* === Status codes === */
#define CIRA_OK             0
#define CIRA_ERROR         -1
#define CIRA_ERROR_FILE    -2
#define CIRA_ERROR_MODEL   -3
#define CIRA_ERROR_MEMORY  -4
#define CIRA_ERROR_INPUT   -5

/* === Context states === */
#define CIRA_STATUS_READY    0
#define CIRA_STATUS_LOADING  1
#define CIRA_STATUS_ERROR    2

/* === Lifecycle functions === */

/**
 * Create a new CiRA context.
 * Must be destroyed with cira_destroy() when done.
 *
 * @return New context, or NULL on failure
 */
cira_ctx* cira_create(void);

/**
 * Load a model from config file or model path.
 *
 * The path can be:
 * - A directory containing .cfg + .weights (Darknet/CiRA CORE)
 * - A .onnx file
 * - A .engine/.trt file (TensorRT)
 * - A .pkl/.joblib file (scikit-learn)
 * - A model_config.json file
 *
 * @param ctx Context handle
 * @param config_path Path to model config or model directory
 * @return CIRA_OK on success, error code on failure
 */
int cira_load(cira_ctx* ctx, const char* config_path);

/**
 * Destroy context and free resources.
 *
 * @param ctx Context to destroy
 */
void cira_destroy(cira_ctx* ctx);

/* === Inference functions === */

/**
 * Run inference on an image.
 *
 * @param ctx Context handle
 * @param data Image data (RGB or BGR, packed)
 * @param w Image width
 * @param h Image height
 * @param channels Number of channels (3 for RGB/BGR)
 * @return CIRA_OK on success, error code on failure
 */
int cira_predict_image(cira_ctx* ctx, const uint8_t* data, int w, int h, int channels);

/**
 * Run inference on sensor data (for anomaly detection).
 *
 * @param ctx Context handle
 * @param values Array of sensor values
 * @param count Number of values
 * @return CIRA_OK on success, error code on failure
 */
int cira_predict_sensor(cira_ctx* ctx, const float* values, int count);

/**
 * Run batch inference on multiple images.
 *
 * @param ctx Context handle
 * @param images Array of image pointers
 * @param count Number of images
 * @param w Image width (all images must be same size)
 * @param h Image height
 * @param channels Number of channels
 * @return CIRA_OK on success, error code on failure
 */
int cira_predict_batch(cira_ctx* ctx, const uint8_t** images, int count, int w, int h, int channels);

/* === Result functions === */

/**
 * Get full inference result as JSON string.
 * The string is valid until the next inference call.
 *
 * @param ctx Context handle
 * @return JSON string with results
 */
const char* cira_result_json(cira_ctx* ctx);

/**
 * Get number of detections in last result.
 *
 * @param ctx Context handle
 * @return Number of detections, or 0 if none
 */
int cira_result_count(cira_ctx* ctx);

/**
 * Get bounding box for a detection.
 *
 * @param ctx Context handle
 * @param index Detection index (0 to count-1)
 * @param x Output: X coordinate (top-left)
 * @param y Output: Y coordinate (top-left)
 * @param w Output: Width
 * @param h Output: Height
 * @return CIRA_OK on success, CIRA_ERROR if index invalid
 */
int cira_result_bbox(cira_ctx* ctx, int index, float* x, float* y, float* w, float* h);

/**
 * Get confidence score for a detection.
 *
 * @param ctx Context handle
 * @param index Detection index
 * @return Confidence score (0.0-1.0), or -1 on error
 */
float cira_result_score(cira_ctx* ctx, int index);

/**
 * Get label for a detection.
 *
 * @param ctx Context handle
 * @param index Detection index
 * @return Label string, or NULL on error
 */
const char* cira_result_label(cira_ctx* ctx, int index);

/* === Status functions === */

/**
 * Get current context status.
 *
 * @param ctx Context handle
 * @return CIRA_STATUS_READY, CIRA_STATUS_LOADING, or CIRA_STATUS_ERROR
 */
int cira_status(cira_ctx* ctx);

/**
 * Get last error message.
 *
 * @param ctx Context handle
 * @return Human-readable error message, or NULL if no error
 */
const char* cira_error(cira_ctx* ctx);

/**
 * Get library version string.
 *
 * @return Version string (e.g., "1.0.0")
 */
const char* cira_version(void);

/* === Streaming functions (optional, for daemon mode) === */

/**
 * Start camera capture and inference loop.
 * Runs in background thread.
 *
 * @param ctx Context handle
 * @param device_id Camera device ID (0, 1, etc.)
 * @return CIRA_OK on success
 */
int cira_start_camera(cira_ctx* ctx, int device_id);

/**
 * Stop camera capture.
 *
 * @param ctx Context handle
 * @return CIRA_OK on success
 */
int cira_stop_camera(cira_ctx* ctx);

/**
 * Start HTTP streaming server.
 *
 * @param ctx Context handle
 * @param port HTTP port (default 8080)
 * @return CIRA_OK on success
 */
int cira_start_server(cira_ctx* ctx, int port);

/**
 * Stop HTTP server.
 *
 * @param ctx Context handle
 * @return CIRA_OK on success
 */
int cira_stop_server(cira_ctx* ctx);

/**
 * Get current FPS (frames per second).
 *
 * @param ctx Context handle
 * @return Current FPS, or 0 if not running
 */
float cira_get_fps(cira_ctx* ctx);

#ifdef __cplusplus
}
#endif

#endif /* CIRA_H */
