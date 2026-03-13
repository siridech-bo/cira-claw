/**
 * Signal Buffer Implementation
 *
 * Ring buffer for multichannel time-series signals
 * Spec C v4: Signal Ingestion
 */

#include "signal_buffer.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

signal_buffer_t* signal_buffer_create(
    int num_channels,
    const char* channel_names[],
    const float* channel_min,
    const float* channel_max,
    int window_size,
    int stride,
    float sample_rate_hz
) {
    if (num_channels <= 0 || window_size <= 0 || stride <= 0) {
        return NULL;
    }

    signal_buffer_t* buf = (signal_buffer_t*)malloc(sizeof(signal_buffer_t));
    if (!buf) {
        return NULL;
    }

    buf->num_channels = num_channels;
    buf->window_size = window_size;
    buf->stride = stride;
    buf->sample_rate_hz = sample_rate_hz;

    /* Allocate channels array */
    buf->channels = (signal_channel_t*)calloc(num_channels, sizeof(signal_channel_t));
    if (!buf->channels) {
        free(buf);
        return NULL;
    }

    /* Initialize each channel */
    for (int i = 0; i < num_channels; i++) {
        signal_channel_t* ch = &buf->channels[i];

        ch->capacity = window_size;  /* Ring buffer size = window_size */
        ch->head = 0;
        ch->count = 0;

        /* Copy channel name */
        strncpy(ch->name, channel_names[i], sizeof(ch->name) - 1);
        ch->name[sizeof(ch->name) - 1] = '\0';

        /* Set normalization bounds */
        ch->min_val = channel_min[i];
        ch->max_val = channel_max[i];

        /* Allocate ring buffer */
        ch->data = (float*)calloc(ch->capacity, sizeof(float));
        if (!ch->data) {
            /* Cleanup on failure */
            for (int j = 0; j < i; j++) {
                free(buf->channels[j].data);
            }
            free(buf->channels);
            free(buf);
            return NULL;
        }
    }

    return buf;
}

void signal_buffer_destroy(signal_buffer_t* buf) {
    if (!buf) {
        return;
    }

    if (buf->channels) {
        for (int i = 0; i < buf->num_channels; i++) {
            free(buf->channels[i].data);
        }
        free(buf->channels);
    }

    free(buf);
}

int signal_buffer_feed(signal_buffer_t* buf, int channel_idx, float value) {
    if (!buf || channel_idx < 0 || channel_idx >= buf->num_channels) {
        return -1;
    }

    signal_channel_t* ch = &buf->channels[channel_idx];

    /* Write to ring buffer */
    ch->data[ch->head] = value;
    ch->head = (ch->head + 1) % ch->capacity;

    /* Increment count up to capacity */
    if (ch->count < ch->capacity) {
        ch->count++;
    }

    return 0;
}

int signal_buffer_ready(const signal_buffer_t* buf) {
    if (!buf) {
        return 0;
    }

    /* All channels must have at least window_size samples */
    for (int i = 0; i < buf->num_channels; i++) {
        if (buf->channels[i].count < buf->window_size) {
            return 0;
        }
    }

    return 1;
}

int signal_buffer_get_window(const signal_buffer_t* buf, float* out_window) {
    if (!buf || !out_window) {
        return -1;
    }

    if (!signal_buffer_ready(buf)) {
        return -1;
    }

    /* Extract window in chronological order with normalization */
    for (int sample_idx = 0; sample_idx < buf->window_size; sample_idx++) {
        for (int ch_idx = 0; ch_idx < buf->num_channels; ch_idx++) {
            const signal_channel_t* ch = &buf->channels[ch_idx];

            /* Calculate read position (oldest to newest) */
            /* oldest = (head - count + capacity) % capacity */
            /* sample at offset = (oldest + sample_idx) % capacity */
            int oldest = (ch->head - ch->count + ch->capacity) % ch->capacity;
            int read_pos = (oldest + sample_idx) % ch->capacity;

            float raw_value = ch->data[read_pos];

            /* Min-max normalization */
            float range = ch->max_val - ch->min_val + 1e-10f;
            float normalized = (raw_value - ch->min_val) / range;

            /* Clamp to [0, 1] */
            if (normalized < 0.0f) normalized = 0.0f;
            if (normalized > 1.0f) normalized = 1.0f;

            /* Layout: out_window[sample * num_channels + ch] */
            out_window[sample_idx * buf->num_channels + ch_idx] = normalized;
        }
    }

    return 0;
}

