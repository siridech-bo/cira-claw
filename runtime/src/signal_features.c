/**
 * Signal Features Implementation
 *
 * 46 canonical features - order MUST NOT change
 * Spec C v4: Signal Ingestion
 */

#include "signal_features.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>

/* Feature names in canonical order */
static const char* FEATURE_NAMES[46] = {
    /* Statistical [0..24] */
    "mean", "std", "min", "max", "median",
    "sum", "variance", "skewness", "kurtosis", "abs_energy",
    "root_mean_square", "mean_abs_change", "mean_change", "count_above_mean", "count_below_mean",
    "first_location_of_maximum", "first_location_of_minimum", "last_location_of_maximum",
    "last_location_of_minimum", "percentage_of_reoccurring_values", "sum_of_reoccurring_values",
    "abs_sum_of_changes", "range", "interquartile_range", "mean_second_derivative",
    /* DSP [25..45] */
    "rms", "peak_to_peak", "crest_factor", "shape_factor", "impulse_factor",
    "margin_factor", "zero_crossing_rate", "autocorr_lag1", "autocorr_lag5", "binned_entropy",
    "spectral_centroid", "spectral_bandwidth", "spectral_rolloff", "spectral_flatness",
    "spectral_entropy", "peak_frequency", "spectral_skewness", "spectral_kurtosis",
    "band_power_low", "band_power_mid", "band_power_high"
};

/* Helper: guard NaN/Inf */
static inline float guard_finite(float x) {
    return isfinite(x) ? x : 0.0f;
}

/* Helper: compare function for qsort */
static int compare_float(const void* a, const void* b) {
    float diff = (*(float*)a - *(float*)b);
    return (diff > 0) - (diff < 0);
}

/* Helper: linear interpolation for percentile */
static float percentile(const float* sorted, int n, float p) {
    if (n == 0) return 0.0f;
    if (n == 1) return sorted[0];

    float pos = p * (n - 1);
    int idx = (int)pos;
    float frac = pos - idx;

    if (idx >= n - 1) return sorted[n - 1];
    return sorted[idx] + frac * (sorted[idx + 1] - sorted[idx]);
}

/* Helper: radix-2 Cooley-Tukey FFT (in-place, complex) */
static void fft_radix2(float* real, float* imag, int n) {
    /* Bit-reversal permutation */
    int j = 0;
    for (int i = 0; i < n - 1; i++) {
        if (i < j) {
            float temp = real[i]; real[i] = real[j]; real[j] = temp;
            temp = imag[i]; imag[i] = imag[j]; imag[j] = temp;
        }
        int m = n >> 1;
        while (m >= 1 && j >= m) {
            j -= m;
            m >>= 1;
        }
        j += m;
    }

    /* Cooley-Tukey decimation-in-time */
    for (int s = 1; s <= (int)log2(n); s++) {
        int m = 1 << s;
        int m2 = m >> 1;
        float wm_r = cosf(-2.0f * M_PI / m);
        float wm_i = sinf(-2.0f * M_PI / m);

        for (int k = 0; k < n; k += m) {
            float w_r = 1.0f;
            float w_i = 0.0f;

            for (int j = 0; j < m2; j++) {
                int t_idx = k + j + m2;
                int u_idx = k + j;

                float t_r = w_r * real[t_idx] - w_i * imag[t_idx];
                float t_i = w_r * imag[t_idx] + w_i * real[t_idx];

                real[t_idx] = real[u_idx] - t_r;
                imag[t_idx] = imag[u_idx] - t_i;
                real[u_idx] = real[u_idx] + t_r;
                imag[u_idx] = imag[u_idx] + t_i;

                float temp_w_r = w_r * wm_r - w_i * wm_i;
                w_i = w_r * wm_i + w_i * wm_r;
                w_r = temp_w_r;
            }
        }
    }
}

