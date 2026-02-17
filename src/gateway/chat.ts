import { FastifyInstance } from 'fastify';
import { WebSocket } from 'ws';
import { CiraAgent, AgentMessage } from '../agent/agent.js';
import { NodeManager } from '../nodes/manager.js';
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

export class ChatHandler {
  private clients: Set<WebSocket> = new Set();
  private agent: CiraAgent;
  private nodeManager: NodeManager;
  private conversationHistory: Map<WebSocket, AgentMessage[]> = new Map();

  constructor(agent: CiraAgent, nodeManager: NodeManager) {
    this.agent = agent;
    this.nodeManager = nodeManager;
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
          // Get response from agent with full history
          const response = await this.agent.chat(history, {
            nodeManager: this.nodeManager as unknown as undefined,
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
  agent: CiraAgent,
  nodeManager: NodeManager
): Promise<ChatHandler> {
  const handler = new ChatHandler(agent, nodeManager);

  // Chat WebSocket endpoint for AI agent conversations
  fastify.get('/chat', { websocket: true }, (socket, _request) => {
    handler.handleConnection(socket);
  });

  logger.info('Chat routes registered at /chat');
  return handler;
}
