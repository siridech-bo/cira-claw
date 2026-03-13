/**
 * Signal Buffer - Ring buffer for multichannel time-series signals
 *
 * Spec C v4: Signal Ingestion
 * (c) CiRA Robotics / KMITL 2026
 */

#ifndef SIGNAL_BUFFER_H
#define SIGNAL_BUFFER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * Single channel ring buffer
 */
typedef struct {
    float*  data;           /* Ring buffer array [capacity] */
    int     capacity;       /* Equal to window_size */
    int     head;           /* Write position (0 to capacity-1) */
    int     count;          /* Samples written (0 to capacity) */
    char    name[64];       /* Channel name */
    float   min_val;        /* Min value for normalization */
    float   max_val;        /* Max value for normalization */
} signal_channel_t;

/**
 * Multi-channel signal buffer
 */
typedef struct {
    signal_channel_t*  channels;        /* Array of channels */
    int                num_channels;    /* Number of channels */
    int                window_size;     /* Samples per window */
    int                stride;          /* Samples to advance */
    float              sample_rate_hz;  /* Sampling rate */
} signal_buffer_t;

/**
 * Create a signal buffer
 *
 * @param num_channels Number of channels
 * @param channel_names Array of channel names
 * @param channel_min Array of min values for normalization
 * @param channel_max Array of max values for normalization
 * @param window_size Window size in samples
 * @param stride Stride in samples
 * @param sample_rate_hz Sampling rate in Hz
 * @return Allocated buffer or NULL on error
 */
signal_buffer_t* signal_buffer_create(
    int num_channels,
    const char* channel_names[],
    const float* channel_min,
    const float* channel_max,
    int window_size,
    int stride,
    float sample_rate_hz
);

/**
 * Destroy signal buffer and free memory
 */
void signal_buffer_destroy(signal_buffer_t* buf);

/**
 * Feed one sample to a channel
 *
 * @param buf Buffer
 * @param channel_idx Channel index (0 to num_channels-1)
 * @param value Sample value
 * @return 0 on success, -1 on error
 */
int signal_buffer_feed(signal_buffer_t* buf, int channel_idx, float value);

/**
 * Check if buffer is ready for window extraction
 *
 * @param buf Buffer
 * @return 1 if all channels have >= window_size samples, 0 otherwise
 */
int signal_buffer_ready(const signal_buffer_t* buf);

/**
 * Get normalized window from all channels
 *
 * Layout: out_window[sample_idx * num_channels + channel_idx]
 * Samples are in chronological order (oldest to newest)
 * Values are min-max normalized to [0, 1]
 *
 * @param buf Buffer
 * @param out_window Output array [window_size * num_channels]
 * @return 0 on success, -1 if not ready
 */
int signal_buffer_get_window(const signal_buffer_t* buf, float* out_window);

/**
 * Get window for a specific channel
 * Returns pointer to internal normalized window data
 * Data is valid until next feed or advance call
 *
 * @param buf Buffer
 * @param channel_idx Channel index
 * @return Pointer to window data [window_size], or NULL if not ready
 */
const float* signal_buffer_get_channel_window(const signal_buffer_t* buf, int channel_idx);

/**
 * Advance buffer by stride samples
 * Discards oldest stride samples from each channel
 */
void signal_buffer_advance(signal_buffer_t* buf);

/**
 * Find channel index by name
 *
 * @param buf Buffer
 * @param name Channel name
 * @return Channel index or -1 if not found
 */
int signal_buffer_channel_idx(const signal_buffer_t* buf, const char* name);

/**
 * Compute RMS over current ring buffer content
 *
 * @param buf Buffer
 * @param channel_idx Channel index
 * @return RMS value or 0 if channel empty
 */
float signal_buffer_rms(const signal_buffer_t* buf, int channel_idx);

/**
 * Compute peak-to-peak over current ring buffer content
 *
 * @param buf Buffer
 * @param channel_idx Channel index
 * @return Peak-to-peak value or 0 if channel empty
 */
float signal_buffer_peak(const signal_buffer_t* buf, int channel_idx);

/**
 * Compute mean over current ring buffer content
 *
 * @param buf Buffer
 * @param channel_idx Channel index
 * @return Mean value or 0 if channel empty
 */
float signal_buffer_mean(const signal_buffer_t* buf, int channel_idx);

#ifdef __cplusplus
}
#endif

#endif /* SIGNAL_BUFFER_H */
