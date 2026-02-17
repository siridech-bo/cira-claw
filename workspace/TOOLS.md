# CiRA Edge Agent Tools

This document describes all tools available to the CiRA Edge Agent.

## NODE MANAGEMENT

### node.list
List all registered devices with their current status.

**Parameters:** None

**Returns:**
```json
{
  "nodes": [
    {
      "id": "jetson-line1",
      "name": "Production Line 1",
      "type": "jetson-nano",
      "host": "192.168.1.101",
      "status": "online",
      "fps": 28,
      "temperature": 65
    }
  ],
  "summary": {
    "total": 4,
    "online": 3,
    "offline": 1
  }
}
```

### node.query(id)
Get detailed status of a specific node.

**Parameters:**
- `id` (string): Node identifier

**Returns:** Full node status including metrics, inference stats, and configuration.

### node.status(id)
Quick health check of a node. Returns only essential metrics.

**Parameters:**
- `id` (string): Node identifier

**Returns:**
```json
{
  "status": "online",
  "fps": 28,
  "temperature": 65,
  "uptime": "47d 3h 21m"
}
```

### node.ssh(id, command)
Execute a command on a node via SSH.

**Parameters:**
- `id` (string): Node identifier
- `command` (string): Shell command to execute

**Returns:** Command output (stdout/stderr)

**Note:** Use with caution. Avoid destructive commands.

### node.reboot(id)
Reboot a node. The node will be temporarily offline during reboot.

**Parameters:**
- `id` (string): Node identifier

**Returns:** Confirmation message

### node.logs(id, lines)
Get recent logs from a node's CiRA Runtime service.

**Parameters:**
- `id` (string): Node identifier
- `lines` (number, optional): Number of log lines (default: 100)

**Returns:** Log entries as text

---

## MODEL MANAGEMENT

### model.list
List all available models in the workspace.

**Parameters:** None

**Returns:**
```json
{
  "models": [
    {
      "name": "scratch_v3",
      "task": "detection",
      "format": "darknet",
      "labels": ["scratch", "dent", "crack"],
      "size": "23.5 MB"
    }
  ]
}
```

### model.deploy(model, node)
Deploy a model to a device.

**Parameters:**
- `model` (string): Model name
- `node` (string): Target node identifier

**Process:**
1. Verify model exists and is compatible
2. Transfer model files via SSH
3. Update node's model_config.json
4. Restart CiRA Runtime
5. Run verification tests

**Returns:** Deployment result with test accuracy

### model.test(model, node)
Run test images through a model on a specific device.

**Parameters:**
- `model` (string): Model name
- `node` (string): Node identifier

**Returns:** Test results with accuracy metrics

### model.convert(model, format)
Convert a model to a different format.

**Parameters:**
- `model` (string): Model name
- `format` (string): Target format (onnx, tensorrt)

**Returns:** Conversion result with output path

### model.verify(model)
Verify model file integrity.

**Parameters:**
- `model` (string): Model name

**Returns:** Verification result (checksum, file sizes, label count)

---

## CAMERA & INFERENCE

### camera.snapshot(node)
Take a snapshot from a node's camera.

**Parameters:**
- `node` (string): Node identifier

**Returns:**
```json
{
  "raw_url": "data:image/jpeg;base64,...",
  "annotated_url": "data:image/jpeg;base64,...",
  "detections": [
    {
      "label": "scratch",
      "confidence": 0.94,
      "bbox": [120, 80, 200, 150]
    }
  ],
  "timestamp": "2026-02-17T14:32:15Z"
}
```

### camera.record(node, seconds)
Record a video clip from a node's camera.

**Parameters:**
- `node` (string): Node identifier
- `seconds` (number): Duration in seconds (max 60)

**Returns:** Path to recorded video file

### camera.stream_url(node)
Get MJPEG stream URLs for a node.

**Parameters:**
- `node` (string): Node identifier

**Returns:**
```json
{
  "raw": "http://192.168.1.101:8080/stream/raw",
  "annotated": "http://192.168.1.101:8080/stream/annotated"
}
```

### inference.results(node, period)
Get inference results for a time period.

**Parameters:**
- `node` (string): Node identifier
- `period` (string): Time period ("1h", "today", "yesterday", "7d")

**Returns:**
```json
{
  "total_detections": 147,
  "by_label": {
    "scratch": 89,
    "dent": 42,
    "crack": 16
  },
  "timeline": [
    {"hour": "08:00", "count": 12},
    {"hour": "09:00", "count": 15}
  ]
}
```

### inference.stats(node, period)
Get inference statistics summary.

**Parameters:**
- `node` (string): Node identifier
- `period` (string): Time period

**Returns:**
```json
{
  "total_defects": 147,
  "defects_per_hour": 12.25,
  "avg_confidence": 0.942,
  "peak_hour": "14:00-15:00",
  "peak_count": 23
}
```

---

## ALERTS & REPORTING

### alert.set(condition, action)
Set up an alert rule.

**Parameters:**
- `condition` (object): Alert condition
  - `type`: "defect_count" | "temperature" | "fps" | "offline"
  - `threshold`: number
  - `duration`: string (e.g., "5m", "1h")
  - `node`: string (optional, applies to all if not specified)
- `action` (object): Alert action
  - `channels`: string[] (e.g., ["line", "mqtt"])
  - `message`: string (template with {node}, {value}, etc.)

**Returns:** Alert rule ID

### alert.list
List all active alert rules.

**Parameters:** None

**Returns:** Array of alert rules with their status

### alert.history(period)
Get alert history.

**Parameters:**
- `period` (string): Time period

**Returns:** Array of triggered alerts with timestamps

### report.generate(type, period)
Generate a quality report.

**Parameters:**
- `type` (string): "defects" | "uptime" | "summary"
- `period` (string): Time period

**Returns:** Report data in JSON format

### report.export(format)
Export the last generated report.

**Parameters:**
- `format` (string): "pdf" | "csv" | "json"

**Returns:** File download URL
