# CiRA CLAW

**AI-powered edge inference gateway for factory environments**

CiRA CLAW is an OpenClaw-inspired headless gateway daemon that manages AI inference on edge devices (NVIDIA Jetson, Raspberry Pi) in factory environments.

## Features

- **Device Management**: Monitor and manage Jetson/RPi edge devices via mDNS discovery and SSH
- **AI Agent**: Natural language interface powered by Claude API for device queries and commands
- **Web Dashboard**: Real-time device overview, camera feeds, and chat interface
- **Multiple Channels**: LINE Bot, Telegram, MQTT, WebSocket, and REST API
- **Video Streaming**: MJPEG streams from edge devices with annotation overlays
- **Model Management**: Deploy and update AI models to edge devices remotely

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

### Configuration

The gateway uses `~/.cira/` as its configuration directory:

```
~/.cira/
├── cira.json           # Main configuration
├── workspace/
│   ├── AGENTS.md       # AI agent personality
│   ├── TOOLS.md        # Tool documentation
│   └── skills/         # Skill definitions
├── nodes/              # Device configurations
├── credentials/        # API keys and SSH keys
└── logs/               # Log files
```

### Running the Gateway

```bash
# Development mode
npm run dev

# Production build
npm run build
npm start
```

The gateway will start on port 18790 by default.

### Accessing the Dashboard

Open http://localhost:18790 in your browser.

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

# Check service status
cira service status
```

## Daemon / Service Management

```bash
# Install as systemd service (Linux)
sudo ./scripts/install.sh

# Or via CLI
cira service install

# Service commands
cira service start
cira service stop
cira service restart
cira service reload    # Reload config (SIGHUP)
cira service status
cira service logs -f   # Follow logs
```

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/status` | GET | Gateway status |
| `/api/nodes` | GET | List all nodes |
| `/api/nodes/:id` | GET | Get node details |
| `/api/nodes/:id/status` | GET | Check node health |
| `/api/nodes/:id/snapshot` | GET | Get camera snapshot |
| `/api/nodes/:id/stream` | GET | Get stream URLs |
| `/ws` | WS | Real-time WebSocket |
| `/chat` | WS | Agent chat WebSocket |

## Configuration

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

## AI Agent

The AI agent can be queried through multiple channels:

- **CLI**: `cira agent query "status of line 1"`
- **Web Chat**: Dashboard chat interface
- **LINE Bot**: Message your LINE bot
- **MQTT**: Publish to `cira/command/query`

### Available Tools

| Tool | Description |
|------|-------------|
| `node_list` | List all devices |
| `node_query` | Get device details |
| `node_status` | Quick health check |
| `camera_snapshot` | Capture camera image |
| `inference_stats` | Get detection statistics |
| `model_list` | List available models |
| `model_deploy` | Deploy model to device |

## Channel Integration

### LINE Bot

1. Create a LINE Messaging API channel
2. Add credentials to `~/.cira/credentials/line.json`
3. Set webhook URL to `https://your-domain/webhook/line`

### MQTT

The gateway subscribes to `cira/command/#` and publishes to `cira/results/#`.

Example command:
```json
// Publish to cira/command/status
{ "action": "status" }

// Result on cira/results/status
{ "nodes": [...], "summary": {...} }
```

## Development

### Project Structure

```
cira-claw/
├── src/
│   ├── index.ts           # Entry point
│   ├── config.ts          # Configuration loader
│   ├── gateway/           # HTTP server
│   ├── agent/             # AI agent
│   ├── channels/          # LINE, MQTT, WebChat
│   ├── nodes/             # Device management
│   └── utils/             # Utilities
├── dashboard/             # Vue.js web dashboard
├── cli/                   # CLI tool
├── workspace/             # Default workspace template
└── runtime/               # CiRA Runtime (C library)
```

### Building the Dashboard

```bash
cd dashboard
npm install
npm run build
```

### Running Tests

```bash
npm test
```

## CiRA Runtime

CiRA Runtime (`libcira`) is a lightweight C library for inference on edge devices. It supports:

- Darknet YOLO models (`.weights` + `.cfg`)
- ONNX models
- TensorRT engines
- Scikit-learn models

See `runtime/` directory for the C implementation.

## License

MIT License - CiRA Robotics / KMITL

## Support

- Issues: https://github.com/siridech-bo/cira-claw/issues
- Documentation: https://docs.cira.io/claw
