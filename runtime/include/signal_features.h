/**
 * Signal Features - Feature extraction for time-series signals
 *
 * Computes 46 canonical features from signal windows
 * Feature order MUST NOT change - this is a strict contract
 *
 * Spec C v4: Signal Ingestion
 * (c) CiRA Robotics / KMITL 2026
 */

#ifndef SIGNAL_FEATURES_H
#define SIGNAL_FEATURES_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Compute all 46 features from a signal window
 *
 * @param window Input signal window
 * @param window_size Number of samples
 * @param sample_rate_hz Sampling rate in Hz
 * @param out Output array [46] for feature values
 */
void signal_compute_features(
    const float* window,
    int window_size,
    float sample_rate_hz,
    float out[46]
);

/**
 * Get feature index by name
 *
 * @param name Feature name
 * @return Feature index (0-45) or -1 if not found
 */
int signal_feature_index(const char* name);

/**
 * Get feature name by index
 *
 * @param index Feature index (0-45)
 * @return Feature name or NULL if index out of range
 */
const char* signal_feature_name(int index);

#ifdef __cplusplus
}
#endif

#endif /* SIGNAL_FEATURES_H */
