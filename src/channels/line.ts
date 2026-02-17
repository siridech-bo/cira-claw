import { FastifyInstance, FastifyRequest, FastifyReply } from 'fastify';
import { Client, middleware, WebhookEvent, TextMessage, ImageMessage, MessageAPIResponseBase } from '@line/bot-sdk';
import { CiraAgent } from '../agent/agent.js';
import { NodeManager } from '../nodes/manager.js';
import { createLogger } from '../utils/logger.js';

const logger = createLogger('line-channel');

export interface LineConfig {
  channelAccessToken: string;
  channelSecret: string;
}

export class LineChannel {
  private client: Client;
  private config: LineConfig;
  private agent: CiraAgent;
  private nodeManager: NodeManager;

  constructor(config: LineConfig, agent: CiraAgent, nodeManager: NodeManager) {
    this.config = config;
    this.agent = agent;
    this.nodeManager = nodeManager;

    this.client = new Client({
      channelAccessToken: config.channelAccessToken,
      channelSecret: config.channelSecret,
    });
  }

  // Register webhook route
  async register(fastify: FastifyInstance): Promise<void> {
    // LINE webhook endpoint
    fastify.post('/webhook/line', {
      config: {
        rawBody: true,
      },
    }, async (request: FastifyRequest, reply: FastifyReply) => {
      try {
        // Verify signature
        const signature = request.headers['x-line-signature'] as string;
        const body = (request as FastifyRequest & { rawBody?: Buffer }).rawBody;

        if (!body) {
          return reply.status(400).send({ error: 'Missing body' });
        }

        // Process events
        const events = (request.body as { events?: WebhookEvent[] })?.events || [];

        // Process events asynchronously
        Promise.all(events.map(event => this.handleEvent(event)))
          .catch(err => logger.error(`Error processing LINE events: ${err}`));

        // Always return 200 OK immediately
        return reply.status(200).send({ status: 'OK' });
      } catch (error) {
        logger.error(`LINE webhook error: ${error}`);
        return reply.status(500).send({ error: 'Internal error' });
      }
    });

    logger.info('LINE channel registered at /webhook/line');
  }

  // Handle a single LINE event
  private async handleEvent(event: WebhookEvent): Promise<void> {
    if (event.type !== 'message' || event.message.type !== 'text') {
      // Only handle text messages for now
      return;
    }

    const userId = event.source.userId;
    const replyToken = event.replyToken;
    const userMessage = event.message.text;

    logger.info(`LINE message from ${userId}: ${userMessage}`);

    try {
      // Get response from agent
      const response = await this.agent.chat(
        [{ role: 'user', content: userMessage }],
        { nodeManager: this.nodeManager as unknown as undefined }
      );

      // Build reply messages
      const messages: (TextMessage | ImageMessage)[] = [];

      // Add text response
      if (response.content) {
        // Split long messages (LINE has 5000 char limit)
        const chunks = this.splitMessage(response.content, 4500);
        for (const chunk of chunks) {
          messages.push({
            type: 'text',
            text: chunk,
          });
        }
      }

      // Add images if present
      if (response.images && response.images.length > 0) {
        for (const image of response.images) {
          // Note: LINE requires images to be hosted URLs, not base64
          // In production, you'd upload the image and get a URL
          // For now, we'll skip images and mention they were captured
          messages.push({
            type: 'text',
            text: '[Image captured - see web dashboard for full image]',
          });
        }
      }

      // Send reply
      if (messages.length > 0) {
        await this.client.replyMessage(replyToken, messages);
        logger.debug(`Sent ${messages.length} messages to LINE user ${userId}`);
      }
    } catch (error) {
      logger.error(`Error handling LINE message: ${error}`);

      // Send error message
      try {
        await this.client.replyMessage(replyToken, {
          type: 'text',
          text: 'ขออภัย เกิดข้อผิดพลาด กรุณาลองใหม่อีกครั้ง\n(Sorry, an error occurred. Please try again.)',
        });
      } catch (replyError) {
        logger.error(`Failed to send error reply: ${replyError}`);
      }
    }
  }

  // Split long message into chunks
  private splitMessage(text: string, maxLength: number): string[] {
    if (text.length <= maxLength) {
      return [text];
    }

    const chunks: string[] = [];
    let remaining = text;

    while (remaining.length > 0) {
      if (remaining.length <= maxLength) {
        chunks.push(remaining);
        break;
      }

      // Find a good break point (newline or space)
      let breakPoint = remaining.lastIndexOf('\n', maxLength);
      if (breakPoint === -1 || breakPoint < maxLength * 0.5) {
        breakPoint = remaining.lastIndexOf(' ', maxLength);
      }
      if (breakPoint === -1 || breakPoint < maxLength * 0.5) {
        breakPoint = maxLength;
      }

      chunks.push(remaining.substring(0, breakPoint));
      remaining = remaining.substring(breakPoint).trim();
    }

    return chunks;
  }

  // Push message to a user (not in response to webhook)
  async pushMessage(userId: string, text: string): Promise<MessageAPIResponseBase> {
    return this.client.pushMessage(userId, {
      type: 'text',
      text,
    });
  }

  // Broadcast message to all followers
  async broadcast(text: string): Promise<MessageAPIResponseBase> {
    return this.client.broadcast({
      type: 'text',
      text,
    });
  }
}

export function createLineChannel(
  config: LineConfig,
  agent: CiraAgent,
  nodeManager: NodeManager
): LineChannel {
  return new LineChannel(config, agent, nodeManager);
}
