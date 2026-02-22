/**
 * CiRA Runtime - YOLO Version-Specific Decoder
 *
 * Shared output parsing for YOLOv4/v5/v8/v10 across all backends.
 *
 * (c) CiRA Robotics / KMITL 2026
 */

#include "yolo_decoder.h"
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

/* Sigmoid activation */
static inline float sigmoid(float x) {
    return 1.0f / (1.0f + expf(-x));
}

/* IoU calculation for NMS */
static float iou(const yolo_detection_t* a, const yolo_detection_t* b) {
    float ix1 = fmaxf(a->x1, b->x1);
    float iy1 = fmaxf(a->y1, b->y1);
    float ix2 = fminf(a->x2, b->x2);
    float iy2 = fminf(a->y2, b->y2);

    float iw = fmaxf(0.0f, ix2 - ix1);
    float ih = fmaxf(0.0f, iy2 - iy1);
    float inter = iw * ih;

    float area_a = (a->x2 - a->x1) * (a->y2 - a->y1);
    float area_b = (b->x2 - b->x1) * (b->y2 - b->y1);
    float uni = area_a + area_b - inter;

    return (uni > 0) ? inter / uni : 0.0f;
}

/* Compare detections by score (descending) for qsort */
static int cmp_score_desc(const void* a, const void* b) {
    const yolo_detection_t* da = (const yolo_detection_t*)a;
    const yolo_detection_t* db = (const yolo_detection_t*)b;
    if (db->score > da->score) return 1;
    if (db->score < da->score) return -1;
    return 0;
}

/**
 * Decode YOLOv4/v3 output (anchor-based, per-scale)
 * Shape: [1, num_boxes, 5+num_classes] per scale (3D pre-decoded)
 *
 * TODO: Add 5D raw grid decoder for [1, grid_h, grid_w, num_anchors, 5+C] format.
 *       Reference implementation in git history commit cd6e83b (onnx_loader.c lines ~770-980).
 *       Would require anchor-based decoding with sigmoid activations.
 */
static int decode_yolov4(const float* output, const int64_t* shape, int num_dims,
                         const yolo_decode_config_t* config,
                         yolo_detection_t* dets, int max_dets) {
    /* Currently only supports 3D pre-decoded output. 5D raw grid returns -1. */
    if (num_dims < 3) return -1;

    int num_boxes = (int)shape[1];
    int box_size = (int)shape[2];
    int num_classes = config->num_classes;

    if (box_size < 5 + num_classes) {
        num_classes = box_size - 5;
        if (num_classes <= 0) return -1;
    }

    int count = 0;
    int checked = 0;
    float max_obj_seen = 0.0f;

    for (int i = 0; i < num_boxes && count < max_dets; i++) {
        const float* box = output + i * box_size;

        /* Get objectness score */
        float obj = box[4];
        if (obj < 0.0f || obj > 1.0f) {
            obj = sigmoid(obj);  /* Raw logits - apply sigmoid */
        }

        if (obj > max_obj_seen) max_obj_seen = obj;
        checked++;

        if (obj < config->conf_threshold) continue;

        /* Find best class */
        int best_class = 0;
        float best_prob = box[5];
        for (int c = 1; c < num_classes; c++) {
            if (box[5 + c] > best_prob) {
                best_prob = box[5 + c];
                best_class = c;
            }
        }

        /* Apply sigmoid if raw logits */
        if (best_prob < 0.0f || best_prob > 1.0f) {
            best_prob = sigmoid(best_prob);
        }

        float score = obj * best_prob;
        if (score < config->conf_threshold) continue;

        /* Decode box (cx, cy, w, h) -> (x1, y1, x2, y2) */
        float cx = box[0];
        float cy = box[1];
        float w = box[2];
        float h = box[3];

        /* Check if normalized or pixel coords */
        if (cx <= 1.0f && cy <= 1.0f && w <= 1.0f && h <= 1.0f) {
            cx *= config->input_w;
            cy *= config->input_h;
            w *= config->input_w;
            h *= config->input_h;
        }

        dets[count].x1 = cx - w * 0.5f;
        dets[count].y1 = cy - h * 0.5f;
        dets[count].x2 = cx + w * 0.5f;
        dets[count].y2 = cy + h * 0.5f;
        dets[count].score = score;
        dets[count].class_id = best_class;
        count++;
    }

    fprintf(stderr, "decode_yolov4: checked %d boxes, max_obj=%.4f, found %d detections\n",
            checked, max_obj_seen, count);

    return count;
}

/**
 * Decode YOLOv5/v7 output (concatenated, pre-decoded)
 * Shape: [1, 25200, 5+num_classes]
 */
