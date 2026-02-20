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

    /* Step 5: Process all output tensors using unified YOLO decoder */
    int max_dets_buffer = CIRA_MAX_DETECTIONS * 4;  /* Pre-NMS buffer */
    yolo_detection_t* detections = (yolo_detection_t*)malloc(max_dets_buffer * sizeof(yolo_detection_t));
    int total_detections = 0;

    if (!detections) {
        for (size_t i = 0; i < model->num_outputs; i++) {
            if (output_tensors[i]) g_ort->ReleaseValue(output_tensors[i]);
        }
        return CIRA_ERROR_MEMORY;
    }

    /* Setup decoder config */
    yolo_decode_config_t decode_config;
    decode_config.version = ctx->yolo_version;
    decode_config.input_w = model->input_w;
    decode_config.input_h = model->input_h;
    decode_config.num_classes = model->num_classes;
    decode_config.conf_threshold = ctx->confidence_threshold;
    decode_config.nms_threshold = ctx->nms_threshold;
    decode_config.max_detections = CIRA_MAX_DETECTIONS;

    fprintf(stderr, "YOLO decoder: version=%s, input=%dx%d, classes=%d\n",
            yolo_version_name(decode_config.version),
            decode_config.input_w, decode_config.input_h, decode_config.num_classes);

    /* Process each output scale */
    for (size_t out_idx = 0; out_idx < model->num_outputs; out_idx++) {
        if (!output_tensors[out_idx]) continue;

        float* output_data;
        status = g_ort->GetTensorMutableData(output_tensors[out_idx], (void**)&output_data);
        if (status != NULL) {
            g_ort->ReleaseStatus(status);
            continue;
        }

        /* Get output shape */
        OrtTensorTypeAndShapeInfo* output_info = NULL;
        ORT_IGNORE(g_ort->GetTensorTypeAndShape(output_tensors[out_idx], &output_info));

        size_t num_dims = 0;
        int64_t output_shape[6] = {0};

        if (output_info) {
            ORT_IGNORE(g_ort->GetDimensionsCount(output_info, &num_dims));
            if (num_dims <= 6) {
                ORT_IGNORE(g_ort->GetDimensions(output_info, output_shape, num_dims));
            }
            g_ort->ReleaseTensorTypeAndShapeInfo(output_info);
        }

        /* Debug output */
        fprintf(stderr, "ONNX output[%zu]: dims=%zu, shape=[", out_idx, num_dims);
        for (size_t i = 0; i < num_dims && i < 6; i++) {
            fprintf(stderr, "%lld%s", (long long)output_shape[i], i < num_dims - 1 ? ", " : "");
        }
        fprintf(stderr, "]\n");

        /* Decode this output scale */
        int space_left = max_dets_buffer - total_detections;
        if (space_left <= 0) break;

        int count = yolo_decode(output_data, output_shape, (int)num_dims,
                               &decode_config,
                               detections + total_detections, space_left);

        if (count > 0) {
            fprintf(stderr, "  Scale %zu: %d detections\n", out_idx, count);
            total_detections += count;
        }
    }

    /* Release all output tensors */
    for (size_t i = 0; i < model->num_outputs; i++) {
        if (output_tensors[i]) g_ort->ReleaseValue(output_tensors[i]);
    }

    /* Step 6: Apply NMS across all scales (decoder already applied per-scale NMS) */
    if (ctx->nms_threshold > 0 && total_detections > 1) {
        total_detections = yolo_nms(detections, total_detections, ctx->nms_threshold);
    }

    /* Step 7: Add detections to context (convert to normalized x,y,w,h format) */
    for (int i = 0; i < total_detections; i++) {
        /* Normalize pixel coords to 0-1 */
        float x1 = detections[i].x1 / model->input_w;
        float y1 = detections[i].y1 / model->input_h;
        float x2 = detections[i].x2 / model->input_w;
        float y2 = detections[i].y2 / model->input_h;

        /* Clamp to valid range */
        x1 = clamp_f(x1, 0.0f, 1.0f);
        y1 = clamp_f(y1, 0.0f, 1.0f);
        x2 = clamp_f(x2, 0.0f, 1.0f);
        y2 = clamp_f(y2, 0.0f, 1.0f);

        /* Convert corners to top-left + size */
        float bw = x2 - x1;
        float bh = y2 - y1;

        if (!cira_add_detection(ctx, x1, y1, bw, bh,
                                detections[i].score, detections[i].class_id)) {
            break;  /* Detection array full */
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
