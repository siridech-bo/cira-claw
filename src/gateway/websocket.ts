import { FastifyInstance } from 'fastify';
import { WebSocket } from 'ws';
import { NodeManager, NodeStatus } from '../nodes/manager.js';
import { createLogger } from '../utils/logger.js';

const logger = createLogger('websocket');

interface WebSocketMessage {
  type: string;
  payload: unknown;
}

interface SubscriptionMessage {
  type: 'subscribe' | 'unsubscribe';
  channel: string;
}

export class WebSocketHandler {
  private clients: Set<WebSocket> = new Set();
  private subscriptions: Map<WebSocket, Set<string>> = new Map();
  private nodeManager: NodeManager;

  constructor(nodeManager: NodeManager) {
    this.nodeManager = nodeManager;
    this.setupNodeEvents();
  }

  // Set up listeners for node manager events
  private setupNodeEvents(): void {
    this.nodeManager.on('node:status', (status: NodeStatus) => {
      this.broadcast('nodes', {
        type: 'node:status',
        payload: status,
      });
    });

    this.nodeManager.on('node:online', (nodeId: string) => {
      this.broadcast('nodes', {
        type: 'node:online',
        payload: { nodeId },
      });
    });

    this.nodeManager.on('node:offline', (nodeId: string) => {
      this.broadcast('nodes', {
        type: 'node:offline',
        payload: { nodeId },
      });
    });

    this.nodeManager.on('node:added', (node) => {
      this.broadcast('nodes', {
        type: 'node:added',
        payload: node,
      });
    });

    this.nodeManager.on('node:removed', (nodeId: string) => {
      this.broadcast('nodes', {
        type: 'node:removed',
        payload: { nodeId },
      });
    });

    this.nodeManager.on('node:alert', (nodeId: string, message: string) => {
      this.broadcast('alerts', {
        type: 'node:alert',
        payload: { nodeId, message, timestamp: new Date().toISOString() },
      });
    });
  }

  // Handle new WebSocket connection
  handleConnection(socket: WebSocket): void {
    this.clients.add(socket);
    this.subscriptions.set(socket, new Set(['nodes'])); // Subscribe to nodes by default

    logger.info(`WebSocket client connected. Total clients: ${this.clients.size}`);

    // Send initial state
    socket.send(
      JSON.stringify({
        type: 'connected',
        payload: {
          nodes: this.nodeManager.getAllStatuses(),
          summary: this.nodeManager.getSummary(),
        },
      })
    );

    socket.on('message', (data) => {
      try {
        const message = JSON.parse(data.toString()) as WebSocketMessage;
        this.handleMessage(socket, message);
      } catch (error) {
        logger.error(`Failed to parse WebSocket message: ${error}`);
        socket.send(
          JSON.stringify({
            type: 'error',
            payload: { message: 'Invalid JSON message' },
          })
        );
      }
    });

    socket.on('close', () => {
      this.clients.delete(socket);
      this.subscriptions.delete(socket);
      logger.info(`WebSocket client disconnected. Total clients: ${this.clients.size}`);
    });

    socket.on('error', (error) => {
      logger.error(`WebSocket error: ${error}`);
    });
  }

  // Handle incoming messages
  private handleMessage(socket: WebSocket, message: WebSocketMessage): void {
    switch (message.type) {
      case 'subscribe':
      case 'unsubscribe': {
        const subMessage = message as unknown as SubscriptionMessage;
        const subs = this.subscriptions.get(socket);
        if (subs) {
          if (subMessage.type === 'subscribe') {
            subs.add(subMessage.channel);
            logger.debug(`Client subscribed to ${subMessage.channel}`);
          } else {
            subs.delete(subMessage.channel);
            logger.debug(`Client unsubscribed from ${subMessage.channel}`);
          }
        }
        break;
      }

      case 'ping':
        socket.send(JSON.stringify({ type: 'pong', payload: { timestamp: Date.now() } }));
        break;

      case 'get:nodes':
        socket.send(
          JSON.stringify({
            type: 'nodes',
            payload: {
              nodes: this.nodeManager.getAllStatuses(),
              summary: this.nodeManager.getSummary(),
            },
          })
        );
        break;

      case 'get:node': {
        const nodeId = (message.payload as { nodeId?: string })?.nodeId;
        if (nodeId) {
          const status = this.nodeManager.getNodeStatus(nodeId);
          socket.send(
            JSON.stringify({
              type: 'node',
              payload: status || { error: 'Node not found' },
            })
          );
        }
        break;
      }

      default:
        logger.debug(`Unknown message type: ${message.type}`);
    }
  }

  // Broadcast message to all clients subscribed to a channel
  broadcast(channel: string, message: WebSocketMessage): void {
    const payload = JSON.stringify(message);

    for (const [socket, subs] of this.subscriptions) {
      if (subs.has(channel) && socket.readyState === WebSocket.OPEN) {
        socket.send(payload);
      }
    }
  }

  // Broadcast to all clients regardless of subscription
  broadcastAll(message: WebSocketMessage): void {
    const payload = JSON.stringify(message);

    for (const socket of this.clients) {
      if (socket.readyState === WebSocket.OPEN) {
        socket.send(payload);
      }
    }
  }

  // Get connected client count
  getClientCount(): number {
    return this.clients.size;
  }
}

// Register WebSocket routes
export async function registerWebSocketRoutes(
  fastify: FastifyInstance,
  nodeManager: NodeManager
): Promise<WebSocketHandler> {
  const handler = new WebSocketHandler(nodeManager);

  // Main WebSocket endpoint for real-time data
  fastify.get('/ws', { websocket: true }, (socket, _request) => {
    handler.handleConnection(socket);
  });

  logger.info('WebSocket routes registered');
  return handler;
}
