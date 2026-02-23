# CiRA CLAW

**AI-powered edge inference gateway for factory environments**

CiRA CLAW is an OpenClaw-inspired headless gateway daemon that manages AI inference on edge devices (NVIDIA Jetson, Raspberry Pi, Windows PCs) in factory environments. It combines a powerful inference runtime, real-time video streaming, AI-powered automation rules, and a modern web dashboard.

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                        CiRA CLAW System                         │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────┐ │
│  │  Dashboard  │  │   Gateway   │  │      AI Agent           │ │
│  │  (Vue.js)   │◄─┤  (Node.js)  │◄─┤  (Claude API)           │ │
│  │             │  │  Port 18790 │  │  + Tool Use + Rules     │ │
│  └─────────────┘  └──────┬──────┘  └─────────────────────────┘ │
│                          │                                      │
│         ┌────────────────┼────────────────┐                     │
│         ▼                ▼                ▼                     │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐             │
│  │  LINE Bot   │  │    MQTT     │  │   Modbus    │             │
│  │  Channel    │  │   Broker    │  │   Server    │             │
│  └─────────────┘  └─────────────┘  └─────────────┘             │
│                                                                 │
├─────────────────────────────────────────────────────────────────┤
│                     Edge Devices (Nodes)                        │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│  ┌───────────────────────────────────────────────────────────┐ │
│  │                    CiRA Runtime (libcira)                  │ │
│  │  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────────────┐  │ │
│  │  │  NCNN   │ │ Darknet │ │  ONNX   │ │   TensorRT      │  │ │
│  │  │ Loader  │ │ Loader  │ │ Loader  │ │   Loader        │  │ │
│  │  └────┬────┘ └────┬────┘ └────┬────┘ └───────┬─────────┘  │ │
│  │       └───────────┴───────────┴──────────────┘            │ │
│  │                       ▼                                    │ │
│  │  ┌─────────────┐ ┌─────────────┐ ┌─────────────────────┐  │ │
│  │  │   Camera    │ │   YOLO      │ │   MJPEG Stream      │  │ │
│  │  │   Capture   │─┤   Decoder   │─┤   + Annotations     │  │ │
│  │  └─────────────┘ └─────────────┘ └─────────────────────┘  │ │
│  └───────────────────────────────────────────────────────────┘ │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

## Features

### Gateway & Dashboard
- **Device Management**: Monitor and manage edge devices via mDNS discovery and SSH
- **Real-time Dashboard**: Vue.js web interface with live camera feeds, metrics, and chat
- **Multi-Camera Grid**: View multiple camera streams simultaneously (2x2, 3x3 layouts)
- **Video Streaming**: MJPEG streams with auto-fallback to polling mode, detection overlays
- **Model Management**: Deploy, switch, and manage AI models on devices remotely
- **Model Conversion**: Convert Darknet models to NCNN format for optimized inference

### AI Agent & Automation
- **Claude-Powered Agent**: Natural language interface for device queries and commands
- **Tool Use**: Extensible tool system for device control, inference, and automation
- **JavaScript Rule Engine**: Create automation rules with visual flow diagrams
- **AI-Assisted Rule Editing**: Modify rules via natural language with Quick Edit

### Communication Channels
- **Web Chat**: Real-time chat interface in dashboard
- **LINE Bot**: Message integration with LINE Messaging API
- **MQTT**: Publish/subscribe messaging for IoT integration
- **Modbus TCP**: Industrial protocol server for PLC communication
- **REST API**: Full HTTP API for system integration

### CiRA Runtime (libcira)
- **Multi-Format Support**: NCNN, Darknet, ONNX, TensorRT model loaders
- **YOLO Detection**: YOLOv3, YOLOv4, YOLOv5, YOLOv8 with NMS
- **Video Streaming**: Built-in HTTP server with MJPEG and frame polling
- **Annotation Overlay**: Bounding boxes, labels, confidence scores, FPS counter
- **Detection Persistence**: Smooth annotations by holding detections across frames

