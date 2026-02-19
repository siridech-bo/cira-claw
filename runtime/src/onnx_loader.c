/**
 * CiRA Runtime - ONNX Model Loader
 *
 * This file implements loading and inference for ONNX models
 * using the ONNX Runtime C API.
 *
 * Supported YOLO output formats:
 * - Format A: [1, num_detections, 6] - [class_id, score, x1, y1, x2, y2]
 * - Format B: [1, num_detections, 7] - [batch_id, class_id, score, x1, y1, x2, y2]
 * - Format C: [1, num_detections, 5+num_classes] - [cx, cy, w, h, obj_conf, class_probs...]
 *
 * (c) CiRA Robotics / KMITL 2026
 */

#include "cira.h"
#include "cira_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>

#ifdef CIRA_ONNX_ENABLED

#include <onnxruntime_c_api.h>

/* Suppress unused result warnings for non-critical ORT calls */
#define ORT_IGNORE(expr) do { OrtStatus* _s = (expr); if (_s) g_ort->ReleaseStatus(_s); } while(0)

/* ONNX Runtime globals */
static const OrtApi* g_ort = NULL;

/* Internal ONNX model structure */
typedef struct {
    OrtEnv* env;
    OrtSession* session;
    OrtSessionOptions* session_options;
    OrtMemoryInfo* memory_info;

    /* Input/output info */
    char* input_name;
    char* output_names[4];    /* Up to 4 outputs (YOLO has 3 scales) */
    size_t num_outputs;
    int64_t input_shape[4];   /* NCHW or NHWC */
    int64_t output_shape[4];  /* For parsing output format */
    size_t output_dims;
    int input_w;
    int input_h;
    int input_c;
    int num_classes;
    int is_nhwc;              /* 1 if input is NHWC, 0 if NCHW */
} onnx_model_t;

/* Detection structure for NMS */
typedef struct {
    float x1, y1, x2, y2;   /* Bounding box corners (normalized) */
    float confidence;
    int label_id;
} onnx_detection_t;

/* ============================================
 * Helper Functions
 * ============================================ */

