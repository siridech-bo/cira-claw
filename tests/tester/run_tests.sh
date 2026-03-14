#!/usr/bin/env bash
#
# CiRA CLAW Tester - Linux/Mac Runner
# Runs black-box HTTP tests against runtime
#

set -euo pipefail

# Configuration
RUNTIME="../../runtime/build/test_stream"
MODEL="../../test_signal_model"
PORT=8089

# Check if runtime exists
if [ ! -f "$RUNTIME" ]; then
    echo "ERROR: Runtime not found: $RUNTIME"
    echo ""
    echo "Please build the runtime first:"
    echo "  cd runtime/build"
    echo "  cmake --build ."
    exit 1
fi

# Check if model exists
if [ ! -d "$MODEL" ]; then
    echo "ERROR: Test model not found: $MODEL"
    echo ""
    echo "Please generate test model first:"
    echo "  python tools/make_test_signal_model.py test_signal_model"
    exit 1
fi

# Check if Python dependencies are installed
if ! python3 -c "import requests" 2>/dev/null; then
    echo "Installing Python dependencies..."
    pip3 install -r requirements.txt
fi

# Run tests
echo ""
echo "========================================"
echo "CiRA CLAW HTTP Tester"
echo "========================================"
echo "Runtime: $RUNTIME"
echo "Model:   $MODEL"
echo "Port:    $PORT"
echo "========================================"
echo ""

python3 tester.py --runtime "$RUNTIME" --model "$MODEL" --port $PORT
