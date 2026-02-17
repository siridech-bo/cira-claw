/**
 * CiRA Runtime - Camera Capture
 *
 * This file implements video capture from cameras using V4L2 (Linux)
 * or GStreamer (Jetson). The capture runs in a background thread.
 *
 * (c) CiRA Robotics / KMITL 2026
 */

#include "cira.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

#ifdef CIRA_STREAMING_ENABLED

#ifdef __linux__
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#endif

/* Maximum capture buffers */
#define NUM_BUFFERS 4

/* Default capture settings */
#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 720
#define DEFAULT_FPS 30

/* V4L2 buffer structure */
typedef struct {
    void* start;
    size_t length;
} v4l2_buffer_t;

/* Internal camera state */
typedef struct {
    int fd;                         /* V4L2 device file descriptor */
    v4l2_buffer_t buffers[NUM_BUFFERS];
    int num_buffers;
    int width;
    int height;
    int running;
} camera_state_t;

/* Access to ctx internals - declared in cira.c */
struct cira_ctx;

/* Timing helper */
static double get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1000000.0;
}

#ifdef __linux__

/**
 * Open and configure V4L2 camera device.
 */
static int camera_open_v4l2(camera_state_t* cam, int device_id) {
    char dev_path[32];
    snprintf(dev_path, sizeof(dev_path), "/dev/video%d", device_id);

    cam->fd = open(dev_path, O_RDWR | O_NONBLOCK);
    if (cam->fd < 0) {
        fprintf(stderr, "Failed to open %s\n", dev_path);
        return CIRA_ERROR_FILE;
    }

    /* Query capabilities */
    struct v4l2_capability cap;
    if (ioctl(cam->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        fprintf(stderr, "VIDIOC_QUERYCAP failed\n");
        close(cam->fd);
        return CIRA_ERROR;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "Device does not support video capture\n");
        close(cam->fd);
        return CIRA_ERROR;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "Device does not support streaming\n");
        close(cam->fd);
        return CIRA_ERROR;
    }

    /* Set format */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = cam->width;
    fmt.fmt.pix.height = cam->height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;  /* Try MJPEG first */
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    if (ioctl(cam->fd, VIDIOC_S_FMT, &fmt) < 0) {
        /* Fall back to YUYV */
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        if (ioctl(cam->fd, VIDIOC_S_FMT, &fmt) < 0) {
            fprintf(stderr, "Failed to set video format\n");
            close(cam->fd);
            return CIRA_ERROR;
        }
    }

    cam->width = fmt.fmt.pix.width;
    cam->height = fmt.fmt.pix.height;

    fprintf(stderr, "Camera format: %dx%d\n", cam->width, cam->height);

    /* Request buffers */
    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = NUM_BUFFERS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(cam->fd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "Failed to request buffers\n");
        close(cam->fd);
        return CIRA_ERROR;
    }

    cam->num_buffers = req.count;

    /* Map buffers */
    for (int i = 0; i < cam->num_buffers; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(cam->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            fprintf(stderr, "Failed to query buffer %d\n", i);
            close(cam->fd);
            return CIRA_ERROR;
        }

        cam->buffers[i].length = buf.length;
        cam->buffers[i].start = mmap(NULL, buf.length,
                                      PROT_READ | PROT_WRITE,
                                      MAP_SHARED, cam->fd, buf.m.offset);

        if (cam->buffers[i].start == MAP_FAILED) {
            fprintf(stderr, "Failed to mmap buffer %d\n", i);
            close(cam->fd);
            return CIRA_ERROR;
        }
    }

    /* Queue buffers */
    for (int i = 0; i < cam->num_buffers; i++) {
        struct v4l2_buffer buf;
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (ioctl(cam->fd, VIDIOC_QBUF, &buf) < 0) {
            fprintf(stderr, "Failed to queue buffer %d\n", i);
            return CIRA_ERROR;
        }
    }

    /* Start streaming */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(cam->fd, VIDIOC_STREAMON, &type) < 0) {
        fprintf(stderr, "Failed to start streaming\n");
        return CIRA_ERROR;
    }

    fprintf(stderr, "Camera started: /dev/video%d\n", device_id);
    return CIRA_OK;
}

