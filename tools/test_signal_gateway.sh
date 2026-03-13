#!/usr/bin/env bash
#
# Gateway Signal Endpoint Smoke Test (Spec C v4)
#
# Prerequisites:
#   1. Build runtime with signal support:
#      cd runtime/build && cmake -DCIRA_ENABLE_ONNX=ON .. && cmake --build .
#
#   2. Generate test model:
#      python tools/make_test_signal_model.py test_signal_model
#
#   3. Start gateway server:
#      cd cira-edge && npm run dev
#      (or NODE_ENV=production npm start)
#
# This script tests:
#   - /api/signals endpoint returns channel stats
#   - /api/signal-prediction endpoint returns prediction result
#   - Non-blocking fetch behavior (no 500 errors if runtime not loaded)
#

set -euo pipefail

# Configuration
GATEWAY_URL="${GATEWAY_URL:-http://localhost:3000}"
TIMEOUT=5

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "=== CiRA Signal Gateway Smoke Test ==="
echo ""

# Helper functions
pass() {
    echo -e "${GREEN}[PASS]${NC} $1"
}

fail() {
    echo -e "${RED}[FAIL]${NC} $1"
    exit 1
}

warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

info() {
    echo "  $1"
}

# Check prerequisites
if ! command -v curl &> /dev/null; then
    fail "curl is not installed. Install it first."
fi

if ! command -v jq &> /dev/null; then
    warn "jq is not installed. JSON validation will be skipped."
    warn "  Install: sudo apt install jq (Linux) or brew install jq (macOS)"
    HAS_JQ=false
else
    HAS_JQ=true
fi

# Test 1: Gateway health check
echo "Test 1: Gateway Health Check"
HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" --max-time $TIMEOUT "$GATEWAY_URL/api/stats" || echo "000")

if [ "$HTTP_CODE" = "200" ]; then
    pass "Gateway is reachable at $GATEWAY_URL"
elif [ "$HTTP_CODE" = "000" ]; then
    fail "Cannot connect to gateway at $GATEWAY_URL (connection refused or timeout)"
else
    fail "Gateway returned HTTP $HTTP_CODE (expected 200)"
fi

# Test 2: /api/signals endpoint
echo ""
echo "Test 2: Signal Stats Endpoint"
RESPONSE=$(curl -s --max-time $TIMEOUT "$GATEWAY_URL/api/signals" || echo "ERROR")

if [ "$RESPONSE" = "ERROR" ]; then
    fail "/api/signals request failed (timeout or connection error)"
fi

HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" --max-time $TIMEOUT "$GATEWAY_URL/api/signals")

if [ "$HTTP_CODE" != "200" ]; then
    fail "/api/signals returned HTTP $HTTP_CODE (expected 200)"
fi

# Check if response is valid JSON
if [ "$HAS_JQ" = true ]; then
    if echo "$RESPONSE" | jq empty 2>/dev/null; then
        pass "/api/signals returned valid JSON"
        info "Response preview:"
        echo "$RESPONSE" | jq '.' | head -20
    else
        fail "/api/signals returned invalid JSON"
    fi
else
    pass "/api/signals returned response (JSON validation skipped)"
    info "Response preview:"
    echo "$RESPONSE" | head -10
fi

# Test 3: Check response structure
if [ "$HAS_JQ" = true ]; then
    echo ""
    echo "Test 3: Response Structure Validation"

    # Check if it's an array
    if echo "$RESPONSE" | jq -e '. | type == "array"' > /dev/null 2>&1; then
        pass "Response is an array"

        # Check array length
        CHANNEL_COUNT=$(echo "$RESPONSE" | jq '. | length')
        info "Found $CHANNEL_COUNT channel(s)"

        # If channels exist, check first channel structure
        if [ "$CHANNEL_COUNT" -gt 0 ]; then
            FIRST_CHANNEL=$(echo "$RESPONSE" | jq '.[0]')

            # Check required fields
            if echo "$FIRST_CHANNEL" | jq -e '.name' > /dev/null 2>&1 && \
               echo "$FIRST_CHANNEL" | jq -e '.rms' > /dev/null 2>&1 && \
               echo "$FIRST_CHANNEL" | jq -e '.peak' > /dev/null 2>&1 && \
               echo "$FIRST_CHANNEL" | jq -e '.mean' > /dev/null 2>&1; then
                pass "Channel has required fields (name, rms, peak, mean)"
            else
                warn "Channel missing required fields"
            fi
        else
            warn "No signal channels found (runtime may not have signal model loaded)"
            info "This is expected if no signal model is loaded"
        fi
    else
        fail "Response is not an array"
    fi
fi

# Test 4: /api/signal-prediction endpoint (optional - may not exist if no prediction)
echo ""
echo "Test 4: Signal Prediction Endpoint (Optional)"
PRED_RESPONSE=$(curl -s --max-time $TIMEOUT "$GATEWAY_URL/api/signal-prediction" || echo "ERROR")

if [ "$PRED_RESPONSE" = "ERROR" ]; then
    warn "/api/signal-prediction request failed (may not be implemented yet)"
else
    HTTP_CODE=$(curl -s -o /dev/null -w "%{http_code}" --max-time $TIMEOUT "$GATEWAY_URL/api/signal-prediction")

    if [ "$HTTP_CODE" = "200" ]; then
        pass "/api/signal-prediction returned HTTP 200"

        if [ "$HAS_JQ" = true ]; then
            if echo "$PRED_RESPONSE" | jq empty 2>/dev/null; then
                pass "Prediction response is valid JSON"
                info "Prediction preview:"
                echo "$PRED_RESPONSE" | jq '.'
            else
                warn "Prediction response is not valid JSON"
            fi
        fi
    elif [ "$HTTP_CODE" = "404" ]; then
        warn "/api/signal-prediction not found (endpoint may not be implemented)"
    else
        warn "/api/signal-prediction returned HTTP $HTTP_CODE"
    fi
fi

# Test 5: Non-blocking behavior (rapid successive calls should not hang)
echo ""
echo "Test 5: Non-Blocking Fetch Behavior"
START_TIME=$(date +%s)
for i in {1..5}; do
    curl -s --max-time $TIMEOUT "$GATEWAY_URL/api/signals" > /dev/null
done
END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))

if [ $ELAPSED -lt 10 ]; then
    pass "5 successive calls completed in ${ELAPSED}s (non-blocking confirmed)"
else
    warn "5 successive calls took ${ELAPSED}s (may indicate blocking behavior)"
fi

# Summary
echo ""
echo "=== Summary ==="
echo -e "${GREEN}All critical tests passed!${NC}"
echo ""
echo "Next steps:"
echo "  1. Feed live signal data to the runtime"
echo "  2. Verify /api/signals shows updated RMS/peak/mean values"
echo "  3. Verify signal prediction updates when window is ready"
echo "  4. Check rule-engine payload.signals integration"
