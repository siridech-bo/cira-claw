"""
Basic health check tests - verify runtime is responding
"""

import requests


def test_health_returns_200(base_url):
    """T1: GET /health returns HTTP 200"""
    response = requests.get(f"{base_url}/health")
    assert response.status_code == 200, f"Expected 200, got {response.status_code}"


def test_health_returns_json(base_url):
    """T2: response is valid JSON"""
    response = requests.get(f"{base_url}/health")
    data = response.json()  # Will raise if not valid JSON
    assert isinstance(data, dict), "Response should be a JSON object"


def test_health_has_status_field(base_url):
    """T3: response contains 'status' field"""
    response = requests.get(f"{base_url}/health")
    data = response.json()
    assert "status" in data, "Response missing 'status' field"


def test_stats_returns_200(base_url):
    """T4: GET /api/stats returns HTTP 200"""
    response = requests.get(f"{base_url}/api/stats")
    assert response.status_code == 200, f"Expected 200, got {response.status_code}"


def test_signals_returns_200(base_url):
    """T5: GET /api/signals returns HTTP 200"""
    response = requests.get(f"{base_url}/api/signals")
    assert response.status_code == 200, f"Expected 200, got {response.status_code}"
