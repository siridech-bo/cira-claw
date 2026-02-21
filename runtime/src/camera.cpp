/**
 * CiRA Runtime - Camera Capture (OpenCV)
 *
 * Cross-platform video capture using OpenCV VideoCapture.
 * Works on Windows (DirectShow), Linux (V4L2), and macOS (AVFoundation).
 *
 * (c) CiRA Robotics / KMITL 2026
 */

#include "cira.h"
#include "cira_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>

#ifdef _WIN32
#include <windows.h>
#define usleep(x) Sleep((x) / 1000)
#else
#include <unistd.h>
#include <time.h>
#endif

#ifdef CIRA_STREAMING_ENABLED
#ifdef CIRA_OPENCV_ENABLED

#include <opencv2/core.hpp>
#include <opencv2/videoio.hpp>
#include <opencv2/imgproc.hpp>

/* Default capture settings */
#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 720

/* Internal camera state */
struct camera_state_t {
    cv::VideoCapture* cap;
    int device_id;
    int width;
    int height;
};

/* Forward declarations for predict functions (extern "C" linkage) */
#ifdef CIRA_DARKNET_ENABLED
extern "C" int darknet_predict(cira_ctx* ctx, const uint8_t* data, int w, int h, int channels);
#endif
#ifdef CIRA_NCNN_ENABLED
extern "C" int ncnn_predict(cira_ctx* ctx, const uint8_t* data, int w, int h, int channels);
#endif
#ifdef CIRA_ONNX_ENABLED
extern "C" int onnx_predict(cira_ctx* ctx, const uint8_t* data, int w, int h, int channels);
#endif
#ifdef CIRA_TRT_ENABLED
extern "C" int trt_predict(cira_ctx* ctx, const uint8_t* data, int w, int h, int channels);
#endif

/* Forward declaration for frame file writing */
extern "C" int cira_write_frame_file(cira_ctx* ctx, int annotated);

/* Timing helper */
static double get_time_ms(void) {
#ifdef _WIN32
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return (double)count.QuadPart * 1000.0 / (double)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
#endif
}

/* Store VideoCapture pointer - using a simple global for now */
/* TODO: Better approach would be to extend cira_ctx with a void* camera_handle */
static cv::VideoCapture* g_capture = NULL;
static camera_state_t g_cam_state = {NULL, 0, 0, 0};

/**
 * Camera capture thread (actual implementation).
 */
static void* camera_thread_func_impl(void* arg) {
    cira_ctx* ctx = static_cast<cira_ctx*>(arg);

    if (!g_capture || !g_capture->isOpened()) {
        fprintf(stderr, "Camera not opened in thread\n");
        return NULL;
    }

    cv::Mat frame, rgb;
    double last_time = get_time_ms();
    int frame_count = 0;

    fprintf(stderr, "Camera capture thread started (device %d, %dx%d)\n",
            g_cam_state.device_id, g_cam_state.width, g_cam_state.height);

    while (ctx->camera_running) {
        /* Capture frame */
        if (!g_capture->read(frame)) {
            fprintf(stderr, "Failed to read frame\n");
            usleep(10000);
            continue;
        }

        if (frame.empty()) {
            usleep(1000);
            continue;
        }

        /* Convert BGR to RGB */
        cv::cvtColor(frame, rgb, cv::COLOR_BGR2RGB);

        /* Store frame for streaming server */
        cira_store_frame(ctx, rgb.data, rgb.cols, rgb.rows);

        /* Write frame to temp file periodically for file-based transfer */
        /* Write every 3 frames (~10 FPS at 30 FPS capture) to reduce disk I/O */
        static int write_counter = 0;
        if (++write_counter >= 3) {
            write_counter = 0;
            cira_write_frame_file(ctx, 1);  /* 1 = annotated */
        }

        /* Run inference if model is loaded and not being swapped */
        if (ctx->format != CIRA_FORMAT_UNKNOWN && ctx->model_handle != NULL && !ctx->model_swapping) {
            /* Lock model mutex to prevent model unload during inference */
            if (pthread_mutex_trylock(&ctx->model_mutex) == 0) {
                /* Double-check after acquiring lock */
                if (ctx->model_handle != NULL && !ctx->model_swapping) {
                    /* Call predict with RGB data */
                    int result = CIRA_ERROR;

                    switch (ctx->format) {
#ifdef CIRA_DARKNET_ENABLED
                        case CIRA_FORMAT_DARKNET:
                            result = darknet_predict(ctx, rgb.data, rgb.cols, rgb.rows, 3);
                            break;
#endif
#ifdef CIRA_NCNN_ENABLED
                        case CIRA_FORMAT_NCNN:
                            result = ncnn_predict(ctx, rgb.data, rgb.cols, rgb.rows, 3);
                            break;
#endif
#ifdef CIRA_ONNX_ENABLED
                        case CIRA_FORMAT_ONNX:
                            result = onnx_predict(ctx, rgb.data, rgb.cols, rgb.rows, 3);
                            break;
#endif
#ifdef CIRA_TRT_ENABLED
                        case CIRA_FORMAT_TENSORRT:
                            result = trt_predict(ctx, rgb.data, rgb.cols, rgb.rows, 3);
                            break;
#endif
                        default:
                            break;
                    }

                    if (result == CIRA_OK) {
                        /* Increment total frames for stats */
                        ctx->total_frames++;
                    } else if (result != CIRA_ERROR) {
                        /* Log inference errors occasionally */
                        static int err_count = 0;
                        if (++err_count % 100 == 1) {
                            fprintf(stderr, "Inference error: %d\n", result);
                        }
                    }
                }
                pthread_mutex_unlock(&ctx->model_mutex);
            }
            /* If trylock fails, model is being swapped - skip this frame */
        }

        /* Calculate FPS */
        frame_count++;
        double now = get_time_ms();
        double elapsed = now - last_time;

        if (elapsed >= 1000.0) {
            ctx->current_fps = (float)(frame_count * 1000.0 / elapsed);
            frame_count = 0;
            last_time = now;

            /* Log FPS periodically */
            fprintf(stderr, "Camera FPS: %.1f, Detections: %d\n",
                    ctx->current_fps, ctx->num_detections);
        }

        /* Small sleep to prevent CPU spinning */
        usleep(1000);  /* 1ms */
    }

    fprintf(stderr, "Camera capture thread stopped\n");
    return NULL;
}

