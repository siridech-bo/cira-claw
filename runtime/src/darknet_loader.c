/**
 * CiRA Runtime - Darknet Model Loader
 *
 * This file wraps the CiRA-AMI/darknet C API to load YOLO models.
 * It supports the 3-file format exported by CiRA CORE:
 * - obj.names (class labels)
 * - *.cfg (network architecture)
 * - *.weights (trained weights)
 *
 * Supported architectures:
 * - YOLOv4, YOLOv4-tiny, YOLOv7, YOLOv7-tiny
 * - Fastest-1.1-XL, Fastest-1.1
 * - All CiRA CORE exported models
 *
 * (c) CiRA Robotics / KMITL 2026
 */

#include "cira.h"
#include "cira_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef CIRA_DARKNET_ENABLED

/* Darknet API declarations (from darknet.h) */
typedef struct network network;
typedef struct image {
    int w;
    int h;
    int c;
    float *data;
} image;

typedef struct detection {
    float x, y, w, h;
    int classes;
    float *prob;
    float objectness;
    int sort_class;
} detection;

/* Darknet function prototypes - these come from libdarknet.so */
extern network *load_network(char *cfg, char *weights, int clear);
extern void free_network(network *net);
extern void set_batch_network(network *net, int b);
extern float *network_predict(network *net, float *input);
extern detection *get_network_boxes(network *net, int w, int h, float thresh,
                                     float hier_thresh, int *map, int relative,
                                     int *num, int letter);
extern void free_detections(detection *dets, int n);
extern void do_nms_sort(detection *dets, int total, int classes, float thresh);
extern image make_image(int w, int h, int c);
extern void free_image(image m);
extern image resize_image(image im, int w, int h);
extern int network_width(network *net);
extern int network_height(network *net);

/* Internal Darknet model structure */
typedef struct {
    network *net;
    int input_w;
    int input_h;
    int num_classes;
} darknet_model_t;

