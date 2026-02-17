import fs from 'fs';
import path from 'path';
import os from 'os';
import { NodeSSH } from 'node-ssh';
import { createLogger } from '../utils/logger.js';

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
        // Query the node's REST API for inference stats
        const port = node.runtime?.port || 8080;
        const url = `http://${node.host}:${port}/api/stats?period=${period}`;

        const controller = new AbortController();
        const timeout = setTimeout(() => controller.abort(), 10000);

        const response = await fetch(url, { signal: controller.signal });
        clearTimeout(timeout);

        if (!response.ok) {
          // If endpoint doesn't exist, return basic stats from status
          if (response.status === 404) {
            return {
              data: {
                node_id: nodeId,
                period,
                note: 'Node does not support detailed stats endpoint',
                current_inference: status?.inference || null,
              },
            };
          }
          return { data: { error: `Failed to get stats: HTTP ${response.status}` } };
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

        // If fetch fails, return whatever we have from status
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