/* Check if path is a directory */
static int is_dir(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

/* Find file with extension in directory */
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

/* Bilinear interpolation resize (pure C, no OpenCV) */
static void bilinear_resize(const uint8_t* src, int src_w, int src_h,
                            uint8_t* dst, int dst_w, int dst_h, int channels) {
    float x_ratio = (float)(src_w - 1) / (dst_w - 1);
    float y_ratio = (float)(src_h - 1) / (dst_h - 1);

    for (int y = 0; y < dst_h; y++) {
        float fy = y * y_ratio;
        int y_low = (int)fy;
        int y_high = y_low + 1;
        if (y_high >= src_h) y_high = src_h - 1;
        float y_lerp = fy - y_low;

        for (int x = 0; x < dst_w; x++) {
            float fx = x * x_ratio;
            int x_low = (int)fx;
            int x_high = x_low + 1;
            if (x_high >= src_w) x_high = src_w - 1;
            float x_lerp = fx - x_low;

            for (int c = 0; c < channels; c++) {
                /* Get four corners */
                float c00 = src[(y_low * src_w + x_low) * channels + c];
                float c10 = src[(y_low * src_w + x_high) * channels + c];
                float c01 = src[(y_high * src_w + x_low) * channels + c];
                float c11 = src[(y_high * src_w + x_high) * channels + c];

                /* Bilinear interpolation */
                float top = c00 * (1.0f - x_lerp) + c10 * x_lerp;
                float bottom = c01 * (1.0f - x_lerp) + c11 * x_lerp;
                float value = top * (1.0f - y_lerp) + bottom * y_lerp;

                dst[(y * dst_w + x) * channels + c] = (uint8_t)(value + 0.5f);
            }
        }
    }
}

/* Compute IoU (Intersection over Union) */
static float compute_iou(const onnx_detection_t* a, const onnx_detection_t* b) {
    float inter_x1 = a->x1 > b->x1 ? a->x1 : b->x1;
    float inter_y1 = a->y1 > b->y1 ? a->y1 : b->y1;
    float inter_x2 = a->x2 < b->x2 ? a->x2 : b->x2;
    float inter_y2 = a->y2 < b->y2 ? a->y2 : b->y2;

    float inter_w = inter_x2 - inter_x1;
    float inter_h = inter_y2 - inter_y1;
    if (inter_w < 0) inter_w = 0;
    if (inter_h < 0) inter_h = 0;
    float inter_area = inter_w * inter_h;

    float area_a = (a->x2 - a->x1) * (a->y2 - a->y1);
    float area_b = (b->x2 - b->x1) * (b->y2 - b->y1);

    return inter_area / (area_a + area_b - inter_area + 1e-6f);
}

/* Comparison function for qsort (descending by confidence) */
static int detection_compare(const void* a, const void* b) {
    const onnx_detection_t* da = (const onnx_detection_t*)a;
    const onnx_detection_t* db = (const onnx_detection_t*)b;
    if (da->confidence > db->confidence) return -1;
    if (da->confidence < db->confidence) return 1;
    return 0;
}

/* Non-Maximum Suppression (pure C implementation) */
static int nms_detections(onnx_detection_t* dets, int count, float nms_thresh) {
    if (count <= 1) return count;

    /* Sort by confidence descending */
    qsort(dets, count, sizeof(onnx_detection_t), detection_compare);

    /* Allocate suppression flags */
    int* suppressed = (int*)calloc(count, sizeof(int));
    if (!suppressed) return count;

    int result_count = 0;

    for (int i = 0; i < count; i++) {
        if (suppressed[i]) continue;

        /* Keep this detection */
        if (i != result_count) {
            dets[result_count] = dets[i];
        }
        result_count++;

        /* Suppress overlapping detections */
        for (int j = i + 1; j < count; j++) {
            if (suppressed[j]) continue;

            /* Only suppress same class or all classes if doing class-agnostic NMS */
            if (compute_iou(&dets[i], &dets[j]) > nms_thresh) {
                suppressed[j] = 1;
            }
        }
    }

    free(suppressed);
    return result_count;
}

/* Clamp value to range */
static float clamp_f(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/**
 * Initialize ONNX Runtime (call once)
 */
static int init_onnx_runtime(void) {
    if (g_ort) return CIRA_OK;

    g_ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!g_ort) {
        fprintf(stderr, "Failed to get ONNX Runtime API\n");
        return CIRA_ERROR;
    }

    return CIRA_OK;
}

/**
 * Load an ONNX model.
 *
 * @param ctx Context handle
 * @param model_path Path to .onnx file or directory containing one
 * @return CIRA_OK on success
 */
int onnx_load(cira_ctx* ctx, const char* model_path) {
    if (init_onnx_runtime() != CIRA_OK) {
        return CIRA_ERROR;
    }

    char onnx_file_path[1024];
    const char* actual_path = model_path;

    /* If path is a directory, find the .onnx file inside */
    if (is_dir(model_path)) {
        if (!find_file_ext(model_path, ".onnx", onnx_file_path, sizeof(onnx_file_path))) {
            cira_set_error(ctx, "No .onnx file found in directory: %s", model_path);
            return CIRA_ERROR_FILE;
        }
        actual_path = onnx_file_path;
        fprintf(stderr, "Found ONNX model: %s\n", actual_path);
    }

    /* Allocate model structure */
    onnx_model_t* model = (onnx_model_t*)calloc(1, sizeof(onnx_model_t));
    if (!model) {
        cira_set_error(ctx, "Failed to allocate ONNX model structure");
        return CIRA_ERROR_MEMORY;
    }

    OrtStatus* status = NULL;

    /* Create environment */
    status = g_ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "cira", &model->env);
    if (status != NULL) {
        fprintf(stderr, "Failed to create ONNX environment: %s\n",
                g_ort->GetErrorMessage(status));
        g_ort->ReleaseStatus(status);
        free(model);
        return CIRA_ERROR;
    }

    /* Create session options */
    status = g_ort->CreateSessionOptions(&model->session_options);
    if (status != NULL) {
        fprintf(stderr, "Failed to create session options: %s\n",
                g_ort->GetErrorMessage(status));
        g_ort->ReleaseStatus(status);
        g_ort->ReleaseEnv(model->env);
        free(model);
        return CIRA_ERROR;
    }

    /* Set optimization level (ignore return - non-critical) */
    ORT_IGNORE(g_ort->SetSessionGraphOptimizationLevel(model->session_options,
                                                        ORT_ENABLE_EXTENDED));

    /* Set number of threads (ignore return - non-critical) */
    ORT_IGNORE(g_ort->SetIntraOpNumThreads(model->session_options, 0));

    /* Create session */
#ifdef _WIN32
    /* Windows needs wide string path */
    wchar_t wide_path[1024];
    size_t converted = 0;
    mbstowcs_s(&converted, wide_path, sizeof(wide_path)/sizeof(wchar_t), actual_path, _TRUNCATE);
    status = g_ort->CreateSession(model->env, wide_path,
                                   model->session_options, &model->session);
#else
    status = g_ort->CreateSession(model->env, actual_path,
                                   model->session_options, &model->session);
#endif
    if (status != NULL) {
        fprintf(stderr, "Failed to create ONNX session: %s\n",
                g_ort->GetErrorMessage(status));
        g_ort->ReleaseStatus(status);
        g_ort->ReleaseSessionOptions(model->session_options);
        g_ort->ReleaseEnv(model->env);
        free(model);
        return CIRA_ERROR_MODEL;
    }

    /* Create memory info for CPU */
    status = g_ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault,
                                         &model->memory_info);
    if (status != NULL) {
        fprintf(stderr, "Failed to create memory info: %s\n",
                g_ort->GetErrorMessage(status));
        g_ort->ReleaseStatus(status);
        g_ort->ReleaseSession(model->session);
        g_ort->ReleaseSessionOptions(model->session_options);
        g_ort->ReleaseEnv(model->env);
        free(model);
        return CIRA_ERROR;
    }

    /* Get allocator */
    OrtAllocator* allocator = NULL;
    ORT_IGNORE(g_ort->GetAllocatorWithDefaultOptions(&allocator));

    /* Get input info */
    status = g_ort->SessionGetInputName(model->session, 0, allocator,
                                         &model->input_name);
    if (status != NULL) {
        fprintf(stderr, "Failed to get input name: %s\n",
                g_ort->GetErrorMessage(status));
        g_ort->ReleaseStatus(status);
    }

    /* Get input shape */
    OrtTypeInfo* input_type_info;
    status = g_ort->SessionGetInputTypeInfo(model->session, 0, &input_type_info);
    if (status == NULL) {
        const OrtTensorTypeAndShapeInfo* tensor_info = NULL;
        ORT_IGNORE(g_ort->CastTypeInfoToTensorInfo(input_type_info, &tensor_info));

        size_t num_dims = 0;
        ORT_IGNORE(g_ort->GetDimensionsCount(tensor_info, &num_dims));

        if (num_dims == 4) {
            ORT_IGNORE(g_ort->GetDimensions(tensor_info, model->input_shape, num_dims));

            /* Handle dynamic batch dimension (-1) */
            if (model->input_shape[0] <= 0) {
                model->input_shape[0] = 1;
            }

            /* Detect NHWC vs NCHW format:
             * NCHW: [batch, channels, height, width] - channels is small (1,3,4)
             * NHWC: [batch, height, width, channels] - last dim is small (1,3,4)
             */
            if (model->input_shape[3] <= 4 && model->input_shape[1] > 4) {
                /* NHWC format (e.g., 1x416x416x3) */
                model->is_nhwc = 1;
                model->input_h = (int)model->input_shape[1];
                model->input_w = (int)model->input_shape[2];
                model->input_c = (int)model->input_shape[3];
                fprintf(stderr, "ONNX input shape: %lldx%lldx%lldx%lld (NHWC)\n",
                        (long long)model->input_shape[0], (long long)model->input_shape[1],
                        (long long)model->input_shape[2], (long long)model->input_shape[3]);
            } else {
                /* NCHW format (e.g., 1x3x416x416) */
                model->is_nhwc = 0;
                model->input_c = (int)model->input_shape[1];
                model->input_h = (int)model->input_shape[2];
                model->input_w = (int)model->input_shape[3];
                fprintf(stderr, "ONNX input shape: %lldx%lldx%lldx%lld (NCHW)\n",
                        (long long)model->input_shape[0], (long long)model->input_shape[1],
                        (long long)model->input_shape[2], (long long)model->input_shape[3]);
            }

            /* Handle dynamic dimensions */
            if (model->input_w <= 0) model->input_w = 416;
            if (model->input_h <= 0) model->input_h = 416;
            if (model->input_c <= 0) model->input_c = 3;
        }

        g_ort->ReleaseTypeInfo(input_type_info);
    }

    /* Check number of outputs */
    ORT_IGNORE(g_ort->SessionGetOutputCount(model->session, &model->num_outputs));
    if (model->num_outputs > 4) model->num_outputs = 4;  /* Limit to 4 */
    fprintf(stderr, "ONNX model has %zu output(s)\n", model->num_outputs);

    /* Get all output names (for multi-scale YOLO) */
    for (size_t i = 0; i < model->num_outputs; i++) {
        model->output_names[i] = NULL;
        status = g_ort->SessionGetOutputName(model->session, i, allocator, &model->output_names[i]);
        if (status == NULL && model->output_names[i]) {
            fprintf(stderr, "  Output[%zu]: %s\n", i, model->output_names[i]);
        }
    }

    /* Get output shape for format detection */
    OrtTypeInfo* output_type_info;
    status = g_ort->SessionGetOutputTypeInfo(model->session, 0, &output_type_info);
    if (status == NULL) {
        const OrtTensorTypeAndShapeInfo* tensor_info = NULL;
        ORT_IGNORE(g_ort->CastTypeInfoToTensorInfo(output_type_info, &tensor_info));

        ORT_IGNORE(g_ort->GetDimensionsCount(tensor_info, &model->output_dims));

        /* Handle up to 5 dimensions */
        int64_t out_shape[5] = {0};
        if (model->output_dims <= 5) {
            ORT_IGNORE(g_ort->GetDimensions(tensor_info, out_shape, model->output_dims));
            for (size_t i = 0; i < model->output_dims && i < 4; i++) {
                model->output_shape[i] = out_shape[i];
            }
        }

        fprintf(stderr, "ONNX output dims: %zu, shape: [", model->output_dims);
        for (size_t i = 0; i < model->output_dims; i++) {
            fprintf(stderr, "%lld%s", (long long)out_shape[i],
                    i < model->output_dims - 1 ? ", " : "");
        }
        fprintf(stderr, "]\n");

        /* Try to infer num_classes from output shape */
        if (model->output_dims == 3 && out_shape[2] > 6) {
            /* Format C: [1, N, 5+num_classes] */
            model->num_classes = (int)(out_shape[2] - 5);
        }

        g_ort->ReleaseTypeInfo(output_type_info);
    }

    /* Use labels already loaded by cira_load() if available */
    if (ctx->num_labels > 0) {
        model->num_classes = ctx->num_labels;
    } else if (model->num_classes == 0) {
        model->num_classes = 80;  /* Default to COCO classes */
    }

    fprintf(stderr, "ONNX model loaded successfully\n");
    fprintf(stderr, "  Input: %s (%dx%d)\n", model->input_name,
            model->input_w, model->input_h);
    fprintf(stderr, "  Outputs: %zu\n", model->num_outputs);
    fprintf(stderr, "  Classes: %d\n", model->num_classes);

    /* Store model in context */
    ctx->model_handle = model;
    ctx->input_w = model->input_w;
    ctx->input_h = model->input_h;
    ctx->format = CIRA_FORMAT_ONNX;

    return CIRA_OK;
}