static int decode_yolov5(const float* output, const int64_t* shape, int num_dims,
                         const yolo_decode_config_t* config,
                         yolo_detection_t* dets, int max_dets) {
    /* Same format as v4 but typically pre-decoded coordinates */
    return decode_yolov4(output, shape, num_dims, config, dets, max_dets);
}

/**
 * Decode YOLOv8/v9/v11 output (transposed, no objectness)
 * Shape: [1, 4+num_classes, 8400]
 */
static int decode_yolov8(const float* output, const int64_t* shape, int num_dims,
                         const yolo_decode_config_t* config,
                         yolo_detection_t* dets, int max_dets) {
    if (num_dims < 3) return -1;

    int channels = (int)shape[1];
    int num_boxes = (int)shape[2];
    int num_classes = channels - 4;

    if (num_classes <= 0) return -1;

    int count = 0;
    for (int i = 0; i < num_boxes && count < max_dets; i++) {
        /* Find best class (no objectness in v8+) */
        int best_class = 0;
        float best_score = output[4 * num_boxes + i];
        for (int c = 1; c < num_classes; c++) {
            float score = output[(4 + c) * num_boxes + i];
            if (score > best_score) {
                best_score = score;
                best_class = c;
            }
        }

        /* Apply sigmoid if raw logits */
        if (best_score < 0.0f || best_score > 1.0f) {
            best_score = sigmoid(best_score);
        }

        if (best_score < config->conf_threshold) continue;

        /* Decode transposed box */
        float cx = output[0 * num_boxes + i];
        float cy = output[1 * num_boxes + i];
        float w = output[2 * num_boxes + i];
        float h = output[3 * num_boxes + i];

        /* Check if normalized or pixel coords */
        if (cx <= 1.0f && cy <= 1.0f && w <= 1.0f && h <= 1.0f) {
            cx *= config->input_w;
            cy *= config->input_h;
            w *= config->input_w;
            h *= config->input_h;
        }

        dets[count].x1 = cx - w * 0.5f;
        dets[count].y1 = cy - h * 0.5f;
        dets[count].x2 = cx + w * 0.5f;
        dets[count].y2 = cy + h * 0.5f;
        dets[count].score = best_score;
        dets[count].class_id = best_class;
        count++;
    }

    return count;
}

/**
 * Decode YOLOv10 output (NMS-free)
 * Shape: [1, 300, 6]
 */
static int decode_yolov10(const float* output, const int64_t* shape, int num_dims,
                          const yolo_decode_config_t* config,
                          yolo_detection_t* dets, int max_dets) {
    if (num_dims < 3) return -1;

    int num_boxes = (int)shape[1];
    int box_size = (int)shape[2];

    if (box_size < 6) return -1;

    int count = 0;
    for (int i = 0; i < num_boxes && count < max_dets; i++) {
        const float* box = output + i * box_size;

        float score = box[4];
        if (score < config->conf_threshold) continue;

        int class_id = (int)box[5];

        /* YOLOv10 outputs corner coords directly */
        dets[count].x1 = box[0];
        dets[count].y1 = box[1];
        dets[count].x2 = box[2];
        dets[count].y2 = box[3];
        dets[count].score = score;
        dets[count].class_id = class_id;
        count++;
    }

    return count;
}

/* Main decode function */
int yolo_decode(const float* output, const int64_t* output_shape, int num_dims,
                const yolo_decode_config_t* config,
                yolo_detection_t* detections, int max_dets) {
    if (!output || !output_shape || !config || !detections || max_dets <= 0) {
        return -1;
    }

    yolo_version_t version = config->version;
    if (version == YOLO_VERSION_AUTO) {
        version = yolo_detect_version(output_shape, num_dims, config->num_classes);
    }

    fprintf(stderr, "yolo_decode: detected version=%s, shape=[%lld,%lld,%lld], conf_thresh=%.2f\n",
            yolo_version_name(version),
            (long long)(num_dims > 0 ? output_shape[0] : 0),
            (long long)(num_dims > 1 ? output_shape[1] : 0),
            (long long)(num_dims > 2 ? output_shape[2] : 0),
            config->conf_threshold);

    int count = 0;
    switch (version) {
        case YOLO_VERSION_V4:
            count = decode_yolov4(output, output_shape, num_dims, config, detections, max_dets);
            break;
        case YOLO_VERSION_V5:
            count = decode_yolov5(output, output_shape, num_dims, config, detections, max_dets);
            break;
        case YOLO_VERSION_V8:
            count = decode_yolov8(output, output_shape, num_dims, config, detections, max_dets);
            break;
        case YOLO_VERSION_V10:
            count = decode_yolov10(output, output_shape, num_dims, config, detections, max_dets);
            break;
        default:
            /* Try v5 as fallback */
            count = decode_yolov5(output, output_shape, num_dims, config, detections, max_dets);
            break;
    }

    if (count <= 0) return count;

    /* Apply NMS (except for v10 which is NMS-free) */
    if (version != YOLO_VERSION_V10) {
        count = yolo_nms(detections, count, config->nms_threshold);
    }

    /* Limit to max_detections */
    if (count > config->max_detections) {
        count = config->max_detections;
    }

    return count;
}