/**
 * Start camera capture.
 */
extern "C" int camera_start(cira_ctx* ctx, int device_id) {
    if (!ctx) return CIRA_ERROR_INPUT;

    /* Check if already running */
    if (ctx->camera_running) {
        fprintf(stderr, "Camera already running\n");
        return CIRA_OK;
    }

    fprintf(stderr, "Opening camera %d...\n", device_id);

    /* Create VideoCapture */
    g_capture = new cv::VideoCapture();

    /* Open camera - OpenCV auto-selects backend (DirectShow on Windows, V4L2 on Linux) */
#ifdef _WIN32
    /* On Windows, use DirectShow backend explicitly for better compatibility */
    if (!g_capture->open(device_id, cv::CAP_DSHOW)) {
        /* Fall back to default backend */
        if (!g_capture->open(device_id)) {
            fprintf(stderr, "Failed to open camera %d\n", device_id);
            delete g_capture;
            g_capture = NULL;
            return CIRA_ERROR;
        }
    }
#else
    if (!g_capture->open(device_id)) {
        fprintf(stderr, "Failed to open camera %d\n", device_id);
        delete g_capture;
        g_capture = NULL;
        return CIRA_ERROR;
    }
#endif

    /* Set resolution to 1280x720 */
    g_capture->set(cv::CAP_PROP_FRAME_WIDTH, DEFAULT_WIDTH);
    g_capture->set(cv::CAP_PROP_FRAME_HEIGHT, DEFAULT_HEIGHT);

    /* Get actual resolution (may differ from requested) */
    g_cam_state.width = static_cast<int>(g_capture->get(cv::CAP_PROP_FRAME_WIDTH));
    g_cam_state.height = static_cast<int>(g_capture->get(cv::CAP_PROP_FRAME_HEIGHT));
    g_cam_state.device_id = device_id;
    g_cam_state.cap = g_capture;

    fprintf(stderr, "Camera opened: device %d, resolution %dx%d\n",
            device_id, g_cam_state.width, g_cam_state.height);

    /* Start capture thread */
    ctx->camera_running = 1;

    int ret = pthread_create(&ctx->camera_thread, NULL, camera_thread_func_impl, ctx);
    if (ret != 0) {
        fprintf(stderr, "Failed to create camera thread: %d\n", ret);
        ctx->camera_running = 0;
        g_capture->release();
        delete g_capture;
        g_capture = NULL;
        return CIRA_ERROR;
    }

    fprintf(stderr, "Camera capture started\n");
    return CIRA_OK;
}

/**
 * Stop camera capture.
 */
extern "C" int camera_stop(cira_ctx* ctx) {
    if (!ctx) return CIRA_ERROR_INPUT;

    if (!ctx->camera_running) {
        fprintf(stderr, "Camera not running\n");
        return CIRA_OK;
    }

    fprintf(stderr, "Stopping camera...\n");

    /* Signal thread to stop */
    ctx->camera_running = 0;

    /* Wait for thread to finish */
    pthread_join(ctx->camera_thread, NULL);

    /* Release VideoCapture */
    if (g_capture) {
        g_capture->release();
        delete g_capture;
        g_capture = NULL;
    }

    /* Clear state */
    g_cam_state.cap = NULL;
    g_cam_state.device_id = 0;
    g_cam_state.width = 0;
    g_cam_state.height = 0;

    fprintf(stderr, "Camera stopped\n");
    return CIRA_OK;
}

#else /* CIRA_OPENCV_ENABLED */

/* Stubs when OpenCV is not enabled */
extern "C" int camera_start(cira_ctx* ctx, int device_id) {
    (void)ctx;
    (void)device_id;
    fprintf(stderr, "OpenCV camera support not enabled in this build\n");
    return CIRA_ERROR;
}

extern "C" int camera_stop(cira_ctx* ctx) {
    (void)ctx;
    return CIRA_ERROR;
}

#endif /* CIRA_OPENCV_ENABLED */

#else /* CIRA_STREAMING_ENABLED */

/* Stubs when streaming is not enabled */
extern "C" int camera_start(cira_ctx* ctx, int device_id) {
    (void)ctx;
    (void)device_id;
    fprintf(stderr, "Streaming not enabled in this build\n");
    return CIRA_ERROR;
}

extern "C" int camera_stop(cira_ctx* ctx) {
    (void)ctx;
    return CIRA_ERROR;
}

#endif /* CIRA_STREAMING_ENABLED */
