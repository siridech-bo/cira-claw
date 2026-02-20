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

  // Get available models for a node
  fastify.get<{ Params: NodeParams }>(
    '/api/nodes/:id/models',
    async (request: FastifyRequest<{ Params: NodeParams }>, reply: FastifyReply) => {
      const { id } = request.params;
      const node = nodeManager.getNode(id);

      if (!node) {
        return reply.status(404).send({
          error: 'Node not found',
          message: `Node with id '${id}' does not exist`,
        });
      }

      try {
        // Proxy models list from the node's runtime
        const url = `http://${node.host}:${node.runtime.port}/api/models`;

        const controller = new AbortController();
        const timeout = setTimeout(() => controller.abort(), 10000);

        const response = await fetch(url, { signal: controller.signal });
        clearTimeout(timeout);

        if (!response.ok) {
          // Return node's configured models as fallback
          return {
            nodeId: id,
            current: nodeManager.getNodeStatus(id)?.inference?.modelName || null,
            available: node.models || [],
            source: 'config',
          };
        }

        const data = await response.json() as { models?: unknown[]; models_dir?: string };
        return {
          nodeId: id,
          current: nodeManager.getNodeStatus(id)?.inference?.modelName || null,
          available: data.models || [],
          modelsDir: data.models_dir || '',
          source: 'runtime',
        };
      } catch (error) {
        // Return node's configured models as fallback
        return {
          nodeId: id,
          current: nodeManager.getNodeStatus(id)?.inference?.modelName || null,
          available: node.models || [],
          source: 'config',
          error: error instanceof Error ? error.message : 'Connection failed',
        };
      }
    }
  );

  // Switch model on a node
  fastify.post<{ Params: NodeParams; Body: { path: string } }>(
    '/api/nodes/:id/model',
    async (
      request: FastifyRequest<{ Params: NodeParams; Body: { path: string } }>,
      reply: FastifyReply
    ) => {
      const { id } = request.params;
      const { path } = request.body;
      const node = nodeManager.getNode(id);

      if (!node) {
        return reply.status(404).send({
          error: 'Node not found',
          message: `Node with id '${id}' does not exist`,
        });
      }

      if (!path) {
        return reply.status(400).send({
          error: 'Missing model path',
          message: 'Request body must include a "path" field',
        });
      }

      try {
        // Proxy model switch to the node's runtime
        const url = `http://${node.host}:${node.runtime.port}/api/model`;

        const controller = new AbortController();
        const timeout = setTimeout(() => controller.abort(), 30000); // 30s for model loading

        const response = await fetch(url, {
          method: 'POST',
          headers: { 'Content-Type': 'application/json' },
          body: JSON.stringify({ path }),
          signal: controller.signal,
        });
        clearTimeout(timeout);

        const data = await response.json() as {
          success?: boolean;
          error?: string;
          format?: string;
          model?: string;
        };

        if (!response.ok || !data.success) {
          return reply.status(502).send({
            error: 'Model switch failed',
            message: data.error || `Node returned HTTP ${response.status}`,
          });
        }

        logger.info(`Model switched on node ${id}: ${path}`);

        // Update inference status with new model name
        const status = nodeManager.getNodeStatus(id);
        if (status?.inference) {
          nodeManager.updateStatus(id, {
            inference: {
              ...status.inference,
              modelName: data.format || path.split('/').pop() || 'unknown',
            },
          });
        }

        return {
          success: true,
          nodeId: id,
          model: data.model,
          format: data.format,
        };
      } catch (error) {
        const errorMessage = error instanceof Error ? error.message : 'Unknown error';
        logger.error(`Failed to switch model on node ${id}: ${errorMessage}`);
        return reply.status(502).send({
          error: 'Failed to connect to node',
          message: errorMessage,
        });
      }
    }
  );

  // =====================
  // Utility Endpoints
  // =====================

  // Convert Darknet model to NCNN
  fastify.post<{ Body: { sourcePath: string; targetFormat: string; darknet2ncnnPath?: string } }>(
    '/api/utility/convert-model',
    async (request, reply) => {
      const { sourcePath, targetFormat, darknet2ncnnPath = 'darknet2ncnn' } = request.body;

      if (!sourcePath) {
        return reply.status(400).send({
          error: 'Missing source path',
          message: 'Request body must include "sourcePath"',
        });
      }

      if (targetFormat !== 'ncnn') {
        return reply.status(400).send({
          error: 'Unsupported target format',
          message: 'Only "ncnn" format is currently supported',
        });
      }

      try {
        const fs = await import('fs');
        const path = await import('path');
        const { exec } = await import('child_process');
        const { promisify } = await import('util');
        const execAsync = promisify(exec);

        // Validate source directory exists
        if (!fs.existsSync(sourcePath)) {
          return reply.status(400).send({
            error: 'Source path not found',
            message: `Directory does not exist: ${sourcePath}`,
          });
        }

        // Find .cfg and .weights files
        const files = fs.readdirSync(sourcePath);
        const cfgFile = files.find((f: string) => f.endsWith('.cfg'));
        const weightsFile = files.find((f: string) => f.endsWith('.weights'));

        if (!cfgFile || !weightsFile) {
          return reply.status(400).send({
            error: 'Invalid Darknet model',
            message: 'Directory must contain .cfg and .weights files',
          });
        }

        // Find labels file
        const labelsFile = files.find((f: string) =>
          f === 'obj.names' || f === 'labels.txt' || f.endsWith('.names')
        );

        // Create output directory
        const modelName = path.basename(sourcePath).replace('-darknet', '');
        const outputDir = path.join(path.dirname(sourcePath), `${modelName}-ncnn`);

        if (!fs.existsSync(outputDir)) {
          fs.mkdirSync(outputDir, { recursive: true });
        }

        const cfgPath = path.join(sourcePath, cfgFile);
        const weightsPath = path.join(sourcePath, weightsFile);
        const paramPath = path.join(outputDir, `${modelName}.param`);
        const binPath = path.join(outputDir, `${modelName}.bin`);

        // Run darknet2ncnn
        const cmd = `"${darknet2ncnnPath}" "${cfgPath}" "${weightsPath}" "${paramPath}" "${binPath}"`;
        logger.info(`Running: ${cmd}`);

        await execAsync(cmd, { timeout: 120000 }); // 2 minute timeout

        // Copy labels file
        if (labelsFile) {
          const srcLabels = path.join(sourcePath, labelsFile);
          const dstLabels = path.join(outputDir, 'obj.names');
          fs.copyFileSync(srcLabels, dstLabels);
        }

        // Create cira_model.json manifest
        const manifest = {
          name: modelName,
          description: `Converted from Darknet: ${cfgFile}`,
          yolo_version: cfgFile.includes('v4') ? 'yolov4' : cfgFile.includes('v3') ? 'yolov3' : 'yolov4',
          input_size: 416, // Default, could parse from cfg
          num_classes: 80, // Default, could count from labels
          confidence_threshold: 0.25,
          nms_threshold: 0.45,
        };

        fs.writeFileSync(
          path.join(outputDir, 'cira_model.json'),
          JSON.stringify(manifest, null, 4)
        );

        logger.info(`Model converted successfully: ${outputDir}`);

        return {
          success: true,
          outputPath: outputDir,
          files: {
            param: paramPath,
            bin: binPath,
            manifest: path.join(outputDir, 'cira_model.json'),
          },
        };
      } catch (error) {
        const errorMessage = error instanceof Error ? error.message : 'Unknown error';
        logger.error(`Model conversion failed: ${errorMessage}`);
        return reply.status(500).send({
          error: 'Conversion failed',
          message: errorMessage,
        });
      }
    }
  );

  logger.info('API routes registered');
}
