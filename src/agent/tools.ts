import fs from 'fs';
import path from 'path';
import os from 'os';
import { NodeSSH } from 'node-ssh';
import { createLogger } from '../utils/logger.js';
import { RuleEngine, RulePayload, SavedRule } from '../services/rule-engine.js';
import { StateStore, CompositeRule, CompositeNode, CompositeConnection, OutputAction } from '../services/state-store.js';
import { CompositeRuleEngine } from '../services/composite-rule-engine.js';

const logger = createLogger('tools');

// Get workspace path (same logic as config.ts)
function getWorkspacePath(): string {
  const ciraHome = process.env.CIRA_HOME || path.join(os.homedir(), '.cira');
  return path.join(ciraHome, 'workspace');
}

export interface Tool {
  name: string;
  description: string;
  inputSchema: {
    type: 'object';
    properties: Record<string, unknown>;
    required?: string[];
  };
}

export interface ToolResult {
  data: unknown;
  images?: string[];
}

export interface NodeStats {
  nodeId: string;
  timestamp: string;
  totalDetections: number;
  totalFrames: number;
  fps: number;
  uptimeSec: number;
  modelLoaded: boolean;
  byLabel: Record<string, number>;
}

export interface AccumulatedStats {
  nodeId: string;
  hourStart: string;
  detections: number;
  frames: number;
  avgFps: number;
  samples: number;
}

export interface AlertsConfig {
  defect_threshold: number;
  temperature_max: number;
  fps_min: number;
  notify_channels: string[];
}

// ─── Spec D Stub — Heartbeat Scheduler (not yet implemented) ─────────────────
// When Spec D lands, inject HeartbeatScheduler here.
// Rule evaluation scheduling migrates from StatsCollector to this.
export interface HeartbeatScheduler {
  scheduleRuleEvaluation(nodeId: string, intervalMs: number): void;
  // Full interface defined in Spec D
}

// ─── Spec E Stub — Memory Manager (not yet implemented) ──────────────────────
// When Spec E lands, inject MemoryManager here.
// Tool handlers that need memory context check this field before using it.
export interface MemoryManager {
  getContext(nodeId: string): Promise<string | null>;
  // Full interface defined in Spec E
}

export interface ToolContext {
  nodeManager?: {
    getAllNodes: () => Array<{ id: string; name: string; host: string; type: string }>;
    getNode: (id: string) => {
      id: string;
      name: string;
      host: string;
      runtime?: { port: number };
      ssh?: { user?: string; port?: number; key?: string; password?: string };
    } | undefined;
    getNodeStatus: (id: string) => { status: string; metrics?: Record<string, unknown>; inference?: Record<string, unknown> } | undefined;
    getAllStatuses: () => Array<{ id: string; status: string; metrics?: Record<string, unknown> }>;
    getSummary: () => { total: number; online: number; offline: number };
    checkNodeHealth: (node: unknown) => Promise<{ status: string }>;
  };
  statsCollector?: {
    getCurrentStats: (nodeId: string) => NodeStats | undefined;
    getAllCurrentStats: () => NodeStats[];
    getDailySummary: () => Promise<Record<string, { totalDetections: number; hours: AccumulatedStats[] }>>;
    buildPayload: (nodeId: string) => Promise<RulePayload | null>;
  };
  alertsConfig?: AlertsConfig;
  ruleEngine?: RuleEngine;
  // Spec G: Composite rule engine
  compositeRuleEngine?: CompositeRuleEngine;
  // Spec D stub — Heartbeat Scheduler (not yet implemented)
  heartbeatScheduler?: HeartbeatScheduler;
  // Spec E stub — Memory Manager (not yet implemented)
  memoryManager?: MemoryManager;
}

