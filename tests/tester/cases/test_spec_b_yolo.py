"""
Spec B YOLO Tests - Image inference pipeline (STUB)
TODO: Implement after signal tests are stable
"""

import requests


def test_yolo_model_loaded(base_url):
    """T1: Runtime started with a YOLO model - GET /health returns input_type: image"""
    # NOTE: This test will fail with signal model
    # TODO: Create separate test run with YOLO model
    pass  # Skip for now


def test_results_endpoint_returns_json(base_url):
    """T2: GET /api/results returns valid JSON"""
    response = requests.get(f"{base_url}/api/results")
    assert response.status_code == 200, f"Expected 200, got {response.status_code}"

    data = response.json()
    assert isinstance(data, dict), "Response should be a JSON object"

# TODO: Add more tests:
# - POST /api/inference/image with test image
# - Verify detection bounding boxes
# - Verify confidence scores
# - Test with multiple object classes
