import { FastifyInstance, FastifyRequest, FastifyReply } from 'fastify';
import { NodeManager } from '../../nodes/manager.js';
import { NodeConfig, NodeConfigSchema } from '../../utils/config-schema.js';
import { RuleEngine, SavedRule } from '../../services/rule-engine.js';
import { StatsCollector } from '../../services/stats-collector.js';
import { CompositeRule } from '../../services/state-store.js';
import { createLogger } from '../../utils/logger.js';
import { randomUUID } from 'crypto';

// ─── Spec H: Bundle Types ─────────────────────────────────────────────────────

interface BundledAtomicRule {
  id: string;
  name: string;
  description: string;
  socket_type: string;
  reads: string[];
  produces: string[];
  enabled: boolean;
  created_at: string;
  created_by: string;
  prompt?: string;
  tags?: string[];
  code: string;
}

interface CiraBundle {
  bundle_format: string;
  bundle_id: string;
  name: string;
  description: string;
  tags: string[];
  exported_at: string;
  exported_by: string;
  cira_version: string;
  atomic_rules: BundledAtomicRule[];
  composite_rules: CompositeRule[];
}

const logger = createLogger('api-routes');

// Module-level references for rules API
let _ruleEngine: RuleEngine | null = null;
let _statsCollector: StatsCollector | null = null;

export function setRuleEngine(engine: RuleEngine): void {
  _ruleEngine = engine;
}