// Tool definitions
const tools: Tool[] = [
  {
    name: 'node_list',
    description: 'List all registered edge devices with their current status',
    inputSchema: {
      type: 'object',
      properties: {},
    },
  },
  {
    name: 'node_query',
    description: 'Get detailed status of a specific node including metrics and inference stats',
    inputSchema: {
      type: 'object',
      properties: {
        node_id: {
          type: 'string',
          description: 'The ID of the node to query',
        },
      },
      required: ['node_id'],
    },
  },
  {
    name: 'node_status',
    description: 'Quick health check of a node, returns only essential metrics',
    inputSchema: {
      type: 'object',
      properties: {
        node_id: {
          type: 'string',
          description: 'The ID of the node to check',
        },
      },
      required: ['node_id'],
    },
  },
  {
    name: 'camera_snapshot',
    description: 'Capture a snapshot from a node camera. Returns both raw and annotated images.',
    inputSchema: {
      type: 'object',
      properties: {
        node_id: {
          type: 'string',
          description: 'The ID of the node to capture from',
        },
        annotated: {
          type: 'boolean',
          description: 'Whether to return annotated image (default: true)',
        },
      },
      required: ['node_id'],
    },
  },
  {
    name: 'camera_stream_url',
    description: 'Get MJPEG stream URLs for a node',
    inputSchema: {
      type: 'object',
      properties: {
        node_id: {
          type: 'string',
          description: 'The ID of the node',
        },
      },
      required: ['node_id'],
    },
  },
  {
    name: 'inference_stats',
    description: 'Get inference statistics for a node over a time period',
    inputSchema: {
      type: 'object',
      properties: {
        node_id: {
          type: 'string',
          description: 'The ID of the node',
        },
        period: {
          type: 'string',
          description: 'Time period: "1h", "today", "yesterday", "7d"',
          enum: ['1h', 'today', 'yesterday', '7d'],
        },
      },
      required: ['node_id', 'period'],
    },
  },
  {
    name: 'inference_results',
    description: 'Get detailed inference results for a node over a time period',
    inputSchema: {
      type: 'object',
      properties: {
        node_id: {
          type: 'string',
          description: 'The ID of the node',
        },
        period: {
          type: 'string',
          description: 'Time period: "1h", "today", "yesterday", "7d"',
        },
      },
      required: ['node_id', 'period'],
    },
  },
  {
    name: 'model_list',
    description: 'List all available models in the workspace',
    inputSchema: {
      type: 'object',
      properties: {},
    },
  },
  {
    name: 'model_deploy',
    description: 'Deploy a model to a device',
    inputSchema: {
      type: 'object',
      properties: {
        model_name: {
          type: 'string',
          description: 'Name of the model to deploy',
        },
        node_id: {
          type: 'string',
          description: 'ID of the target node',
        },
      },
      required: ['model_name', 'node_id'],
    },
  },
  {
    name: 'alert_list',
    description: 'List all active alert rules',
    inputSchema: {
      type: 'object',
      properties: {},
    },
  },
  {
    name: 'alert_history',
    description: 'Get alert history for a time period',
    inputSchema: {
      type: 'object',
      properties: {
        period: {
          type: 'string',
          description: 'Time period: "1h", "today", "yesterday", "7d"',
        },
      },
      required: ['period'],
    },
  },
  {
    name: 'js_query',
    description: 'Execute a one-shot JavaScript query against current detection data and stats. Use when the user asks a question requiring custom data analysis that other tools cannot answer. The code receives a "payload" object with detections, stats, and hourly data. Return the answer as the last expression.',
    inputSchema: {
      type: 'object',
      properties: {
        code: {
          type: 'string',
          description: 'JavaScript code to execute. Has access to payload.detections (array of {label, confidence, x, y, w, h}), payload.stats ({total_detections, by_label, fps, uptime_sec, defects_per_hour}), payload.hourly (array of {hour, detections}). Must return a JSON-serializable value.',
        },
        node_id: {
          type: 'string',
          description: 'The node to query data from (default: "local-dev")',
        },
      },
      required: ['code'],
    },
  },
  {
    name: 'js_rule_create',
    description: 'Create a persistent JavaScript rule that runs automatically on each evaluation cycle. The code must export a function receiving "payload" and returning: { action: "pass"|"reject"|"alert"|"log"|"modbus_write", reason?: string, register?: number, value?: number, severity?: "info"|"warning"|"critical", message?: string }. Rule is dry-run validated before saving. IMPORTANT: You must specify socket_type, reads, and produces based on what the code accesses.',
    inputSchema: {
      type: 'object',
      properties: {
        name: {
          type: 'string',
          description: 'Filename-safe lowercase-hyphenated name',
        },
        description: {
          type: 'string',
          description: 'Human-readable description of what the rule does',
        },
        code: {
          type: 'string',
          description: 'JavaScript as module.exports = function(payload) { ... }. Synchronous, stateless, no require.',
        },
        node_id: {
          type: 'string',
          description: 'Node this rule applies to (default: "local-dev")',
        },
        socket_type: {
          type: 'string',
          enum: ['vision.detection', 'vision.confidence', 'signal.threshold', 'signal.rate', 'system.health', 'any.boolean'],
          description: 'Signal category this rule evaluates. INFER from which payload fields the code accesses. vision.detection for detections[]/by_label; vision.confidence for detections[].confidence; signal.threshold for stats.fps/uptime; signal.rate for defects_per_hour/hourly; system.health for node.status/frame.number; any.boolean otherwise.',
        },
        reads: {
          type: 'array',
          items: { type: 'string' },
          description: 'Payload field paths this rule reads. Use dot notation: ["detections.length", "stats.fps"]. For array element access: "detections[].label".',
        },
        produces: {
          type: 'array',
          items: { type: 'string', enum: ['pass', 'reject', 'alert', 'log', 'modbus_write'] },
          description: 'Action types this rule can return. Inspect every return statement in the code and list unique action values.',
        },
        tags: {
          type: 'array',
          items: { type: 'string' },
          description: 'Optional tags for rule management e.g. ["reject-logic", "defect-rate"]',
        },
      },
      required: ['name', 'description', 'code', 'socket_type', 'reads', 'produces'],
    },
  },
  {
    name: 'js_rule_list',
    description: 'List, enable, disable, or delete persistent rules. Can filter by tag or signal_type.',
    inputSchema: {
      type: 'object',
      properties: {
        action: {
          type: 'string',
          enum: ['list', 'enable', 'disable', 'delete'],
          description: 'Action to perform',
        },
        rule_id: {
          type: 'string',
          description: 'Rule ID (required for enable/disable/delete)',
        },
        filter_tag: {
          type: 'string',
          description: 'Optional: filter list by tag',
        },
        filter_signal_type: {
          type: 'string',
          description: 'Optional: filter list by signal_type',
        },
      },
      required: ['action'],
    },
  },
  // Spec G: Composite rule tools
  {
    name: 'composite_rule_list',
    description: 'List all composite rules (Spec G). Composite rules combine atomic rules via logic gates (AND, OR, NOT) to form complex conditions.',
    inputSchema: {
      type: 'object',
      properties: {
        enabled_only: {
          type: 'boolean',
          description: 'If true, only return enabled composite rules',
        },
      },
    },
  },
  {
    name: 'composite_rule_create',
    description: 'Create a composite rule that combines multiple atomic rules via logic gates. The rule graph is edited visually in the Rule Graph dashboard page. This tool creates an empty composite rule skeleton that the operator can then configure in the UI.',
    inputSchema: {
      type: 'object',
      properties: {
        name: {
          type: 'string',
          description: 'Human-readable name for the composite rule',
        },
        description: {
          type: 'string',
          description: 'Description of what this composite rule does',
        },
        output_action: {
          type: 'string',
          enum: ['pass', 'reject', 'alert', 'log', 'modbus_write'],
          description: 'Default action when composite triggers (can be overridden in output nodes)',
        },
      },
      required: ['name', 'description'],
    },
  },
  {
    name: 'composite_rule_toggle',
    description: 'Enable or disable a composite rule',
    inputSchema: {
      type: 'object',
      properties: {
        rule_id: {
          type: 'string',
          description: 'ID of the composite rule',
        },
        enabled: {
          type: 'boolean',
          description: 'Whether to enable (true) or disable (false) the rule',
        },
      },
      required: ['rule_id', 'enabled'],
    },
  },
  {
    name: 'composite_rule_delete',
    description: 'Delete a composite rule',
    inputSchema: {
      type: 'object',
      properties: {
        rule_id: {
          type: 'string',
          description: 'ID of the composite rule to delete',
        },
      },
      required: ['rule_id'],
    },
  },
];

