/**
 * CiRA Runtime - NCNN Model Loader
 *
 * This file implements NCNN inference with optional Vulkan GPU acceleration.
 * NCNN is a high-performance neural network inference framework optimized
 * for mobile platforms, but also works on desktop (Windows, Linux, macOS).
 *
 * Key features:
 * - Zero-copy design for minimal memory overhead
 * - Vulkan GPU acceleration when available
 * - CPU fallback for universal compatibility
 * - Supports YOLO detection models exported from CiRA CORE
 *
 * Model format:
 * - *.param (network architecture in text format)
 * - *.bin (trained weights in binary format)
 * - obj.names or labels.txt (class labels)
 *
 * (c) CiRA Robotics / KMITL 2026
 */

#include "cira_internal.h"
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <vector>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>

#ifdef CIRA_NCNN_ENABLED

#include <ncnn/net.h>
#include <ncnn/layer.h>
#include <ncnn/cpu.h>

#if defined(CIRA_VULKAN_ENABLED) && NCNN_VULKAN
#include <ncnn/gpu.h>
#endif

/* Maximum output layers to store */
#define NCNN_MAX_OUTPUT_LAYERS 8

/* Internal NCNN model structure */
struct ncnn_model_t {
    ncnn::Net net;
    int input_w;
    int input_h;
    int num_classes;
    bool use_vulkan;

