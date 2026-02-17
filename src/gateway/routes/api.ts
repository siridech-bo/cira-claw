import { FastifyInstance, FastifyRequest, FastifyReply } from 'fastify';
import { NodeManager } from '../../nodes/manager.js';
import { NodeConfig, NodeConfigSchema } from '../../utils/config-schema.js';
import { createLogger } from '../../utils/logger.js';

const logger = createLogger('api-routes');

interface NodeParams {
  id: string;
}

interface AddNodeBody {
  id: string;
  name: string;
  type: string;
  host: string;
  ssh?: {
    user?: string;
    key?: string;
    password?: string;
    port?: number;
  };
  runtime?: {
    port?: number;
    config?: string;
  };
  cameras?: Array<{
    id: string;
    device?: number;
    name: string;
    resolution?: string;
  }>;
  models?: Array<{
    name: string;
    task: string;
    labels: string[];
  }>;
  location?: string;
}

export async function registerApiRoutes(
  fastify: FastifyInstance,
  nodeManager: NodeManager
): Promise<void> {
  // List all nodes
  fastify.get('/api/nodes', async (_request: FastifyRequest, _reply: FastifyReply) => {
    const nodes = nodeManager.getAllNodes();
    const statuses = nodeManager.getAllStatuses();

    const nodesWithStatus = nodes.map(node => {
      const status = statuses.find(s => s.id === node.id);
      return {
        ...node,
        status: status?.status || 'unknown',
        lastSeen: status?.lastSeen || null,
        metrics: status?.metrics || null,
        inference: status?.inference || null,
      };
    });

    return {
      nodes: nodesWithStatus,
      summary: nodeManager.getSummary(),
    };
  });

  // Get single node details
  fastify.get<{ Params: NodeParams }>(
    '/api/nodes/:id',
    async (request: FastifyRequest<{ Params: NodeParams }>, reply: FastifyReply) => {
      const { id } = request.params;
      const node = nodeManager.getNode(id);

      if (!node) {
        return reply.status(404).send({
          error: 'Node not found',
          message: `Node with id '${id}' does not exist`,
        });
      }

      const status = nodeManager.getNodeStatus(id);

      return {
        ...node,
        status: status?.status || 'unknown',
        lastSeen: status?.lastSeen || null,
        metrics: status?.metrics || null,
        inference: status?.inference || null,
        error: status?.error || null,
      };
    }
  );

  // Add a new node
  fastify.post<{ Body: AddNodeBody }>(
    '/api/nodes',
    async (request: FastifyRequest<{ Body: AddNodeBody }>, reply: FastifyReply) => {
      const body = request.body;

      // Validate node configuration
      const result = NodeConfigSchema.safeParse(body);
      if (!result.success) {
        return reply.status(400).send({
          error: 'Invalid node configuration',
          details: result.error.errors.map(e => ({
            path: e.path.join('.'),
            message: e.message,
          })),
        });
      }

      const node: NodeConfig = result.data;

      // Check if node already exists
      if (nodeManager.getNode(node.id)) {
        return reply.status(409).send({
          error: 'Node already exists',
          message: `Node with id '${node.id}' already exists`,
        });
      }

      await nodeManager.addNode(node);
      logger.info(`Added new node via API: ${node.id}`);

      return reply.status(201).send({
        message: 'Node added successfully',
        node,
      });
    }
  );

  // Update a node
  fastify.put<{ Params: NodeParams; Body: Partial<AddNodeBody> }>(
    '/api/nodes/:id',
    async (
      request: FastifyRequest<{ Params: NodeParams; Body: Partial<AddNodeBody> }>,
      reply: FastifyReply
    ) => {
      const { id } = request.params;
      const existingNode = nodeManager.getNode(id);

      if (!existingNode) {
        return reply.status(404).send({
          error: 'Node not found',
          message: `Node with id '${id}' does not exist`,
        });
      }

      // Merge with existing config
      const updatedConfig = {
        ...existingNode,
        ...request.body,
        id, // Ensure ID cannot be changed
      };

      const result = NodeConfigSchema.safeParse(updatedConfig);
      if (!result.success) {
        return reply.status(400).send({
          error: 'Invalid node configuration',
          details: result.error.errors.map(e => ({
            path: e.path.join('.'),
            message: e.message,
          })),
        });
      }

      await nodeManager.addNode(result.data); // saveNode is called internally
      logger.info(`Updated node via API: ${id}`);

      return {
        message: 'Node updated successfully',
        node: result.data,
      };
    }
  );

  // Delete a node
  fastify.delete<{ Params: NodeParams }>(
    '/api/nodes/:id',
    async (request: FastifyRequest<{ Params: NodeParams }>, reply: FastifyReply) => {
      const { id } = request.params;

      const removed = await nodeManager.removeNode(id);
      if (!removed) {
        return reply.status(404).send({
          error: 'Node not found',
          message: `Node with id '${id}' does not exist`,
        });
      }

      logger.info(`Deleted node via API: ${id}`);
      return { message: 'Node deleted successfully' };
    }
  );

  // Get node status/health
  fastify.get<{ Params: NodeParams }>(
    '/api/nodes/:id/status',
    async (request: FastifyRequest<{ Params: NodeParams }>, reply: FastifyReply) => {
      const { id } = request.params;
      const node = nodeManager.getNode(id);

      if (!node) {
        return reply.status(404).send({
          error: 'Node not found',
          message: `Node with id '${id}' does not exist`,
        });
      }

      // Perform fresh health check
      const status = await nodeManager.checkNodeHealth(node);
      return status;
    }
  );

  // Get node snapshot
  fastify.get<{ Params: NodeParams; Querystring: { annotated?: string } }>(
    '/api/nodes/:id/snapshot',
    async (
      request: FastifyRequest<{ Params: NodeParams; Querystring: { annotated?: string } }>,
      reply: FastifyReply
    ) => {
      const { id } = request.params;
      const annotated = request.query.annotated !== 'false';
      const node = nodeManager.getNode(id);

      if (!node) {
        return reply.status(404).send({
          error: 'Node not found',
          message: `Node with id '${id}' does not exist`,
        });
      }

      try {
        // Proxy snapshot from the node's runtime
        const endpoint = annotated ? '/snapshot?annotated=true' : '/snapshot';
        const url = `http://${node.host}:${node.runtime.port}${endpoint}`;

        const controller = new AbortController();
        const timeout = setTimeout(() => controller.abort(), 10000);

        const response = await fetch(url, { signal: controller.signal });
        clearTimeout(timeout);

        if (!response.ok) {
          return reply.status(502).send({
            error: 'Failed to get snapshot from node',
            message: `Node returned HTTP ${response.status}`,
          });
        }

        const contentType = response.headers.get('content-type') || 'image/jpeg';
        const buffer = Buffer.from(await response.arrayBuffer());

        reply.type(contentType).send(buffer);
      } catch (error) {
        const errorMessage = error instanceof Error ? error.message : 'Unknown error';
        return reply.status(502).send({
          error: 'Failed to connect to node',
          message: errorMessage,
        });
      }
    }
  );

  // Get stream URL for a node
  fastify.get<{ Params: NodeParams }>(
    '/api/nodes/:id/stream',
    async (request: FastifyRequest<{ Params: NodeParams }>, reply: FastifyReply) => {
      const { id } = request.params;
      const node = nodeManager.getNode(id);

      if (!node) {
        return reply.status(404).send({
          error: 'Node not found',
          message: `Node with id '${id}' does not exist`,
        });
      }

      return {
        nodeId: id,
        streams: {
          raw: `http://${node.host}:${node.runtime.port}/stream/raw`,
          annotated: `http://${node.host}:${node.runtime.port}/stream/annotated`,
          websocket: `ws://${node.host}:${node.runtime.port}/ws/video`,
        },
      };
    }
  );

  // Trigger node reboot
  fastify.post<{ Params: NodeParams }>(
    '/api/nodes/:id/reboot',
    async (request: FastifyRequest<{ Params: NodeParams }>, reply: FastifyReply) => {
      const { id } = request.params;
      const node = nodeManager.getNode(id);

      if (!node) {
        return reply.status(404).send({
          error: 'Node not found',
          message: `Node with id '${id}' does not exist`,
        });
      }

      // Update status to indicate reboot in progress
      nodeManager.updateStatus(id, { status: 'updating', error: 'Rebooting...' });

      // Note: Actual SSH reboot would be implemented in the SSH manager
      // For now, return success
      logger.info(`Reboot requested for node: ${id}`);

      return {
        message: 'Reboot initiated',
        nodeId: id,
      };
    }
  );

  logger.info('API routes registered');
}