const float* signal_buffer_get_channel_window(const signal_buffer_t* buf, int channel_idx) {
    if (!buf || channel_idx < 0 || channel_idx >= buf->num_channels) {
        return NULL;
    }

    if (!signal_buffer_ready(buf)) {
        return NULL;
    }

    const signal_channel_t* ch = &buf->channels[channel_idx];

    /* Return pointer to the ring buffer data */
    /* Caller will need to handle ring buffer wraparound */
    /* For simplicity, we'll use a static temp buffer to linearize */
    static float temp_window[4096];  /* Max window size */

    if (buf->window_size > 4096) {
        return NULL;  /* Window too large */
    }

    /* Extract window in chronological order */
    int start_idx = (ch->head - ch->count + ch->capacity) % ch->capacity;
    for (int i = 0; i < buf->window_size; i++) {
        int idx = (start_idx + i) % ch->capacity;
        float raw_value = ch->data[idx];

        /* Min-max normalization */
        float range = ch->max_val - ch->min_val + 1e-10f;
        float normalized = (raw_value - ch->min_val) / range;

        /* Clamp to [0, 1] */
        if (normalized < 0.0f) normalized = 0.0f;
        if (normalized > 1.0f) normalized = 1.0f;

        temp_window[i] = normalized;
    }

    return temp_window;
}

void signal_buffer_advance(signal_buffer_t* buf) {
    if (!buf) {
        return;
    }

    /* Decrement count by stride for each channel */
    for (int i = 0; i < buf->num_channels; i++) {
        signal_channel_t* ch = &buf->channels[i];
        ch->count -= buf->stride;
        if (ch->count < 0) {
            ch->count = 0;
        }
        /* Note: head pointer keeps advancing naturally as new samples come in */
        /* No need to move data */
    }
}

int signal_buffer_channel_idx(const signal_buffer_t* buf, const char* name) {
    if (!buf || !name) {
        return -1;
    }

    for (int i = 0; i < buf->num_channels; i++) {
        if (strcmp(buf->channels[i].name, name) == 0) {
            return i;
        }
    }

    return -1;
}

float signal_buffer_rms(const signal_buffer_t* buf, int channel_idx) {
    if (!buf || channel_idx < 0 || channel_idx >= buf->num_channels) {
        return 0.0f;
    }

    const signal_channel_t* ch = &buf->channels[channel_idx];
    if (ch->count == 0) {
        return 0.0f;
    }

    /* Compute RMS over available samples */
    float sum_squares = 0.0f;
    int oldest = (ch->head - ch->count + ch->capacity) % ch->capacity;

    for (int i = 0; i < ch->count; i++) {
        int pos = (oldest + i) % ch->capacity;
        float val = ch->data[pos];
        sum_squares += val * val;
    }

    return sqrtf(sum_squares / ch->count);
}

float signal_buffer_peak(const signal_buffer_t* buf, int channel_idx) {
    if (!buf || channel_idx < 0 || channel_idx >= buf->num_channels) {
        return 0.0f;
    }

    const signal_channel_t* ch = &buf->channels[channel_idx];
    if (ch->count == 0) {
        return 0.0f;
    }

    /* Find min and max over available samples */
    int oldest = (ch->head - ch->count + ch->capacity) % ch->capacity;
    int pos = oldest;

    float min_val = ch->data[pos];
    float max_val = ch->data[pos];

    for (int i = 1; i < ch->count; i++) {
        pos = (oldest + i) % ch->capacity;
        float val = ch->data[pos];
        if (val < min_val) min_val = val;
        if (val > max_val) max_val = val;
    }

    return max_val - min_val;  /* Peak-to-peak */
}

float signal_buffer_mean(const signal_buffer_t* buf, int channel_idx) {
    if (!buf || channel_idx < 0 || channel_idx >= buf->num_channels) {
        return 0.0f;
    }

    const signal_channel_t* ch = &buf->channels[channel_idx];
    if (ch->count == 0) {
        return 0.0f;
    }

    /* Compute mean over available samples */
    float sum = 0.0f;
    int oldest = (ch->head - ch->count + ch->capacity) % ch->capacity;

    for (int i = 0; i < ch->count; i++) {
        int pos = (oldest + i) % ch->capacity;
        sum += ch->data[pos];
    }

    return sum / ch->count;
}