export function getToolDefinitions(): Tool[] {
  return tools;
}

// Tool executor
export async function executeToolCall(
  toolName: string,
  input: Record<string, unknown>,
  context?: ToolContext
): Promise<ToolResult> {
  logger.debug({ input }, `Executing tool: ${toolName}`);

  const nodeManager = context?.nodeManager;

  switch (toolName) {
    case 'node_list': {
      if (!nodeManager) {
        return { data: { error: 'Node manager not available' } };
      }

      const nodes = nodeManager.getAllNodes();
      const statuses = nodeManager.getAllStatuses();
      const summary = nodeManager.getSummary();

      const nodesWithStatus = nodes.map(node => {
        const status = statuses.find(s => s.id === node.id);
        return {
          id: node.id,
          name: node.name,
          type: node.type,
          host: node.host,
          status: status?.status || 'unknown',
          fps: status?.metrics?.fps ?? null,
          temperature: status?.metrics?.temperature ?? null,
        };
      });

      return {
        data: {
          nodes: nodesWithStatus,
          summary,
        },
      };
    }

    case 'node_query': {
      const nodeId = input.node_id as string;
      if (!nodeManager) {
        return { data: { error: 'Node manager not available' } };
      }

      const node = nodeManager.getNode(nodeId);
      if (!node) {
        return { data: { error: `Node '${nodeId}' not found` } };
      }

      const status = nodeManager.getNodeStatus(nodeId);

      return {
        data: {
          ...node,
          status: status?.status || 'unknown',
          metrics: status?.metrics || null,
          inference: status?.inference || null,
        },
      };
    }

    case 'node_status': {
      const nodeId = input.node_id as string;
      if (!nodeManager) {
        return { data: { error: 'Node manager not available' } };
      }

      const node = nodeManager.getNode(nodeId);
      if (!node) {
        return { data: { error: `Node '${nodeId}' not found` } };
      }

      // Perform fresh health check
      const status = await nodeManager.checkNodeHealth(node);

      return {
        data: {
          node_id: nodeId,
          ...status,
        },
      };
    }

    case 'camera_snapshot': {
      const nodeId = input.node_id as string;
      const annotated = input.annotated !== false;

      if (!nodeManager) {
        return { data: { error: 'Node manager not available' } };
      }

      const node = nodeManager.getNode(nodeId);
      if (!node) {
        return { data: { error: `Node '${nodeId}' not found` } };
      }

      const status = nodeManager.getNodeStatus(nodeId);
      if (status?.status !== 'online') {
        return { data: { error: `Node '${nodeId}' is not online` } };
      }

      try {
        const endpoint = annotated ? '/snapshot?annotated=true' : '/snapshot';
        const url = `http://${node.host}:${node.runtime?.port || 8080}${endpoint}`;

        const controller = new AbortController();
        const timeout = setTimeout(() => controller.abort(), 10000);

        const response = await fetch(url, { signal: controller.signal });
        clearTimeout(timeout);

        if (!response.ok) {
          return { data: { error: `Failed to get snapshot: HTTP ${response.status}` } };
        }

        const buffer = Buffer.from(await response.arrayBuffer());
        const base64Image = `data:image/jpeg;base64,${buffer.toString('base64')}`;

        return {
          data: {
            node_id: nodeId,
            timestamp: new Date().toISOString(),
            annotated,
          },
          images: [base64Image],
        };
      } catch (error) {
        const errorMsg = error instanceof Error ? error.message : 'Unknown error';
        return { data: { error: `Failed to capture snapshot: ${errorMsg}` } };
      }
    }

    case 'camera_stream_url': {
      const nodeId = input.node_id as string;

      if (!nodeManager) {
        return { data: { error: 'Node manager not available' } };
      }

      const node = nodeManager.getNode(nodeId);
      if (!node) {
        return { data: { error: `Node '${nodeId}' not found` } };
      }

      const port = node.runtime?.port || 8080;
      return {
        data: {
          node_id: nodeId,
          streams: {
            raw: `http://${node.host}:${port}/stream/raw`,
            annotated: `http://${node.host}:${port}/stream/annotated`,
            websocket: `ws://${node.host}:${port}/ws/video`,
          },
        },
      };
    }

    case 'inference_stats': {
      const nodeId = input.node_id as string;
      const period = input.period as string;
      const statsCollector = context?.statsCollector;

      if (!nodeManager) {
        return { data: { error: 'Node manager not available' } };
      }

      const node = nodeManager.getNode(nodeId);
      if (!node) {
        return { data: { error: `Node '${nodeId}' not found` } };
      }

      // Get current stats from StatsCollector (most recent poll)
      const currentStats = statsCollector?.getCurrentStats(nodeId);

      // For historical data, get daily summary
      let historicalData: { totalDetections: number; hours: AccumulatedStats[] } | null = null;
      if (statsCollector && (period === 'today' || period === '1h')) {
        try {
          const summary = await statsCollector.getDailySummary();
          historicalData = summary[nodeId] || null;
        } catch {
          // Ignore errors in getting historical data
        }
      }

      // If we have stats from collector, return them
      if (currentStats) {
        return {
          data: {
            node_id: nodeId,
            period,
            current: {
              total_detections: currentStats.totalDetections,
              total_frames: currentStats.totalFrames,
              fps: currentStats.fps,
              uptime_sec: currentStats.uptimeSec,
              model_loaded: currentStats.modelLoaded,
              by_label: currentStats.byLabel,
              timestamp: currentStats.timestamp,
            },
            today: historicalData ? {
              total_detections: historicalData.totalDetections,
              hours_recorded: historicalData.hours.length,
            } : null,
          },
        };
      }

      // Fallback: query runtime directly
      const status = nodeManager.getNodeStatus(nodeId);
      if (status?.status !== 'online') {
        return { data: { error: `Node '${nodeId}' is not online and no cached stats available` } };
      }

      try {
        const port = node.runtime?.port || 8080;
        const url = `http://${node.host}:${port}/api/stats`;

        const controller = new AbortController();
        const timeout = setTimeout(() => controller.abort(), 10000);

        const response = await fetch(url, { signal: controller.signal });
        clearTimeout(timeout);

        if (!response.ok) {
          return {
            data: {
              node_id: nodeId,
              period,
              note: 'Could not fetch stats from runtime',
              current_inference: status?.inference || null,
            },
          };
        }

        const stats = await response.json() as Record<string, unknown>;

        return {
          data: {
            node_id: nodeId,
            period,
            ...stats,
          },
        };
      } catch (error) {
        const errorMsg = error instanceof Error ? error.message : 'Unknown error';
        return {
          data: {
            node_id: nodeId,
            period,
            error: `Could not fetch stats: ${errorMsg}`,
            current_inference: status?.inference || null,
          },
        };
      }
    }

    case 'inference_results': {
      const nodeId = input.node_id as string;
      const period = input.period as string;

      if (!nodeManager) {
        return { data: { error: 'Node manager not available' } };
      }

      const node = nodeManager.getNode(nodeId);
      if (!node) {
        return { data: { error: `Node '${nodeId}' not found` } };
      }

      const status = nodeManager.getNodeStatus(nodeId);
      if (status?.status !== 'online') {
        return { data: { error: `Node '${nodeId}' is not online` } };
      }

      try {
        // Query the node's REST API for inference results
        const port = node.runtime?.port || 8080;
        const url = `http://${node.host}:${port}/api/results?period=${period}`;

        const controller = new AbortController();
        const timeout = setTimeout(() => controller.abort(), 10000);

        const response = await fetch(url, { signal: controller.signal });
        clearTimeout(timeout);

        if (!response.ok) {
          if (response.status === 404) {
            return {
              data: {
                node_id: nodeId,
                period,
                note: 'Node does not support results endpoint',
                current_inference: status?.inference || null,
              },
            };
          }
          return { data: { error: `Failed to get results: HTTP ${response.status}` } };
        }

        const results = await response.json() as Record<string, unknown>;

        return {
          data: {
            node_id: nodeId,
            period,
            ...results,
          },
        };
      } catch (error) {
        const errorMsg = error instanceof Error ? error.message : 'Unknown error';
        return {
          data: {
            node_id: nodeId,
            period,
            error: `Could not fetch results: ${errorMsg}`,
            current_inference: status?.inference || null,
          },
        };
      }
    }

    case 'model_list': {
      const modelsPath = path.join(getWorkspacePath(), 'models');

      if (!fs.existsSync(modelsPath)) {
        return {
          data: {
            models: [],
            note: 'Models directory does not exist',
            path: modelsPath,
          },
        };
      }

      try {
        const entries = fs.readdirSync(modelsPath, { withFileTypes: true });
        const models: Array<{
          name: string;
          task: string;
          format: string;
          labels: string[];
          size: string;
          files: string[];
        }> = [];

        for (const entry of entries) {
          if (!entry.isDirectory()) continue;

          const modelDir = path.join(modelsPath, entry.name);
          const files = fs.readdirSync(modelDir);

          // Detect model format and gather info
          let format = 'unknown';
          let labels: string[] = [];
          let totalSize = 0;

          for (const file of files) {
            const filePath = path.join(modelDir, file);
            const stats = fs.statSync(filePath);
            totalSize += stats.size;

            // Detect format from file extensions
            if (file.endsWith('.weights') || file.endsWith('.cfg')) {
              format = 'darknet';
            } else if (file.endsWith('.onnx')) {
              format = 'onnx';
            } else if (file.endsWith('.engine') || file.endsWith('.trt')) {
              format = 'tensorrt';
            } else if (file.endsWith('.pkl') || file.endsWith('.joblib')) {
              format = 'sklearn';
            }

            // Read labels from obj.names or labels.txt
            if (file === 'obj.names' || file === 'labels.txt') {
              try {
                const content = fs.readFileSync(filePath, 'utf-8');
                labels = content.split('\n').map(l => l.trim()).filter(l => l.length > 0);
              } catch {
                // Ignore read errors
              }
            }
          }

          // Format size
          const sizeStr = totalSize > 1024 * 1024
            ? `${(totalSize / (1024 * 1024)).toFixed(1)} MB`
            : `${(totalSize / 1024).toFixed(1)} KB`;

          models.push({
            name: entry.name,
            task: 'detection', // Default; could be inferred from config file
            format,
            labels,
            size: sizeStr,
            files,
          });
        }

        return {
          data: {
            models,
            count: models.length,
            path: modelsPath,
          },
        };
      } catch (error) {
        const errorMsg = error instanceof Error ? error.message : 'Unknown error';
        return { data: { error: `Failed to list models: ${errorMsg}` } };
      }
    }

    case 'model_deploy': {
      const modelName = input.model_name as string;
      const nodeId = input.node_id as string;

      if (!nodeManager) {
        return { data: { error: 'Node manager not available' } };
      }

      // 1. Verify model exists locally
      const modelPath = path.join(getWorkspacePath(), 'models', modelName);
      if (!fs.existsSync(modelPath)) {
        return { data: { error: `Model '${modelName}' not found in workspace` } };
      }

      // 2. Get node info
      const node = nodeManager.getNode(nodeId);
      if (!node) {
        return { data: { error: `Node '${nodeId}' not found` } };
      }

      const status = nodeManager.getNodeStatus(nodeId);
      if (status?.status !== 'online') {
        return { data: { error: `Node '${nodeId}' is not online` } };
      }

      // 3. Get SSH credentials
      const sshUser = node.ssh?.user || 'cira';
      const sshPort = node.ssh?.port || 22;
      const sshKeyPath = node.ssh?.key || path.join(os.homedir(), '.ssh', 'id_rsa');
      const remoteModelPath = `/home/${sshUser}/.cira/models/${modelName}`;

      const ssh = new NodeSSH();

      try {
        // 4. Connect via SSH
        logger.info(`Connecting to ${node.host} via SSH...`);

        const sshConfig: { host: string; username: string; port: number; privateKey?: string; password?: string } = {
          host: node.host,
          username: sshUser,
          port: sshPort,
        };

        // Use key file if it exists, otherwise try password
        if (fs.existsSync(sshKeyPath)) {
          sshConfig.privateKey = fs.readFileSync(sshKeyPath, 'utf-8');
        } else if (node.ssh?.password) {
          sshConfig.password = node.ssh.password;
        } else {
          return { data: { error: `No SSH credentials available for node '${nodeId}'` } };
        }

        await ssh.connect(sshConfig);
        logger.info(`Connected to ${node.host}`);

        // 5. Create remote directory
        await ssh.execCommand(`mkdir -p ${remoteModelPath}`);

        // 6. Copy model files
        const localFiles = fs.readdirSync(modelPath);
        const copiedFiles: string[] = [];

        for (const file of localFiles) {
          const localFilePath = path.join(modelPath, file);
          const remoteFilePath = `${remoteModelPath}/${file}`;

          await ssh.putFile(localFilePath, remoteFilePath);
          copiedFiles.push(file);
          logger.debug(`Copied ${file} to ${node.host}`);
        }

        // 7. Update model config (create a simple config pointing to the new model)
        const modelConfig = {
          name: modelName,
          path: remoteModelPath,
          deployed_at: new Date().toISOString(),
        };

        await ssh.execCommand(
          `echo '${JSON.stringify(modelConfig, null, 2)}' > ${remoteModelPath}/deploy.json`
        );

        // 8. Restart runtime service (if running as systemd service)
        const restartResult = await ssh.execCommand('sudo systemctl restart cira-runtime || true');
        logger.debug(`Runtime restart: ${restartResult.stdout || restartResult.stderr || 'OK'}`);

        // 9. Wait a moment and verify
        await new Promise(resolve => setTimeout(resolve, 2000));

        // 10. Check if runtime is responding
        let verification = { success: false, message: '' };
        try {
          const port = node.runtime?.port || 8080;
          const verifyResponse = await fetch(`http://${node.host}:${port}/health`, {
            signal: AbortSignal.timeout(5000),
          });
          if (verifyResponse.ok) {
            verification = { success: true, message: 'Runtime responding' };
          } else {
            verification = { success: false, message: `Runtime returned HTTP ${verifyResponse.status}` };
          }
        } catch (verifyError) {
          verification = {
            success: false,
            message: verifyError instanceof Error ? verifyError.message : 'Verification failed',
          };
        }

        ssh.dispose();

        return {
          data: {
            status: 'success',
            model_name: modelName,
            node_id: nodeId,
            message: `Model '${modelName}' deployed to '${nodeId}'`,
            files_copied: copiedFiles,
            remote_path: remoteModelPath,
            verification,
          },
        };
      } catch (error) {
        ssh.dispose();
        const errorMsg = error instanceof Error ? error.message : 'Unknown error';
        logger.error(`Model deploy failed: ${errorMsg}`);
        return { data: { error: `Deployment failed: ${errorMsg}` } };
      }
    }

    case 'alert_list': {
      const alertsConfig = context?.alertsConfig;

      if (!alertsConfig) {
        return { data: { error: 'Alerts configuration not available' } };
      }

      return {
        data: {
          alerts: [
            {
              id: 'defect_threshold',
              type: 'defect_rate',
              condition: `defects > ${alertsConfig.defect_threshold}/min`,
              threshold: alertsConfig.defect_threshold,
              channels: alertsConfig.notify_channels,
              enabled: true,
            },
            {
              id: 'temperature_max',
              type: 'temperature',
              condition: `temperature > ${alertsConfig.temperature_max}°C`,
              threshold: alertsConfig.temperature_max,
              channels: alertsConfig.notify_channels,
              enabled: true,
            },
            {
              id: 'fps_min',
              type: 'fps',
              condition: `fps < ${alertsConfig.fps_min}`,
              threshold: alertsConfig.fps_min,
              channels: alertsConfig.notify_channels,
              enabled: true,
            },
          ],
          notify_channels: alertsConfig.notify_channels,
        },
      };
    }

    case 'alert_history': {
      const period = input.period as string;
      const alertsConfig = context?.alertsConfig;

      // TODO: Implement alert storage in StatsCollector to track triggered alerts
      // For now, return config info and note that history requires storage implementation
      return {
        data: {
          period,
          note: 'Alert history storage not yet implemented. Showing current alert configuration.',
          current_thresholds: alertsConfig ? {
            defect_threshold: `${alertsConfig.defect_threshold}/min`,
            temperature_max: `${alertsConfig.temperature_max}°C`,
            fps_min: alertsConfig.fps_min,
          } : null,
          notify_channels: alertsConfig?.notify_channels || [],
          total_alerts: 0,
          alerts: [],
        },
      };
    }

    case 'js_query': {
      const code = input.code as string;
      const nodeId = (input.node_id as string) || 'local-dev';

      if (!context?.ruleEngine) {
        return { data: { error: 'Rule engine not available' } };
      }

      const payload = await context.statsCollector?.buildPayload(nodeId);
      if (!payload) {
        return { data: { error: 'No data available for node' } };
      }

      const result = await context.ruleEngine.executeQuery(code, payload);
      return {
        data: {
          success: result.success,
          result: result.result,
          error: result.error,
          execution_ms: result.execution_ms,
          code: result.code,
        },
      };
    }

    case 'js_rule_create': {
      const name = input.name as string;
      const description = input.description as string;
      const code = input.code as string;
      const nodeId = (input.node_id as string) || 'local-dev';
      const socket_type = input.socket_type as string;
      const reads = (input.reads as string[]) || [];
      const produces = (input.produces as ('pass' | 'reject' | 'alert' | 'log' | 'modbus_write')[]) || [];
      const tags = (input.tags as string[]) || [];

      if (!context?.ruleEngine) {
        return { data: { error: 'Rule engine not available' } };
      }

      // Import isValidSocketType from rule-engine (re-exported from socket-registry)
      const { isValidSocketType } = await import('../services/rule-engine.js');

      // Validate socket_type
      if (!socket_type || !isValidSocketType(socket_type)) {
        return {
          data: {
            success: false,
            error: `Invalid socket_type: ${socket_type}. Must be one of: vision.detection, vision.confidence, signal.threshold, signal.rate, system.health, any.boolean`,
            code,
          },
        };
      }

      // Validate reads
      if (!Array.isArray(reads)) {
        return {
          data: {
            success: false,
            error: `Missing 'reads' field. Must be an array of payload field paths.`,
            code,
          },
        };
      }

      // Validate produces
      if (!Array.isArray(produces) || produces.length === 0) {
        return {
          data: {
            success: false,
            error: `Missing or empty 'produces' field. Must be a non-empty array of action types.`,
            code,
          },
        };
      }

      const id = name.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');

      const payload = await context.statsCollector?.buildPayload(nodeId);
      const testPayload: RulePayload = payload || {
        detections: [],
        frame: { number: 0, timestamp: new Date().toISOString(), width: 640, height: 480 },
        stats: { total_detections: 0, by_label: {}, fps: 0, uptime_sec: 0, defects_per_hour: 0 },
        hourly: [],
        node: { id: nodeId, status: 'unknown' },
      };

      const rule: SavedRule = {
        id,
        name,
        description,
        socket_type,
        reads,
        produces,
        code,
        enabled: true,
        created_at: new Date().toISOString(),
        created_by: 'ai-agent',
        node_id: nodeId,
        tags,
      };

      // Dry-run: validate the rule executes correctly
      const dryRun = await context.ruleEngine.evaluateRule(rule, testPayload);
      if (!dryRun.success) {
        return { data: { success: false, error: `Rule validation failed: ${dryRun.error}`, code } };
      }

      // Validate dry-run action is in produces array (warn, not hard fail)
      if (dryRun.action && !produces.includes(dryRun.action.action)) {
        logger.warn(`Rule ${id} dry-run returned '${dryRun.action.action}' which is not in produces array`);
      }

      const validActions = ['pass', 'reject', 'alert', 'log', 'modbus_write'];
      if (dryRun.action && !validActions.includes(dryRun.action.action)) {
        return {
          data: {
            success: false,
            error: `Rule must return valid action, got: ${dryRun.action?.action}`,
            code,
          },
        };
      }

      context.ruleEngine.saveRule(rule);

      return {
        data: {
          success: true,
          rule_id: id,
          name,
          description,
          socket_type,
          reads,
          produces,
          tags,
          dry_run_result: dryRun.action,
          execution_ms: dryRun.execution_ms,
          code,
        },
      };
    }

    case 'js_rule_list': {
      const action = input.action as string;
      const ruleId = input.rule_id as string | undefined;
      const filterTag = input.filter_tag as string | undefined;
      const filterSignalType = input.filter_signal_type as string | undefined;

      if (!context?.ruleEngine) {
        return { data: { error: 'Rule engine not available' } };
      }

      switch (action) {
        case 'list': {
          let rules = context.ruleEngine.loadRules();

          // Apply filters
          if (filterTag) {
            rules = rules.filter(r => r.tags?.includes(filterTag));
          }
          if (filterSignalType) {
            rules = rules.filter(r => r.signal_type === filterSignalType);
          }

          return {
            data: {
              rules: rules.map(r => ({
                id: r.id,
                name: r.name,
                description: r.description,
                socket_type: r.socket_type,
                reads: r.reads,
                produces: r.produces,
                enabled: r.enabled,
                node_id: r.node_id,
                tags: r.tags || [],
                created_at: r.created_at,
                created_by: r.created_by,
              })),
              count: rules.length,
            },
          };
        }

        case 'enable': {
          if (!ruleId) {
            return { data: { error: 'rule_id is required for enable action' } };
          }
          const success = context.ruleEngine.enableRule(ruleId, true);
          return {
            data: {
              success,
              message: success ? `Rule '${ruleId}' enabled` : `Rule '${ruleId}' not found`,
            },
          };
        }

        case 'disable': {
          if (!ruleId) {
            return { data: { error: 'rule_id is required for disable action' } };
          }
          const success = context.ruleEngine.enableRule(ruleId, false);
          return {
            data: {
              success,
              message: success ? `Rule '${ruleId}' disabled` : `Rule '${ruleId}' not found`,
            },
          };
        }

        case 'delete': {
          if (!ruleId) {
            return { data: { error: 'rule_id is required for delete action' } };
          }
          const success = context.ruleEngine.deleteRule(ruleId);
          return {
            data: {
              success,
              message: success ? `Rule '${ruleId}' deleted` : `Rule '${ruleId}' not found`,
            },
          };
        }

        default:
          return { data: { error: `Unknown action: ${action}` } };
      }
    }

    // Spec G: Composite rule tools
    case 'composite_rule_list': {
      const enabledOnly = input.enabled_only as boolean || false;

      if (!context?.compositeRuleEngine) {
        return { data: { error: 'Composite rule engine not available' } };
      }

      const stateStore = context.compositeRuleEngine.getStateStore();
      const rules = stateStore.getAllCompositeRules(enabledOnly);

      return {
        data: {
          rules: rules.map(r => ({
            id: r.id,
            name: r.name,
            description: r.description,
            enabled: r.enabled,
            created_at: r.created_at,
            created_by: r.created_by,
            node_count: r.nodes.length,
            connection_count: r.connections.length,
            output_action: r.output_action.action,
          })),
          count: rules.length,
        },
      };
    }

    case 'composite_rule_create': {
      const name = input.name as string;
      const description = input.description as string;
      const outputAction = (input.output_action as string) || 'alert';

      if (!context?.compositeRuleEngine) {
        return { data: { error: 'Composite rule engine not available' } };
      }

      const stateStore = context.compositeRuleEngine.getStateStore();
      const id = name.toLowerCase().replace(/[^a-z0-9]+/g, '-').replace(/^-|-$/g, '');

      // Check if rule with this ID already exists
      const existing = stateStore.getCompositeRule(id);
      if (existing) {
        return {
          data: {
            success: false,
            error: `Composite rule with id '${id}' already exists`,
          },
        };
      }

      const rule: CompositeRule = {
        id,
        name,
        description,
        enabled: false, // Start disabled until configured
        created_at: new Date().toISOString(),
        created_by: 'ai-agent',
        nodes: [],
        connections: [],
        output_action: {
          action: outputAction as OutputAction['action'],
        },
      };

      stateStore.saveCompositeRule(rule);

      return {
        data: {
          success: true,
          rule_id: id,
          name,
          description,
          message: `Composite rule '${name}' created. Configure it in the Rule Graph dashboard page.`,
          dashboard_url: '/rule-graph',
        },
      };
    }

    case 'composite_rule_toggle': {
      const ruleId = input.rule_id as string;
      const enabled = input.enabled as boolean;

      if (!context?.compositeRuleEngine) {
        return { data: { error: 'Composite rule engine not available' } };
      }

      const stateStore = context.compositeRuleEngine.getStateStore();
      const success = stateStore.setCompositeRuleEnabled(ruleId, enabled);

      if (!success) {
        return {
          data: {
            success: false,
            error: `Composite rule '${ruleId}' not found`,
          },
        };
      }

      return {
        data: {
          success: true,
          rule_id: ruleId,
          enabled,
          message: `Composite rule '${ruleId}' ${enabled ? 'enabled' : 'disabled'}`,
        },
      };
    }

    case 'composite_rule_delete': {
      const ruleId = input.rule_id as string;

      if (!context?.compositeRuleEngine) {
        return { data: { error: 'Composite rule engine not available' } };
      }

      const stateStore = context.compositeRuleEngine.getStateStore();
      const success = stateStore.deleteCompositeRule(ruleId);

      if (!success) {
        return {
          data: {
            success: false,
            error: `Composite rule '${ruleId}' not found`,
          },
        };
      }

      return {
        data: {
          success: true,
          rule_id: ruleId,
          message: `Composite rule '${ruleId}' deleted`,
        },
      };
    }

    default:
      logger.warn(`Unknown tool: ${toolName}`);
      return { data: { error: `Unknown tool: ${toolName}` } };
  }
}
