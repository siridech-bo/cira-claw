# Classification Skill

## When to Use
User asks about image classification, product categorization, pass/fail inspection,
or sorting decisions based on AI classification models.

## Keywords (Thai & English)
- classify, classification, จำแนก, แยกประเภท
- category, หมวดหมู่
- pass, fail, ผ่าน, ไม่ผ่าน
- good, bad, ดี, เสีย
- sort, sorting, คัด, คัดแยก
- grade, เกรด
- type, ประเภท

## Available Tools
- `camera.snapshot`: Get current frame with classification result
- `inference.results`: Get classification history
- `inference.stats`: Get classification statistics
- `node.query`: Check device and model info

## Classification vs Detection
- **Detection**: Finds and locates objects (bounding boxes)
- **Classification**: Assigns a single label to entire image

Classification models output:
- Single label (e.g., "good", "defective", "grade_A")
- Confidence score for that label
- Sometimes top-N predictions with scores

## Workflow

### Current Classification Query
1. Use `camera.snapshot(node)` to capture frame
2. Return classification result with confidence
3. No bounding boxes (it's whole-image classification)

### Historical Classification Stats
1. Use `inference.stats(node, period)` for summary
2. Break down by classification labels
3. Calculate pass/fail rates if applicable

### Quality Grading Reports
1. Gather classification stats for time period
2. Calculate distribution across grades
3. Present as percentages and counts

## Example Interactions

### Example 1: Current Classification
```
User: "What grade is the current product on line 2?"
Agent:
1. Call camera.snapshot(jetson-line2)
2. Say: "[Image]
        Classification: Grade A
        Confidence: 97.3%

        This product passes quality inspection."
```

### Example 2: Pass/Fail Rate (Thai)
```
User: "อัตรา pass/fail วันนี้ line 1"
Agent:
1. Call inference.stats(jetson-line1, today)
2. Say: "Line 1 วันนี้:
        ✓ Pass: 1,247 (94.2%)
        ✗ Fail: 77 (5.8%)

        รวม: 1,324 ชิ้น
        เวลา fail มากสุด: 10:00-11:00 (12 ชิ้น)"
```

### Example 3: Grade Distribution
```
User: "Show me the grade breakdown for today"
Agent:
1. Call inference.results(jetson-line1, today)
2. Say: "Today's Grade Distribution (Line 1):

        Grade A: 823 (62.1%)  ████████████░░░░
        Grade B: 312 (23.6%)  ████████░░░░░░░░
        Grade C: 112 (8.5%)   ███░░░░░░░░░░░░░
        Reject:   77 (5.8%)   ██░░░░░░░░░░░░░░

        Total inspected: 1,324 units
        Overall pass rate: 94.2%"
```

### Example 4: Trend Analysis
```
User: "Is the reject rate increasing?"
Agent:
1. Call inference.stats(jetson-line1, 7d)
2. Analyze trend
3. Say: "Reject Rate Trend (Last 7 Days):

        Mon: 4.2%
        Tue: 4.5%
        Wed: 5.1%
        Thu: 5.3%
        Fri: 5.8%  ← Today

        ⚠ Yes, reject rate has increased by 1.6% this week.

        Possible causes:
        1. Material quality variation
        2. Machine calibration drift
        3. Model threshold needs adjustment

        Recommend: Review recent reject samples to identify pattern."
```

## Response Format Guidelines
- Show confidence scores for live classifications
- Calculate and show pass/fail rates as percentages
- Use visual indicators (✓, ✗, progress bars) for clarity
- Include total counts for context
- Identify trends and anomalies proactively
- Suggest actions when rates are concerning
