#!/usr/bin/env python3
"""
Quick manual test - runs against existing runtime on port 8080
Assumes runtime is already started with signal model loaded

Usage:
  1. Start runtime: cd cira-edge && .\start-system.bat
  2. Load signal model via dashboard or:
     curl -X POST http://localhost:8080/api/model -H "Content-Type: application/json" -d '{"path":"D:\\CiRA Claw\\cira-edge\\test_signal_model"}'
  3. Run this script: python test_manual.py
"""

import requests
import math
import sys


def generate_sine_wave(freq_hz, sample_rate_hz, num_samples):
    """Generate sine wave test signal"""
    return [math.sin(2 * math.pi * freq_hz * i / sample_rate_hz)
            for i in range(num_samples)]


def test_spec_c_signal_pipeline():
    """Test the full Spec C signal ingestion pipeline"""
    base_url = "http://localhost:8080"

    print("="*60)
    print("Spec C Signal Pipeline Test")
    print("="*60)

    # Reset: Reload model to clear buffer
    print("\n[0] Reloading model to reset buffer...")
    payload = {"path": "D:\\CiRA Claw\\cira-edge\\test_signal_model"}
    response = requests.post(f"{base_url}/api/model", json=payload, timeout=30)
    assert response.status_code == 200
    assert response.json()['success'] == True
    print(f"   [OK] Model reloaded")

    # Test 1: Check signals endpoint returns 3 channels
    print("\n[1] GET /api/signals - Check channels...")
    response = requests.get(f"{base_url}/api/signals")
    assert response.status_code == 200
    data = response.json()
    assert isinstance(data, list)
    assert len(data) == 3
    channel_names = [ch['name'] for ch in data]
    assert 'ch0' in channel_names
    assert 'ch1' in channel_names
    assert 'ch2' in channel_names
    print(f"   [OK] Found 3 channels: {channel_names}")

    # Test 2: Feed 64 samples to ch0 (not enough for window)
    print("\n[2] POST /api/signal/feed - Feed 64 samples to ch0...")
    samples = generate_sine_wave(250, 1000, 64)
    payload = {"channel": 0, "samples": samples}
    response = requests.post(f"{base_url}/api/signal/feed", json=payload)
    assert response.status_code == 200
    data = response.json()
    assert data['success'] == True
    assert data['fed'] == 64
    assert data['buffer_ready'] == False
    assert data['prediction_ran'] == False
    print(f"   [OK] Fed 64 samples, buffer not ready yet")

    # Test 3: Feed 64 samples to ch1 and ch2
    print("\n[3] POST /api/signal/feed - Feed 64 samples to ch1 and ch2...")
    for channel in [1, 2]:
        payload = {"channel": channel, "samples": samples}
        response = requests.post(f"{base_url}/api/signal/feed", json=payload)
        assert response.status_code == 200
        assert response.json()['buffer_ready'] == False
    print(f"   [OK] Fed ch1 and ch2, buffer still not ready")

    # Test 4: Feed remaining 64 samples to all channels (buffer becomes ready)
    print("\n[4] POST /api/signal/feed - Feed remaining 64 samples (window ready)...")
    for channel in [0, 1, 2]:
        payload = {"channel": channel, "samples": samples}
        response = requests.post(f"{base_url}/api/signal/feed", json=payload)
        assert response.status_code == 200
        data = response.json()
        if channel == 2:
            assert data['buffer_ready'] == True, "Buffer should be ready after 128 samples"
            print(f"   [OK] Buffer is ready (has 128 samples)")

    # Test 4b: Feed stride amount (64 more samples) to trigger first prediction
    print("\n[4b] POST /api/signal/feed - Feed stride (64 samples) to trigger prediction...")
    samples_stride = generate_sine_wave(250, 1000, 64)
    for channel in [0, 1, 2]:
        payload = {"channel": channel, "samples": samples_stride}
        response = requests.post(f"{base_url}/api/signal/feed", json=payload)
        assert response.status_code == 200
        data = response.json()
        print(f"       ch{channel}: fed={data['fed']}, ready={data['buffer_ready']}, pred_ran={data['prediction_ran']}")
        if 'pred_error' in data:
            print(f"       pred_error: {data['pred_error']}")
        if channel == 2:  # Last channel should trigger prediction
            assert data['buffer_ready'] == True, "Buffer should still be ready"
            if not data['prediction_ran']:
                error_msg = f"Prediction should run after stride advance"
                if 'pred_error' in data:
                    error_msg += f" (error: {data['pred_error']})"
                assert False, error_msg
            assert data['result'] is not None, "Should have prediction result"
            print(f"   [OK] Prediction ran after stride advance!")
            print(f"       Result has {len(data['result'])} fields")
            # Check for expected output format (classification with 3 classes)
            if 'probabilities' in data['result']:
                print(f"       Probabilities: {data['result']['probabilities']}")
            elif 'class' in data['result']:
                print(f"       Predicted class: {data['result']['class']}")

    # Test 5: Check signal stats after feeding
    print("\n[5] GET /api/signals - Check stats after feeding...")
    response = requests.get(f"{base_url}/api/signals")
    data = response.json()
    ch0 = next((ch for ch in data if ch['name'] == 'ch0'), None)
    assert ch0 is not None
    assert 'rms' in ch0
    assert 'peak' in ch0
    assert 'mean' in ch0
    assert abs(ch0['rms']) > 0.1  # RMS should be non-zero for sine wave
    print(f"   [OK] Channel stats updated: rms={ch0['rms']:.4f}")

    # Test 6: Sustained feeding
    print("\n[6] POST /api/signal/feed - Sustained feeding (200 more samples)...")
    samples = generate_sine_wave(250, 1000, 200)
    for channel in [0, 1, 2]:
        payload = {"channel": channel, "samples": samples}
        response = requests.post(f"{base_url}/api/signal/feed", json=payload)
        assert response.status_code == 200
        assert response.json()['success'] == True
    print(f"   [OK] Sustained feeding successful")

    # Test 7: Health check
    print("\n[7] GET /health - Runtime still healthy...")
    response = requests.get(f"{base_url}/health")
    assert response.status_code == 200
    data = response.json()
    assert data['status'] == 'ok'
    print(f"   [OK] Runtime healthy")

    print("\n" + "="*60)
    print("[OK] All tests passed!")
    print("="*60)


if __name__ == '__main__':
    try:
        test_spec_c_signal_pipeline()
    except Exception as e:
        print(f"\n[FAIL] Test failed: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
