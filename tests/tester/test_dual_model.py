#!/usr/bin/env python3
"""
Test dual model support - camera (IMAGE slot) + signal (SIGNAL slot)
"""
import requests
import json

base_url = "http://localhost:8080"

print("="*60)
print("Testing Dual Model Support")
print("="*60)

# Step 1: Check current slots status
print("\n[1] Checking current slot status...")
response = requests.get(f"{base_url}/api/slots")
print(f"Current slots: {json.dumps(response.json(), indent=2)}")

# Step 2: Load signal model into SIGNAL slot (IMAGE slot already has NCNN)
print("\n[2] Loading signal model into SIGNAL slot...")
payload = {
    "path": "D:\\CiRA Claw\\cira-edge\\test_signal_model",
    "slot": "signal"
}
response = requests.post(f"{base_url}/api/model", json=payload, timeout=30)
print(f"Response: {response.json()}")
assert response.status_code == 200, f"Failed to load: {response.text}"
assert response.json()['success'] == True, "Load failed"
print("   [OK] Signal model loaded into SIGNAL slot")

# Step 3: Check slots again - should show both models
print("\n[3] Checking slots after loading signal model...")
response = requests.get(f"{base_url}/api/slots")
slots = response.json()
print(f"Slots status:\n{json.dumps(slots, indent=2)}")

# Verify both slots are loaded
image_slot = slots['slots'][0]
signal_slot = slots['slots'][1]

print(f"\n   IMAGE slot: {image_slot['format']} - {'LOADED' if image_slot['loaded'] else 'EMPTY'}")
print(f"   SIGNAL slot: {signal_slot['format']} - {'LOADED' if signal_slot['loaded'] else 'EMPTY'}")

assert image_slot['loaded'], "IMAGE slot should have model"
assert signal_slot['loaded'], "SIGNAL slot should have model"
print("\n   [OK] Both slots are loaded!")

# Step 4: Test signal inference while camera runs
print("\n[4] Testing signal inference (camera still running)...")
samples = [0.1 * i for i in range(64)]  # Simple test data

# Feed samples to trigger inference
for channel in [0, 1, 2]:
    payload = {"channel": channel, "samples": samples}
    response = requests.post(f"{base_url}/api/signal/feed", json=payload)
    if response.status_code != 200:
        print(f"   Error feeding ch{channel}: {response.text}")
        break
    data = response.json()
    print(f"   ch{channel}: fed={data['fed']}, ready={data['buffer_ready']}")

# Feed stride to trigger prediction
print("\n[5] Feeding stride samples to trigger prediction...")
for channel in [0, 1, 2]:
    payload = {"channel": channel, "samples": samples}
    response = requests.post(f"{base_url}/api/signal/feed", json=payload)
    data = response.json()
    print(f"   ch{channel}: pred_ran={data.get('prediction_ran', False)}")

    if 'pred_error' in data:
        print(f"   ERROR: {data['pred_error']}")

    if channel == 2:  # Last channel
        if data.get('prediction_ran'):
            print("\n   [SUCCESS] Signal prediction ran while camera is active!")
            print(f"   Result: {data.get('result')}")
        else:
            print("\n   [FAILED] Signal prediction did not run")
            if 'pred_error' in data:
                print(f"   Error: {data['pred_error']}")

print("\n" + "="*60)
print("Dual Model Test Complete")
print("="*60)