## Quick Start

### Installation

```bash
# Clone or download the project
cd cira-claw

# Install dependencies
npm install

# Initialize configuration
npm run cli -- onboard
```

### Running the Gateway

```bash
# Development mode (with hot reload)
npm run dev

# Production build
npm run build
npm start
```

The gateway starts on port **18790** by default.

### Dashboard

Open http://localhost:18790 in your browser.

**Dashboard Pages:**
- **Overview**: Device status cards, system health metrics
- **Device Detail**: Live streams (annotated + raw), model management, device info
- **Camera Grid**: Multi-camera view with 2x2 or 3x3 layout
- **Image Tester**: Upload images for inference testing
- **Rules**: JavaScript automation rules with Mermaid flow diagrams
- **Chat**: AI agent conversation interface
- **Model Conversion**: Darknet to NCNN converter

## Configuration

### Directory Structure

```
~/.cira/
├── cira.json           # Main configuration
├── workspace/
│   ├── AGENTS.md       # AI agent personality
│   ├── TOOLS.md        # Tool documentation
│   └── skills/         # Skill definitions
├── nodes/              # Device configurations
├── rules/              # JavaScript automation rules
├── credentials/        # API keys and SSH keys
└── logs/               # Log files
```

### Main Configuration (`cira.json`)

```json
{
  "gateway": {
    "port": 18790,
    "host": "0.0.0.0",
    "name": "Factory-A Gateway"
  },
  "agent": {
    "provider": "claude",
    "model": "claude-sonnet-4-20250514",
    "workspace": "~/.cira/workspace"
  },
  "channels": {
    "line": { "enabled": true },
    "mqtt": { "enabled": true, "broker": "mqtt://localhost:1883" },
    "webchat": { "enabled": true }
  },
  "modbus": {
    "enabled": true,
    "port": 502
  }
}
```

### Node Configuration

Nodes are stored in `~/.cira/nodes/<node-id>.json`:

```json
{
  "id": "jetson-line1",
  "name": "Production Line 1",
  "type": "jetson-nano",
  "host": "192.168.1.101",
  "ssh": { "user": "cira", "port": 22 },
  "runtime": { "port": 8080 },
  "models": [{ "name": "scratch_v3", "task": "detection" }]
}
```

## CLI Usage

```bash
# List all nodes
cira node list

# Check node status
cira node status jetson-line1

# Add a new node
cira node add 192.168.1.101 --name "Line 1" --type jetson-nano

# Query the AI agent
cira agent query "What's the status of all devices?"

# Interactive chat
cira agent interactive

# Service management
cira service start|stop|restart|status|logs
```

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/status` | GET | Gateway status |
| `/api/nodes` | GET | List all nodes |
| `/api/nodes/:id` | GET | Get node details |
| `/api/nodes/:id/status` | GET | Check node health |
| `/api/nodes/:id/models` | GET | List available models |
| `/api/nodes/:id/model` | POST | Switch active model |
| `/api/nodes/:id/snapshot` | GET | Get camera snapshot |
| `/api/rules` | GET/POST | Automation rules CRUD |
| `/api/rules/:id` | GET/PUT/DELETE | Individual rule operations |
| `/ws` | WS | Real-time WebSocket |
| `/chat` | WS | Agent chat WebSocket |

## Rule Engine

Create JavaScript automation rules that respond to detection events:

```javascript
// Example: Alert when defect count exceeds threshold
function evaluate(ctx) {
  const defects = ctx.detections.filter(d => d.label === 'scratch');
  if (defects.length >= ctx.params.threshold) {
    return {
      triggered: true,
      action: 'alert',
      data: { count: defects.length, severity: 'high' }
    };
  }
  return { triggered: false };
}
```

Rules support:
- **Parameters**: Configurable thresholds and options
- **Actions**: Alerts, MQTT publish, LINE notifications, Modbus writes
- **Mermaid Diagrams**: Auto-generated visual flow charts
- **AI Quick Edit**: Modify rules using natural language

## CiRA Runtime

The runtime is a C/C++ library for inference on edge devices.

### Supported Model Formats

| Format | Extension | Backend | Use Case |
|--------|-----------|---------|----------|
| NCNN | `.param` + `.bin` | NCNN | CPU inference (ARM/x86) |
| Darknet | `.cfg` + `.weights` | Darknet | Original YOLO format |
| ONNX | `.onnx` | ONNX Runtime | Cross-platform |
| TensorRT | `.engine` | TensorRT | NVIDIA GPU optimized |

### Building the Runtime

```bash
cd runtime

