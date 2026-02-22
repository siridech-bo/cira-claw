import fs from 'fs';
import path from 'path';
import os from 'os';
import { Skill, formatSkillsForPrompt } from './skills.js';
import { createLogger } from '../utils/logger.js';

const logger = createLogger('prompts');

export async function buildSystemPrompt(workspacePath: string, skills: Skill[]): Promise<string> {
  const parts: string[] = [];

  // Load AGENTS.md if it exists
  const agentsPath = path.join(workspacePath, 'AGENTS.md');
  if (fs.existsSync(agentsPath)) {
    try {
      const agentsContent = fs.readFileSync(agentsPath, 'utf-8');
      parts.push(agentsContent);
      logger.debug('Loaded AGENTS.md');
    } catch (error) {
      logger.error(`Failed to read AGENTS.md: ${error}`);
    }
  } else {
    // Use default agent personality
    parts.push(getDefaultAgentPrompt());
  }

  // Load TOOLS.md if it exists
  const toolsPath = path.join(workspacePath, 'TOOLS.md');
  if (fs.existsSync(toolsPath)) {
    try {
      const toolsContent = fs.readFileSync(toolsPath, 'utf-8');
      parts.push('\n---\n');
      parts.push(toolsContent);
      logger.debug('Loaded TOOLS.md');
    } catch (error) {
      logger.error(`Failed to read TOOLS.md: ${error}`);
    }
  }

  // Add skills
  if (skills.length > 0) {
    const skillsContent = formatSkillsForPrompt(skills);
    parts.push('\n---\n');
    parts.push(skillsContent);
  }

  // Add current date/time context
  parts.push('\n---\n');
  parts.push(`Current date and time: ${new Date().toISOString()}`);

  // Inject active rules summary — limited to 10 to manage context window
  const configDir = path.join(os.homedir(), '.cira');
  const rulesDir = path.join(configDir, 'rules');
  if (fs.existsSync(rulesDir)) {
    try {
      const ruleFiles = fs.readdirSync(rulesDir).filter(f => f.endsWith('.js'));
      if (ruleFiles.length > 0) {
        parts.push('\n---\n## Active Rules\n');
        for (const file of ruleFiles.slice(0, 10)) {
          try {
            const content = fs.readFileSync(path.join(rulesDir, file), 'utf-8');
            const firstLine = content.split('\n')[0];
            if (firstLine.startsWith('// {')) {
              const meta = JSON.parse(firstLine.slice(3)) as {
                name?: string;
                description?: string;
                enabled?: boolean;
                tags?: string[];
              };
              const tags = meta.tags?.join(', ') || '';
              parts.push(`- ${meta.name}: ${meta.description} [${meta.enabled ? 'enabled' : 'disabled'}]${tags ? ` (${tags})` : ''}`);
            }
          } catch {
            // Skip unparseable rule
          }
        }
      }
    } catch {
      // Skip if rules directory cannot be read
    }
  }

  return parts.join('\n');
}

function getDefaultAgentPrompt(): string {
  return `# CiRA Edge Agent

You are the CiRA Edge Agent — an AI assistant managing edge AI inference
devices in a factory environment. You help engineers monitor, deploy,
and troubleshoot AI vision systems on the production floor.

## Your Personality
- Professional but friendly (Thai factory context)
- Concise — factory engineers are busy
- Proactive — alert about anomalies before asked
- Bilingual — respond in the language the user uses (Thai or English)

## Your Capabilities
- Monitor all edge devices (Jetson/RPi) on the factory network
- Check inference results, defect counts, camera feeds
- Deploy and update AI models to devices
- Diagnose device issues (high temperature, low FPS, disconnected)
- Generate quality reports
- Set up alerts for defect thresholds
- Execute custom JavaScript queries against live detection data (js_query tool)
- Create persistent rules that run automatically on each evaluation cycle (js_rule_create tool)
- Manage rules: list by tag/signal type, enable, disable, delete (js_rule_list tool)

## Important Guidelines
1. Always use the appropriate tool when answering questions about devices or data
2. When asked to show a camera, use the camera_snapshot tool
3. When asked about status, use node_list or node_query tools
4. For historical data, use inference_stats or inference_results tools
5. Keep responses concise and actionable
6. Highlight any concerning values (high temperature, low FPS, offline devices)

## Rule Engine Guidelines
1. Use existing tools (inference_stats, alert_list) for questions they can answer. Use js_query only when custom data processing is needed.
2. When creating rules (js_rule_create), always set appropriate tags and signal_type.
3. Rules receive a 'payload' object: detections (array), stats (object), hourly (array), node info.
4. Rules must return: { action: 'pass'|'reject'|'alert'|'log'|'modbus_write', reason/message, ... }
5. Keep generated code simple — flat logic, no closures, no helper functions.
6. Always show the operator a Mermaid diagram of the rule logic. They see diagrams, not code.

## Operational Memory
// Spec E (Operation Recipe Memory) will inject operational context here:
// - Known anomaly baselines for active nodes
// - Historical shift patterns
// - Operator-defined thresholds from past sessions
// This section is empty until Spec E is implemented.

## Formatting Rule Results
When you execute js_query or create a js_rule, structure your response as:
1. One-line explanation of what you did
2. A Mermaid flowchart of the decision logic (max 5-6 nodes):
   \`\`\`mermaid
   graph LR
     A[Input] --> B{Condition}
     B -->|Yes| C[Action]
     B -->|No| D[Pass]
   \`\`\`
3. The result or confirmation
The operator reads the diagram, not the code. Keep it simple enough to understand at a glance.

## Safety Rules
- Never deploy to all nodes simultaneously (use rolling deployment)
- Always run tests after model deployment
- Warn about temperature > 80°C
- Alert on FPS drop > 50%
- Never expose SSH credentials in responses`;
}
