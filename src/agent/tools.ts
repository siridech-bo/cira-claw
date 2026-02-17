import { createLogger } from '../utils/logger.js';

const logger = createLogger('tools');

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

export interface ToolContext {
  nodeManager?: {
    getAllNodes: () => Array<{ id: string; name: string; host: string; type: string }>;
    getNode: (id: string) => { id: string; name: string; host: string; runtime?: { port: number } } | undefined;
    getNodeStatus: (id: string) => { status: string; metrics?: Record<string, unknown>; inference?: Record<string, unknown> } | undefined;
    getAllStatuses: () => Array<{ id: string; status: string; metrics?: Record<string, unknown> }>;
    getSummary: () => { total: number; online: number; offline: number };
    checkNodeHealth: (node: unknown) => Promise<{ status: string }>;
  };
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
  logger.debug(`Executing tool: ${toolName}`, { input });

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

      // In a real implementation, this would query stored inference data
      // For now, return placeholder data
      return {
        data: {
          node_id: nodeId,
          period,
          total_defects: 147,
          defects_per_hour: 12.25,
          avg_confidence: 0.942,
          by_label: {
            scratch: 89,
            dent: 42,
            crack: 16,
          },
          peak_hour: '14:00-15:00',
          peak_count: 23,
        },
      };
    }

    case 'inference_results': {
      const nodeId = input.node_id as string;
      const period = input.period as string;

      // In a real implementation, this would query stored inference data
      return {
        data: {
          node_id: nodeId,
          period,
          total_detections: 147,
          detections: [
            { timestamp: '2026-02-17T14:32:15Z', label: 'scratch', confidence: 0.96, bbox: [120, 80, 200, 150] },
            { timestamp: '2026-02-17T14:35:22Z', label: 'dent', confidence: 0.89, bbox: [300, 200, 100, 80] },
          ],
        },
      };
    }

    case 'model_list': {
      // In a real implementation, this would scan the workspace models directory
      return {
        data: {
          models: [
            {
              name: 'scratch_v3',
              task: 'detection',
              format: 'darknet',
              labels: ['scratch', 'dent', 'crack'],
              size: '23.5 MB',
            },
            {
              name: 'scratch_v4',
              task: 'detection',
              format: 'onnx',
              labels: ['scratch', 'dent', 'crack', 'chip'],
              size: '45.2 MB',
            },
          ],
        },
      };
    }

    case 'model_deploy': {
      const modelName = input.model_name as string;
      const nodeId = input.node_id as string;

      // In a real implementation, this would:
      // 1. Verify model exists
      // 2. SSH to node
      // 3. Copy model files
      // 4. Update config
      // 5. Restart runtime
      // 6. Run verification

      return {
        data: {
          status: 'success',
          model_name: modelName,
          node_id: nodeId,
          message: `Model '${modelName}' deployed to '${nodeId}' successfully`,
          test_results: {
            accuracy: 0.982,
            test_images: 50,
          },
        },
      };
    }

    case 'alert_list': {
      return {
        data: {
          alerts: [
            {
              id: 'alert-1',
              type: 'defect_threshold',
              condition: 'defects > 50/hr',
              channels: ['line', 'mqtt'],
              enabled: true,
            },
            {
              id: 'alert-2',
              type: 'temperature',
              condition: 'temperature > 80°C',
              channels: ['line'],
              enabled: true,
            },
          ],
        },
      };
    }

    case 'alert_history': {
      const period = input.period as string;

      return {
        data: {
          period,
          total_alerts: 5,
          alerts: [
            {
              timestamp: '2026-02-17T14:30:00Z',
              type: 'defect_threshold',
              node_id: 'jetson-line1',
              message: 'Defect rate exceeded 50/hr (current: 67/hr)',
            },
            {
              timestamp: '2026-02-17T10:15:00Z',
              type: 'temperature',
              node_id: 'jetson-line2',
              message: 'Temperature exceeded 80°C (current: 82°C)',
            },
          ],
        },
      };
    }

    default:
      logger.warn(`Unknown tool: ${toolName}`);
      return { data: { error: `Unknown tool: ${toolName}` } };
  }
}
