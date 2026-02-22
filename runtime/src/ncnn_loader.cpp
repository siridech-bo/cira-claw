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

/* DFL (Distribution Focal Loss) decoding for YOLOv8 raw output
 * Takes 16 distribution values, applies softmax, returns weighted sum
 */
static float dfl_decode(const float* dfl_vals, int reg_max = 16) {
    /* Find max for numerical stability */
    float max_val = dfl_vals[0];
    for (int i = 1; i < reg_max; i++) {
        if (dfl_vals[i] > max_val) max_val = dfl_vals[i];
    }

    /* Softmax and weighted sum in one pass */
    float sum_exp = 0.0f;
    float weighted_sum = 0.0f;
    for (int i = 0; i < reg_max; i++) {
        float exp_val = expf(dfl_vals[i] - max_val);
        sum_exp += exp_val;
        weighted_sum += exp_val * i;
    }

    return weighted_sum / sum_exp;
}

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

    /* Input layer name (stored from network at load time) */
    char input_layer[64];

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

    /* Store input layer name from the network */
    const std::vector<const char*>& input_names = model->net.input_names();
    if (!input_names.empty()) {
        strncpy(model->input_layer, input_names[0], 63);
        model->input_layer[63] = '\0';
    } else {
        /* Fallback to common input layer names */
        strcpy(model->input_layer, "data");
    }

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

    /* Print input/output layer names for debugging */
    fprintf(stderr, "  Input layer: %s\n", model->input_layer);

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

    /* Set input using stored input layer name */
    ex.input(model->input_layer, in);

    /* Get output using stored output layer names */
    ncnn::Mat out;
    int ret = -1;
    bool is_multi_output = false;

    /* Check for YOLOv11 multi-scale outputs (out0, out1, out2) */
    bool has_out0 = false, has_out1 = false, has_out2 = false;
    for (int i = 0; i < model->num_output_layers; i++) {
        if (strcmp(model->output_layers[i], "out0") == 0) has_out0 = true;
        if (strcmp(model->output_layers[i], "out1") == 0) has_out1 = true;
        if (strcmp(model->output_layers[i], "out2") == 0) has_out2 = true;
    }

    /* YOLOv11 multi-scale output handling */
    if (has_out0 && has_out1 && has_out2) {
        ncnn::Mat out0, out1, out2;
        int ret0 = ex.extract("out0", out0);
        int ret1 = ex.extract("out1", out1);
        int ret2 = ex.extract("out2", out2);

        if (ret0 == 0 && ret1 == 0 && ret2 == 0) {
            fprintf(stderr, "NCNN: YOLOv11 multi-scale outputs detected:\n");
            fprintf(stderr, "  out0: w=%d, h=%d, c=%d\n", out0.w, out0.h, out0.c);
            fprintf(stderr, "  out1: w=%d, h=%d, c=%d\n", out1.w, out1.h, out1.c);
            fprintf(stderr, "  out2: w=%d, h=%d, c=%d\n", out2.w, out2.h, out2.c);

            /* YOLOv11 outputs have shape: [c=grid_h, h=grid_w, w=144]
             * where 144 = 64 DFL + 80 classes
             * num_boxes = c * h for each scale
             *
             * IMPORTANT: Determine scale order from actual dimensions!
             * Larger c*h = smaller stride = larger grid
             */

            int boxes0 = out0.c * out0.h;
            int boxes1 = out1.c * out1.h;
            int boxes2 = out2.c * out2.h;
            int total_boxes = boxes0 + boxes1 + boxes2;
            int box_width = out0.w;  /* Should be 144 */

            /* Determine scale order: sort by box count (descending = stride ascending) */
            struct ScaleInfo {
                ncnn::Mat* mat;
                int boxes;
                int grid_size;  /* sqrt of boxes */
                int stride;
            };
            ScaleInfo scales[3] = {
                {&out0, boxes0, (int)sqrtf((float)boxes0), 0},
                {&out1, boxes1, (int)sqrtf((float)boxes1), 0},
                {&out2, boxes2, (int)sqrtf((float)boxes2), 0}
            };

            /* Calculate strides based on input size and grid size */
            for (int i = 0; i < 3; i++) {
                if (scales[i].grid_size > 0) {
                    scales[i].stride = model->input_w / scales[i].grid_size;
                }
            }

            /* Sort scales by box count descending (largest grid = smallest stride first) */
            for (int i = 0; i < 2; i++) {
                for (int j = i + 1; j < 3; j++) {
                    if (scales[j].boxes > scales[i].boxes) {
                        ScaleInfo tmp = scales[i];
                        scales[i] = scales[j];
                        scales[j] = tmp;
                    }
                }
            }

            fprintf(stderr, "NCNN: Scale order after sorting:\n");
            for (int i = 0; i < 3; i++) {
                fprintf(stderr, "  Scale %d: %d boxes (%dx%d grid), stride=%d\n",
                        i, scales[i].boxes, scales[i].grid_size, scales[i].grid_size, scales[i].stride);
            }

            fprintf(stderr, "NCNN: Boxes per scale: %d + %d + %d = %d total\n",
                    scales[0].boxes, scales[1].boxes, scales[2].boxes, total_boxes);

            /* Create combined output: [c=1, h=total_boxes, w=144] */
            out.create(box_width, total_boxes, 1);
            if (!out.empty()) {
                float* dst = (float*)out.data;

                /* Copy scales in sorted order (largest grid first) */
                for (int i = 0; i < 3; i++) {
                    const float* src = (const float*)scales[i].mat->data;
                    memcpy(dst, src, scales[i].boxes * box_width * sizeof(float));
                    dst += scales[i].boxes * box_width;
                }

                /* Store stride info for decoding */
                /* We'll pass this via a global or recompute in decode loop */

                ret = 0;
                is_multi_output = true;
                fprintf(stderr, "NCNN: Combined output: w=%d, h=%d, c=%d (%d total boxes)\n",
                        out.w, out.h, out.c, total_boxes);

                /* Debug: print first few class scores to verify sigmoid */
                if (out.h > 0) {
                    const float* first_box = (const float*)out.data;
                    fprintf(stderr, "NCNN: First box class scores (64-73): ");
                    for (int c = 0; c < 10; c++) {
                        fprintf(stderr, "%.3f ", first_box[64 + c]);
                    }
                    fprintf(stderr, "\n");
                }
            } else {
                cira_set_error(ctx, "Failed to allocate combined output tensor");
                return CIRA_ERROR_MEMORY;
            }
        }
    }

    /* Single output layer fallback */
    if (!is_multi_output) {
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
    }

    if (ret != 0 || (out.w == 0 && out.h == 0 && out.c == 0)) {
        cira_set_error(ctx, "Failed to extract NCNN output (no valid output layer found)");
        return CIRA_ERROR;
    }

    /* Parse YOLO output using unified decoder */
    fprintf(stderr, "NCNN output: w=%d, h=%d, c=%d (YOLO version: %s)\n",
            out.w, out.h, out.c, yolo_version_name(ctx->yolo_version));

    std::vector<yolo_detection_t> detections;
    float conf_thresh = ctx->confidence_threshold;
    int num_classes = model->num_classes;

    /* Detect output format and use unified decoder when possible */
    bool use_unified_decoder = false;
    int64_t output_shape[4] = {1, 0, 0, 0};
    int num_dims = 3;

    /* Check for NCNN Yolov3DetectionOutput format: [num_dets, 6] */
    if (out.w == 6 || out.w == 7) {
        /* Pre-decoded detection format - parse directly */
        for (int i = 0; i < out.h; i++) {
            const float* row = out.row(i);
            float score;
            int label_id;
            float x1, y1, x2, y2;

            if (out.w == 7) {
                label_id = static_cast<int>(row[1]);
                score = row[2];
                x1 = row[3]; y1 = row[4]; x2 = row[5]; y2 = row[6];
            } else {
                label_id = static_cast<int>(row[0]);
                score = row[1];
                x1 = row[2]; y1 = row[3]; x2 = row[4]; y2 = row[5];
            }

            if (score > conf_thresh) {
                yolo_detection_t det = {x1, y1, x2, y2, score, label_id};
                detections.push_back(det);
            }
        }
    }
    /* Check for YOLOv8 DFL format: [c=1, h=num_boxes, w=64+classes] (raw Distribution Focal Loss) */
    /* 64 = 4 coords * 16 DFL bins, followed by class scores */
    else if (out.c == 1 && out.h > 1000 && out.w == 64 + num_classes) {
        fprintf(stderr, "NCNN: Detected YOLOv8 DFL format (h=%d boxes, w=%d = 64 DFL + %d classes)\n",
                out.h, out.w, num_classes);

        /* YOLOv8 anchor-free detection with DFL
         * Output shape: [num_boxes, 64 + num_classes]
         * - First 64 values: DFL for bbox (4 * 16 bins for left, top, right, bottom)
         * - Remaining values: class scores (raw logits)
         *
         * Decode steps:
         * 1. Apply DFL to get 4 distance values (dist_left, dist_top, dist_right, dist_bottom)
         * 2. Convert distances to bbox: x1 = cx - dist_left, y1 = cy - dist_top, etc.
         * 3. Apply sigmoid to class scores and find max
         */

        /* YOLOv8/v11 uses 3 scales with 8400 total boxes for 640 input
         * Dynamically compute grid sizes and strides from actual box count
         */
        int total_boxes = out.h;

        /* Standard YOLOv8/v11 ratios: 64:16:4 = 6400:1600:400 for 640 input */
        /* Grid sizes are sqrt of box counts, strides are input_w / grid_size */
        int grid_sizes[3];
        int strides[3];
        int expected_boxes[3];

        /* Compute based on input size */
        int base_grid = model->input_w / 8;  /* Largest grid at stride 8 */
        grid_sizes[0] = base_grid;           /* 80 for 640 input */
        grid_sizes[1] = base_grid / 2;       /* 40 for 640 input */
        grid_sizes[2] = base_grid / 4;       /* 20 for 640 input */

        strides[0] = 8;
        strides[1] = 16;
        strides[2] = 32;

        expected_boxes[0] = grid_sizes[0] * grid_sizes[0];
        expected_boxes[1] = grid_sizes[1] * grid_sizes[1];
        expected_boxes[2] = grid_sizes[2] * grid_sizes[2];

        int expected_total = expected_boxes[0] + expected_boxes[1] + expected_boxes[2];

        fprintf(stderr, "NCNN: DFL decode - input=%dx%d, total_boxes=%d (expected %d)\n",
                model->input_w, model->input_h, total_boxes, expected_total);
        fprintf(stderr, "NCNN: Grid sizes: %d, %d, %d, Strides: %d, %d, %d\n",
                grid_sizes[0], grid_sizes[1], grid_sizes[2],
                strides[0], strides[1], strides[2]);

        int scale_factor = 1;  /* Already computed in grid sizes */

        /* Check if class scores need sigmoid (YOLOv11 NCNN exports have convsigmoid layers) */
        /* Sample first box to detect if values are already probabilities */
        bool needs_sigmoid = false;
        if (out.h > 0) {
            const float* sample = (const float*)out.data;
            for (int c = 0; c < num_classes && c < 10; c++) {
                float val = sample[64 + c];
                if (val < 0.0f || val > 1.0f) {
                    needs_sigmoid = true;
                    break;
                }
            }
        }
        fprintf(stderr, "NCNN: Class scores need sigmoid: %s\n", needs_sigmoid ? "yes" : "no (already probabilities)");

        int box_idx = 0;
        for (int scale = 0; scale < 3 && box_idx < out.h; scale++) {
            int grid_h = grid_sizes[scale] * scale_factor;
            int grid_w = grid_sizes[scale] * scale_factor;
            int stride = strides[scale];

            for (int gy = 0; gy < grid_h && box_idx < out.h; gy++) {
                for (int gx = 0; gx < grid_w && box_idx < out.h; gx++) {
                    const float* row = (const float*)out.data + box_idx * out.w;

                    /* Find best class score */
                    int best_class = 0;
                    float best_score = 0.0f;
                    for (int c = 0; c < num_classes; c++) {
                        float score = row[64 + c];
                        if (needs_sigmoid) {
                            score = 1.0f / (1.0f + expf(-score));  /* sigmoid */
                        }
                        if (score > best_score) {
                            best_score = score;
                            best_class = c;
                        }
                    }

                    if (best_score > conf_thresh) {
                        /* Decode DFL to get distances */
                        float dist_left = dfl_decode(row + 0);
                        float dist_top = dfl_decode(row + 16);
                        float dist_right = dfl_decode(row + 32);
                        float dist_bottom = dfl_decode(row + 48);

                        /* Anchor point is center of grid cell */
                        float cx = (gx + 0.5f) * stride;
                        float cy = (gy + 0.5f) * stride;

                        /* Convert distances to bbox coordinates */
                        float x1 = cx - dist_left * stride;
                        float y1 = cy - dist_top * stride;
                        float x2 = cx + dist_right * stride;
                        float y2 = cy + dist_bottom * stride;

                        /* Clamp to image bounds */
                        x1 = fmaxf(0.0f, fminf(x1, (float)model->input_w));
                        y1 = fmaxf(0.0f, fminf(y1, (float)model->input_h));
                        x2 = fmaxf(0.0f, fminf(x2, (float)model->input_w));
                        y2 = fmaxf(0.0f, fminf(y2, (float)model->input_h));

                        /* Scale to original image size */
                        float scale_x = (float)w / model->input_w;
                        float scale_y = (float)h / model->input_h;

                        yolo_detection_t det;
                        det.x1 = x1 * scale_x;
                        det.y1 = y1 * scale_y;
                        det.x2 = x2 * scale_x;
                        det.y2 = y2 * scale_y;
                        det.score = best_score;
                        det.class_id = best_class;

                        detections.push_back(det);
                    }

                    box_idx++;
                }
            }
        }

        fprintf(stderr, "NCNN: DFL decoded %zu candidates (before NMS, threshold=%.2f)\n",
                detections.size(), conf_thresh);

        /* Debug: show score distribution of first few detections */
        if (detections.size() > 0) {
            fprintf(stderr, "NCNN: Sample detection scores: ");
            for (size_t i = 0; i < detections.size() && i < 10; i++) {
                fprintf(stderr, "%.3f ", detections[i].score);
            }
            fprintf(stderr, "\n");
        }
    }
    /* Check for YOLOv8/v11 transposed format: [1, 4+C, num_boxes] */
    /* NCNN Mat: c=1, h=4+classes, w=num_boxes (e.g., 8400) */
    else if (out.c == 1 && out.h == 4 + num_classes && out.w > 1000) {
        fprintf(stderr, "NCNN: Detected YOLOv8/v11 transposed format\n");
        output_shape[1] = out.h;  /* 4+classes */
        output_shape[2] = out.w;  /* num_boxes */
        use_unified_decoder = true;
    }
    /* Check for YOLOv8/v11 alternative: c=4+classes, h=num_boxes, w=1 */
    else if (out.w == 1 && out.c == 4 + num_classes && out.h > 1000) {
        fprintf(stderr, "NCNN: Detected YOLOv8/v11 format (c=%d, h=%d)\n", out.c, out.h);
        /* Need to reshape: treat as [1, c, h] */
        output_shape[1] = out.c;
        output_shape[2] = out.h;
        use_unified_decoder = true;
    }
    /* Check for YOLOv5/v7 format: [num_boxes, 5+classes] */
    else if (out.h > 1000 && out.w == 5 + num_classes) {
        fprintf(stderr, "NCNN: Detected YOLOv5/v7 format\n");
        output_shape[1] = out.h;
        output_shape[2] = out.w;
        use_unified_decoder = true;
    }
    /* Fallback: try YOLOv4 grid format */
    else if (out.c > 0) {
        /* Parse as raw YOLO grid output - YOLOv4 style */
        int num_anchors = 3;
        int stride = 5 + num_classes;

        if (out.c >= stride * num_anchors) {
            float scale_w = static_cast<float>(w) / model->input_w;
            float scale_h = static_cast<float>(h) / model->input_h;

            for (int a = 0; a < num_anchors; a++) {
                for (int gh = 0; gh < out.h; gh++) {
                    for (int gw = 0; gw < out.w; gw++) {
                        int base = a * stride;
                        float obj_conf = out.channel(base + 4).row(gh)[gw];
                        if (obj_conf < conf_thresh) continue;

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

    /* Use unified decoder for YOLOv5/v7/v8/v11 formats */
    if (use_unified_decoder) {
        yolo_decode_config_t decode_config;
        decode_config.version = ctx->yolo_version;
        decode_config.input_w = model->input_w;
        decode_config.input_h = model->input_h;
        decode_config.num_classes = num_classes;
        decode_config.conf_threshold = conf_thresh;
        decode_config.nms_threshold = ctx->nms_threshold;
        decode_config.max_detections = CIRA_MAX_DETECTIONS;

        /* Flatten NCNN Mat to contiguous array */
        std::vector<float> flat_output(out.total());
        memcpy(flat_output.data(), out.data, out.total() * sizeof(float));

        detections.resize(CIRA_MAX_DETECTIONS);
        int count = yolo_decode(flat_output.data(), output_shape, num_dims,
                               &decode_config, detections.data(), CIRA_MAX_DETECTIONS);
        if (count > 0) {
            detections.resize(count);
        } else {
            detections.clear();
        }
    }

    /* Apply NMS for non-unified decoder paths */
    if (!use_unified_decoder && ctx->nms_threshold > 0 && detections.size() > 1) {
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