# Linux/macOS
mkdir build && cd build
cmake .. -DCIRA_OPENCV_ENABLED=ON -DCIRA_NCNN_ENABLED=ON
make

# Windows (MinGW)
mkdir build && cd build
cmake .. -G "MinGW Makefiles" -DCIRA_OPENCV_ENABLED=ON
mingw32-make
```

### Model Conversion

Convert Darknet models to NCNN for optimized edge inference:

1. Use the **Model Conversion** page in the dashboard
2. Or use `darknet2ncnn` CLI tool

Requirements:
- `darknet2ncnn` executable (from [darknet2ncnn repo](https://github.com/xiangweizeng/darknet2ncnn))
- Model directory with `.cfg`, `.weights`, and `labels.txt`

## Video Streaming

The runtime provides HTTP endpoints for video streaming:

| Endpoint | Description |
|----------|-------------|
| `/stream/raw` | Raw MJPEG stream |
| `/stream/annotated` | MJPEG with detection overlays |
| `/frame/latest` | Single frame (polling mode) |
| `/api/detections` | JSON detection results |
| `/api/stats` | Inference statistics |

### Stream Features
- **MJPEG**: Continuous stream with automatic reconnection
- **Polling Mode**: File-based fallback for cross-platform compatibility
- **Auto Mode**: Starts with MJPEG, falls back to polling on errors
- **Watchdog**: Detects stalled streams and auto-reconnects
- **Detection Persistence**: Holds annotations for 3 frames to reduce flickering

## Development

### Project Structure

```
cira-claw/
├── src/                    # Gateway source (TypeScript)
│   ├── index.ts            # Entry point
│   ├── gateway/            # HTTP server, WebSocket, routes
│   ├── agent/              # AI agent, tools, prompts
│   ├── channels/           # LINE, MQTT, WebChat
│   ├── services/           # Rule engine, Modbus, stats
│   ├── nodes/              # Device management
│   └── utils/              # Logger, helpers
├── dashboard/              # Vue.js web dashboard
│   └── src/
│       ├── pages/          # Overview, DeviceDetail, Rules, etc.
│       └── components/     # CameraStream, NodeCard
├── runtime/                # CiRA Runtime (C/C++)
│   ├── include/            # Public headers
│   └── src/                # Implementation
│       ├── cira.c          # Core API
│       ├── ncnn_loader.cpp # NCNN backend
│       ├── darknet_loader.c
│       ├── onnx_loader.c
│       ├── camera.cpp      # OpenCV camera
│       ├── stream_server.c # HTTP streaming
│       ├── jpeg_encoder.cpp # Annotation rendering
│       └── yolo_decoder.c  # YOLO post-processing
├── workspace/              # Default workspace template
└── cli/                    # CLI tool
```

### Building the Dashboard

```bash
cd dashboard
npm install
npm run build   # Production build
npm run dev     # Development with hot reload
```

### Running Tests

```bash
npm test
```

## License

MIT License - CiRA Robotics / KMITL 2026

## Support

- Issues: https://github.com/siridech-bo/cira-claw/issues
- Documentation: https://docs.cira.io/claw
