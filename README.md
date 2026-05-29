# CiRA CLAW

**AI-powered edge inference gateway for factory environments**

CiRA CLAW is an OpenClaw-inspired headless gateway daemon that manages AI inference on edge devices (NVIDIA Jetson, Raspberry Pi, Windows PCs) in factory environments. It combines a powerful inference runtime, real-time video streaming with WebRTC support, AI-powered automation rules, signal protocol bridges, and a modern web dashboard.

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                          CiRA CLAW System                                │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  ┌─────────────┐  ┌─────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │  Dashboard  │  │   Gateway   │  │   AI Agent   │  │   go2rtc     │  │
│  │  (Vue.js)   │◄─┤  (Node.js)  │◄─┤  (Claude)    │  │   (WebRTC)   │  │
│  │             │  │  Port 18790 │  │  + Tools     │  │   Port 1984  │  │
│  └─────────────┘  └──────┬──────┘  └──────────────┘  └──────────────┘  │
│                          │                                               │
│    ┌─────────────────────┼─────────────────────┐                        │
│    ▼                     ▼                     ▼                        │
│  ┌───────────────┐  ┌───────────┐  ┌───────────────────┐               │
│  │ Signal Bridge │  │   MQTT    │  │     Modbus        │               │
│  │ (Protocol     │  │  Broker   │  │     TCP Server    │               │
│  │  Translator)  │  │           │  │     Port 502      │               │
│  └───────────────┘  └───────────┘  └───────────────────┘               │
│                                                                          │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                    Communication Channels                         │   │
│  │  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────────┐ │   │
│  │  │ LINE Bot │  │ Web Chat │  │   MQTT   │  │ REST API + WS    │ │   │
│  │  └──────────┘  └──────────┘  └──────────┘  └──────────────────┘ │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                                                                          │
├─────────────────────────────────────────────────────────────────────────┤
│                       Edge Devices (Nodes)                               │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│  ┌───────────────────────────────────────────────────────────────────┐ │
│  │                      CiRA Runtime (libcira)                        │ │
│  │  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────────────┐          │ │
│  │  │  NCNN   │ │ Darknet │ │  ONNX   │ │   TensorRT      │          │ │
│  │  │ Loader  │ │ Loader  │ │ Loader  │ │   Loader        │          │ │
│  │  └────┬────┘ └────┬────┘ └────┬────┘ └───────┬─────────┘          │ │
│  │       └───────────┴───────────┴──────────────┘                    │ │
│  │                           ▼                                        │ │
│  │  ┌─────────────┐ ┌─────────────┐ ┌─────────────────────────────┐  │ │
│  │  │   Camera    │ │   YOLO      │ │  MJPEG Stream + WebRTC      │  │ │
│  │  │   Capture   │─┤   Decoder   │─┤  + Annotations              │  │ │
│  │  └─────────────┘ └─────────────┘ └─────────────────────────────┘  │ │
│  └───────────────────────────────────────────────────────────────────┘ │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

## Features

### Gateway & Dashboard
- **Device Management**: Monitor and manage edge devices via mDNS discovery and SSH
- **Real-time Dashboard**: Vue.js web interface with live camera feeds, metrics, and chat
- **Multi-Camera Grid**: View multiple camera streams simultaneously (2x2, 3x3 layouts)
- **WebRTC Streaming**: Low-latency video via go2rtc with automatic MJPEG fallback
- **Model Management**: Deploy, switch, and manage AI models on devices remotely
- **Model Conversion**: Convert Darknet models to NCNN format for optimized inference
- **Browser Memory Management**: Scheduled page reloads to prevent OOM in long-running sessions

### AI Agent & Automation
- **Claude-Powered Agent**: Natural language interface for device queries and commands
- **Tool Use**: Extensible tool system for device control, inference, and automation
- **JavaScript Rule Engine**: Create automation rules with visual flow diagrams
- **AI-Assisted Rule Editing**: Modify rules via natural language with Quick Edit
- **Rule Execution Monitor**: Real-time visualization of rule triggers and actions

### Signal Protocol Bridges
- **Signal Dashboard**: Monitor real-time signals from all nodes with filtering and history
- **MQTT Bridge**: Publish detection signals to MQTT topics for IoT integration
- **Modbus TCP Server**: Expose detection counts and signals via Modbus registers (Port 502)
- **State Store**: SQLite-backed persistent storage for signal history and analytics
- **Composite Rule Engine**: React to signals from multiple sources with complex conditions

### Communication Channels
- **Web Chat**: Real-time chat interface in dashboard
- **LINE Bot**: Message integration with LINE Messaging API
- **MQTT**: Publish/subscribe messaging for IoT integration
- **Modbus TCP**: Industrial protocol server for PLC communication
- **REST API**: Full HTTP API for system integration
- **WebSocket**: Real-time events, detection streams, and page reload coordination

### CiRA Runtime (libcira)
- **Multi-Format Support**: NCNN, Darknet, ONNX, TensorRT model loaders
- **YOLO Detection**: YOLOv3, YOLOv4, YOLOv5, YOLOv8, YOLOv11 with NMS
- **Video Streaming**: Built-in HTTP server with MJPEG and frame polling
- **Annotation Overlay**: Bounding boxes, labels, confidence scores, FPS counter
- **Detection Persistence**: Time-based annotation holding to reduce flickering
- **Double-Buffered Inference**: Separate capture and inference threads for smooth streaming

## Quick Start

### Installation

```bash
# Clone or download the project
git clone https://github.com/siridech-bo/cira-claw.git
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

# Windows: Use the batch files
start-system.bat   # Start gateway + go2rtc
stop-system.bat    # Stop all services
```

