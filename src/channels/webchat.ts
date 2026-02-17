import { FastifyInstance } from 'fastify';
import { WebSocket } from 'ws';
import { CiraAgent } from '../agent/agent.js';
import { NodeManager } from '../nodes/manager.js';
import { createLogger } from '../utils/logger.js';

const logger = createLogger('webchat');

interface ChatMessage {
  type: 'message' | 'typing' | 'ping';
  content?: string;
  images?: string[];
}

interface ChatSession {
  socket: WebSocket;
  history: Array<{ role: 'user' | 'assistant'; content: string }>;
}

export class WebChatChannel {
  private agent: CiraAgent;
  private nodeManager: NodeManager;
  private sessions: Map<string, ChatSession> = new Map();

  constructor(agent: CiraAgent, nodeManager: NodeManager) {
    this.agent = agent;
    this.nodeManager = nodeManager;
  }

  async register(fastify: FastifyInstance, path: string = '/chat'): Promise<void> {
    fastify.get(path, { websocket: true }, (socket, request) => {
      const sessionId = `chat-${Date.now()}-${Math.random().toString(36).substring(7)}`;

      logger.info(`WebChat session started: ${sessionId}`);

      const session: ChatSession = {
        socket,
        history: [],
      };

      this.sessions.set(sessionId, session);

      // Send welcome message
      this.sendMessage(socket, {
        type: 'message',
        role: 'assistant',
        content: `Hello! I'm the CiRA Edge Agent. I can help you monitor and manage your edge AI devices.

Try asking me:
- "What's the status of all devices?"
- "How many defects on line 1 today?"
- "Show me the camera on line 2"
- "Deploy scratch_v4 to line 1"`,
      });

      socket.on('message', async (data) => {
        try {
          const message = JSON.parse(data.toString()) as ChatMessage;
          await this.handleMessage(sessionId, message);
        } catch (error) {
          logger.error(`Failed to process chat message: ${error}`);
          this.sendMessage(socket, {
            type: 'error',
            content: 'Failed to process your message',
          });
        }
      });

      socket.on('close', () => {
        this.sessions.delete(sessionId);
        logger.info(`WebChat session ended: ${sessionId}`);
      });

      socket.on('error', (error) => {
        logger.error(`WebChat error for ${sessionId}: ${error}`);
      });
    });

    logger.info(`WebChat channel registered at ${path}`);
  }

  private async handleMessage(sessionId: string, message: ChatMessage): Promise<void> {
    const session = this.sessions.get(sessionId);
    if (!session) {
      logger.warn(`No session found for ${sessionId}`);
      return;
    }

    const { socket, history } = session;

    switch (message.type) {
      case 'ping':
        this.sendMessage(socket, { type: 'pong' });
        break;

      case 'message':
        if (!message.content?.trim()) {
          return;
        }

        // Add user message to history
        history.push({ role: 'user', content: message.content });

        // Send typing indicator
        this.sendMessage(socket, { type: 'typing', isTyping: true });

        try {
          // Check if agent is available
          if (!this.agent.isAvailable()) {
            this.sendMessage(socket, {
              type: 'message',
              role: 'assistant',
              content: 'AI agent is not available. Please configure your Anthropic API key in ~/.cira/credentials/claude.json or set the ANTHROPIC_API_KEY environment variable.',
            });
            this.sendMessage(socket, { type: 'typing', isTyping: false });
            return;
          }

          // Get response from agent
          const response = await this.agent.chat(
            history.map(m => ({ role: m.role, content: m.content })),
            { nodeManager: this.nodeManager as unknown as undefined }
          );

          // Add assistant response to history
          history.push({ role: 'assistant', content: response.content });

          // Keep history limited to last 20 messages
          if (history.length > 20) {
            history.splice(0, history.length - 20);
          }

          // Send response
          this.sendMessage(socket, {
            type: 'message',
            role: 'assistant',
            content: response.content,
            images: response.images,
            toolCalls: response.toolCalls,
          });
        } catch (error) {
          logger.error(`Error getting agent response: ${error}`);
          this.sendMessage(socket, {
            type: 'message',
            role: 'assistant',
            content: 'Sorry, an error occurred while processing your request. Please try again.',
          });
        } finally {
          // Stop typing indicator
          this.sendMessage(socket, { type: 'typing', isTyping: false });
        }
        break;

      default:
        logger.debug(`Unknown message type: ${message.type}`);
    }
  }

  private sendMessage(socket: WebSocket, message: Record<string, unknown>): void {
    if (socket.readyState === WebSocket.OPEN) {
      socket.send(JSON.stringify({
        timestamp: new Date().toISOString(),
        ...message,
      }));
    }
  }

  // Get active session count
  getSessionCount(): number {
    return this.sessions.size;
  }
}

export function createWebChatChannel(agent: CiraAgent, nodeManager: NodeManager): WebChatChannel {
  return new WebChatChannel(agent, nodeManager);
}