/**
 * Close V4L2 camera.
 */
static void camera_close_v4l2(camera_state_t* cam) {
    if (cam->fd < 0) return;

    /* Stop streaming */
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl(cam->fd, VIDIOC_STREAMOFF, &type);

    /* Unmap buffers */
    for (int i = 0; i < cam->num_buffers; i++) {
        if (cam->buffers[i].start && cam->buffers[i].start != MAP_FAILED) {
            munmap(cam->buffers[i].start, cam->buffers[i].length);
        }
    }

    close(cam->fd);
    cam->fd = -1;
}

#endif /* __linux__ */

/**
 * Camera capture thread.
 */
static void* camera_thread_func(void* arg) {
    cira_ctx* ctx = (cira_ctx*)arg;
    (void)ctx;  /* Suppress unused warning */

    /* TODO: Implement capture loop */
    /*
     * camera_state_t* cam = get_camera_state(ctx);
     * double last_time = get_time_ms();
     * int frame_count = 0;
     *
     * while (ctx->camera_running) {
     *     // Dequeue buffer
     *     struct v4l2_buffer buf;
     *     memset(&buf, 0, sizeof(buf));
     *     buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
     *     buf.memory = V4L2_MEMORY_MMAP;
     *
     *     if (ioctl(cam->fd, VIDIOC_DQBUF, &buf) < 0) {
     *         if (errno == EAGAIN) {
     *             usleep(1000);
     *             continue;
     *         }
     *         break;
     *     }
     *
     *     // Process frame
     *     void* frame_data = cam->buffers[buf.index].start;
     *     size_t frame_size = buf.bytesused;
     *
     *     // Convert to RGB if needed (YUYV -> RGB)
     *     // Run inference
     *     // Copy result to ctx->frame_buffer
     *
     *     pthread_mutex_lock(&ctx->frame_mutex);
     *     // Copy frame...
     *     pthread_mutex_unlock(&ctx->frame_mutex);
     *
     *     // Re-queue buffer
     *     ioctl(cam->fd, VIDIOC_QBUF, &buf);
     *
     *     // Calculate FPS
     *     frame_count++;
     *     double now = get_time_ms();
     *     if (now - last_time >= 1000.0) {
     *         ctx->current_fps = frame_count * 1000.0 / (now - last_time);
     *         frame_count = 0;
     *         last_time = now;
     *     }
     * }
     */

    return NULL;
}

/**
 * Start camera capture.
 */
int camera_start(cira_ctx* ctx, int device_id) {
    if (!ctx) return CIRA_ERROR_INPUT;

    /* TODO: Implement camera start */
    (void)device_id;
    (void)camera_thread_func;

    fprintf(stderr, "Starting camera %d...\n", device_id);

    /* Allocate camera state */
    /* Open V4L2 device */
    /* Create capture thread */
    /* ctx->camera_running = 1; */
    /* pthread_create(&ctx->camera_thread, NULL, camera_thread_func, ctx); */

    return CIRA_OK;
}

/**
 * Stop camera capture.
 */
int camera_stop(cira_ctx* ctx) {
    if (!ctx) return CIRA_ERROR_INPUT;

    /* TODO: Implement camera stop */
    /* ctx->camera_running = 0; */
    /* pthread_join(ctx->camera_thread, NULL); */
    /* Close V4L2 device */

    fprintf(stderr, "Camera stopped\n");
    return CIRA_OK;
}

#else /* CIRA_STREAMING_ENABLED */

/* Stubs when streaming is not enabled */
int camera_start(cira_ctx* ctx, int device_id) {
    (void)ctx;
    (void)device_id;
    fprintf(stderr, "Streaming not enabled in this build\n");
    return CIRA_ERROR;
}

int camera_stop(cira_ctx* ctx) {
    (void)ctx;
    return CIRA_ERROR;
}

#endif /* CIRA_STREAMING_ENABLED */