/* Helper: next power of 2 */
static int next_pow2(int n) {
    int p = 1;
    while (p < n && p < 16384) p <<= 1;
    return p;
}

void signal_compute_features(
    const float* window,
    int window_size,
    float sample_rate_hz,
    float out[46]
) {
    if (!window || window_size <= 0 || !out) {
        memset(out, 0, 46 * sizeof(float));
        return;
    }

    const int n = window_size;

    /* === Statistical Features [0..24] === */

    /* [0] mean */
    float mean = 0.0f;
    for (int i = 0; i < n; i++) mean += window[i];
    mean /= n;
    out[0] = guard_finite(mean);

    /* [1] std, [6] variance */
    float variance = 0.0f;
    for (int i = 0; i < n; i++) {
        float diff = window[i] - mean;
        variance += diff * diff;
    }
    variance /= n;
    out[6] = guard_finite(variance);
    out[1] = guard_finite(sqrtf(variance));

    /* [2] min, [3] max, [22] range */
    float min_val = window[0], max_val = window[0];
    for (int i = 1; i < n; i++) {
        if (window[i] < min_val) min_val = window[i];
        if (window[i] > max_val) max_val = window[i];
    }
    out[2] = guard_finite(min_val);
    out[3] = guard_finite(max_val);
    out[22] = guard_finite(max_val - min_val);

    /* [4] median, [23] interquartile_range */
    float* sorted = (float*)malloc(n * sizeof(float));
    if (sorted) {
        memcpy(sorted, window, n * sizeof(float));
        qsort(sorted, n, sizeof(float), compare_float);
        out[4] = guard_finite(percentile(sorted, n, 0.5f));
        float q1 = percentile(sorted, n, 0.25f);
        float q3 = percentile(sorted, n, 0.75f);
        out[23] = guard_finite(q3 - q1);
        free(sorted);
    } else {
        out[4] = out[23] = 0.0f;
    }

    /* [5] sum */
    float sum = 0.0f;
    for (int i = 0; i < n; i++) sum += window[i];
    out[5] = guard_finite(sum);

    /* [7] skewness */
    float skewness = 0.0f;
    float sigma = sqrtf(variance);
    if (sigma > 1e-10f) {
        for (int i = 0; i < n; i++) {
            float z = (window[i] - mean) / sigma;
            skewness += z * z * z;
        }
        skewness /= n;
    }
    out[7] = guard_finite(skewness);

    /* [8] kurtosis (excess) */
    float kurtosis = 0.0f;
    if (sigma > 1e-10f) {
        for (int i = 0; i < n; i++) {
            float z = (window[i] - mean) / sigma;
            kurtosis += z * z * z * z;
        }
        kurtosis = kurtosis / n - 3.0f;
    }
    out[8] = guard_finite(kurtosis);

    /* [9] abs_energy */
    float abs_energy = 0.0f;
    for (int i = 0; i < n; i++) {
        abs_energy += window[i] * window[i];
    }
    out[9] = guard_finite(abs_energy);

    /* [10] root_mean_square */
    out[10] = guard_finite(sqrtf(abs_energy / n));

    /* [11] mean_abs_change, [21] abs_sum_of_changes */
    float mean_abs_change = 0.0f;
    float abs_sum_changes = 0.0f;
    for (int i = 1; i < n; i++) {
        float diff = window[i] - window[i-1];
        mean_abs_change += fabsf(diff);
        abs_sum_changes += fabsf(diff);
    }
    mean_abs_change /= (n - 1);
    out[11] = guard_finite(mean_abs_change);
    out[21] = guard_finite(abs_sum_changes);

    /* [12] mean_change */
    float mean_change = (n > 1) ? ((window[n-1] - window[0]) / (n - 1)) : 0.0f;
    out[12] = guard_finite(mean_change);

    /* [13] count_above_mean, [14] count_below_mean */
    int count_above = 0, count_below = 0;
    for (int i = 0; i < n; i++) {
        if (window[i] > mean) count_above++;
        else if (window[i] < mean) count_below++;
    }
    out[13] = (float)count_above;
    out[14] = (float)count_below;

    /* [15] first_location_of_maximum, [16] first_location_of_minimum */
    /* [17] last_location_of_maximum, [18] last_location_of_minimum */
    int first_max = 0, first_min = 0, last_max = 0, last_min = 0;
    for (int i = 0; i < n; i++) {
        if (window[i] == max_val) {
            if (first_max == 0 && i > 0) first_max = i;
            if (first_max == 0 && i == 0) first_max = 0;
            last_max = i;
        }
        if (window[i] == min_val) {
            if (first_min == 0 && i > 0) first_min = i;
            if (first_min == 0 && i == 0) first_min = 0;
            last_min = i;
        }
    }
    out[15] = (float)first_max / (n - 1);
    out[16] = (float)first_min / (n - 1);
    out[17] = (float)last_max / (n - 1);
    out[18] = (float)last_min / (n - 1);

    /* [19] percentage_of_reoccurring_values */
    sorted = (float*)malloc(n * sizeof(float));
    if (sorted) {
        memcpy(sorted, window, n * sizeof(float));
        qsort(sorted, n, sizeof(float), compare_float);
        int unique_count = 1;
        for (int i = 1; i < n; i++) {
            if (sorted[i] != sorted[i-1]) unique_count++;
        }
        out[19] = guard_finite((float)unique_count / n);
        free(sorted);
    } else {
        out[19] = 1.0f;
    }

    /* [20] sum_of_reoccurring_values */
    float sum_reoccur = 0.0f;
    sorted = (float*)malloc(n * sizeof(float));
    if (sorted) {
        memcpy(sorted, window, n * sizeof(float));
        qsort(sorted, n, sizeof(float), compare_float);
        int i = 0;
        while (i < n) {
            int count = 1;
            float val = sorted[i];
            while (i + count < n && sorted[i + count] == val) count++;
            if (count > 1) {
                sum_reoccur += val * count;
            }
            i += count;
        }
        free(sorted);
    }
    out[20] = guard_finite(sum_reoccur);

    /* [24] mean_second_derivative */
    float mean_second_deriv = 0.0f;
    if (n >= 3) {
        for (int i = 1; i < n - 1; i++) {
            float deriv2 = window[i+1] - 2*window[i] + window[i-1];
            mean_second_deriv += deriv2;
        }
        mean_second_deriv /= (n - 2);
    }
    out[24] = guard_finite(mean_second_deriv);

    /* === DSP Features [25..45] === */

    /* [25] rms */
    out[25] = out[10];  /* Same as root_mean_square */

    /* [26] peak_to_peak */
    out[26] = out[22];  /* Same as range */

    /* [27] crest_factor */
    float peak = fmaxf(fabsf(min_val), fabsf(max_val));
    float rms = out[10];
    out[27] = guard_finite((rms > 1e-10f) ? (peak / rms) : 0.0f);

    /* [28] shape_factor */
    float mean_abs = 0.0f;
    for (int i = 0; i < n; i++) mean_abs += fabsf(window[i]);
    mean_abs /= n;
    out[28] = guard_finite((mean_abs > 1e-10f) ? (rms / mean_abs) : 0.0f);

    /* [29] impulse_factor */
    out[29] = guard_finite((mean_abs > 1e-10f) ? (peak / mean_abs) : 0.0f);

    /* [30] margin_factor */
    float mean_sqrt_abs = 0.0f;
    for (int i = 0; i < n; i++) mean_sqrt_abs += sqrtf(fabsf(window[i]));
    mean_sqrt_abs /= n;
    float msq = mean_sqrt_abs * mean_sqrt_abs;
    out[30] = guard_finite((msq > 1e-10f) ? (peak / msq) : 0.0f);

    /* [31] zero_crossing_rate */
    int zero_crossings = 0;
    for (int i = 1; i < n; i++) {
        if ((window[i] >= 0 && window[i-1] < 0) || (window[i] < 0 && window[i-1] >= 0)) {
            zero_crossings++;
        }
    }
    out[31] = guard_finite((float)zero_crossings / (n - 1));

    /* [32] autocorr_lag1, [33] autocorr_lag5 */
    float var_denom = variance * n;
    if (var_denom > 1e-10f) {
        /* lag 1 */
        float corr1 = 0.0f;
        for (int i = 0; i < n - 1; i++) {
            corr1 += (window[i] - mean) * (window[i+1] - mean);
        }
        out[32] = guard_finite(corr1 / var_denom);

        /* lag 5 */
        float corr5 = 0.0f;
        for (int i = 0; i < n - 5; i++) {
            corr5 += (window[i] - mean) * (window[i+5] - mean);
        }
        out[33] = guard_finite(corr5 / var_denom);
    } else {
        out[32] = out[33] = 0.0f;
    }

    /* [34] binned_entropy */
    const int NUM_BINS = 10;
    int bins[10];
    memset(bins, 0, sizeof(bins));
    float bin_width = (max_val - min_val + 1e-10f) / NUM_BINS;
    for (int i = 0; i < n; i++) {
        int bin_idx = (int)((window[i] - min_val) / bin_width);
        if (bin_idx >= NUM_BINS) bin_idx = NUM_BINS - 1;
        if (bin_idx < 0) bin_idx = 0;
        bins[bin_idx]++;
    }
    float entropy = 0.0f;
    for (int i = 0; i < NUM_BINS; i++) {
        if (bins[i] > 0) {
            float p = (float)bins[i] / n;
            entropy -= p * logf(p + 1e-10f);
        }
    }
    out[34] = guard_finite(entropy);

    /* === Spectral Features [35..45] - Require FFT === */

    int fft_size = next_pow2(n);
    float* real = (float*)calloc(fft_size, sizeof(float));
    float* imag = (float*)calloc(fft_size, sizeof(float));

    if (real && imag) {
        /* Copy window and zero-pad */
        memcpy(real, window, n * sizeof(float));

        /* Perform FFT */
        fft_radix2(real, imag, fft_size);

        /* Compute magnitude spectrum (first half only) */
        int num_freqs = fft_size / 2;
        float* magnitude = (float*)malloc(num_freqs * sizeof(float));
        if (magnitude) {
            float total_power = 0.0f;
            for (int i = 0; i < num_freqs; i++) {
                magnitude[i] = sqrtf(real[i]*real[i] + imag[i]*imag[i]);
                total_power += magnitude[i];
            }

            float freq_resolution = sample_rate_hz / fft_size;

            /* [35] spectral_centroid */
            float centroid = 0.0f;
            if (total_power > 1e-10f) {
                for (int i = 0; i < num_freqs; i++) {
                    centroid += magnitude[i] * i * freq_resolution;
                }
                centroid /= total_power;
            }
            out[35] = guard_finite(centroid);

            /* [36] spectral_bandwidth */
            float bandwidth = 0.0f;
            if (total_power > 1e-10f) {
                for (int i = 0; i < num_freqs; i++) {
                    float diff = i * freq_resolution - centroid;
                    bandwidth += magnitude[i] * diff * diff;
                }
                bandwidth = sqrtf(bandwidth / total_power);
            }
            out[36] = guard_finite(bandwidth);

            /* [37] spectral_rolloff */
            float rolloff = 0.0f;
            float cumsum = 0.0f;
            float threshold = 0.85f * total_power;
            for (int i = 0; i < num_freqs; i++) {
                cumsum += magnitude[i];
                if (cumsum >= threshold) {
                    rolloff = i * freq_resolution;
                    break;
                }
            }
            out[37] = guard_finite(rolloff);

            /* [38] spectral_flatness */
            float geo_mean = 0.0f;
            float arith_mean = total_power / num_freqs;
            int count_nonzero = 0;
            for (int i = 0; i < num_freqs; i++) {
                if (magnitude[i] > 1e-10f) {
                    geo_mean += logf(magnitude[i]);
                    count_nonzero++;
                }
            }
            float flatness = 0.0f;
            if (count_nonzero > 0 && arith_mean > 1e-10f) {
                geo_mean = expf(geo_mean / count_nonzero);
                flatness = geo_mean / arith_mean;
            }
            out[38] = guard_finite(flatness);

            /* [39] spectral_entropy */
            float spec_entropy = 0.0f;
            if (total_power > 1e-10f) {
                for (int i = 0; i < num_freqs; i++) {
                    float p = magnitude[i] / total_power;
                    if (p > 1e-10f) {
                        spec_entropy -= p * logf(p);
                    }
                }
            }
            out[39] = guard_finite(spec_entropy);

            /* [40] peak_frequency */
            int peak_idx = 0;
            float peak_mag = magnitude[0];
            for (int i = 1; i < num_freqs; i++) {
                if (magnitude[i] > peak_mag) {
                    peak_mag = magnitude[i];
                    peak_idx = i;
                }
            }
            out[40] = guard_finite(peak_idx * freq_resolution);

            /* [41] spectral_skewness */
            float spec_skew = 0.0f;
            if (total_power > 1e-10f && bandwidth > 1e-10f) {
                for (int i = 0; i < num_freqs; i++) {
                    float z = (i * freq_resolution - centroid) / bandwidth;
                    spec_skew += magnitude[i] * z * z * z;
                }
                spec_skew /= total_power;
            }
            out[41] = guard_finite(spec_skew);

            /* [42] spectral_kurtosis */
            float spec_kurt = 0.0f;
            if (total_power > 1e-10f && bandwidth > 1e-10f) {
                for (int i = 0; i < num_freqs; i++) {
                    float z = (i * freq_resolution - centroid) / bandwidth;
                    spec_kurt += magnitude[i] * z * z * z * z;
                }
                spec_kurt = spec_kurt / total_power - 3.0f;
            }
            out[42] = guard_finite(spec_kurt);

            /* [43] band_power_low [0, sr/6) */
            /* [44] band_power_mid [sr/6, sr/3) */
            /* [45] band_power_high [sr/3, sr/2] */
            float sr = sample_rate_hz;
            float low_cutoff = sr / 6.0f;
            float mid_cutoff = sr / 3.0f;

            float band_low = 0.0f, band_mid = 0.0f, band_high = 0.0f;
            for (int i = 0; i < num_freqs; i++) {
                float freq = i * freq_resolution;
                if (freq < low_cutoff) {
                    band_low += magnitude[i];
                } else if (freq < mid_cutoff) {
                    band_mid += magnitude[i];
                } else {
                    band_high += magnitude[i];
                }
            }
            out[43] = guard_finite(band_low);
            out[44] = guard_finite(band_mid);
            out[45] = guard_finite(band_high);

            free(magnitude);
        } else {
            /* FFT failed - zero out spectral features */
            for (int i = 35; i <= 45; i++) out[i] = 0.0f;
        }
    } else {
        /* Allocation failed - zero out spectral features */
        for (int i = 35; i <= 45; i++) out[i] = 0.0f;
    }

    if (real) free(real);
    if (imag) free(imag);
}

int signal_feature_index(const char* name) {
    if (!name) return -1;

    for (int i = 0; i < 46; i++) {
        if (strcmp(name, FEATURE_NAMES[i]) == 0) {
            return i;
        }
    }

    return -1;
}

const char* signal_feature_name(int index) {
    if (index < 0 || index >= 46) {
        return NULL;
    }

    return FEATURE_NAMES[index];
}
