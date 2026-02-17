# Device Monitoring Skill

## When to Use
User asks about device health, status, temperature, FPS, uptime,
connectivity, or overall system health.

## Keywords (Thai & English)
- status, สถานะ
- health, สุขภาพ
- temperature, อุณหภูมิ, ร้อน
- FPS, frame rate
- uptime, เปิดมา
- online, offline, ออนไลน์, ออฟไลน์
- memory, RAM, หน่วยความจำ
- CPU, ประมวลผล
- device, เครื่อง, อุปกรณ์

## Available Tools
- `node.list`: Get all devices with status summary
- `node.query`: Get detailed info for specific device
- `node.status`: Quick health check
- `node.logs`: View recent logs
- `node.reboot`: Restart a device

## Workflow

### Status Overview
1. Use `node.list` to get all devices
2. Summarize: online/offline counts
3. Highlight any devices with issues (high temp, low FPS)

### Single Device Status
1. Use `node.query(id)` for full details
2. Present key metrics clearly
3. Flag any concerning values

### Troubleshooting Offline Device
1. Confirm device is offline with `node.status(id)`
2. Check recent logs with `node.logs(id, 50)`
3. Suggest possible causes
4. Offer to reboot if appropriate

## Alert Thresholds
- Temperature > 80°C: Warning
- Temperature > 90°C: Critical
- FPS < 10: Warning
- FPS < 5: Critical
- Memory > 90%: Warning
- Device offline > 5 minutes: Alert

## Example Interactions

### Example 1: Overall Status
```
User: "How are all devices?"
Agent:
1. Call node.list
2. Say: "System Status: 3/4 devices online

        🟢 jetson-line1: 28 FPS, 65°C
        🟢 jetson-line2: 30 FPS, 58°C
        🔴 jetson-line3: Offline (last seen 2h ago)
        🟢 rpi-sensor-1: OK, 42°C

        ⚠ Action needed: Check jetson-line3 connectivity"
```

### Example 2: Single Device (Thai)
```
User: "สถานะ line 1"
Agent:
1. Call node.query(jetson-line1)
2. Say: "Line 1 (jetson-line1)

        สถานะ: 🟢 Online
        FPS: 28 | อุณหภูมิ: 65°C
        CPU: 45% | RAM: 2.1/4 GB
        Uptime: 47 วัน 3 ชั่วโมง

        Model: scratch_v3
        Defects วันนี้: 147

        ไม่มีปัญหา"
```

### Example 3: Temperature Warning
```
User: "Check line 2 temperature"
Agent:
1. Call node.status(jetson-line2)
2. If temp > 80°C:
   Say: "⚠ Warning: Line 2 temperature is 82°C

        This is above safe threshold (80°C).
        Recommended actions:
        1. Check ventilation/cooling
        2. Reduce workload if possible
        3. Consider temporary shutdown if temp continues rising

        Current status: Still operational but monitoring closely."
```

### Example 4: Troubleshoot Offline
```
User: "Line 3 is showing offline, why?"
Agent:
1. Confirm: node.status(jetson-line3) → offline
2. Check: node.logs(jetson-line3, 50) → (may fail if truly offline)
3. Say: "Line 3 (jetson-line3) is offline.
        Last seen: 2 hours ago at 12:32

        Possible causes:
        1. Network disconnection
        2. Device powered off
        3. CiRA Runtime crashed

        Troubleshooting steps:
        1. Check physical network connection
        2. Verify device has power
        3. Try SSH manually: ssh cira@192.168.1.103
        4. If accessible, check: systemctl status cira-runtime

        Would you like me to attempt a remote reboot once the device is reachable?"
```

### Example 5: Reboot Request
```
User: "Reboot line 1"
Agent:
1. Confirm: "This will restart jetson-line1.
            The device will be offline for ~2 minutes.
            Current inference will be interrupted.
            Proceed?"
User: "Yes"
2. Call node.reboot(jetson-line1)
3. Say: "Reboot initiated for Line 1.
        The device should be back online in ~2 minutes.
        I'll monitor and notify you when it's back."
4. (After device comes back online)
   Say: "✓ Line 1 is back online.
        FPS: 28 | Temperature: 52°C
        Model loaded and running."
```

## Response Format Guidelines
- Use status indicators: 🟢 online, 🟡 warning, 🔴 offline/error
- Always show key metrics: FPS, temperature, memory
- Highlight concerning values with ⚠
- Include actionable suggestions for issues
- Mention uptime for context
- Use Thai if user writes in Thai
