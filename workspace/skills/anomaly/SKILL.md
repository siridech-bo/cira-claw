# Anomaly Detection Skill

## When to Use
User asks about anomaly detection, unusual patterns, sensor data analysis,
predictive maintenance, or outlier detection.

## Keywords (Thai & English)
- anomaly, anomalies, ผิดปกติ, ความผิดปกติ
- unusual, ไม่ปกติ
- outlier, ค่าผิดปกติ
- sensor, เซนเซอร์
- predictive, ทำนาย
- maintenance, บำรุงรักษา
- vibration, การสั่น
- pattern, รูปแบบ
- threshold, ค่าขีด

## Available Tools
- `inference.results`: Get anomaly detection results
- `inference.stats`: Get anomaly statistics
- `node.query`: Get sensor node status
- `alert.set`: Configure anomaly alerts
- `alert.history`: View past anomaly alerts

## Anomaly Detection Types
1. **Sensor-based**: Vibration, temperature, pressure readings
2. **Visual**: Detect unusual patterns in images (unsupervised)
3. **Time-series**: Detect deviations from normal operational patterns

## Workflow

### Current Anomaly Status
1. Use `node.query(id)` to get sensor readings
2. Check if any values are outside normal range
3. Report current status and any anomalies

### Historical Anomaly Analysis
1. Use `inference.results(node, period)` for anomaly events
2. Summarize frequency and types
3. Identify patterns or trends

### Alert Configuration
1. Understand user's threshold requirements
2. Use `alert.set` to configure anomaly alerts
3. Confirm alert is active

## Anomaly Severity Levels
- **Info**: Minor deviation, within tolerance
- **Warning**: Approaching threshold, monitor closely
- **Critical**: Exceeds threshold, action required
- **Emergency**: Severe anomaly, immediate attention

## Example Interactions

### Example 1: Current Status
```
User: "Any anomalies on sensor 1?"
Agent:
1. Call node.query(rpi-sensor-1)
2. Say: "Sensor 1 Status: ✓ Normal

        Current readings:
        - Vibration: 2.3 mm/s (normal: <5)
        - Temperature: 42°C (normal: <60)
        - Pressure: 4.2 bar (normal: 4-5)

        No anomalies detected. All values within normal range."
```

### Example 2: Anomaly Detected (Thai)
```
User: "ตรวจสอบ sensor line 2"
Agent:
1. Call node.query(rpi-sensor-2)
2. Say: "⚠ Sensor Line 2: พบความผิดปกติ

        การสั่น: 7.8 mm/s ⚠ (ปกติ: <5)
        อุณหภูมิ: 58°C (ปกติ: <60)
        แรงดัน: 4.5 bar ✓

        ⚠ การสั่นสูงกว่าปกติ 56%

        แนะนำ:
        1. ตรวจสอบ bearing หรือชิ้นส่วนหมุน
        2. ตรวจสอบการติดตั้งว่าหลวมหรือไม่
        3. วางแผนบำรุงรักษาถ้าค่ายังสูง"
```

### Example 3: Anomaly History
```
User: "How many anomalies this week?"
Agent:
1. Call inference.stats(rpi-sensor-1, 7d)
2. Call alert.history(7d)
3. Say: "Anomaly Summary (Last 7 Days):

        Total anomaly events: 12

        By type:
        - Vibration spike: 7
        - Temperature rise: 3
        - Pressure drop: 2

        Timeline:
        Mon: 1 | Tue: 0 | Wed: 2 | Thu: 3 | Fri: 6 ⬆

        ⚠ Trend: Anomalies increasing (6 today vs 1 on Monday)

        Most frequent: Vibration spikes on Line 2 sensor
        Recommend: Schedule maintenance check for Line 2 motor"
```

### Example 4: Set Anomaly Alert
```
User: "Alert me if vibration goes above 6"
Agent:
1. Call alert.set({
     condition: { type: "sensor", metric: "vibration", threshold: 6, node: "rpi-sensor-1" },
     action: { channels: ["line"], message: "⚠ Vibration alert: {value} mm/s on {node}" }
   })
2. Say: "✓ Anomaly alert configured:

        Condition: Vibration > 6 mm/s
        Applies to: rpi-sensor-1
        Notification: LINE message

        You'll receive an alert when vibration exceeds 6 mm/s."
```

### Example 5: Predictive Maintenance
```
User: "When should we schedule maintenance?"
Agent:
1. Analyze recent anomaly trends
2. Check sensor degradation patterns
3. Say: "Predictive Maintenance Analysis:

        Based on the last 30 days of data:

        🔧 Line 2 Motor (rpi-sensor-2):
           - Vibration trending up: +15% over 2 weeks
           - Estimated time to threshold: 5-7 days
           - Recommend: Schedule maintenance this weekend

        ✓ Line 1 Motor (rpi-sensor-1):
           - All readings stable
           - No maintenance needed for ~30 days

        Priority: Address Line 2 motor first to prevent unplanned downtime."
```

## Response Format Guidelines
- Clearly indicate normal vs anomalous values
- Show current value vs threshold
- Use severity indicators (✓, ⚠, ⛔)
- Include trend analysis when relevant
- Provide actionable maintenance recommendations
- Calculate percentage deviation from normal
