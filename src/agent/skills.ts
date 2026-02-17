import fs from 'fs';
import path from 'path';
import { createLogger } from '../utils/logger.js';

const logger = createLogger('skills');

export interface Skill {
  name: string;
  path: string;
  content: string;
}

export async function loadSkills(workspacePath: string): Promise<Skill[]> {
  const skillsDir = path.join(workspacePath, 'skills');
  const skills: Skill[] = [];

  if (!fs.existsSync(skillsDir)) {
    logger.warn(`Skills directory not found: ${skillsDir}`);
    return skills;
  }

  try {
    const entries = fs.readdirSync(skillsDir, { withFileTypes: true });

    for (const entry of entries) {
      if (entry.isDirectory()) {
        const skillPath = path.join(skillsDir, entry.name, 'SKILL.md');

        if (fs.existsSync(skillPath)) {
          try {
            const content = fs.readFileSync(skillPath, 'utf-8');
            skills.push({
              name: entry.name,
              path: skillPath,
              content,
            });
            logger.debug(`Loaded skill: ${entry.name}`);
          } catch (error) {
            logger.error(`Failed to read skill file ${skillPath}: ${error}`);
          }
        }
      }
    }
  } catch (error) {
    logger.error(`Failed to read skills directory: ${error}`);
  }

  return skills;
}

export function formatSkillsForPrompt(skills: Skill[]): string {
  if (skills.length === 0) {
    return '';
  }

  const formatted = skills.map(skill => {
    return `## Skill: ${skill.name}\n\n${skill.content}`;
  });

  return `# Available Skills\n\n${formatted.join('\n\n---\n\n')}`;
}
