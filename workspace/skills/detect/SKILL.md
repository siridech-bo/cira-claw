# Object Detection Skill

## When to Use
User asks about defects, detections, objects found, what the camera sees,
counting items, or quality inspection results.

## Keywords (Thai & English)
- defect, defects, ตรวจพบ, พบ
- scratch, dent, crack, รอย, บุบ, แตก
- detection, detect, ตรวจจับ
- count, จำนวน, เท่าไหร่, กี่
- quality, คุณภาพ
- inspection, ตรวจสอบ

## Available Tools
- `node.query`: Get node status and current detection info
- `camera.snapshot`: Capture current frame with annotations
- `inference.results`: Get detection results for a time period
- `inference.stats`: Get detection statistics

## Workflow

### For Current/Live Detection Queries
1. Identify which node/camera the user is asking about
2. Use `camera.snapshot(node)` to get current frame
3. Return the annotated image with detection summary
4. Include confidence scores if detections exist

### For Historical Data Queries
1. Identify time period (today, yesterday, last hour, etc.)
2. Use `inference.stats(node, period)` for summary statistics
3. Use `inference.results(node, period)` for detailed breakdown
4. Format response with totals, breakdown by label, and peak times

### For Comparison Queries
1. Gather stats for multiple nodes or time periods
2. Present comparison in a clear format (table-like)
3. Highlight significant differences

## Example Interactions

### Example 1: Current Status
```
User: "How many defects on line 1 today?"
Agent:
1. Call inference.stats(jetson-line1, today)
2. Format response:
   "Line 1 today: 147 defects detected.
    Breakdown: 89 scratches, 42 dents, 16 cracks.
    Average confidence: 94.2%.
    Peak hour was 14:00-15:00 (23 defects)."
```

### Example 2: Show Camera (Thai)
```
User: "ดูกล้อง line 3"
Agent:
1. Call camera.snapshot(jetson-line3)
2. Return annotated image
3. Say: "[ภาพ] กล้อง Line 3: ตรวจพบ 2 scratches (confidence 94%, 87%)"
```

### Example 3: Comparison
```
User: "Compare defects between line 1 and line 2 today"
Agent:
1. Call inference.stats(jetson-line1, today)
2. Call inference.stats(jetson-line2, today)
3. Format comparison:
   "Today's comparison:
    Line 1: 147 defects (12.3/hr)
    Line 2: 89 defects (7.4/hr)
    Line 1 has 65% more defects than Line 2."
```

## Response Format Guidelines
- Always include total count
- Break down by label when available
- Include confidence scores for live detections
- Mention peak times for daily/longer queries
- Use Thai language if user writes in Thai
- Keep numbers prominent and easy to scan
