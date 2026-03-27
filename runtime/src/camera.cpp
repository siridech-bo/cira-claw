/**
 * CiRA Runtime - Camera Capture (OpenCV)
 *
 * Cross-platform video capture using OpenCV VideoCapture.
 * Works on Windows (DirectShow), Linux (V4L2), and macOS (AVFoundation).
 *
 * Architecture: Double-buffered with separate capture and inference threads
 * - Capture thread: Fast loop (~30fps), writes to buffer A/B alternately
 * - Inference thread: Processes frames from the other buffer, doesn't block capture
 * - Stream server: Reads from the latest complete buffer
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
static cv::VideoCapture* g_capture = NULL;
static camera_state_t g_cam_state = {NULL, 0, 0, 0};

/**
 * Inference thread - processes frames without blocking capture.
 * Waits for signal from capture thread, then runs AI inference.
 */
static void* inference_thread_func(void* arg) {
    cira_ctx* ctx = static_cast<cira_ctx*>(arg);

    fprintf(stderr, "Inference thread started\n");

    double last_time = get_time_ms();
    int inference_count = 0;

    while (ctx->inference_running) {
        /* Wait for frame ready signal */
        pthread_mutex_lock(&ctx->frame_mutex);
        while (!ctx->frame_ready && ctx->inference_running) {
            /* Wait with timeout to allow checking inference_running flag */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 100000000; /* 100ms timeout */
            if (ts.tv_nsec >= 1000000000) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000;
            }
            pthread_cond_timedwait(&ctx->frame_cond, &ctx->frame_mutex, &ts);
        }

        if (!ctx->inference_running) {
            pthread_mutex_unlock(&ctx->frame_mutex);
            break;
        }

        /* Get the read buffer (the one not being written to) */
        int read_idx = ctx->read_buffer_idx;
        uint8_t* frame_data = ctx->frame_buffers[read_idx];
        int w = ctx->frame_w;
        int h = ctx->frame_h;
        ctx->frame_ready = 0;  /* Clear flag */
        pthread_mutex_unlock(&ctx->frame_mutex);

        if (!frame_data || w <= 0 || h <= 0) {
            continue;
        }

        /* Run inference if IMAGE slot model is loaded */
        if (SLOT_FORMAT(ctx, MODEL_SLOT_IMAGE) != CIRA_FORMAT_UNKNOWN &&
            SLOT_HANDLE(ctx, MODEL_SLOT_IMAGE) != NULL &&
            !ctx->model_swapping) {

            /* Lock IMAGE slot mutex to prevent model unload during inference */
            if (pthread_mutex_trylock(&SLOT_MUTEX(ctx, MODEL_SLOT_IMAGE)) == 0) {
                /* Double-check after acquiring lock */
                if (SLOT_HANDLE(ctx, MODEL_SLOT_IMAGE) != NULL && !ctx->model_swapping) {
                    int result = CIRA_ERROR;

                    switch (SLOT_FORMAT(ctx, MODEL_SLOT_IMAGE)) {
#ifdef CIRA_DARKNET_ENABLED
                        case CIRA_FORMAT_DARKNET:
                            result = darknet_predict(ctx, frame_data, w, h, 3);
                            break;
#endif
#ifdef CIRA_NCNN_ENABLED
                        case CIRA_FORMAT_NCNN:
                            result = ncnn_predict(ctx, frame_data, w, h, 3);
                            break;
#endif
#ifdef CIRA_ONNX_ENABLED
                        case CIRA_FORMAT_ONNX:
                            result = onnx_predict(ctx, frame_data, w, h, 3);
                            break;
#endif
#ifdef CIRA_TRT_ENABLED
                        case CIRA_FORMAT_TENSORRT:
                            result = trt_predict(ctx, frame_data, w, h, 3);
                            break;
#endif
                        default:
                            break;
                    }

                    if (result == CIRA_OK) {
                        ctx->total_frames++;
                        inference_count++;
                    } else if (result != CIRA_ERROR) {
                        static int err_count = 0;
                        if (++err_count % 100 == 1) {
                            fprintf(stderr, "Inference error: %d\n", result);
                        }
                    }
                }
                pthread_mutex_unlock(&SLOT_MUTEX(ctx, MODEL_SLOT_IMAGE));
            }
        }

        /* Calculate inference FPS */
        double now = get_time_ms();
        double elapsed = now - last_time;
        if (elapsed >= 1000.0) {
            ctx->inference_fps = (float)(inference_count * 1000.0 / elapsed);
            inference_count = 0;
            last_time = now;
        }
    }

    fprintf(stderr, "Inference thread stopped\n");
    return NULL;
}