/* Helper: Check if path is a directory */
static int is_dir(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

/* Helper: Find file with extension in directory */
static int find_file_ext(const char* dir, const char* ext, char* out, size_t out_size) {
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

/* Helper: Convert uint8 RGB to Darknet float image format (CHW) */
static image make_image_from_bytes(const uint8_t* data, int w, int h, int c) {
    image im = make_image(w, h, c);

    /* Convert from packed RGB (HWC) to planar (CHW) and normalize to 0-1 */
    for (int k = 0; k < c; k++) {
        for (int j = 0; j < h; j++) {
            for (int i = 0; i < w; i++) {
                int src_idx = j * w * c + i * c + k;
                int dst_idx = k * h * w + j * w + i;
                im.data[dst_idx] = (float)data[src_idx] / 255.0f;
            }
        }
    }

    return im;
}

/**
 * Load a Darknet model from a directory.
 *
 * Expected directory structure:
 * - obj.names or labels.txt (class labels)
 * - *.cfg (network config)
 * - *.weights (trained weights)
 */
int darknet_load(cira_ctx* ctx, const char* model_path) {
    char cfg_path[1024] = {0};
    char weights_path[1024] = {0};

    if (!is_dir(model_path)) {
        cira_set_error(ctx, "Path must be a directory containing .cfg and .weights: %s", model_path);
        return CIRA_ERROR_INPUT;
    }

    /* Find .cfg file */
    if (!find_file_ext(model_path, ".cfg", cfg_path, sizeof(cfg_path))) {
        cira_set_error(ctx, "No .cfg file found in %s", model_path);
        return CIRA_ERROR_FILE;
    }

    /* Find .weights file */
    if (!find_file_ext(model_path, ".weights", weights_path, sizeof(weights_path))) {
        cira_set_error(ctx, "No .weights file found in %s", model_path);
        return CIRA_ERROR_FILE;
    }

    /* Allocate model structure */
    darknet_model_t* model = (darknet_model_t*)calloc(1, sizeof(darknet_model_t));
    if (!model) {
        cira_set_error(ctx, "Failed to allocate model structure");
        return CIRA_ERROR_MEMORY;
    }

    /* Load network */
    fprintf(stderr, "Loading Darknet model:\n");
    fprintf(stderr, "  Config:  %s\n", cfg_path);
    fprintf(stderr, "  Weights: %s\n", weights_path);

    model->net = load_network(cfg_path, weights_path, 0);
    if (!model->net) {
        cira_set_error(ctx, "Failed to load Darknet network");
        free(model);
        return CIRA_ERROR_MODEL;
    }

    /* Set batch size to 1 for inference */
    set_batch_network(model->net, 1);

    /* Get network input dimensions */
    model->input_w = network_width(model->net);
    model->input_h = network_height(model->net);
    model->num_classes = ctx->num_labels;  /* Use labels loaded by cira_load() */

    /* Update context with model dimensions */
    ctx->input_w = model->input_w;
    ctx->input_h = model->input_h;

    fprintf(stderr, "  Input size: %dx%d\n", model->input_w, model->input_h);
    fprintf(stderr, "  Classes: %d\n", model->num_classes);

    /* Store model handle in context */
    ctx->model_handle = model;

    fprintf(stderr, "Darknet model loaded successfully\n");
    return CIRA_OK;
}

/**
 * Unload Darknet model and free resources.
 */
void darknet_unload(cira_ctx* ctx) {
    if (!ctx || !ctx->model_handle) return;

    darknet_model_t* model = (darknet_model_t*)ctx->model_handle;

    if (model->net) {
        free_network(model->net);
    }

    free(model);
    ctx->model_handle = NULL;

    fprintf(stderr, "Darknet model unloaded\n");
}

/**
 * Run YOLO inference on an image.
 *
 * @param ctx Context with loaded Darknet model
 * @param data RGB image data (packed HWC, row-major)
 * @param w Image width
 * @param h Image height
 * @param channels Number of channels (must be 3)
 * @return CIRA_OK on success
 */
int darknet_predict(cira_ctx* ctx, const uint8_t* data, int w, int h, int channels) {
    if (!ctx || !ctx->model_handle || !data) {
        return CIRA_ERROR_INPUT;
    }

    if (channels != 3) {
        cira_set_error(ctx, "Only 3-channel images supported");
        return CIRA_ERROR_INPUT;
    }

    darknet_model_t* model = (darknet_model_t*)ctx->model_handle;

    /* Clear previous detections */
    cira_clear_detections(ctx);

    /* Convert input image to Darknet format (CHW, float, 0-1) */
    image im = make_image_from_bytes(data, w, h, channels);

    /* Resize to network input size */
    image resized = resize_image(im, model->input_w, model->input_h);

    /* Run inference */
    network_predict(model->net, resized.data);

    /* Get detections */
    int nboxes = 0;
    float thresh = ctx->confidence_threshold;
    float nms_thresh = ctx->nms_threshold;

    /* Get detection boxes (relative coordinates) */
    detection* dets = get_network_boxes(model->net, w, h, thresh, 0.5f, NULL, 1, &nboxes, 0);

    /* Apply Non-Maximum Suppression */
    if (nms_thresh > 0 && model->num_classes > 0) {
        do_nms_sort(dets, nboxes, model->num_classes, nms_thresh);
    }

    /* Convert detections to cira format */
    for (int i = 0; i < nboxes; i++) {
        /* Check each class */
        for (int j = 0; j < model->num_classes; j++) {
            if (dets[i].prob[j] > thresh) {
                /* Darknet uses center coordinates, convert to top-left */
                float det_x = dets[i].x - dets[i].w / 2.0f;
                float det_y = dets[i].y - dets[i].h / 2.0f;
                float det_w = dets[i].w;
                float det_h = dets[i].h;

                /* Clamp to [0, 1] */
                if (det_x < 0) det_x = 0;
                if (det_y < 0) det_y = 0;
                if (det_x + det_w > 1) det_w = 1 - det_x;
                if (det_y + det_h > 1) det_h = 1 - det_y;

                /* Add detection */
                if (!cira_add_detection(ctx, det_x, det_y, det_w, det_h,
                                        dets[i].prob[j], j)) {
                    /* Detection array full */
                    break;
                }
            }
        }

        /* Check if detection array is full */
        if (ctx->num_detections >= CIRA_MAX_DETECTIONS) {
            break;
        }
    }

    /* Cleanup */
    free_detections(dets, nboxes);
    free_image(resized);
    free_image(im);

    fprintf(stderr, "Darknet inference: %d detections\n", ctx->num_detections);
    return CIRA_OK;
}

#else /* CIRA_DARKNET_ENABLED */

/* Stubs when Darknet is not enabled */
int darknet_load(cira_ctx* ctx, const char* model_path) {
    (void)model_path;
    cira_set_error(ctx, "Darknet support not enabled in this build");
    return CIRA_ERROR_MODEL;
}

void darknet_unload(cira_ctx* ctx) {
    (void)ctx;
}

int darknet_predict(cira_ctx* ctx, const uint8_t* data, int w, int h, int channels) {
    (void)data;
    (void)w;
    (void)h;
    (void)channels;
    cira_set_error(ctx, "Darknet support not enabled in this build");
    return CIRA_ERROR_MODEL;
}

#endif /* CIRA_DARKNET_ENABLED */
