/**
 * CiRA Runtime - YOLO Version-Specific Decoder
 *
 * Shared output parsing for YOLOv4/v5/v8/v10 across all backends.
 *
 * (c) CiRA Robotics / KMITL 2026
 */

#ifndef YOLO_DECODER_H
#define YOLO_DECODER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* YOLO version enumeration */
typedef enum {
    YOLO_VERSION_AUTO = 0,  /* Auto-detect from output shape */
    YOLO_VERSION_V4,        /* YOLOv3/v4: per-scale anchors, sigmoid */
    YOLO_VERSION_V5,        /* YOLOv5/v7: concatenated, pre-decoded */
    YOLO_VERSION_V8,        /* YOLOv8/v9/v11: transposed, no objectness */
    YOLO_VERSION_V10        /* YOLOv10: NMS-free, [1,300,6] */
} yolo_version_t;

/* Detection result in corners format (x1,y1,x2,y2) */
typedef struct {
    float x1, y1, x2, y2;   /* Bounding box corners (pixels) */
    float score;            /* Confidence score [0,1] */
    int class_id;           /* Class index */
} yolo_detection_t;

/* Decoder configuration */
typedef struct {
    yolo_version_t version; /* YOLO version (or AUTO) */
    int input_w, input_h;   /* Model input dimensions */
    int num_classes;        /* Number of classes */
    float conf_threshold;   /* Confidence threshold */
    float nms_threshold;    /* NMS IoU threshold */
    int max_detections;     /* Maximum detections to return */
} yolo_decode_config_t;

/**
 * Decode YOLO model output to detections.
 *
 * @param output        Raw model output tensor
 * @param output_shape  Output tensor shape (up to 4 dims)
 * @param num_dims      Number of dimensions in output_shape
 * @param config        Decoder configuration
 * @param detections    Output array (caller allocates)
 * @param max_dets      Size of detections array
 * @return              Number of detections found, or -1 on error
 */
int yolo_decode(const float* output, const int64_t* output_shape, int num_dims,
                const yolo_decode_config_t* config,
                yolo_detection_t* detections, int max_dets);

/**
 * Apply Non-Maximum Suppression to detections.
 *
 * @param detections    Array of detections (modified in-place)
 * @param count         Number of detections
 * @param nms_threshold IoU threshold for suppression
 * @return              Number of detections after NMS
 */
int yolo_nms(yolo_detection_t* detections, int count, float nms_threshold);

/**
 * Parse YOLO version string from manifest.
 *
 * @param version_str   Version string (e.g., "yolov8", "v5", "auto")
 * @return              Corresponding yolo_version_t enum value
 */
yolo_version_t yolo_parse_version(const char* version_str);

/**
 * Get YOLO version name string.
 *
 * @param version       YOLO version enum
 * @return              Human-readable version name
 */
const char* yolo_version_name(yolo_version_t version);

/**
 * Auto-detect YOLO version from output shape.
 *
 * @param output_shape  Output tensor shape
 * @param num_dims      Number of dimensions
 * @param num_classes   Number of classes (for validation)
 * @return              Detected version, or YOLO_VERSION_AUTO if unknown
 */
yolo_version_t yolo_detect_version(const int64_t* output_shape, int num_dims, int num_classes);

#ifdef __cplusplus
}
#endif

#endif /* YOLO_DECODER_H */
