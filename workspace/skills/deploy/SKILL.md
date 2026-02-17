# Model Deployment Skill

## When to Use
User wants to deploy, update, or swap models on edge devices.

## Keywords (Thai & English)
- deploy, deployment, ติดตั้ง, วาง
- update, อัพเดท, ปรับปรุง
- model, โมเดล
- swap, change, เปลี่ยน
- install, ลง

## Available Tools
- `model.list`: See available models
- `model.deploy`: Deploy model to device
- `model.test`: Test model after deployment
- `model.verify`: Verify model integrity
- `node.query`: Check device status before deployment

## Workflow

### Single Device Deployment
1. Verify target device is online using `node.query(id)`
2. Verify model exists using `model.list`
3. Verify model integrity using `model.verify(model)`
4. Deploy model using `model.deploy(model, node)`
5. Wait for deployment completion
6. Run test using `model.test(model, node)`
7. Report results with accuracy metrics

### Multi-Device (Rolling) Deployment
1. Get list of target devices
2. For each device (sequentially, not parallel):
   a. Check device status
   b. Deploy model
   c. Run verification test
   d. Only proceed to next device if test passes
3. Report overall results
4. List any failed deployments

### Model Rollback
If deployment fails or accuracy drops:
1. Identify the previous model version
2. Re-deploy previous version
3. Verify rollback success
4. Report the rollback

## Safety Rules
- **Never deploy to all nodes at once** — use rolling deployment
- **Always run test after deployment** — never skip verification
- **Keep previous model as backup** — don't delete until new model verified
- **Report any accuracy drop > 2%** — alert the user
- **Check device health before deployment** — skip if device has issues
- **Verify model compatibility** — ensure model works with device hardware

## Example Interactions

### Example 1: Single Deployment
```
User: "Deploy scratch_v4 to line 1"
Agent:
1. Check: model.list → scratch_v4 exists ✓
2. Check: node.query(jetson-line1) → online ✓
3. Verify: model.verify(scratch_v4) → integrity OK ✓
4. Deploy: model.deploy(scratch_v4, jetson-line1)
5. Test: model.test(scratch_v4, jetson-line1) → 98.2% accuracy ✓
6. Say: "scratch_v4 deployed to Line 1 successfully.
        Test results: 98.2% accuracy on 50 test images.
        Model is now active."
```

### Example 2: Multi-Device Deployment (Thai)
```
User: "deploy model ใหม่ ไปทุก line"
Agent:
1. Confirm: "จะ deploy scratch_v4 ไปยัง 4 lines. ใช้ rolling deployment (ทีละเครื่อง). ยืนยัน?"
User: "ยืนยัน"
2. For jetson-line1:
   - Deploy → Test → 98.5% ✓
3. For jetson-line2:
   - Deploy → Test → 97.8% ✓
4. For jetson-line3:
   - Deploy → Test → 98.1% ✓
5. For jetson-line4:
   - Deploy → Test → 98.0% ✓
6. Say: "Deploy เสร็จสิ้น 4/4 เครื่อง
        Average accuracy: 98.1%
        ไม่มีปัญหา"
```

### Example 3: Deployment Failure
```
User: "Deploy scratch_v4 to line 3"
Agent:
1. Check: node.query(jetson-line3) → offline ✗
2. Say: "Cannot deploy to Line 3 — device is offline.
        Last seen: 2 hours ago.
        Please check device connectivity and try again."
```

### Example 4: Accuracy Drop
```
User: "Deploy new_model to line 2"
Agent:
1. Deploy: model.deploy(new_model, jetson-line2)
2. Test: model.test(new_model, jetson-line2) → 91.3% accuracy ⚠
3. Say: "⚠ Warning: new_model accuracy is 91.3%
        This is 5.2% lower than previous model (96.5%).

        Options:
        1. Keep new model (lower accuracy)
        2. Roll back to previous model

        What would you like to do?"
```

## Response Format Guidelines
- Always confirm deployment completion
- Include accuracy metrics from test
- Compare with previous model if available
- Use clear status indicators (✓ ✗ ⚠)
- List any warnings or issues prominently
- Provide rollback option if accuracy drops