/* Non-Maximum Suppression */
int yolo_nms(yolo_detection_t* detections, int count, float nms_threshold) {
    if (!detections || count <= 0) return 0;

    /* Sort by score descending */
    qsort(detections, count, sizeof(yolo_detection_t), cmp_score_desc);

    /* Mark suppressed detections with negative score */
    for (int i = 0; i < count; i++) {
        if (detections[i].score < 0) continue;

        for (int j = i + 1; j < count; j++) {
            if (detections[j].score < 0) continue;
            if (detections[i].class_id != detections[j].class_id) continue;

            if (iou(&detections[i], &detections[j]) > nms_threshold) {
                detections[j].score = -1.0f;  /* Mark as suppressed */
            }
        }
    }

    /* Compact array - remove suppressed */
    int out = 0;
    for (int i = 0; i < count; i++) {
        if (detections[i].score >= 0) {
            if (out != i) {
                detections[out] = detections[i];
            }
            out++;
        }
    }

    return out;
}

/* Parse version string */
yolo_version_t yolo_parse_version(const char* version_str) {
    if (!version_str) return YOLO_VERSION_AUTO;

    /* Convert to lowercase for comparison */
    char lower[32];
    int i = 0;
    for (; version_str[i] && i < 31; i++) {
        lower[i] = tolower((unsigned char)version_str[i]);
    }
    lower[i] = '\0';

    /* Check for version patterns */
    if (strstr(lower, "v10") || strstr(lower, "yolov10")) {
        return YOLO_VERSION_V10;
    }
    if (strstr(lower, "v8") || strstr(lower, "v9") || strstr(lower, "v11") ||
        strstr(lower, "yolov8") || strstr(lower, "yolov9") || strstr(lower, "yolov11")) {
        return YOLO_VERSION_V8;
    }
    if (strstr(lower, "v5") || strstr(lower, "v7") ||
        strstr(lower, "yolov5") || strstr(lower, "yolov7")) {
        return YOLO_VERSION_V5;
    }
    if (strstr(lower, "v3") || strstr(lower, "v4") ||
        strstr(lower, "yolov3") || strstr(lower, "yolov4")) {
        return YOLO_VERSION_V4;
    }
    if (strcmp(lower, "auto") == 0) {
        return YOLO_VERSION_AUTO;
    }

    return YOLO_VERSION_AUTO;
}

/* Get version name string */
const char* yolo_version_name(yolo_version_t version) {
    switch (version) {
        case YOLO_VERSION_V4:  return "YOLOv4";
        case YOLO_VERSION_V5:  return "YOLOv5/v7";
        case YOLO_VERSION_V8:  return "YOLOv8/v9/v11";
        case YOLO_VERSION_V10: return "YOLOv10";
        default:               return "auto";
    }
}

/* Auto-detect version from output shape */
yolo_version_t yolo_detect_version(const int64_t* output_shape, int num_dims, int num_classes) {
    if (!output_shape || num_dims < 2) {
        return YOLO_VERSION_AUTO;
    }

    /* Get dimensions (handle batch dimension) */
    int dim1 = (num_dims >= 2) ? (int)output_shape[1] : 0;
    int dim2 = (num_dims >= 3) ? (int)output_shape[2] : 0;

    /* YOLOv10: [1, 300, 6] - NMS-free output */
    if (dim1 == 300 && dim2 == 6) {
        return YOLO_VERSION_V10;
    }

    /* YOLOv8/v9/v11: [1, 4+C, 8400] - transposed with no objectness */
    if (dim2 == 8400 && dim1 == 4 + num_classes) {
        return YOLO_VERSION_V8;
    }

    /* YOLOv5/v7: [1, 25200, 5+C] - concatenated anchors */
    if (dim1 == 25200 || dim1 == 18900 || dim1 == 6300) {
        return YOLO_VERSION_V5;
    }

    /* Check for transposed format: dim1 small (~4+classes), dim2 large (many boxes) */
    if (dim1 < 100 && dim2 > 1000) {
        return YOLO_VERSION_V8;
    }

    /* Check for standard format: dim1 large (boxes), dim2 small (5+classes) */
    if (dim1 > 1000 && dim2 < 100) {
        return YOLO_VERSION_V5;
    }

    /* Per-scale outputs typically have grid dimensions */
    if (dim1 == 507 || dim1 == 2028 || dim1 == 8112) {  /* 13*13*3, 26*26*3, 52*52*3 */
        return YOLO_VERSION_V4;
    }

    return YOLO_VERSION_AUTO;
}
