import Fastify, { FastifyInstance } from 'fastify';
import fastifyWebsocket from '@fastify/websocket';
import fastifyCors from '@fastify/cors';
import fastifyStatic from '@fastify/static';
import path from 'path';
import fs from 'fs';
import { createLogger } from '../utils/logger.js';
import { CiraConfig } from '../utils/config-schema.js';

const logger = createLogger('gateway');

export interface GatewayServer {
  fastify: FastifyInstance;
  start(): Promise<void>;
  stop(): Promise<void>;
}

export async function createGatewayServer(config: CiraConfig): Promise<GatewayServer> {
  const fastify = Fastify({
    logger: false, // We use our own logger
  });

  // Register CORS
  await fastify.register(fastifyCors, {
    origin: true,
    credentials: true,
  });

  // Register WebSocket support
  await fastify.register(fastifyWebsocket, {
    options: {
      maxPayload: 1048576, // 1MB
    },
  });

  // Serve static dashboard files if they exist
  const dashboardPath = path.join(process.cwd(), 'dashboard', 'dist');
  const dashboardExists = fs.existsSync(dashboardPath);
  if (dashboardExists) {
    await fastify.register(fastifyStatic, {
      root: dashboardPath,
      prefix: '/',
      wildcard: false, // Disable wildcard to allow SPA fallback
    });
    logger.info(`Serving dashboard from ${dashboardPath}`);
  } else {
    // Serve a simple placeholder page
    fastify.get('/', async (_request, reply) => {
      reply.type('text/html').send(`
        <!DOCTYPE html>
        <html>
        <head>
          <title>CiRA Edge Gateway</title>
          <style>
            body { font-family: system-ui, sans-serif; max-width: 800px; margin: 0 auto; padding: 40px; }
            h1 { color: #2563eb; }
            .status { background: #f0fdf4; border: 1px solid #86efac; padding: 12px; border-radius: 8px; margin: 20px 0; }
            .api { background: #f8fafc; padding: 20px; border-radius: 8px; }
            code { background: #e2e8f0; padding: 2px 6px; border-radius: 4px; }
          </style>
        </head>
        <body>
          <h1>CiRA Edge Gateway</h1>
          <div class="status">
            <strong>Status:</strong> Running on port ${config.gateway.port}
          </div>
          <div class="api">
            <h3>API Endpoints</h3>
            <ul>
              <li><code>GET /api/status</code> - Gateway status</li>
              <li><code>GET /api/nodes</code> - List all nodes</li>
              <li><code>GET /api/nodes/:id</code> - Get node details</li>
              <li><code>GET /api/nodes/:id/snapshot</code> - Get camera snapshot</li>
              <li><code>WS /ws</code> - Real-time data stream</li>
              <li><code>WS /chat</code> - Chat with AI agent</li>
            </ul>
          </div>
        </body>
        </html>
      `);
    });
  }

  // Health check endpoint
  fastify.get('/health', async () => {
    return { status: 'healthy', timestamp: new Date().toISOString() };
  });

  // API status endpoint
  fastify.get('/api/status', async () => {
    return {
      name: config.gateway.name,
      version: '1.0.0',
      status: 'running',
      uptime: process.uptime(),
      timestamp: new Date().toISOString(),
      channels: {
        line: config.channels.line.enabled,
        mqtt: config.channels.mqtt.enabled,
        webchat: config.channels.webchat.enabled,
        modbus: config.channels.modbus.enabled,
      },
    };
  });

  // SPA fallback: serve index.html for client-side routing
  // This must be registered AFTER all API routes
  if (dashboardExists) {
    fastify.setNotFoundHandler(async (request, reply) => {
      // Only serve index.html for non-API routes
      if (request.url.startsWith('/api/') || request.url.startsWith('/ws') || request.url.startsWith('/chat')) {
        reply.code(404).send({ error: 'Not Found', message: `Route ${request.method}:${request.url} not found` });
        return;
      }
      // Serve index.html for SPA routes
      return reply.sendFile('index.html');
    });
  }

  return {
    fastify,
    async start() {
      const address = await fastify.listen({
        port: config.gateway.port,
        host: config.gateway.host,
      });
      logger.info(`Gateway server listening at ${address}`);
    },
    async stop() {
      await fastify.close();
      logger.info('Gateway server stopped');
    },
  };
}
