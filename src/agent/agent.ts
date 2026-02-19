import Anthropic from '@anthropic-ai/sdk';
import fs from 'fs';
import path from 'path';
import { AgentConfig } from '../utils/config-schema.js';
import { createLogger } from '../utils/logger.js';
import { Tool, ToolResult, ToolContext, getToolDefinitions, executeToolCall } from './tools.js';
import { loadSkills, Skill } from './skills.js';
import { buildSystemPrompt } from './prompts.js';

const logger = createLogger('agent');

export interface AgentMessage {
  role: 'user' | 'assistant';
  content: string;
  images?: string[]; // base64 encoded images
}

export interface AgentResponse {
  content: string;
  images?: string[];
  toolCalls?: Array<{
    name: string;
    input: Record<string, unknown>;
    result: unknown;
  }>;
}

export class CiraAgent {
  private client: Anthropic | null = null;
  private config: AgentConfig;
  private workspacePath: string;
  private skills: Skill[] = [];
  private systemPrompt: string = '';
  private conversationHistory: Anthropic.MessageParam[] = [];

  constructor(config: AgentConfig, workspacePath: string) {
    this.config = config;
    this.workspacePath = workspacePath;
  }

  async init(): Promise<void> {
    // Load API key
    const apiKey = process.env.ANTHROPIC_API_KEY || await this.loadApiKey();

    if (!apiKey) {
      logger.warn('No Anthropic API key found. Agent will not be available.');
      return;
    }

    this.client = new Anthropic({ apiKey });

    // Load skills from workspace
    this.skills = await loadSkills(this.workspacePath);
    logger.info(`Loaded ${this.skills.length} skills`);

    // Build system prompt
    this.systemPrompt = await buildSystemPrompt(this.workspacePath, this.skills);
    logger.info('Agent initialized successfully');
  }

  private async loadApiKey(): Promise<string | undefined> {
    const credentialsPath = path.join(path.dirname(this.workspacePath), 'credentials', 'claude.json');

    if (fs.existsSync(credentialsPath)) {
      try {
        const content = fs.readFileSync(credentialsPath, 'utf-8');
        const credentials = JSON.parse(content) as { api_key?: string };
        return credentials.api_key;
      } catch (error) {
        logger.error(`Failed to load Claude credentials: ${error}`);
      }
    }

    return undefined;
  }

  isAvailable(): boolean {
    return this.client !== null;
  }

  async chat(
    messages: AgentMessage[],
    context?: ToolContext
  ): Promise<AgentResponse> {
    if (!this.client) {
      return {
        content: 'AI agent is not available. Please configure your Anthropic API key.',
      };
    }

    // Convert messages to Anthropic format
    const anthropicMessages: Anthropic.MessageParam[] = messages.map(msg => {
      if (msg.images && msg.images.length > 0) {
        // Message with images
        const content: Anthropic.ContentBlockParam[] = msg.images.map(img => ({
          type: 'image' as const,
          source: {
            type: 'base64' as const,
            media_type: 'image/jpeg' as const,
            data: img.replace(/^data:image\/\w+;base64,/, ''),
          },
        }));
        content.push({ type: 'text' as const, text: msg.content });
        return { role: msg.role, content };
      }
      return { role: msg.role, content: msg.content };
    });

    // Get tool definitions
    const tools = getToolDefinitions();

    try {
      // Initial request
      let response = await this.client.messages.create({
        model: this.config.model,
        max_tokens: 4096,
        system: this.systemPrompt,
        tools: tools.map(t => ({
          name: t.name,
          description: t.description,
          input_schema: t.inputSchema as Anthropic.Tool.InputSchema,
        })),
        messages: anthropicMessages,
      });

      const toolCalls: AgentResponse['toolCalls'] = [];
      let finalContent = '';
      const images: string[] = [];

      // Process response and handle tool calls
      while (response.stop_reason === 'tool_use') {
        const assistantContent = response.content;
        anthropicMessages.push({ role: 'assistant', content: assistantContent });

        // Process each tool use block
        const toolResults: Anthropic.ToolResultBlockParam[] = [];

        for (const block of assistantContent) {
          if (block.type === 'tool_use') {
            const toolName = block.name;
            const toolInput = block.input as Record<string, unknown>;

            logger.debug(`Executing tool: ${toolName}`, { input: toolInput });

            try {
              const result = await executeToolCall(toolName, toolInput, context);

              toolCalls.push({
                name: toolName,
                input: toolInput,
                result: result.data,
              });

              // Handle image results
              if (result.images) {
                images.push(...result.images);
              }

              toolResults.push({
                type: 'tool_result',
                tool_use_id: block.id,
                content: JSON.stringify(result.data),
              });
            } catch (error) {
              const errorMessage = error instanceof Error ? error.message : 'Tool execution failed';
              logger.error(`Tool ${toolName} failed: ${errorMessage}`);

              toolResults.push({
                type: 'tool_result',
                tool_use_id: block.id,
                content: JSON.stringify({ error: errorMessage }),
                is_error: true,
              });
            }
          }
        }

        // Add tool results and continue conversation
        anthropicMessages.push({ role: 'user', content: toolResults });

        response = await this.client.messages.create({
          model: this.config.model,
          max_tokens: 4096,
          system: this.systemPrompt,
          tools: tools.map(t => ({
            name: t.name,
            description: t.description,
            input_schema: t.inputSchema as Anthropic.Tool.InputSchema,
          })),
          messages: anthropicMessages,
        });
      }

      // Extract final text content
      for (const block of response.content) {
        if (block.type === 'text') {
          finalContent += block.text;
        }
      }

      return {
        content: finalContent,
        images: images.length > 0 ? images : undefined,
        toolCalls: toolCalls.length > 0 ? toolCalls : undefined,
      };

    } catch (error) {
      logger.error(`Agent chat error: ${error}`);

      if (error instanceof Anthropic.APIError) {
        if (error.status === 401) {
          return { content: 'Authentication failed. Please check your API key.' };
        }
        if (error.status === 429) {
          return { content: 'Rate limit exceeded. Please try again later.' };
        }
      }

      return { content: 'An error occurred while processing your request. Please try again.' };
    }
  }

  // Simple single-turn chat for CLI usage
  async query(prompt: string, context?: ToolContext): Promise<string> {
    const response = await this.chat([{ role: 'user', content: prompt }], context);
    return response.content;
  }

  // Clear conversation history
  clearHistory(): void {
    this.conversationHistory = [];
  }
}

// Singleton instance
let agentInstance: CiraAgent | null = null;

export function getAgent(config: AgentConfig, workspacePath: string): CiraAgent {
  if (!agentInstance) {
    agentInstance = new CiraAgent(config, workspacePath);
  }
  return agentInstance;
}

export default CiraAgent;
