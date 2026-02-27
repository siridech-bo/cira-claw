import { FastifyInstance } from 'fastify';
import { WebSocket } from 'ws';
import { CiraAgent, AgentMessage } from '../agent/agent.js';
import { ToolContext, HeartbeatScheduler, MemoryManager } from '../agent/tools.js';
import { NodeManager } from '../nodes/manager.js';
import { StatsCollector } from '../services/stats-collector.js';
import { RuleEngine } from '../services/rule-engine.js';
import { CompositeRuleEngine } from '../services/composite-rule-engine.js';
import { AlertsConfig } from '../utils/config-schema.js';
import { createLogger } from '../utils/logger.js';

const logger = createLogger('chat');

interface ChatMessage {
  type: 'message' | 'clear' | 'ping';
  content?: string;
  images?: string[];
}

interface ChatResponse {
  type: 'response' | 'error' | 'pong' | 'typing';
  content?: string;
  images?: string[];
  toolCalls?: Array<{
    name: string;
    input: Record<string, unknown>;
    result: unknown;
  }>;
}

export interface ChatDependencies {
  agent: CiraAgent;
  nodeManager: NodeManager;
  statsCollector?: StatsCollector;
  alertsConfig?: AlertsConfig;
  ruleEngine?: RuleEngine;
  // Spec G: Composite rule engine
  compositeRuleEngine?: CompositeRuleEngine;
  // Spec D stub — Heartbeat Scheduler (not yet implemented)
  heartbeatScheduler?: HeartbeatScheduler;
  // Spec E stub — Memory Manager (not yet implemented)
  memoryManager?: MemoryManager;
}

export class ChatHandler {
  private clients: Set<WebSocket> = new Set();
  private agent: CiraAgent;
  private nodeManager: NodeManager;
  private statsCollector?: StatsCollector;
  private alertsConfig?: AlertsConfig;
  private ruleEngine?: RuleEngine;
  private compositeRuleEngine?: CompositeRuleEngine;
  private conversationHistory: Map<WebSocket, AgentMessage[]> = new Map();

  constructor(deps: ChatDependencies) {
    this.agent = deps.agent;
    this.nodeManager = deps.nodeManager;
    this.statsCollector = deps.statsCollector;
    this.alertsConfig = deps.alertsConfig;
    this.ruleEngine = deps.ruleEngine;
    this.compositeRuleEngine = deps.compositeRuleEngine;
  }

  handleConnection(socket: WebSocket): void {
    this.clients.add(socket);
    this.conversationHistory.set(socket, []);

    logger.info(`Chat client connected. Total clients: ${this.clients.size}`);

    // Send welcome message
    this.sendMessage(socket, {
      type: 'response',
      content: this.agent.isAvailable()
        ? 'Connected to CiRA CLAW. How can I help you manage your edge devices?'
        : 'Connected to CiRA CLAW. Note: AI agent is not available (no API key configured).',
    });

    socket.on('message', async (data) => {
      try {
        const message = JSON.parse(data.toString()) as ChatMessage;
        await this.handleMessage(socket, message);
      } catch (error) {
        logger.error(`Failed to parse chat message: ${error}`);
        this.sendMessage(socket, {
          type: 'error',
          content: 'Invalid message format',
        });
      }
    });

    socket.on('close', () => {
      this.clients.delete(socket);
      this.conversationHistory.delete(socket);
      logger.info(`Chat client disconnected. Total clients: ${this.clients.size}`);
    });

    socket.on('error', (error) => {
      logger.error(`Chat WebSocket error: ${error}`);
    });
  }

  private async handleMessage(socket: WebSocket, message: ChatMessage): Promise<void> {
    switch (message.type) {
      case 'ping':
        this.sendMessage(socket, { type: 'pong' });
        break;

      case 'clear':
        this.conversationHistory.set(socket, []);
        this.sendMessage(socket, {
          type: 'response',
          content: 'Conversation cleared.',
        });
        break;

      case 'message':
        if (!message.content?.trim()) {
          return;
        }

        if (!this.agent.isAvailable()) {
          this.sendMessage(socket, {
            type: 'error',
            content: 'AI agent is not available. Please configure your Anthropic API key.',
          });
          return;
        }

        // Add user message to history
        const history = this.conversationHistory.get(socket) || [];
        const userMessage: AgentMessage = {
          role: 'user',
          content: message.content,
          images: message.images,
        };
        history.push(userMessage);

        // Indicate we're processing
        this.sendMessage(socket, { type: 'typing' });

        try {
          // Get response from agent with full history and context
          const response = await this.agent.chat(history, {
            nodeManager: this.nodeManager as unknown as ToolContext['nodeManager'],
            statsCollector: this.statsCollector as unknown as ToolContext['statsCollector'],
            alertsConfig: this.alertsConfig,
            ruleEngine: this.ruleEngine,
            compositeRuleEngine: this.compositeRuleEngine,
          });

          // Add assistant response to history
          history.push({
            role: 'assistant',
            content: response.content,
            images: response.images,
          });

          // Keep history to last 20 messages to prevent context overflow
          if (history.length > 20) {
            history.splice(0, history.length - 20);
          }

          this.conversationHistory.set(socket, history);

          // Send response
          this.sendMessage(socket, {
            type: 'response',
            content: response.content,
            images: response.images,
            toolCalls: response.toolCalls,
          });
        } catch (error) {
          logger.error(`Chat error: ${error}`);
          this.sendMessage(socket, {
            type: 'error',
            content: 'An error occurred while processing your message. Please try again.',
          });
        }
        break;

      default:
        logger.debug(`Unknown chat message type: ${message.type}`);
    }
  }

  private sendMessage(socket: WebSocket, message: ChatResponse): void {
    if (socket.readyState === WebSocket.OPEN) {
      socket.send(JSON.stringify(message));
    }
  }

  getClientCount(): number {
    return this.clients.size;
  }
}

export async function registerChatRoutes(
  fastify: FastifyInstance,
  deps: ChatDependencies
): Promise<ChatHandler> {
  const handler = new ChatHandler(deps);

  // Chat WebSocket endpoint for AI agent conversations
  fastify.get('/chat', { websocket: true }, (socket, _request) => {
    handler.handleConnection(socket);
  });

  logger.info('Chat routes registered at /chat');
  return handler;
}