The gateway starts on port **18790** by default.

### Dashboard

Open http://localhost:18790 in your browser.

**Dashboard Pages:**
- **Overview**: Device status cards, system health metrics
- **Device Detail**: Live streams (annotated + raw), model management, device info
- **Camera Grid**: Multi-camera view with 2x2 or 3x3 layout, WebRTC support
- **Camera Manager**: Manage camera sources and stream settings
- **Signal Monitor**: Real-time signal dashboard with filtering and history
- **Image Tester**: Upload images for inference testing
- **Rules**: JavaScript automation rules with Mermaid flow diagrams
- **Rule Graph**: Visual rule execution flow and debugging
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
  },
  "go2rtc": {
    "enabled": true,
    "apiPort": 1984,
    "webrtcPort": 8555
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

## Services

### Core Services
| Service | Description |
|---------|-------------|
| `Gateway` | Fastify HTTP server, WebSocket handler, REST API |
| `NodeManager` | Device registration, health checks, SSH management |
| `AIAgent` | Claude-powered conversational agent with tools |
| `Go2RTCService` | WebRTC streaming server for low-latency video |

### Signal & Automation
| Service | Description |
|---------|-------------|
| `SignalBridge` | Protocol translator for MQTT/Modbus signals |
| `RuleEngine` | JavaScript sandbox for automation rules |
| `CompositeRuleEngine` | Multi-source signal aggregation |
| `ActionRunner` | Execute rule actions (alerts, MQTT, LINE, Modbus) |
| `StateStore` | SQLite persistence for signals and state |
| `StatsCollector` | Metrics aggregation and reporting |

### Communication
| Service | Description |
|---------|-------------|
| `LINEChannel` | LINE Messaging API bot integration |
| `MQTTChannel` | MQTT publish/subscribe messaging |
| `WebChatChannel` | Browser-based chat interface |
| `ModbusServer` | Modbus TCP server for PLC integration |

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
| `/api/nodes/:id/stream` | GET | Get stream URLs (MJPEG + WebRTC) |
| `/api/rules` | GET/POST | Automation rules CRUD |
| `/api/rules/:id` | GET/PUT/DELETE | Individual rule operations |
| `/api/signals` | GET | Query signal history |
| `/ws` | WS | Real-time WebSocket |
| `/chat` | WS | Agent chat WebSocket |

## Video Streaming

### Stream Types

| Type | Protocol | Latency | Use Case |
|------|----------|---------|----------|
| WebRTC | go2rtc | ~100ms | Real-time monitoring |
| MJPEG | HTTP | ~500ms | Cross-platform compatibility |
| Polling | HTTP | ~1s | Fallback mode |

### Stream Endpoints (per node)

| Endpoint | Description |
|----------|-------------|
| `/stream/raw` | Raw MJPEG stream |
| `/stream/annotated` | MJPEG with detection overlays |
| `/frame/latest` | Single frame (polling mode) |
| `/api/detections` | JSON detection results |
| `/api/stats` | Inference statistics |

### Stream Features
- **WebRTC via go2rtc**: Ultra-low latency streaming with STUN/TURN support
- **MJPEG**: Continuous stream with automatic reconnection
- **Polling Mode**: File-based fallback for cross-platform compatibility
- **Auto Mode**: Starts with WebRTC, falls back to MJPEG, then polling
- **Watchdog**: Detects stalled streams and auto-reconnects
- **Detection Persistence**: Holds annotations across frames to reduce flickering

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
- **Signal Integration**: React to signals from MQTT, Modbus, and detection events
- **Real-time Monitor**: Live execution visualization with performance metrics

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

## Development

### Project Structure

```
cira-claw/
├── src/                    # Gateway source (TypeScript)
│   ├── index.ts            # Entry point
│   ├── gateway/            # HTTP server, WebSocket, routes
│   ├── agent/              # AI agent, tools, prompts
│   ├── channels/           # LINE, MQTT, WebChat
│   ├── services/           # Rule engine, Modbus, signals, go2rtc
│   │   ├── go2rtc-service.ts      # WebRTC streaming
│   │   ├── signal-bridge.ts       # Protocol bridges
│   │   ├── composite-rule-engine.ts
│   │   ├── state-store.ts         # SQLite persistence
│   │   └── ...
│   ├── nodes/              # Device management
│   └── utils/              # Logger, helpers
├── dashboard/              # Vue.js web dashboard
│   └── src/
│       ├── pages/          # Overview, DeviceDetail, SignalMonitor, etc.
│       └── components/     # CameraStream, NodeCard
├── bin/                    # go2rtc binaries (Windows, Linux, macOS)
├── runtime/                # CiRA Runtime (C/C++)
│   ├── include/            # Public headers
│   └── src/                # Implementation
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

## Recent Updates

- **WebRTC Streaming**: Low-latency video via go2rtc integration
- **Signal Protocol Bridges**: MQTT and Modbus signal translation
- **Signal Dashboard**: Real-time signal monitoring and filtering
- **Rule Engine Signals**: React to signals from multiple protocol sources
- **Browser Memory Management**: Scheduled page reloads prevent OOM crashes
- **Detection Persistence**: Time-based annotation holding reduces flickering
- **Double-Buffered Inference**: Smoother video with separate capture/inference threads

## License

MIT License - CiRA Robotics / KMITL 2026

## Support

- Issues: https://github.com/siridech-bo/cira-claw/issues
- Documentation: https://docs.cira.io/claw