/**
 * Unload ONNX model.
 */
void onnx_unload(cira_ctx* ctx) {
    if (!ctx || !ctx->model_handle) return;

    onnx_model_t* model = (onnx_model_t*)ctx->model_handle;

    /* Free allocated names using default allocator */
    OrtAllocator* allocator = NULL;
    ORT_IGNORE(g_ort->GetAllocatorWithDefaultOptions(&allocator));

    if (allocator && model->input_name) {
        allocator->Free(allocator, model->input_name);
    }
    for (size_t i = 0; i < model->num_outputs && i < 4; i++) {
        if (allocator && model->output_names[i]) {
            allocator->Free(allocator, model->output_names[i]);
        }
    }
    if (model->memory_info) g_ort->ReleaseMemoryInfo(model->memory_info);
    if (model->session) g_ort->ReleaseSession(model->session);
    if (model->session_options) g_ort->ReleaseSessionOptions(model->session_options);
    if (model->env) g_ort->ReleaseEnv(model->env);

    free(model);
    ctx->model_handle = NULL;

    fprintf(stderr, "ONNX model unloaded\n");
}

/**
 * Run ONNX inference on an image.
 *
 * @param ctx Context with loaded ONNX model
 * @param data RGB image data (packed HWC, row-major)
 * @param w Image width
 * @param h Image height
 * @param channels Number of channels (must be 3)
 * @return CIRA_OK on success
 */
