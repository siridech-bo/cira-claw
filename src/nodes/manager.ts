import { EventEmitter } from 'events';
import { NodeConfig } from '../utils/config-schema.js';
import { createLogger } from '../utils/logger.js';
import { ConfigLoader } from '../config.js';

const logger = createLogger('node-manager');

export interface NodeStatus {
  id: string;
  name: string;
  type: string;
  host: string;
  status: 'online' | 'offline' | 'updating' | 'error' | 'unknown';
  lastSeen: Date | null;
  error: string | null;
  metrics: {
    fps: number | null;
    temperature: number | null;
    cpuUsage: number | null;
    memoryUsage: number | null;
    uptime: number | null;
  };
  inference: {
    modelName: string | null;
    defectsTotal: number;
    defectsPerHour: number;
    lastDefect: Date | null;
  };
}

export interface NodeManagerEvents {
  'node:added': (node: NodeConfig) => void;
  'node:removed': (nodeId: string) => void;
  'node:status': (status: NodeStatus) => void;
  'node:online': (nodeId: string) => void;
  'node:offline': (nodeId: string) => void;
  'node:alert': (nodeId: string, message: string) => void;
}

export class NodeManager extends EventEmitter {
  private configLoader: ConfigLoader;
  private statuses: Map<string, NodeStatus> = new Map();
  private healthCheckInterval: NodeJS.Timeout | null = null;

  constructor(configLoader: ConfigLoader) {
    super();
    this.configLoader = configLoader;
  }

  // Initialize node manager and load existing nodes
  async init(): Promise<void> {
    await this.configLoader.loadNodes();

    // Initialize status for all nodes
    for (const node of this.configLoader.getAllNodes()) {
      this.statuses.set(node.id, this.createInitialStatus(node));
    }

    logger.info(`Node manager initialized with ${this.statuses.size} nodes`);
  }

  // Create initial status for a node
  private createInitialStatus(node: NodeConfig): NodeStatus {
    return {
      id: node.id,
      name: node.name,
      type: node.type,
      host: node.host,
      status: 'unknown',
      lastSeen: null,
      error: null,
      metrics: {
        fps: null,
        temperature: null,
        cpuUsage: null,
        memoryUsage: null,
        uptime: null,
      },
      inference: {
        modelName: node.models[0]?.name || null,
        defectsTotal: 0,
        defectsPerHour: 0,
        lastDefect: null,
      },
    };
  }

  // Get all nodes
  getAllNodes(): NodeConfig[] {
    return this.configLoader.getAllNodes();
  }

  // Get a specific node configuration
  getNode(id: string): NodeConfig | undefined {
    return this.configLoader.getNode(id);
  }

  // Get node status
  getNodeStatus(id: string): NodeStatus | undefined {
    return this.statuses.get(id);
  }

  // Get all node statuses
  getAllStatuses(): NodeStatus[] {
    return Array.from(this.statuses.values());
  }

  // Add a new node
  async addNode(node: NodeConfig): Promise<void> {
    await this.configLoader.saveNode(node);
    this.statuses.set(node.id, this.createInitialStatus(node));
    this.emit('node:added', node);
    logger.info(`Added node: ${node.id}`);
  }

  // Remove a node
  async removeNode(id: string): Promise<boolean> {
    const removed = await this.configLoader.deleteNode(id);
    if (removed) {
      this.statuses.delete(id);
      this.emit('node:removed', id);
      logger.info(`Removed node: ${id}`);
    }
    return removed;
  }

  // Update node status
  updateStatus(id: string, update: Partial<NodeStatus>): void {
    const current = this.statuses.get(id);
    if (!current) {
      logger.warn(`Cannot update status for unknown node: ${id}`);
      return;
    }

    const prevStatus = current.status;
    const newStatus: NodeStatus = {
      ...current,
      ...update,
      metrics: { ...current.metrics, ...update.metrics },
      inference: { ...current.inference, ...update.inference },
    };

    this.statuses.set(id, newStatus);
    this.emit('node:status', newStatus);

    // Emit online/offline events
    if (prevStatus !== 'online' && newStatus.status === 'online') {
      this.emit('node:online', id);
      logger.info(`Node ${id} is now online`);
    } else if (prevStatus === 'online' && newStatus.status === 'offline') {
      this.emit('node:offline', id);
      logger.warn(`Node ${id} is now offline`);
    }
  }

