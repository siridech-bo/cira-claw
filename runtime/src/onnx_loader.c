/**
 * CiRA Runtime - ONNX Model Loader
 *
 * This file implements loading and inference for ONNX models
 * using the ONNX Runtime C API.
 *
 * (c) CiRA Robotics / KMITL 2026
 */

#include "cira.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef CIRA_ONNX_ENABLED

#include <onnxruntime_c_api.h>

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
    char* output_name;
    int64_t input_shape[4];   /* NCHW or NHWC */
    int input_w;
    int input_h;
    int input_c;
    int num_classes;
} onnx_model_t;

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

    /* Allocate model structure */
    onnx_model_t* model = (onnx_model_t*)calloc(1, sizeof(onnx_model_t));
    if (!model) {
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

    /* Set optimization level */
    g_ort->SetSessionGraphOptimizationLevel(model->session_options,
                                             ORT_ENABLE_EXTENDED);

    /* Create session */
    status = g_ort->CreateSession(model->env, model_path,
                                   model->session_options, &model->session);
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
        fprintf(stderr, "Failed to create memory info\n");
        g_ort->ReleaseStatus(status);
        g_ort->ReleaseSession(model->session);
        g_ort->ReleaseSessionOptions(model->session_options);
        g_ort->ReleaseEnv(model->env);
        free(model);
        return CIRA_ERROR;
    }

    /* Get input info */
    OrtAllocator* allocator;
    g_ort->GetAllocatorWithDefaultOptions(&allocator);

    status = g_ort->SessionGetInputName(model->session, 0, allocator,
                                         &model->input_name);
    if (status != NULL) {
        fprintf(stderr, "Failed to get input name\n");
        g_ort->ReleaseStatus(status);
    }

    /* Get input shape */
    OrtTypeInfo* input_type_info;
    status = g_ort->SessionGetInputTypeInfo(model->session, 0, &input_type_info);
    if (status == NULL) {
        const OrtTensorTypeAndShapeInfo* tensor_info;
        g_ort->CastTypeInfoToTensorInfo(input_type_info, &tensor_info);

        size_t num_dims;
        g_ort->GetDimensionsCount(tensor_info, &num_dims);

        if (num_dims == 4) {
            g_ort->GetDimensions(tensor_info, model->input_shape, num_dims);

            /* Assume NCHW format */
            model->input_c = (int)model->input_shape[1];
            model->input_h = (int)model->input_shape[2];
            model->input_w = (int)model->input_shape[3];

            fprintf(stderr, "ONNX input shape: %lldx%lldx%lldx%lld\n",
                    model->input_shape[0], model->input_shape[1],
                    model->input_shape[2], model->input_shape[3]);
        }

        g_ort->ReleaseTypeInfo(input_type_info);
    }

    /* Get output name */
    status = g_ort->SessionGetOutputName(model->session, 0, allocator,
                                          &model->output_name);

    fprintf(stderr, "ONNX model loaded successfully\n");
    fprintf(stderr, "  Input: %s (%dx%d)\n", model->input_name,
            model->input_w, model->input_h);
    fprintf(stderr, "  Output: %s\n", model->output_name);

    /* Store model in context */
    /* ctx->model_handle = model; */
    (void)ctx;

    return CIRA_OK;
}

/**
 * Unload ONNX model.
 */
void onnx_unload(cira_ctx* ctx) {
    /* onnx_model_t* model = (onnx_model_t*)ctx->model_handle; */
    (void)ctx;

    /* TODO: Properly clean up model resources */
    /*
    if (model) {
        if (model->input_name) {
            OrtAllocator* allocator;
            g_ort->GetAllocatorWithDefaultOptions(&allocator);
            allocator->Free(allocator, model->input_name);
        }
        if (model->output_name) {
            OrtAllocator* allocator;
            g_ort->GetAllocatorWithDefaultOptions(&allocator);
            allocator->Free(allocator, model->output_name);
        }
        if (model->memory_info) g_ort->ReleaseMemoryInfo(model->memory_info);
        if (model->session) g_ort->ReleaseSession(model->session);
        if (model->session_options) g_ort->ReleaseSessionOptions(model->session_options);
        if (model->env) g_ort->ReleaseEnv(model->env);
        free(model);
    }
    ctx->model_handle = NULL;
    */
}

/**
 * Run ONNX inference on an image.
 */
int onnx_predict(cira_ctx* ctx, const uint8_t* data, int w, int h, int channels) {
    (void)ctx;
    (void)data;
    (void)w;
    (void)h;
    (void)channels;

    /* TODO: Implement ONNX inference */
    /*
     * 1. Get model from context
     * onnx_model_t* model = (onnx_model_t*)ctx->model_handle;
     *
     * 2. Preprocess image (resize, normalize, convert to NCHW)
     *
     * 3. Create input tensor
     * OrtValue* input_tensor = NULL;
     * g_ort->CreateTensorWithDataAsOrtValue(
     *     model->memory_info, input_data, input_size,
     *     model->input_shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
     *     &input_tensor);
     *
     * 4. Run inference
     * const char* input_names[] = { model->input_name };
     * const char* output_names[] = { model->output_name };
     * OrtValue* output_tensor = NULL;
     * g_ort->Run(model->session, NULL, input_names, &input_tensor, 1,
     *            output_names, 1, &output_tensor);
     *
     * 5. Get output data and parse detections
     *
     * 6. Store results in ctx->detections
     */

    return CIRA_OK;
}

#else /* CIRA_ONNX_ENABLED */

/* Stubs when ONNX is not enabled */
int onnx_load(cira_ctx* ctx, const char* model_path) {
    (void)ctx;
    (void)model_path;
    fprintf(stderr, "ONNX support not enabled in this build\n");
    return CIRA_ERROR_MODEL;
}

void onnx_unload(cira_ctx* ctx) {
    (void)ctx;
}

int onnx_predict(cira_ctx* ctx, const uint8_t* data, int w, int h, int channels) {
    (void)ctx;
    (void)data;
    (void)w;
    (void)h;
    (void)channels;
    return CIRA_ERROR_MODEL;
}

#endif /* CIRA_ONNX_ENABLED */