int onnx_predict(cira_ctx* ctx, const uint8_t* data, int w, int h, int channels) {
    if (!ctx || !ctx->model_handle || !data) {
        cira_set_error(ctx, "Invalid parameters to onnx_predict");
        return CIRA_ERROR_INPUT;
    }

    if (channels != 3) {
        cira_set_error(ctx, "Only 3-channel RGB images supported");
        return CIRA_ERROR_INPUT;
    }

    onnx_model_t* model = (onnx_model_t*)ctx->model_handle;

    /* Clear previous detections */
    cira_clear_detections(ctx);

    OrtStatus* status = NULL;

    /* Step 1: Resize image to model input size */
    int input_size = model->input_w * model->input_h * channels;
    uint8_t* resized = (uint8_t*)malloc(input_size);
    if (!resized) {
        cira_set_error(ctx, "Failed to allocate resize buffer");
        return CIRA_ERROR_MEMORY;
    }

    if (w == model->input_w && h == model->input_h) {
        memcpy(resized, data, input_size);
    } else {
        bilinear_resize(data, w, h, resized, model->input_w, model->input_h, channels);
    }

    /* Step 2: Convert to float32 and normalize to 0-1 */
    /* Handle both NCHW (PyTorch style) and NHWC (TensorFlow style) formats */
    size_t tensor_size = 1 * model->input_c * model->input_h * model->input_w;
    float* input_tensor_data = (float*)malloc(tensor_size * sizeof(float));
    if (!input_tensor_data) {
        free(resized);
        cira_set_error(ctx, "Failed to allocate input tensor");
        return CIRA_ERROR_MEMORY;
    }

    if (model->is_nhwc) {
        /* NHWC format: keep HWC layout, just normalize */
        for (int y = 0; y < model->input_h; y++) {
            for (int x = 0; x < model->input_w; x++) {
                for (int c = 0; c < model->input_c; c++) {
                    int idx = (y * model->input_w + x) * channels + c;
                    input_tensor_data[idx] = resized[idx] / 255.0f;
                }
            }
        }
    } else {
        /* NCHW format: convert HWC -> CHW */
        for (int c = 0; c < model->input_c; c++) {
            for (int y = 0; y < model->input_h; y++) {
                for (int x = 0; x < model->input_w; x++) {
                    int src_idx = (y * model->input_w + x) * channels + c;
                    int dst_idx = c * model->input_h * model->input_w + y * model->input_w + x;
                    input_tensor_data[dst_idx] = resized[src_idx] / 255.0f;
                }
            }
        }
    }

    free(resized);

    /* Step 3: Create input tensor with correct shape based on format */
    int64_t input_shape[4];
    if (model->is_nhwc) {
        input_shape[0] = 1;
        input_shape[1] = model->input_h;
        input_shape[2] = model->input_w;
        input_shape[3] = model->input_c;
    } else {
        input_shape[0] = 1;
        input_shape[1] = model->input_c;
        input_shape[2] = model->input_h;
        input_shape[3] = model->input_w;
    }
    OrtValue* input_tensor = NULL;

    status = g_ort->CreateTensorWithDataAsOrtValue(
        model->memory_info,
        input_tensor_data, tensor_size * sizeof(float),
        input_shape, 4,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
        &input_tensor);

    if (status != NULL) {
        fprintf(stderr, "Failed to create input tensor: %s\n",
                g_ort->GetErrorMessage(status));
        g_ort->ReleaseStatus(status);
        free(input_tensor_data);
        return CIRA_ERROR;
    }

    /* Step 4: Run inference with all outputs */
    const char* input_names[] = { model->input_name };
    const char* out_names[4];
    for (size_t i = 0; i < model->num_outputs && i < 4; i++) {
        out_names[i] = model->output_names[i];
    }

    OrtValue* output_tensors[4] = {NULL, NULL, NULL, NULL};

    status = g_ort->Run(model->session, NULL,
                        input_names, (const OrtValue* const*)&input_tensor, 1,
                        out_names, model->num_outputs, output_tensors);

    g_ort->ReleaseValue(input_tensor);
    free(input_tensor_data);

    if (status != NULL) {
        fprintf(stderr, "ONNX inference failed: %s\n",
                g_ort->GetErrorMessage(status));
        g_ort->ReleaseStatus(status);
        return CIRA_ERROR;
    }

    /* Step 5: Process all output tensors */
    float conf_thresh = ctx->confidence_threshold;
    int max_detections = CIRA_MAX_DETECTIONS * 4;  /* Pre-NMS buffer */
    onnx_detection_t* detections = (onnx_detection_t*)malloc(max_detections * sizeof(onnx_detection_t));
    int num_detections = 0;

    if (!detections) {
        for (size_t i = 0; i < model->num_outputs; i++) {
            if (output_tensors[i]) g_ort->ReleaseValue(output_tensors[i]);
        }
        return CIRA_ERROR_MEMORY;
    }

    /* Process each output scale */
    for (size_t out_idx = 0; out_idx < model->num_outputs; out_idx++) {
        if (!output_tensors[out_idx]) continue;

        float* output_data;
        status = g_ort->GetTensorMutableData(output_tensors[out_idx], (void**)&output_data);
        if (status != NULL) {
            g_ort->ReleaseStatus(status);
            continue;  /* Skip this output */
        }

        /* Get output shape for parsing */
        OrtTensorTypeAndShapeInfo* output_info = NULL;
        ORT_IGNORE(g_ort->GetTensorTypeAndShape(output_tensors[out_idx], &output_info));

    size_t num_dims = 0;
    size_t total_elements = 0;
    int64_t output_shape[6] = {0};

    if (output_info) {
        ORT_IGNORE(g_ort->GetDimensionsCount(output_info, &num_dims));
        ORT_IGNORE(g_ort->GetTensorShapeElementCount(output_info, &total_elements));

        if (num_dims <= 6) {
            ORT_IGNORE(g_ort->GetDimensions(output_info, output_shape, num_dims));
        }
        g_ort->ReleaseTensorTypeAndShapeInfo(output_info);
    }

    /* Debug: print output shape and sample values */
    fprintf(stderr, "ONNX output: dims=%zu, elements=%zu, shape=[", num_dims, total_elements);
    for (size_t i = 0; i < num_dims && i < 6; i++) {
        fprintf(stderr, "%lld%s", (long long)output_shape[i], i < num_dims - 1 ? ", " : "");
    }
    fprintf(stderr, "]\n");

    /* Print first few output values for debugging */
    if (total_elements > 0) {
        fprintf(stderr, "  First 10 values: ");
        for (size_t i = 0; i < 10 && i < total_elements; i++) {
            fprintf(stderr, "%.3f ", output_data[i]);
        }
        fprintf(stderr, "\n");
    }

        /* Determine output format and parse */
    if (num_dims == 3) {
        /* Shape: [batch, num_boxes, values_per_box] */
        int64_t num_boxes = output_shape[1];
        int64_t values_per_box = output_shape[2];

        if (values_per_box == 6) {
            /* Format A: [class_id, score, x1, y1, x2, y2] */
            for (int64_t i = 0; i < num_boxes && num_detections < max_detections; i++) {
                const float* row = output_data + i * 6;
                int label_id = (int)row[0];
                float score = row[1];

                if (score > conf_thresh && label_id >= 0 && label_id < model->num_classes) {
                    float x1 = row[2];
                    float y1 = row[3];
                    float x2 = row[4];
                    float y2 = row[5];

                    /* Normalize if in pixel coordinates */
                    if (x2 > 1.0f || y2 > 1.0f) {
                        x1 /= model->input_w;
                        y1 /= model->input_h;
                        x2 /= model->input_w;
                        y2 /= model->input_h;
                    }

                    detections[num_detections].x1 = clamp_f(x1, 0.0f, 1.0f);
                    detections[num_detections].y1 = clamp_f(y1, 0.0f, 1.0f);
                    detections[num_detections].x2 = clamp_f(x2, 0.0f, 1.0f);
                    detections[num_detections].y2 = clamp_f(y2, 0.0f, 1.0f);
                    detections[num_detections].confidence = score;
                    detections[num_detections].label_id = label_id;
                    num_detections++;
                }
            }
        }
        else if (values_per_box == 7) {
            /* Format B: [batch_id, class_id, score, x1, y1, x2, y2] */
            for (int64_t i = 0; i < num_boxes && num_detections < max_detections; i++) {
                const float* row = output_data + i * 7;
                int label_id = (int)row[1];
                float score = row[2];

                if (score > conf_thresh && label_id >= 0 && label_id < model->num_classes) {
                    float x1 = row[3];
                    float y1 = row[4];
                    float x2 = row[5];
                    float y2 = row[6];

                    /* Normalize if in pixel coordinates */
                    if (x2 > 1.0f || y2 > 1.0f) {
                        x1 /= model->input_w;
                        y1 /= model->input_h;
                        x2 /= model->input_w;
                        y2 /= model->input_h;
                    }

                    detections[num_detections].x1 = clamp_f(x1, 0.0f, 1.0f);
                    detections[num_detections].y1 = clamp_f(y1, 0.0f, 1.0f);
                    detections[num_detections].x2 = clamp_f(x2, 0.0f, 1.0f);
                    detections[num_detections].y2 = clamp_f(y2, 0.0f, 1.0f);
                    detections[num_detections].confidence = score;
                    detections[num_detections].label_id = label_id;
                    num_detections++;
                }
            }
        }
        else if (values_per_box >= 5 + model->num_classes) {
            /* Format C: [cx, cy, w, h, obj_conf, class_probs...] (YOLOv4/v7 raw) */
            int num_classes = model->num_classes;

            for (int64_t i = 0; i < num_boxes && num_detections < max_detections; i++) {
                const float* row = output_data + i * values_per_box;

                float obj_conf = row[4];
                if (obj_conf < conf_thresh) continue;

                /* Find best class */
                int best_class = 0;
                float best_prob = 0;
                for (int c = 0; c < num_classes; c++) {
                    float prob = row[5 + c];
                    if (prob > best_prob) {
                        best_prob = prob;
                        best_class = c;
                    }
                }

                float score = obj_conf * best_prob;
                if (score < conf_thresh) continue;

                /* Get box (center format) */
                float cx = row[0];
                float cy = row[1];
                float bw = row[2];
                float bh = row[3];

                /* Normalize if in pixel coordinates */
                if (cx > 1.0f || cy > 1.0f || bw > 1.0f || bh > 1.0f) {
                    cx /= model->input_w;
                    cy /= model->input_h;
                    bw /= model->input_w;
                    bh /= model->input_h;
                }

                /* Convert center to corners */
                float x1 = cx - bw / 2.0f;
                float y1 = cy - bh / 2.0f;
                float x2 = cx + bw / 2.0f;
                float y2 = cy + bh / 2.0f;

                detections[num_detections].x1 = clamp_f(x1, 0.0f, 1.0f);
                detections[num_detections].y1 = clamp_f(y1, 0.0f, 1.0f);
                detections[num_detections].x2 = clamp_f(x2, 0.0f, 1.0f);
                detections[num_detections].y2 = clamp_f(y2, 0.0f, 1.0f);
                detections[num_detections].confidence = score;
                detections[num_detections].label_id = best_class;
                num_detections++;
            }
        }
    }
    else if (num_dims == 2) {
        /* Shape: [num_boxes, values_per_box] - same formats as above */
        int64_t num_boxes = output_shape[0];
        int64_t values_per_box = output_shape[1];

        if (values_per_box == 6) {
            /* Format A */
            for (int64_t i = 0; i < num_boxes && num_detections < max_detections; i++) {
                const float* row = output_data + i * 6;
                int label_id = (int)row[0];
                float score = row[1];

                if (score > conf_thresh && label_id >= 0 && label_id < model->num_classes) {
                    float x1 = row[2], y1 = row[3], x2 = row[4], y2 = row[5];
                    if (x2 > 1.0f || y2 > 1.0f) {
                        x1 /= model->input_w; y1 /= model->input_h;
                        x2 /= model->input_w; y2 /= model->input_h;
                    }

                    detections[num_detections].x1 = clamp_f(x1, 0.0f, 1.0f);
                    detections[num_detections].y1 = clamp_f(y1, 0.0f, 1.0f);
                    detections[num_detections].x2 = clamp_f(x2, 0.0f, 1.0f);
                    detections[num_detections].y2 = clamp_f(y2, 0.0f, 1.0f);
                    detections[num_detections].confidence = score;
                    detections[num_detections].label_id = label_id;
                    num_detections++;
                }
            }
        }
    }
    else if (num_dims == 5) {
        /* 5D output: raw YOLO grid format */
        /* Could be [batch, grid_h, grid_w, anchors, values] or [batch, anchors, grid_h, grid_w, values] */
        int64_t grid_h, grid_w, num_anchors, values_per_anchor;

        /* Detect layout based on typical values */
        if (output_shape[4] >= 5) {
            /* Last dim is values: [batch, ?, ?, ?, values] */
            values_per_anchor = output_shape[4];

            /* Check if shape[1] is small (anchors) or large (grid) */
            if (output_shape[1] <= 9) {
                /* [batch, anchors, grid_h, grid_w, values] */
                num_anchors = output_shape[1];
                grid_h = output_shape[2];
                grid_w = output_shape[3];
            } else {
                /* [batch, grid_h, grid_w, anchors, values] */
                grid_h = output_shape[1];
                grid_w = output_shape[2];
                num_anchors = output_shape[3];
            }

            int num_classes = model->num_classes;
            if (values_per_anchor >= 5 + num_classes) {
                fprintf(stderr, "5D output: grid=%lldx%lld, anchors=%lld, values=%lld (raw YOLO)\n",
                        (long long)grid_h, (long long)grid_w, (long long)num_anchors, (long long)values_per_anchor);

                /* YOLOv4 anchors for 416x416 (normalized by 416) */
                /* Scale 52x52 uses anchors 0-2, 26x26 uses 3-5, 13x13 uses 6-8 */
                static const float anchors_w[9] = {10.f/416, 16.f/416, 33.f/416, 30.f/416, 62.f/416, 59.f/416, 116.f/416, 156.f/416, 373.f/416};
                static const float anchors_h[9] = {13.f/416, 30.f/416, 23.f/416, 61.f/416, 45.f/416, 119.f/416, 90.f/416, 198.f/416, 326.f/416};

                /* Determine anchor offset based on grid size */
                int anchor_offset = 0;
                if (grid_h == 52) anchor_offset = 0;       /* Small objects */
                else if (grid_h == 26) anchor_offset = 3;  /* Medium objects */
                else if (grid_h == 13) anchor_offset = 6;  /* Large objects */

                /* First pass: find max objectness to detect if output is activated */
                float max_obj_raw = -1e9f;
                float max_obj_activated = 0;
                int64_t max_gh = 0, max_gw = 0, max_a = 0;

                for (int64_t gh = 0; gh < grid_h; gh++) {
                    for (int64_t gw = 0; gw < grid_w; gw++) {
                        for (int64_t a = 0; a < num_anchors; a++) {
                            int64_t offset = ((gh * grid_w + gw) * num_anchors + a) * values_per_anchor;
                            float raw_obj = output_data[offset + 4];
                            if (raw_obj > max_obj_raw) {
                                max_obj_raw = raw_obj;
                                max_gh = gh; max_gw = gw; max_a = a;
                            }
                        }
                    }
                }
                max_obj_activated = 1.0f / (1.0f + expf(-max_obj_raw));

                /* Detect if output is already activated (values bounded 0-1) or raw logits */
                int is_activated = (max_obj_raw <= 1.0f && max_obj_raw >= 0.0f);

                fprintf(stderr, "Max obj_conf: raw=%.4f, sigmoid=%.4f at [%lld,%lld,a%lld], %s\n",
                        max_obj_raw, max_obj_activated, (long long)max_gh, (long long)max_gw, (long long)max_a,
                        is_activated ? "ACTIVATED" : "RAW");

                /* Parse all grid cells */
                for (int64_t gh = 0; gh < grid_h && num_detections < max_detections; gh++) {
                    for (int64_t gw = 0; gw < grid_w && num_detections < max_detections; gw++) {
                        for (int64_t a = 0; a < num_anchors && num_detections < max_detections; a++) {
                            /* Calculate offset for [batch, grid_h, grid_w, anchors, values] */
                            int64_t offset = ((gh * grid_w + gw) * num_anchors + a) * values_per_anchor;
                            const float* cell = output_data + offset;

                            /* Get objectness - apply sigmoid only if output is raw logits */
                            float obj_conf;
                            if (is_activated) {
                                obj_conf = cell[4];  /* Already activated */
                            } else {
                                obj_conf = 1.0f / (1.0f + expf(-cell[4]));  /* Apply sigmoid */
                            }
                            if (obj_conf < conf_thresh) continue;

                            /* Find best class */
                            int best_class = 0;
                            float best_prob = 0;
                            for (int c = 0; c < num_classes; c++) {
                                float prob;
                                if (is_activated) {
                                    prob = cell[5 + c];  /* Already activated */
                                } else {
                                    prob = 1.0f / (1.0f + expf(-cell[5 + c]));  /* Apply sigmoid */
                                }
                                if (prob > best_prob) {
                                    best_prob = prob;
                                    best_class = c;
                                }
                            }

                            float score = obj_conf * best_prob;
                            if (score < conf_thresh) continue;

                            /* Decode box coordinates */
                            float cx, cy, bw, bh;
                            int anchor_idx = anchor_offset + (int)a;
                            if (anchor_idx >= 9) anchor_idx = (int)a % 3;

                            if (is_activated) {
                                /* TensorFlow YOLO: boxes might be in different format */
                                /* Try: cx,cy already normalized (0-1), w,h as fraction of image */
                                cx = cell[0];
                                cy = cell[1];
                                bw = cell[2];
                                bh = cell[3];
                                /* If values are grid-relative, convert */
                                if (cx <= 1.0f && cy <= 1.0f) {
                                    /* Values seem to be offsets within cell, convert to absolute */
                                    cx = (gw + cx) / grid_w;
                                    cy = (gh + cy) / grid_h;
                                }
                            } else {
                                /* Raw YOLO: cx,cy = sigmoid offset, w,h = exp * anchor */
                                cx = (1.0f / (1.0f + expf(-cell[0])) + gw) / grid_w;
                                cy = (1.0f / (1.0f + expf(-cell[1])) + gh) / grid_h;
                                bw = expf(cell[2]) * anchors_w[anchor_idx];
                                bh = expf(cell[3]) * anchors_h[anchor_idx];
                            }

                            /* Convert center to corners */
                            float x1 = cx - bw / 2.0f;
                            float y1 = cy - bh / 2.0f;
                            float x2 = cx + bw / 2.0f;
                            float y2 = cy + bh / 2.0f;

                            detections[num_detections].x1 = clamp_f(x1, 0.0f, 1.0f);
                            detections[num_detections].y1 = clamp_f(y1, 0.0f, 1.0f);
                            detections[num_detections].x2 = clamp_f(x2, 0.0f, 1.0f);
                            detections[num_detections].y2 = clamp_f(y2, 0.0f, 1.0f);
                            detections[num_detections].confidence = score;
                            detections[num_detections].label_id = best_class;
                            num_detections++;
                        }
                    }
                }
            }
        }
    }
        else if (num_dims == 0 && total_elements > 0) {
            /* Fallback: try to infer format from total element count */
            fprintf(stderr, "Unknown output format, trying heuristics with %zu elements\n", total_elements);
        }
    }  /* End of output loop */

    /* Release all output tensors */
    for (size_t i = 0; i < model->num_outputs; i++) {
        if (output_tensors[i]) g_ort->ReleaseValue(output_tensors[i]);
    }

    /* Step 7: Apply NMS */
    if (ctx->nms_threshold > 0 && num_detections > 1) {
        num_detections = nms_detections(detections, num_detections, ctx->nms_threshold);
    }

    /* Step 8: Add detections to context (convert corners to x,y,w,h format) */
    for (int i = 0; i < num_detections; i++) {
        /* Convert from corner format to top-left + size format */
        float x = detections[i].x1;
        float y = detections[i].y1;
        float bw = detections[i].x2 - detections[i].x1;
        float bh = detections[i].y2 - detections[i].y1;

        if (!cira_add_detection(ctx, x, y, bw, bh,
                                detections[i].confidence, detections[i].label_id)) {
            /* Detection array full */
            break;
        }
    }

    free(detections);

    fprintf(stderr, "ONNX inference: %d detections\n", ctx->num_detections);
    return CIRA_OK;
}

#else /* CIRA_ONNX_ENABLED */

/* Stubs when ONNX is not enabled */
int onnx_load(cira_ctx* ctx, const char* model_path) {
    (void)model_path;
    cira_set_error(ctx, "ONNX support not enabled in this build");
    return CIRA_ERROR_MODEL;
}

void onnx_unload(cira_ctx* ctx) {
    (void)ctx;
}

int onnx_predict(cira_ctx* ctx, const uint8_t* data, int w, int h, int channels) {
    (void)data;
    (void)w;
    (void)h;
    (void)channels;
    cira_set_error(ctx, "ONNX support not enabled in this build");
    return CIRA_ERROR_MODEL;
}

#endif /* CIRA_ONNX_ENABLED */