export function setStatsCollector(collector: StatsCollector): void {
  _statsCollector = collector;
}

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

        // Runtime may return malformed JSON with unescaped Windows backslashes
        // Read as text and fix before parsing
        const text = await response.text();

        // Fix Windows path backslashes by escaping single backslashes
        // that aren't part of valid JSON escape sequences
        let fixed = '';
        for (let i = 0; i < text.length; i++) {
          if (text[i] === '\\') {
            if (i + 1 < text.length) {
              const next = text[i + 1];
              // Check if this is a valid JSON escape sequence: \" \\ \/ \b \f \n \r \t \u
              if ('\\"\/bfnrtu'.includes(next)) {
                // Valid escape, copy both characters
                fixed += text[i] + next;
                i++; // Skip next char since we already added it
                continue;
              }
            }
            // Single backslash or invalid escape sequence, double it
            fixed += '\\\\';
          } else {
            fixed += text[i];
          }
        }

        const data = JSON.parse(fixed) as { models?: unknown[]; models_dir?: string };
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

  // =====================
  // Rules API Endpoints
  // =====================

  // List all rules
  fastify.get('/api/rules', async (_request: FastifyRequest, reply: FastifyReply) => {
    if (!_ruleEngine) {
      return reply.status(503).send({
        error: 'Rule engine not available',
        message: 'Rule engine has not been initialized',
      });
    }

    const rules = _ruleEngine.loadRules();
    return { rules };
  });

  // POST /api/rules — Create or replace an atomic rule (Spec H: required for bundle import)
  fastify.post<{ Body: Partial<SavedRule> }>(
    '/api/rules',
    async (request: FastifyRequest<{ Body: Partial<SavedRule> }>, reply: FastifyReply) => {
      if (!_ruleEngine) {
        return reply.status(503).send({ error: 'Rule engine not available' });
      }

      const body = request.body;

      if (!body.id || !body.code) {
        return reply.status(400).send({
          error: 'Invalid rule',
          message: 'Rule must have id and code',
        });
      }

      if (!/^[a-z0-9_-]+$/i.test(body.id)) {
        return reply.status(400).send({
          error: 'Invalid rule id',
          message: 'Rule id must contain only letters, numbers, hyphens and underscores',
        });
      }

      const rule: SavedRule = {
        id: body.id,
        name: body.name || body.id,
        description: body.description || '',
        socket_type: (body.socket_type as any) || 'any.boolean',
        reads: body.reads || [],
        produces: body.produces || [],
        code: body.code,
        enabled: body.enabled ?? false,
        created_at: body.created_at || new Date().toISOString(),
        created_by: body.created_by || 'import',
        prompt: body.prompt,
        tags: body.tags,
      };

      _ruleEngine.saveRule(rule);
      logger.info(`Rule ${rule.id} saved via POST /api/rules`);

      return reply.status(201).send({ success: true, id: rule.id, rule });
    }
  );

  // Get rule evaluation results (v3 API for Spec G)
  fastify.get('/api/rules/results', async (_request: FastifyRequest, _reply: FastifyReply) => {
    if (!_statsCollector) {
      // Return empty state if StatsCollector not available yet
      return { evaluated_at: '', results: {} };
    }

    return _statsCollector.getRuleResults();
  });

  // Get single rule
  fastify.get<{ Params: { id: string } }>(
    '/api/rules/:id',
    async (request: FastifyRequest<{ Params: { id: string } }>, reply: FastifyReply) => {
      if (!_ruleEngine) {
        return reply.status(503).send({
          error: 'Rule engine not available',
        });
      }

      const { id } = request.params;
      const rules = _ruleEngine.loadRules();
      const rule = rules.find(r => r.id === id);

      if (!rule) {
        return reply.status(404).send({
          error: 'Rule not found',
          message: `Rule with id '${id}' does not exist`,
        });
      }

      return rule;
    }
  );

  // Toggle rule enabled/disabled
  fastify.post<{ Params: { id: string }; Body: { enabled: boolean } }>(
    '/api/rules/:id/toggle',
    async (
      request: FastifyRequest<{ Params: { id: string }; Body: { enabled: boolean } }>,
      reply: FastifyReply
    ) => {
      if (!_ruleEngine) {
        return reply.status(503).send({
          error: 'Rule engine not available',
        });
      }

      const { id } = request.params;
      const { enabled } = request.body;

      const success = _ruleEngine.enableRule(id, enabled);

      if (!success) {
        return reply.status(404).send({
          error: 'Rule not found',
          message: `Rule with id '${id}' does not exist`,
        });
      }

      logger.info(`Rule ${id} ${enabled ? 'enabled' : 'disabled'} via API`);
      return { success: true, id, enabled };
    }
  );

  // Delete rule
  fastify.delete<{ Params: { id: string } }>(
    '/api/rules/:id',
    async (request: FastifyRequest<{ Params: { id: string } }>, reply: FastifyReply) => {
      if (!_ruleEngine) {
        return reply.status(503).send({
          error: 'Rule engine not available',
        });
      }

      const { id } = request.params;
      const success = _ruleEngine.deleteRule(id);

      if (!success) {
        return reply.status(404).send({
          error: 'Rule not found',
          message: `Rule with id '${id}' does not exist`,
        });
      }

      logger.info(`Rule ${id} deleted via API`);
      return { success: true, id };
    }
  );

  // Update rule code
  fastify.put<{ Params: { id: string }; Body: { code: string } }>(
    '/api/rules/:id',
    async (
      request: FastifyRequest<{ Params: { id: string }; Body: { code: string } }>,
      reply: FastifyReply
    ) => {
      if (!_ruleEngine) {
        return reply.status(503).send({
          error: 'Rule engine not available',
        });
      }

      const { id } = request.params;
      const { code } = request.body;

      if (!code || typeof code !== 'string') {
        return reply.status(400).send({
          error: 'Invalid request',
          message: 'Request body must include "code" string',
        });
      }

      // Find existing rule
      const rules = _ruleEngine.loadRules();
      const existingRule = rules.find(r => r.id === id);

      if (!existingRule) {
        return reply.status(404).send({
          error: 'Rule not found',
          message: `Rule with id '${id}' does not exist`,
        });
      }

      // Dry-run the code to validate it works
      const testPayload = {
        detections: [{ label: 'test', confidence: 0.9, x: 0.1, y: 0.1, w: 0.2, h: 0.2 }],
        frame: { number: 1, timestamp: new Date().toISOString(), width: 1920, height: 1080 },
        stats: { total_detections: 10, by_label: { test: 10 }, fps: 30, uptime_sec: 100, defects_per_hour: 5 },
        hourly: [{ hour: '10:00', detections: 5 }],
        node: { id: 'local-dev', status: 'online' },
      };

      const testRule = { ...existingRule, code };

      try {
        const result = await _ruleEngine.evaluateRule(testRule, testPayload);
        if (!result.success) {
          return reply.status(400).send({
            error: 'Code validation failed',
            message: result.error || 'Rule code failed dry-run test',
          });
        }
      } catch (err) {
        const errorMessage = err instanceof Error ? err.message : 'Unknown error';
        return reply.status(400).send({
          error: 'Code validation failed',
          message: errorMessage,
        });
      }

      // Update and save the rule
      existingRule.code = code;
      existingRule.created_by = 'manual'; // Mark as user-edited
      _ruleEngine.saveRule(existingRule);

      logger.info(`Rule ${id} code updated via API`);
      return { success: true, id, rule: existingRule };
    }
  );

  // ─── Spec H: Bundle Export / Import ────────────────────────────────────────

  // GET /api/export — Export rules as a self-contained .cira bundle
  // Query params:
  //   (none)                         → all atomic + all composite rules
  //   ?mode=composite&id=<id>        → one composite + its atomic dependencies
  //   ?mode=atomic                   → all atomic rules only, no composites
  fastify.get<{ Querystring: { mode?: string; id?: string } }>(
    '/api/export',
    async (
      request: FastifyRequest<{ Querystring: { mode?: string; id?: string } }>,
      reply: FastifyReply
    ) => {
      if (!_ruleEngine) {
        return reply.status(503).send({ error: 'Rule engine not available' });
      }

      const compositeEngine = _statsCollector?.getCompositeRuleEngine();
      if (!compositeEngine) {
        return reply.status(503).send({ error: 'Composite engine not available' });
      }

      const mode = request.query.mode || 'all';
      const allAtomicRules = _ruleEngine.loadRules();
      const allCompositeRules = compositeEngine.getAllCompositeRules();

      let atomicToExport: SavedRule[] = [];
      let compositeToExport: CompositeRule[] = [];

      if (mode === 'composite' && request.query.id) {
        const composite = compositeEngine.getCompositeRule(request.query.id);
        if (!composite) {
          return reply.status(404).send({ error: 'Composite rule not found' });
        }
        compositeToExport = [composite];

        // Collect only the atomic rule IDs referenced by this composite
        const referencedIds = new Set<string>(
          composite.nodes
            .filter(n => n.type === 'atomic')
            .map(n => (n.data as { rule_id: string }).rule_id)
            .filter(Boolean)
        );
        atomicToExport = allAtomicRules.filter(r => referencedIds.has(r.id));

      } else if (mode === 'atomic') {
        atomicToExport = allAtomicRules;
        compositeToExport = [];

      } else {
        // default: export everything
        atomicToExport = allAtomicRules;
        compositeToExport = allCompositeRules;
      }

      const bundleName = compositeToExport.length === 1
        ? compositeToExport[0].name
        : `CiRA Rules Export — ${atomicToExport.length} atomic, ${compositeToExport.length} composite`;

      const bundle: CiraBundle = {
        bundle_format: 'cira-recipe/1.0',
        bundle_id: randomUUID(),
        name: bundleName,
        description: compositeToExport.length === 1
          ? compositeToExport[0].description
          : `Exported from CiRA CLAW`,
        tags: [],
        exported_at: new Date().toISOString(),
        exported_by: 'api',
        cira_version: '1.0.0',
        atomic_rules: atomicToExport.map(r => ({
          id: r.id,
          name: r.name,
          description: r.description,
          socket_type: r.socket_type,
          reads: r.reads,
          produces: r.produces,
          enabled: r.enabled,
          created_at: r.created_at,
          created_by: r.created_by,
          ...(r.prompt ? { prompt: r.prompt } : {}),
          ...(r.tags?.length ? { tags: r.tags } : {}),
          code: r.code,
        })),
        composite_rules: compositeToExport,
      };

      // Suggest filename for browser download
      const safeName = bundleName.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/-+$/, '');
      reply.header('Content-Disposition', `attachment; filename="${safeName}.cira"`);
      reply.header('Content-Type', 'application/json');
      return bundle;
    }
  );

  // POST /api/import — Import a .cira bundle
  // Body: { bundle: CiraBundle, mode: 'merge' | 'overwrite' }
  //   merge (default): skip rules whose ID already exists
  //   overwrite: replace rules whose ID already exists
  // Imported rules are always set enabled: false regardless of bundle value.
  fastify.post<{ Body: { bundle: CiraBundle; mode?: 'merge' | 'overwrite' } }>(
    '/api/import',
    async (
      request: FastifyRequest<{ Body: { bundle: CiraBundle; mode?: 'merge' | 'overwrite' } }>,
      reply: FastifyReply
    ) => {
      if (!_ruleEngine) {
        return reply.status(503).send({ error: 'Rule engine not available' });
      }

      const compositeEngine = _statsCollector?.getCompositeRuleEngine();
      if (!compositeEngine) {
        return reply.status(503).send({ error: 'Composite engine not available' });
      }

      const { bundle, mode = 'merge' } = request.body;

      if (!bundle || bundle.bundle_format !== 'cira-recipe/1.0') {
        return reply.status(400).send({
          error: 'Invalid bundle',
          message: 'File is not a valid .cira bundle (bundle_format must be "cira-recipe/1.0")',
        });
      }

      const result = {
        atomic:    { imported: 0, skipped: 0, overwritten: 0 },
        composite: { imported: 0, skipped: 0, overwritten: 0 },
        errors: [] as string[],
      };

      // Import atomic rules
      const existingAtomicIds = new Set(_ruleEngine.loadRules().map(r => r.id));

      for (const bundledRule of (bundle.atomic_rules || [])) {
        try {
          if (!bundledRule.id || !bundledRule.code) {
            result.errors.push(`Skipped invalid atomic rule: missing id or code`);
            continue;
          }

          const exists = existingAtomicIds.has(bundledRule.id);

          if (exists && mode === 'merge') {
            result.atomic.skipped++;
            continue;
          }

          _ruleEngine.saveRule({
            id: bundledRule.id,
            name: bundledRule.name || bundledRule.id,
            description: bundledRule.description || '',
            socket_type: (bundledRule.socket_type as any) || 'any.boolean',
            reads: bundledRule.reads || [],
            produces: (bundledRule.produces || []) as any,
            code: bundledRule.code,
            enabled: false, // always import disabled
            created_at: bundledRule.created_at || new Date().toISOString(),
            created_by: bundledRule.created_by || 'import',
            prompt: bundledRule.prompt,
            tags: bundledRule.tags,
          });

          exists ? result.atomic.overwritten++ : result.atomic.imported++;
        } catch (err) {
          result.errors.push(
            `Atomic rule '${bundledRule.id}': ${err instanceof Error ? err.message : 'unknown error'}`
          );
        }
      }

      // Import composite rules
      const existingCompositeIds = new Set(compositeEngine.getAllCompositeRules().map(r => r.id));

      for (const composite of (bundle.composite_rules || [])) {
        try {
          if (!composite.id) {
            result.errors.push(`Skipped invalid composite rule: missing id`);
            continue;
          }

          const exists = existingCompositeIds.has(composite.id);

          if (exists && mode === 'merge') {
            result.composite.skipped++;
            continue;
          }

          compositeEngine.saveCompositeRule({
            ...composite,
            enabled: false, // always import disabled
          });

          exists ? result.composite.overwritten++ : result.composite.imported++;
        } catch (err) {
          result.errors.push(
            `Composite rule '${composite.id}': ${err instanceof Error ? err.message : 'unknown error'}`
          );
        }
      }

      logger.info(`Bundle import (${mode}): atomic ${JSON.stringify(result.atomic)}, composite ${JSON.stringify(result.composite)}`);

      return {
        success: result.errors.length === 0,
        result,
      };
    }
  );

  // =====================
  // Composite Rules API (Spec G) — /api/composite-rules/*
  // =====================

  // GET /api/composite-rules — List all composite rules
  fastify.get('/api/composite-rules', async (_request: FastifyRequest, reply: FastifyReply) => {
    const compositeEngine = _statsCollector?.getCompositeRuleEngine();
    if (!compositeEngine) {
      return reply.status(503).send({
        error: 'Composite rule engine not available',
        message: 'Composite rule engine has not been initialized',
      });
    }

    const rules = compositeEngine.getAllCompositeRules();
    return { rules };
  });

  // GET /api/composite-rules/results — Get all composite rule evaluation results
  fastify.get('/api/composite-rules/results', async (_request: FastifyRequest, _reply: FastifyReply) => {
    if (!_statsCollector) {
      return { evaluated_at: '', results: {} };
    }
    return _statsCollector.getCompositeRuleResults();
  });

  // GET /api/composite-rules/:id — Get single composite rule
  fastify.get<{ Params: { id: string } }>(
    '/api/composite-rules/:id',
    async (request: FastifyRequest<{ Params: { id: string } }>, reply: FastifyReply) => {
      const compositeEngine = _statsCollector?.getCompositeRuleEngine();
      if (!compositeEngine) {
        return reply.status(503).send({
          error: 'Composite rule engine not available',
        });
      }

      const { id } = request.params;
      const rule = compositeEngine.getCompositeRule(id);

      if (!rule) {
        return reply.status(404).send({
          error: 'Composite rule not found',
          message: `Composite rule with id '${id}' does not exist`,
        });
      }

      return rule;
    }
  );

  // POST /api/composite-rules — Create a new composite rule
  fastify.post<{ Body: CompositeRule }>(
    '/api/composite-rules',
    async (request: FastifyRequest<{ Body: CompositeRule }>, reply: FastifyReply) => {
      const compositeEngine = _statsCollector?.getCompositeRuleEngine();
      if (!compositeEngine) {
        return reply.status(503).send({
          error: 'Composite rule engine not available',
        });
      }

      const body = request.body;

      // Validate ID
      if (!body.id) {
        return reply.status(400).send({
          error: 'Invalid composite rule',
          message: 'Composite rule must have an id',
        });
      }

      // Check if rule already exists
      const existing = compositeEngine.getCompositeRule(body.id);
      if (existing) {
        return reply.status(409).send({
          error: 'Composite rule already exists',
          message: `Composite rule with id '${body.id}' already exists. Use PUT to update.`,
        });
      }

      // Build the rule object
      const rule: CompositeRule = {
        id: body.id,
        name: body.name || body.id,
        description: body.description || '',
        enabled: body.enabled ?? false,
        created_at: body.created_at || new Date().toISOString(),
        created_by: body.created_by || 'dashboard',
        nodes: body.nodes || [],
        connections: body.connections || [],
        output_action: body.output_action || { action: 'pass' },
      };

      // Validate graph (cycle detection)
      const validation = compositeEngine.validateGraph(rule);
      if (!validation.valid) {
        return reply.status(400).send({
          error: 'Invalid composite rule',
          message: validation.error,
        });
      }

      compositeEngine.saveCompositeRule(rule);
      logger.info(`Composite rule ${body.id} created via API`);

      return reply.status(201).send({ success: true, id: body.id, rule });
    }
  );

  // PUT /api/composite-rules/:id — Update an existing composite rule
  fastify.put<{ Params: { id: string }; Body: Partial<CompositeRule> }>(
    '/api/composite-rules/:id',
    async (
      request: FastifyRequest<{ Params: { id: string }; Body: Partial<CompositeRule> }>,
      reply: FastifyReply
    ) => {
      const compositeEngine = _statsCollector?.getCompositeRuleEngine();
      if (!compositeEngine) {
        return reply.status(503).send({
          error: 'Composite rule engine not available',
        });
      }

      const { id } = request.params;
      const body = request.body;

      // Build the rule object
      const existing = compositeEngine.getCompositeRule(id);
      const rule: CompositeRule = {
        id,
        name: body.name || existing?.name || id,
        description: body.description || existing?.description || '',
        enabled: body.enabled ?? existing?.enabled ?? true,
        created_at: existing?.created_at || new Date().toISOString(),
        created_by: body.created_by || existing?.created_by || 'dashboard',
        nodes: body.nodes || existing?.nodes || [],
        connections: body.connections || existing?.connections || [],
        output_action: body.output_action || existing?.output_action || { action: 'pass' },
      };

      // Validate graph (cycle detection)
      const validation = compositeEngine.validateGraph(rule);
      if (!validation.valid) {
        return reply.status(400).send({
          error: 'Invalid composite rule',
          message: validation.error,
        });
      }

      compositeEngine.saveCompositeRule(rule);
      logger.info(`Composite rule ${id} saved via API`);

      return { success: true, id, rule };
    }
  );

  // DELETE /api/composite-rules/:id — Delete a composite rule
  fastify.delete<{ Params: { id: string } }>(
    '/api/composite-rules/:id',
    async (request: FastifyRequest<{ Params: { id: string } }>, reply: FastifyReply) => {
      const compositeEngine = _statsCollector?.getCompositeRuleEngine();
      if (!compositeEngine) {
        return reply.status(503).send({
          error: 'Composite rule engine not available',
        });
      }

      const { id } = request.params;
      const success = compositeEngine.deleteCompositeRule(id);

      if (!success) {
        return reply.status(404).send({
          error: 'Composite rule not found',
          message: `Composite rule with id '${id}' does not exist`,
        });
      }

      logger.info(`Composite rule ${id} deleted via API`);
      return { success: true, id };
    }
  );

  // GET /api/composite-rules/:id/results — Get evaluation results for a specific rule
  fastify.get<{ Params: { id: string } }>(
    '/api/composite-rules/:id/results',
    async (request: FastifyRequest<{ Params: { id: string } }>, reply: FastifyReply) => {
      if (!_statsCollector) {
        return { evaluated_at: '', result: null };
      }

      const { id } = request.params;
      const results = _statsCollector.getCompositeRuleResults();
      const result = results.results?.[id] || null;

      if (!result) {
        return reply.status(404).send({
          error: 'Composite rule results not found',
          message: `No evaluation results for composite rule '${id}'`,
        });
      }

      return { evaluated_at: results.evaluated_at, result };
    }
  );

  // GET /api/composite-rules/:id/state — Get stateful node state for a composite rule
  fastify.get<{ Params: { id: string } }>(
    '/api/composite-rules/:id/state',
    async (request: FastifyRequest<{ Params: { id: string } }>, reply: FastifyReply) => {
      const compositeEngine = _statsCollector?.getCompositeRuleEngine();
      if (!compositeEngine) {
        return reply.status(503).send({
          error: 'Composite rule engine not available',
        });
      }

      const { id } = request.params;
      const rule = compositeEngine.getCompositeRule(id);

      if (!rule) {
        return reply.status(404).send({
          error: 'Composite rule not found',
          message: `Composite rule with id '${id}' does not exist`,
        });
      }

      const stateStore = compositeEngine.getStateStore();
      const states = stateStore.getAllStates(id);
      return { rule_id: id, states };
    }
  );

  // POST /api/composite-rules/:id/reset — Reset stateful node state for a composite rule
  fastify.post<{ Params: { id: string } }>(
    '/api/composite-rules/:id/reset',
    async (request: FastifyRequest<{ Params: { id: string } }>, reply: FastifyReply) => {
      const compositeEngine = _statsCollector?.getCompositeRuleEngine();
      if (!compositeEngine) {
        return reply.status(503).send({
          error: 'Composite rule engine not available',
        });
      }

      const { id } = request.params;
      const rule = compositeEngine.getCompositeRule(id);

      if (!rule) {
        return reply.status(404).send({
          error: 'Composite rule not found',
          message: `Composite rule with id '${id}' does not exist`,
        });
      }

      const stateStore = compositeEngine.getStateStore();
      stateStore.clearAllStates(id);
      logger.info(`Composite rule ${id} state reset via API`);
      return { success: true, id, message: 'Stateful node state cleared' };
    }
  );

  logger.info('API routes registered');
}
