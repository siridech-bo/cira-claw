# CiRA Runtime - Inference Engine

## Build

```bash
cd D:\CiRA Claw\cira-edge\runtime
mkdir build && cd build
cmake .. -DONNXRUNTIME_ROOT="D:/CiRA Claw/onnxruntime-win-x64-1.17.0"
cmake --build .
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `-DCIRA_ENABLE_ONNX=ON` | ON | ONNX Runtime backend |
| `-DCIRA_ENABLE_NCNN=OFF` | OFF | NCNN backend |
| `-DCIRA_ENABLE_STREAMING=ON` | ON | HTTP streaming server |
| `-DCIRA_ENABLE_OPENCV=ON` | ON | Camera capture |

## Run

```bash
cd D:\CiRA Claw\cira-edge\runtime\build

# Option 1: Start with a specific model
./test_stream.exe "D:/CiRA Claw/models/yolov4-tiny" -p 8080

# Option 2: Start with models directory (dropdown in dashboard)
./test_stream.exe -m "D:/CiRA Claw/models" -p 8080

# Option 3: Start without model (load later via API)
./test_stream.exe -p 8080

# Example with test model
./test_stream.exe "../test_model/yolov4-tiny" -p 8080
```

### CLI Arguments

| Argument | Description |
|----------|-------------|
| `[model_path]` | Path to model directory (contains `.bin`, `.param`, `labels.txt`) |
| `-p, --port` | HTTP server port (default: 8080) |
| `-m, --models-dir` | Directory with multiple models for API listing |
| `-h, --help` | Show help message |

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/health` | GET | Health check |
| `/api/results` | GET | Current detection results (JSON) |
| `/api/models` | GET | List available models (from `-m` dir) |
| `/api/model` | POST | Switch model at runtime |
| `/snapshot` | GET | Camera snapshot (JPEG) |
| `/stream/annotated` | GET | MJPEG stream with bounding boxes |
| `/stream/raw` | GET | MJPEG stream without annotations |

## Model Directory Structure

```
model/
├── model.bin        # NCNN weights
├── model.param      # NCNN architecture
├── labels.txt       # Class names (one per line)
└── cira_model.json  # Optional: model config
```

### cira_model.json Example

```json
{
    "name": "YOLOv4-tiny Defect Detection",
    "yolo_version": "yolov4",
    "input_size": 416,
    "num_classes": 3,
    "class_names": ["scratch", "dent", "crack"],
    "confidence_threshold": 0.5,
    "nms_threshold": 0.4
}
```

Supported `yolo_version`: `auto`, `yolov3`, `yolov4`, `yolov5`, `yolov7`, `yolov8`, `yolov9`, `yolov10`, `yolov11`

## Test Executables

| Executable | Description |
|------------|-------------|
| `test_stream.exe` | Main inference server with HTTP API |
| `test_onnx.exe` | ONNX Runtime inference test |
| `test_ncnn.exe` | NCNN inference test |

## Integration with cira-edge

The runtime listens on port **8080** by default. The cira-edge gateway (Node.js) proxies requests to this port via Vite config or direct API calls.

```
Browser → Vite (3000) → cira-edge (18790) → cira-runtime (8080)
                              ↓
                         /chat WebSocket
                         Rule Engine
                         Stats Collector
```
