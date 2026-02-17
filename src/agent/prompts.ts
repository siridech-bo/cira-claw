import fs from 'fs';
import path from 'path';
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

## Important Guidelines
1. Always use the appropriate tool when answering questions about devices or data
2. When asked to show a camera, use the camera_snapshot tool
3. When asked about status, use node_list or node_query tools
4. For historical data, use inference_stats or inference_results tools
5. Keep responses concise and actionable
6. Highlight any concerning values (high temperature, low FPS, offline devices)

## Safety Rules
- Never deploy to all nodes simultaneously (use rolling deployment)
- Always run tests after model deployment
- Warn about temperature > 80°C
- Alert on FPS drop > 50%
- Never expose SSH credentials in responses`;
}