/**
 * Camera capture thread - fast loop, doesn't wait for inference.
 * Captures frames and stores to double buffer, signals inference thread.
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

        /* Store frame to write buffer (double-buffered) */
        pthread_mutex_lock(&ctx->frame_mutex);

        int write_idx = ctx->write_buffer_idx;
        int frame_size = rgb.cols * rgb.rows * 3;

        /* Allocate or reallocate buffer if needed */
        if (!ctx->frame_buffers[write_idx] || ctx->frame_size != frame_size) {
            if (ctx->frame_buffers[write_idx]) {
                free(ctx->frame_buffers[write_idx]);
            }
            ctx->frame_buffers[write_idx] = (uint8_t*)malloc(frame_size);
        }

        if (ctx->frame_buffers[write_idx]) {
            memcpy(ctx->frame_buffers[write_idx], rgb.data, frame_size);
            ctx->frame_w = rgb.cols;
            ctx->frame_h = rgb.rows;
            ctx->frame_size = frame_size;

            /* Swap buffers: write becomes read, read becomes write */
            ctx->read_buffer_idx = write_idx;
            ctx->write_buffer_idx = 1 - write_idx;

            /* Update legacy pointer for stream server compatibility */
            ctx->frame_buffer = ctx->frame_buffers[ctx->read_buffer_idx];

            /* Signal inference thread that a new frame is ready */
            ctx->frame_ready = 1;
            pthread_cond_signal(&ctx->frame_cond);
        }

        pthread_mutex_unlock(&ctx->frame_mutex);

        /* Write frame to temp file periodically for file-based transfer */
        static int write_counter = 0;
        if (++write_counter >= 3) {
            write_counter = 0;
            cira_write_frame_file(ctx, 1);
        }

        /* Calculate capture FPS */
        frame_count++;
        double now = get_time_ms();
        double elapsed = now - last_time;

        if (elapsed >= 1000.0) {
            ctx->current_fps = (float)(frame_count * 1000.0 / elapsed);
            frame_count = 0;
            last_time = now;

            /* Log FPS periodically */
            fprintf(stderr, "Capture FPS: %.1f, Inference FPS: %.1f, Detections: %d\n",
                    ctx->current_fps, ctx->inference_fps, ctx->num_detections);
        }

        /* Small sleep to prevent CPU spinning - capture is now decoupled from inference */
        usleep(1000);  /* 1ms */
    }

    fprintf(stderr, "Camera capture thread stopped\n");
    return NULL;
}

/**
 * Start camera capture with separate inference thread.
 */
extern "C" int camera_start(cira_ctx* ctx, int device_id) {
    if (!ctx) return CIRA_ERROR_INPUT;

    /* Check if already running */
    if (ctx->camera_running) {
        fprintf(stderr, "Camera already running\n");
        return CIRA_OK;
    }

    fprintf(stderr, "Opening camera %d...\n", device_id);

    /* Initialize double-buffer state */
    ctx->frame_buffers[0] = NULL;
    ctx->frame_buffers[1] = NULL;
    ctx->write_buffer_idx = 0;
    ctx->read_buffer_idx = 1;
    ctx->frame_ready = 0;
    ctx->inference_fps = 0.0f;

    /* Initialize condition variable */
    pthread_cond_init(&ctx->frame_cond, NULL);

    /* Create VideoCapture */
    g_capture = new cv::VideoCapture();

    /* Open camera - OpenCV auto-selects backend */
#ifdef _WIN32
    if (!g_capture->open(device_id, cv::CAP_DSHOW)) {
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

    /* Get actual resolution */
    g_cam_state.width = static_cast<int>(g_capture->get(cv::CAP_PROP_FRAME_WIDTH));
    g_cam_state.height = static_cast<int>(g_capture->get(cv::CAP_PROP_FRAME_HEIGHT));
    g_cam_state.device_id = device_id;
    g_cam_state.cap = g_capture;

    fprintf(stderr, "Camera opened: device %d, resolution %dx%d\n",
            device_id, g_cam_state.width, g_cam_state.height);

    /* Start inference thread first */
    ctx->inference_running = 1;
    int ret = pthread_create(&ctx->inference_thread, NULL, inference_thread_func, ctx);
    if (ret != 0) {
        fprintf(stderr, "Failed to create inference thread: %d\n", ret);
        ctx->inference_running = 0;
        g_capture->release();
        delete g_capture;
        g_capture = NULL;
        return CIRA_ERROR;
    }

    /* Start capture thread */
    ctx->camera_running = 1;
    ctx->current_camera = device_id;

    ret = pthread_create(&ctx->camera_thread, NULL, camera_thread_func_impl, ctx);
    if (ret != 0) {
        fprintf(stderr, "Failed to create camera thread: %d\n", ret);
        ctx->camera_running = 0;
        ctx->inference_running = 0;
        pthread_cond_signal(&ctx->frame_cond);  /* Wake up inference thread */
        pthread_join(ctx->inference_thread, NULL);
        g_capture->release();
        delete g_capture;
        g_capture = NULL;
        return CIRA_ERROR;
    }

    fprintf(stderr, "Camera capture started (double-buffered with separate inference thread)\n");
    return CIRA_OK;
}

/**
 * Stop camera capture and inference threads.
 */
extern "C" int camera_stop(cira_ctx* ctx) {
    if (!ctx) return CIRA_ERROR_INPUT;

    if (!ctx->camera_running) {
        fprintf(stderr, "Camera not running\n");
        return CIRA_OK;
    }

    fprintf(stderr, "Stopping camera...\n");

    /* Signal threads to stop */
    ctx->camera_running = 0;
    ctx->inference_running = 0;
    ctx->current_camera = -1;

    /* Wake up inference thread if it's waiting */
    pthread_mutex_lock(&ctx->frame_mutex);
    pthread_cond_signal(&ctx->frame_cond);
    pthread_mutex_unlock(&ctx->frame_mutex);

    /* Wait for threads to finish */
    pthread_join(ctx->camera_thread, NULL);
    pthread_join(ctx->inference_thread, NULL);

    /* Release VideoCapture */
    if (g_capture) {
        g_capture->release();
        delete g_capture;
        g_capture = NULL;
    }

    /* Free double buffers */
    if (ctx->frame_buffers[0]) {
        free(ctx->frame_buffers[0]);
        ctx->frame_buffers[0] = NULL;
    }
    if (ctx->frame_buffers[1]) {
        free(ctx->frame_buffers[1]);
        ctx->frame_buffers[1] = NULL;
    }
    ctx->frame_buffer = NULL;

    /* Destroy condition variable */
    pthread_cond_destroy(&ctx->frame_cond);

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
