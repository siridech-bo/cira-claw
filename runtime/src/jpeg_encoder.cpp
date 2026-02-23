/**
 * CiRA Runtime - JPEG Encoder
 *
 * Uses OpenCV to encode RGB frames to JPEG format.
 *
 * (c) CiRA Robotics / KMITL 2026
 */

#include "cira.h"
#include "cira_internal.h"
#include <stdlib.h>
#include <string.h>

#ifdef CIRA_STREAMING_ENABLED
#ifdef CIRA_OPENCV_ENABLED

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

/* Static buffer for encoded JPEG */
static std::vector<uchar> g_jpeg_buffer;
static pthread_mutex_t g_jpeg_mutex = PTHREAD_MUTEX_INITIALIZER;

extern "C" {

/**
 * Encode RGB frame to JPEG.
 *
 * @param rgb_data RGB pixel data
 * @param width Frame width
 * @param height Frame height
 * @param quality JPEG quality (1-100)
 * @param out_data Output pointer (set to internal buffer - do not free)
 * @param out_size Output size in bytes
 * @return CIRA_OK on success
 */
int jpeg_encode(const uint8_t* rgb_data, int width, int height,
                int quality, uint8_t** out_data, size_t* out_size) {
    if (!rgb_data || !out_data || !out_size || width <= 0 || height <= 0) {
        return CIRA_ERROR_INPUT;
    }

    pthread_mutex_lock(&g_jpeg_mutex);

    /* Create cv::Mat from RGB data */
    cv::Mat rgb(height, width, CV_8UC3, (void*)rgb_data);

    /* Convert RGB to BGR (OpenCV uses BGR) */
    cv::Mat bgr;
    cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);

    /* Encode to JPEG */
    std::vector<int> params;
    params.push_back(cv::IMWRITE_JPEG_QUALITY);
    params.push_back(quality);

    g_jpeg_buffer.clear();
    if (!cv::imencode(".jpg", bgr, g_jpeg_buffer, params)) {
        pthread_mutex_unlock(&g_jpeg_mutex);
        return CIRA_ERROR;
    }

    *out_data = g_jpeg_buffer.data();
    *out_size = g_jpeg_buffer.size();

    pthread_mutex_unlock(&g_jpeg_mutex);
    return CIRA_OK;
}

/**
 * Encode RGB frame with detection annotations overlaid.
 *
 * @param ctx Context with detections
 * @param rgb_data RGB pixel data
 * @param width Frame width
 * @param height Frame height
 * @param quality JPEG quality (1-100)
 * @param out_data Output pointer (set to internal buffer - do not free)
 * @param out_size Output size in bytes
 * @return CIRA_OK on success
 */
int jpeg_encode_annotated(cira_ctx* ctx, const uint8_t* rgb_data, int width, int height,
                          int quality, uint8_t** out_data, size_t* out_size) {
    if (!ctx || !rgb_data || !out_data || !out_size || width <= 0 || height <= 0) {
        return CIRA_ERROR_INPUT;
    }

    pthread_mutex_lock(&g_jpeg_mutex);

    /* Create cv::Mat from RGB data */
    cv::Mat rgb(height, width, CV_8UC3, (void*)rgb_data);

    /* Convert RGB to BGR (OpenCV uses BGR) */
    cv::Mat bgr;
    cv::cvtColor(rgb, bgr, cv::COLOR_RGB2BGR);

    /* Draw detections with persistence (reduce flickering) */
    pthread_mutex_lock(&ctx->result_mutex);

    /* Use current detections, or fall back to previous if current is empty */
    cira_detection_t* dets = ctx->detections;
    int num_dets = ctx->num_detections;

    if (num_dets > 0) {
        /* Save current detections for persistence */
        memcpy(ctx->prev_detections, ctx->detections,
               ctx->num_detections * sizeof(cira_detection_t));
        ctx->prev_num_detections = ctx->num_detections;
        ctx->prev_detection_frame = ctx->frame_sequence;
    } else if (ctx->prev_num_detections > 0 &&
               (ctx->frame_sequence - ctx->prev_detection_frame) <= 3) {
        /* Use previous detections if within 3 frames */
        dets = ctx->prev_detections;
        num_dets = ctx->prev_num_detections;
    }

    for (int i = 0; i < num_dets; i++) {
        cira_detection_t* det = &dets[i];

        /* Convert normalized coords to pixel coords */
        int x = (int)(det->x * width);
        int y = (int)(det->y * height);
        int w = (int)(det->w * width);
        int h = (int)(det->h * height);

        /* Draw bounding box (green, thicker line) */
        cv::rectangle(bgr,
                      cv::Point(x, y),
                      cv::Point(x + w, y + h),
                      cv::Scalar(0, 255, 0), 3);

        /* Draw label background */
        const char* label = cira_get_label(ctx, det->label_id);
        char text[128];
        snprintf(text, sizeof(text), "%s %.0f%%", label, det->confidence * 100);

        int baseline;
        cv::Size text_size = cv::getTextSize(text, cv::FONT_HERSHEY_SIMPLEX, 0.7, 2, &baseline);

        cv::rectangle(bgr,
                      cv::Point(x, y - text_size.height - 8),
                      cv::Point(x + text_size.width + 8, y),
                      cv::Scalar(0, 255, 0), cv::FILLED);

        /* Draw label text (black on green, bigger font) */
        cv::putText(bgr, text,
                    cv::Point(x + 4, y - 4),
                    cv::FONT_HERSHEY_SIMPLEX, 0.7,
                    cv::Scalar(0, 0, 0), 2);
    }

    pthread_mutex_unlock(&ctx->result_mutex);

    /* Encode to JPEG */
    std::vector<int> params;
    params.push_back(cv::IMWRITE_JPEG_QUALITY);
    params.push_back(quality);

    g_jpeg_buffer.clear();
    if (!cv::imencode(".jpg", bgr, g_jpeg_buffer, params)) {
        pthread_mutex_unlock(&g_jpeg_mutex);
        return CIRA_ERROR;
    }

    *out_data = g_jpeg_buffer.data();
    *out_size = g_jpeg_buffer.size();

    pthread_mutex_unlock(&g_jpeg_mutex);
    return CIRA_OK;
}

} /* extern "C" */

#else /* CIRA_OPENCV_ENABLED */

extern "C" {

int jpeg_encode(const uint8_t* rgb_data, int width, int height,
                int quality, uint8_t** out_data, size_t* out_size) {
    (void)rgb_data; (void)width; (void)height; (void)quality;
    (void)out_data; (void)out_size;
    return CIRA_ERROR;
}

int jpeg_encode_annotated(cira_ctx* ctx, const uint8_t* rgb_data, int width, int height,
                          int quality, uint8_t** out_data, size_t* out_size) {
    (void)ctx; (void)rgb_data; (void)width; (void)height; (void)quality;
    (void)out_data; (void)out_size;
    return CIRA_ERROR;
}

} /* extern "C" */

#endif /* CIRA_OPENCV_ENABLED */
#endif /* CIRA_STREAMING_ENABLED */