  // Start periodic health checks
  startHealthChecks(intervalMs: number = 30000): void {
    if (this.healthCheckInterval) {
      clearInterval(this.healthCheckInterval);
    }

    this.healthCheckInterval = setInterval(() => {
      this.checkAllNodes();
    }, intervalMs);

    // Run initial check
    this.checkAllNodes();
    logger.info(`Started health checks with ${intervalMs}ms interval`);
  }

  // Stop health checks
  stopHealthChecks(): void {
    if (this.healthCheckInterval) {
      clearInterval(this.healthCheckInterval);
      this.healthCheckInterval = null;
      logger.info('Stopped health checks');
    }
  }

  // Check health of all nodes
  private async checkAllNodes(): Promise<void> {
    const nodes = this.getAllNodes();

    for (const node of nodes) {
      try {
        await this.checkNodeHealth(node);
      } catch (error) {
        logger.error(`Health check failed for ${node.id}: ${error}`);
      }
    }
  }

  // Check health of a single node
  async checkNodeHealth(node: NodeConfig): Promise<NodeStatus> {
    const startTime = Date.now();

    try {
      // Try to reach the runtime HTTP endpoint
      const url = `http://${node.host}:${node.runtime.port}/health`;
      const controller = new AbortController();
      const timeout = setTimeout(() => controller.abort(), 5000);

      const response = await fetch(url, { signal: controller.signal });
      clearTimeout(timeout);

      if (response.ok) {
        const data = await response.json() as {
          fps?: number;
          temperature?: number;
          cpu_usage?: number;
          memory_usage?: number;
          uptime?: number;
          model_name?: string;
          defects_total?: number;
          defects_per_hour?: number;
        };

        this.updateStatus(node.id, {
          status: 'online',
          lastSeen: new Date(),
          error: null,
          metrics: {
            fps: data.fps ?? null,
            temperature: data.temperature ?? null,
            cpuUsage: data.cpu_usage ?? null,
            memoryUsage: data.memory_usage ?? null,
            uptime: data.uptime ?? null,
          },
          inference: {
            modelName: data.model_name ?? null,
            defectsTotal: data.defects_total ?? 0,
            defectsPerHour: data.defects_per_hour ?? 0,
            lastDefect: null,
          },
        });
      } else {
        this.updateStatus(node.id, {
          status: 'error',
          lastSeen: new Date(),
          error: `HTTP ${response.status}`,
        });
      }
    } catch (error) {
      const errorMessage = error instanceof Error ? error.message : 'Unknown error';

      if (errorMessage.includes('ECONNREFUSED') || errorMessage.includes('abort')) {
        this.updateStatus(node.id, {
          status: 'offline',
          error: 'Connection refused',
        });
      } else {
        this.updateStatus(node.id, {
          status: 'error',
          error: errorMessage,
        });
      }
    }

    const latency = Date.now() - startTime;
    logger.debug(`Health check for ${node.id} completed in ${latency}ms`);

    return this.statuses.get(node.id)!;
  }

  // Get summary statistics
  getSummary(): {
    total: number;
    online: number;
    offline: number;
    error: number;
    unknown: number;
  } {
    const statuses = this.getAllStatuses();
    return {
      total: statuses.length,
      online: statuses.filter(s => s.status === 'online').length,
      offline: statuses.filter(s => s.status === 'offline').length,
      error: statuses.filter(s => s.status === 'error').length,
      unknown: statuses.filter(s => s.status === 'unknown').length,
    };
  }
}

// Singleton instance
let nodeManagerInstance: NodeManager | null = null;

export function getNodeManager(configLoader: ConfigLoader): NodeManager {
  if (!nodeManagerInstance) {
    nodeManagerInstance = new NodeManager(configLoader);
  }
  return nodeManagerInstance;
}

export default NodeManager;