    /* YOLO output layer names (stored from network at load time) */
    char output_layers[NCNN_MAX_OUTPUT_LAYERS][64];
    int num_output_layers;
    int active_output_idx;  /* Index of the layer that works for extraction */
};

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
    while ((entry = readdir(d)) != nullptr) {
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

/**
 * Load an NCNN model from a directory.
 *
 * Expected directory structure:
 * - obj.names or labels.txt (class labels)
 * - *.param (network architecture)
 * - *.bin (trained weights)
 */
extern "C" int ncnn_load(cira_ctx* ctx, const char* model_path) {
    char param_path[1024] = {0};
    char bin_path[1024] = {0};

    if (!is_dir(model_path)) {
        cira_set_error(ctx, "Path must be a directory containing .param and .bin: %s", model_path);
        return CIRA_ERROR_INPUT;
    }

    /* Find .param file */
    if (!find_file_ext(model_path, ".param", param_path, sizeof(param_path))) {
        cira_set_error(ctx, "No .param file found in %s", model_path);
        return CIRA_ERROR_FILE;
    }

    /* Find .bin file */
    if (!find_file_ext(model_path, ".bin", bin_path, sizeof(bin_path))) {
        cira_set_error(ctx, "No .bin file found in %s", model_path);
        return CIRA_ERROR_FILE;
    }

    /* Allocate model structure */
    ncnn_model_t* model = new (std::nothrow) ncnn_model_t();
    if (!model) {
        cira_set_error(ctx, "Failed to allocate model structure");
        return CIRA_ERROR_MEMORY;
    }

    /* Initialize NCNN options */
    model->use_vulkan = false;

#if defined(CIRA_VULKAN_ENABLED) && NCNN_VULKAN
    /* Try to initialize Vulkan */
    int gpu_count = ncnn::get_gpu_count();
    if (gpu_count > 0) {
        model->net.opt.use_vulkan_compute = true;
        model->use_vulkan = true;
        fprintf(stderr, "NCNN: Using Vulkan GPU (%d devices available)\n", gpu_count);
    } else {
        fprintf(stderr, "NCNN: No Vulkan GPU found, using CPU\n");
    }
#else
    fprintf(stderr, "NCNN: Vulkan not enabled, using CPU\n");
#endif

    /* Set NCNN options for optimal performance */
    model->net.opt.lightmode = true;
    model->net.opt.num_threads = ncnn::get_big_cpu_count();

    /* Enable FP16 for better performance on supported hardware */
    model->net.opt.use_fp16_packed = true;
    model->net.opt.use_fp16_storage = true;
    model->net.opt.use_fp16_arithmetic = false;  /* Keep false for detection accuracy */

    fprintf(stderr, "Loading NCNN model:\n");
    fprintf(stderr, "  Param:  %s\n", param_path);
    fprintf(stderr, "  Bin:    %s\n", bin_path);
    fprintf(stderr, "  Threads: %d\n", model->net.opt.num_threads);

    /* Load network */
    int ret = model->net.load_param(param_path);
    if (ret != 0) {
        cira_set_error(ctx, "Failed to load NCNN param file: %s (error %d)", param_path, ret);
        delete model;
        return CIRA_ERROR_MODEL;
    }

    ret = model->net.load_model(bin_path);
    if (ret != 0) {
        cira_set_error(ctx, "Failed to load NCNN bin file: %s (error %d)", bin_path, ret);
        delete model;
        return CIRA_ERROR_MODEL;
    }

    /* Get input dimensions from network */
    /* NCNN doesn't expose this directly, so we use manifest values or defaults */
    /* ctx->input_w/h may already be set from cira_model.json manifest by cira_load() */
    model->input_w = (ctx->input_w > 0) ? ctx->input_w : 416;
    model->input_h = (ctx->input_h > 0) ? ctx->input_h : 416;

    /* Try to detect input size from first blob */
    const std::vector<ncnn::Blob>& blobs = model->net.blobs();
    if (!blobs.empty()) {
        /* First blob is usually the input */
        /* NCNN stores shape as [w, h, c] or dynamic */
        /* We'll keep defaults for now */
    }

    /* Use labels already loaded by cira_load() */
    model->num_classes = ctx->num_labels;

    /* Store output layer names from the network */
    model->num_output_layers = 0;
    model->active_output_idx = -1;  /* Not yet determined */

    /* First, add actual output names from the network */
    const std::vector<const char*>& net_output_names = model->net.output_names();
    for (size_t i = 0; i < net_output_names.size() && model->num_output_layers < NCNN_MAX_OUTPUT_LAYERS; i++) {
        strncpy(model->output_layers[model->num_output_layers], net_output_names[i], 63);
        model->output_layers[model->num_output_layers][63] = '\0';
        model->num_output_layers++;
    }

    /* If no output layers found, add common fallback names */
    if (model->num_output_layers == 0) {
        const char* fallback_names[] = {"output", "output0", "detection_out", "Yolov3DetectionOutput", nullptr};
        for (int i = 0; fallback_names[i] != nullptr && model->num_output_layers < NCNN_MAX_OUTPUT_LAYERS; i++) {
            strncpy(model->output_layers[model->num_output_layers], fallback_names[i], 63);
            model->output_layers[model->num_output_layers][63] = '\0';
            model->num_output_layers++;
        }
    }

    /* Update context with model dimensions */
    ctx->input_w = model->input_w;
    ctx->input_h = model->input_h;

    fprintf(stderr, "  Input size: %dx%d\n", model->input_w, model->input_h);
    fprintf(stderr, "  Classes: %d\n", model->num_classes);
    fprintf(stderr, "  Vulkan: %s\n", model->use_vulkan ? "enabled" : "disabled");

    /* Print available input/output layer names for debugging */
    const std::vector<const char*>& input_names = model->net.input_names();

    fprintf(stderr, "  Input layers: ");
    for (size_t i = 0; i < input_names.size(); i++) {
        fprintf(stderr, "%s%s", input_names[i], i < input_names.size() - 1 ? ", " : "");
    }
    fprintf(stderr, "\n");

    fprintf(stderr, "  Output layers (stored %d): ", model->num_output_layers);
    for (int i = 0; i < model->num_output_layers; i++) {
        fprintf(stderr, "%s%s", model->output_layers[i], i < model->num_output_layers - 1 ? ", " : "");
    }
    fprintf(stderr, "\n");

    /* Store model handle in context */
    ctx->model_handle = model;

    fprintf(stderr, "NCNN model loaded successfully\n");
    return CIRA_OK;
}

/**
 * Unload NCNN model and free resources.
 */
extern "C" void ncnn_unload(cira_ctx* ctx) {
    if (!ctx || !ctx->model_handle) return;

    ncnn_model_t* model = static_cast<ncnn_model_t*>(ctx->model_handle);

    model->net.clear();
    delete model;
    ctx->model_handle = nullptr;

    fprintf(stderr, "NCNN model unloaded\n");
}

/**
 * Run YOLO inference on an image using NCNN.
 *
 * @param ctx Context with loaded NCNN model
 * @param data RGB image data (packed HWC, row-major)
 * @param w Image width
 * @param h Image height
 * @param channels Number of channels (must be 3)
 * @return CIRA_OK on success
 */
extern "C" int ncnn_predict(cira_ctx* ctx, const uint8_t* data, int w, int h, int channels) {
    if (!ctx || !ctx->model_handle || !data) {
        return CIRA_ERROR_INPUT;
    }

    if (channels != 3) {
        cira_set_error(ctx, "Only 3-channel images supported");
        return CIRA_ERROR_INPUT;
    }

    ncnn_model_t* model = static_cast<ncnn_model_t*>(ctx->model_handle);

    /* Clear previous detections */
    cira_clear_detections(ctx);

    /* Create NCNN Mat from RGB data */
    /* Darknet models are trained on RGB, darknet2ncnn preserves channel order */
    ncnn::Mat in = ncnn::Mat::from_pixels_resize(
        data, ncnn::Mat::PIXEL_RGB,
        w, h,
        model->input_w, model->input_h
    );

    /* Normalize to 0-1 range (YOLO standard) */
    const float norm_vals[3] = {1.0f / 255.0f, 1.0f / 255.0f, 1.0f / 255.0f};
    in.substract_mean_normalize(nullptr, norm_vals);

    /* Create extractor */
    ncnn::Extractor ex = model->net.create_extractor();

    /* Set input */
    ex.input("data", in);

    /* Get output using stored output layer names */
    ncnn::Mat out;
    int ret = -1;

    /* If we already found a working output layer, use it directly */
    if (model->active_output_idx >= 0 && model->active_output_idx < model->num_output_layers) {
        ret = ex.extract(model->output_layers[model->active_output_idx], out);
        if (ret == 0 && (out.w > 0 || out.h > 0 || out.c > 0)) {
            /* Still working, use it */
        } else {
            /* Reset and search again */
            model->active_output_idx = -1;
        }
    }

    /* Search through stored output layer names */
    if (model->active_output_idx < 0) {
        for (int i = 0; i < model->num_output_layers; i++) {
            ret = ex.extract(model->output_layers[i], out);
            if (ret == 0 && (out.w > 0 || out.h > 0 || out.c > 0)) {
                model->active_output_idx = i;
                fprintf(stderr, "NCNN: Using output layer '%s' (w=%d, h=%d, c=%d)\n",
                        model->output_layers[i], out.w, out.h, out.c);
                break;
            }
        }
    }

    if (ret != 0 || (out.w == 0 && out.h == 0 && out.c == 0)) {
        cira_set_error(ctx, "Failed to extract NCNN output (no valid output layer found)");
        return CIRA_ERROR;
    }

    /* Parse YOLO output using unified decoder types */
    /* Output format: [num_detections, 6] where each row is [x1, y1, x2, y2, conf, class] */
    /* Or for YOLO: [grid_h, grid_w, num_anchors * (5 + num_classes)] */

    std::vector<yolo_detection_t> detections;
    float conf_thresh = ctx->confidence_threshold;

    fprintf(stderr, "NCNN output: w=%d, h=%d, c=%d (YOLO version: %s)\n",
            out.w, out.h, out.c, yolo_version_name(ctx->yolo_version));

    /* Try to parse as detection output format */
    if (out.w == 6 || out.w == 7) {
        /* Standard detection format: [batch_id, class_id, score, x1, y1, x2, y2] or similar */
        for (int i = 0; i < out.h; i++) {
            const float* row = out.row(i);

            float score;
            int label_id;
            float x1, y1, x2, y2;

            if (out.w == 7) {
                /* [batch_id, class_id, score, x1, y1, x2, y2] */
                label_id = static_cast<int>(row[1]);
                score = row[2];
                x1 = row[3];
                y1 = row[4];
                x2 = row[5];
                y2 = row[6];
            } else {
                /* NCNN Yolov3DetectionOutput: [class_id, score, x1, y1, x2, y2] */
                label_id = static_cast<int>(row[0]);
                score = row[1];
                x1 = row[2];
                y1 = row[3];
                x2 = row[4];
                y2 = row[5];
            }

            if (score > conf_thresh) {
                yolo_detection_t det;
                det.x1 = x1;
                det.y1 = y1;
                det.x2 = x2;
                det.y2 = y2;
                det.score = score;
                det.class_id = label_id;
                detections.push_back(det);
            }
        }
    } else {
        /* Parse as raw YOLO grid output */
        /* This handles YOLOv4/v7 style outputs */
        int num_anchors = 3;
        int num_classes = model->num_classes;
        int grid_h = out.h;
        int grid_w = out.w;
        int stride = 5 + num_classes;  /* x, y, w, h, obj_conf, class_probs... */

        /* Check if dimensions match expected format */
        if (out.c >= stride * num_anchors) {
            float scale_w = static_cast<float>(w) / model->input_w;
            float scale_h = static_cast<float>(h) / model->input_h;

            for (int a = 0; a < num_anchors; a++) {
                for (int gh = 0; gh < grid_h; gh++) {
                    for (int gw = 0; gw < grid_w; gw++) {
                        int base = a * stride;

                        float obj_conf = out.channel(base + 4).row(gh)[gw];
                        if (obj_conf < conf_thresh) continue;

                        /* Find best class */
                        int best_class = 0;
                        float best_prob = 0;
                        for (int c = 0; c < num_classes; c++) {
                            float prob = out.channel(base + 5 + c).row(gh)[gw];
                            if (prob > best_prob) {
                                best_prob = prob;
                                best_class = c;
                            }
                        }

                        float score = obj_conf * best_prob;
                        if (score < conf_thresh) continue;

                        /* Get box coordinates */
                        float cx = out.channel(base + 0).row(gh)[gw];
                        float cy = out.channel(base + 1).row(gh)[gw];
                        float bw = out.channel(base + 2).row(gh)[gw];
                        float bh = out.channel(base + 3).row(gh)[gw];

                        yolo_detection_t det;
                        det.x1 = (cx - bw / 2) * scale_w;
                        det.y1 = (cy - bh / 2) * scale_h;
                        det.x2 = (cx + bw / 2) * scale_w;
                        det.y2 = (cy + bh / 2) * scale_h;
                        det.score = score;
                        det.class_id = best_class;
                        detections.push_back(det);
                    }
                }
            }
        }
    }

    /* Apply NMS using unified decoder */
    if (ctx->nms_threshold > 0 && detections.size() > 1) {
        int count = yolo_nms(detections.data(), static_cast<int>(detections.size()),
                            ctx->nms_threshold);
        detections.resize(count);
    }

    /* Convert to cira format */
    for (const auto& det : detections) {
        float norm_x, norm_y, norm_w, norm_h;

        /* Check if coordinates are already normalized (0-1 range) */
        /* Yolov3DetectionOutput layer outputs normalized coordinates */
        bool already_normalized = (det.x1 >= 0.0f && det.x1 <= 1.0f &&
                                   det.y1 >= 0.0f && det.y1 <= 1.0f &&
                                   det.x2 >= 0.0f && det.x2 <= 1.0f &&
                                   det.y2 >= 0.0f && det.y2 <= 1.0f);

        if (already_normalized) {
            /* Coordinates are already in 0-1 range */
            norm_x = det.x1;
            norm_y = det.y1;
            norm_w = det.x2 - det.x1;
            norm_h = det.y2 - det.y1;
        } else {
            /* Convert from pixel coordinates to normalized [0, 1] */
            norm_x = det.x1 / w;
            norm_y = det.y1 / h;
            norm_w = (det.x2 - det.x1) / w;
            norm_h = (det.y2 - det.y1) / h;
        }

        /* Clamp to valid range */
        norm_x = std::max(0.0f, std::min(1.0f, norm_x));
        norm_y = std::max(0.0f, std::min(1.0f, norm_y));
        norm_w = std::max(0.0f, std::min(1.0f - norm_x, norm_w));
        norm_h = std::max(0.0f, std::min(1.0f - norm_y, norm_h));

        if (!cira_add_detection(ctx, norm_x, norm_y, norm_w, norm_h,
                                det.score, det.class_id)) {
            /* Detection array full */
            break;
        }
    }

    fprintf(stderr, "NCNN inference: %d detections\n", ctx->num_detections);
    return CIRA_OK;
}

#else /* CIRA_NCNN_ENABLED */

/* Stubs when NCNN is not enabled */
extern "C" int ncnn_load(cira_ctx* ctx, const char* model_path) {
    (void)model_path;
    cira_set_error(ctx, "NCNN support not enabled in this build");
    return CIRA_ERROR_MODEL;
}

extern "C" void ncnn_unload(cira_ctx* ctx) {
    (void)ctx;
}

extern "C" int ncnn_predict(cira_ctx* ctx, const uint8_t* data, int w, int h, int channels) {
    (void)data;
    (void)w;
    (void)h;
    (void)channels;
    cira_set_error(ctx, "NCNN support not enabled in this build");
    return CIRA_ERROR_MODEL;
}

#endif /* CIRA_NCNN_ENABLED */
