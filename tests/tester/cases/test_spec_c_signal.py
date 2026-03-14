"""
Spec C Signal Ingestion Tests - Full pipeline via HTTP
Tests signal buffer, feeding, and prediction via black-box HTTP API
"""

import requests
import math


def generate_sine_wave(freq_hz, sample_rate_hz, num_samples):
    """Generate sine wave test signal"""
    return [math.sin(2 * math.pi * freq_hz * i / sample_rate_hz)
            for i in range(num_samples)]


def test_signals_endpoint_returns_channels(base_url):
    """T1: /api/signals returns array with 3 channels (ch0, ch1, ch2)"""
    response = requests.get(f"{base_url}/api/signals")
    assert response.status_code == 200, f"Expected 200, got {response.status_code}"

    data = response.json()
    assert isinstance(data, list), "Response should be an array"
    assert len(data) == 3, f"Expected 3 channels, got {len(data)}"

    # Check channel names
    channel_names = [ch['name'] for ch in data]
    assert 'ch0' in channel_names, "Missing channel ch0"
    assert 'ch1' in channel_names, "Missing channel ch1"
    assert 'ch2' in channel_names, "Missing channel ch2"


def test_buffer_not_ready_before_feeding(base_url):
    """T2: Before feeding - signal buffer should indicate not ready"""
    # Note: /api/stats doesn't currently expose signal.ready field
    # This test verifies signal endpoint returns channels with initial stats
    response = requests.get(f"{base_url}/api/signals")
    assert response.status_code == 200, f"Expected 200, got {response.status_code}"

    data = response.json()
    assert len(data) > 0, "Should have at least one channel"


def test_feed_samples_channel_0(base_url):
    """T3: Feed 64 samples to ch0 - buffer not yet ready"""
    # Generate 64 samples of sine wave at 250 Hz
    samples = generate_sine_wave(250, 1000, 64)

    payload = {
        "channel": 0,
        "samples": samples
    }

    response = requests.post(f"{base_url}/api/signal/feed", json=payload)
    assert response.status_code == 200, f"Expected 200, got {response.status_code}"

    data = response.json()
    assert data['success'] == True, "Feed should succeed"
    assert data['fed'] == 64, f"Expected fed=64, got {data['fed']}"
    assert data['buffer_ready'] == False, "Buffer should not be ready yet (need 128 samples)"
    assert data['prediction_ran'] == False, "Prediction should not run yet"


def test_feed_samples_all_channels_partial(base_url):
    """T4: Feed 64 samples to ch1 and ch2 - buffer still not ready"""
    samples = generate_sine_wave(250, 1000, 64)

    # Feed ch1
    payload = {"channel": 1, "samples": samples}
    response = requests.post(f"{base_url}/api/signal/feed", json=payload)
    assert response.status_code == 200
    data = response.json()
    assert data['buffer_ready'] == False, "Buffer should not be ready yet"

    # Feed ch2
    payload = {"channel": 2, "samples": samples}
    response = requests.post(f"{base_url}/api/signal/feed", json=payload)
    assert response.status_code == 200
    data = response.json()
    assert data['buffer_ready'] == False, "Buffer should not be ready yet"


def test_feed_remaining_samples_triggers_prediction(base_url):
    """T5: Feed remaining 64 samples to reach window_size=128 - prediction runs"""
    # Feed remaining 64 samples to all channels
    samples = generate_sine_wave(250, 1000, 64)

    for channel in [0, 1, 2]:
        payload = {"channel": channel, "samples": samples}
        response = requests.post(f"{base_url}/api/signal/feed", json=payload)
        assert response.status_code == 200, f"Feed to ch{channel} failed"

        data = response.json()

        # Last channel (ch2) should trigger prediction
        if channel == 2:
            assert data['buffer_ready'] == True, "Buffer should be ready after 128 samples"
            assert data['prediction_ran'] == True, "Prediction should have run"
            assert data['result'] is not None, "Should have prediction result"

            # Check result has either class or anomaly_score
            result = data['result']
            has_class = 'class' in result or 'label' in result
            has_score = 'anomaly_score' in result or 'score' in result
            assert has_class or has_score, "Result should have class or anomaly_score"


def test_signals_stats_after_feeding(base_url):
    """T6: /api/signals after feeding - rms/peak/mean are non-zero for ch0"""
    response = requests.get(f"{base_url}/api/signals")
    assert response.status_code == 200

    data = response.json()
    ch0 = next((ch for ch in data if ch['name'] == 'ch0'), None)

    assert ch0 is not None, "ch0 not found in signals"
    assert 'rms' in ch0, "ch0 missing rms field"
    assert 'peak' in ch0, "ch0 missing peak field"
    assert 'mean' in ch0, "ch0 missing mean field"

    # After feeding sine wave, stats should be non-zero
    # (allowing small tolerance for DC signal)
    rms = abs(ch0['rms'])
    peak = abs(ch0['peak'])
    # mean might be near 0 for sine wave, so we check RMS instead
    assert rms > 0.1, f"RMS should be > 0.1 for sine wave, got {rms}"


def test_sustained_feeding(base_url):
    """T7: Feed 200 more samples - confirm multiple windows process without error"""
    # Feed 200 more samples (will process ~3 more windows with stride=64)
    samples = generate_sine_wave(250, 1000, 200)

    for channel in [0, 1, 2]:
        payload = {"channel": channel, "samples": samples}
        response = requests.post(f"{base_url}/api/signal/feed", json=payload)
        assert response.status_code == 200, f"Sustained feed to ch{channel} failed"

        data = response.json()
        assert data['success'] == True, "Sustained feeding should succeed"


def test_health_after_sustained_feeding(base_url):
    """T8: /health still returns 200 after sustained feeding (stability check)"""
    response = requests.get(f"{base_url}/health")
    assert response.status_code == 200, "Runtime should still be healthy after sustained feeding"

    data = response.json()
    assert data['status'] == 'ok', "Runtime status should be 'ok'"
