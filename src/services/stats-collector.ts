import { EventEmitter } from 'events';
import { NodeConfig, AlertsConfig } from '../utils/config-schema.js';
import { NodeManager } from '../nodes/manager.js';
import { MqttChannel } from '../channels/mqtt.js';
import { createLogger } from '../utils/logger.js';
import fs from 'fs';
import path from 'path';

const logger = createLogger('stats-collector');

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

export class StatsCollector extends EventEmitter {
  private nodeManager: NodeManager;
  private mqttChannel: MqttChannel | null;
  private alertsConfig: AlertsConfig;
  private dataDir: string;

  private pollInterval: NodeJS.Timeout | null = null;
  private saveInterval: NodeJS.Timeout | null = null;

  // Track stats over time
  private lastStats: Map<string, NodeStats> = new Map();
  private hourlyAccumulator: Map<string, AccumulatedStats> = new Map();

  // Track detections for alerts
  private recentDetections: Map<string, { count: number; since: Date }> = new Map();

  constructor(
    nodeManager: NodeManager,
    mqttChannel: MqttChannel | null,
    alertsConfig: AlertsConfig,
    dataDir: string
  ) {
    super();
    this.nodeManager = nodeManager;
    this.mqttChannel = mqttChannel;
    this.alertsConfig = alertsConfig;
    this.dataDir = dataDir;

    // Ensure data directory exists
    if (!fs.existsSync(dataDir)) {
      fs.mkdirSync(dataDir, { recursive: true });
    }
  }

  // Start collecting stats
  start(pollIntervalMs: number = 10000): void {
    if (this.pollInterval) {
      clearInterval(this.pollInterval);
    }
    if (this.saveInterval) {
      clearInterval(this.saveInterval);
    }

    // Poll stats every N seconds
    this.pollInterval = setInterval(() => {
      this.pollAllNodes();
    }, pollIntervalMs);

    // Save hourly snapshots every hour
    this.saveInterval = setInterval(() => {
      this.saveHourlySnapshot();
    }, 3600000); // 1 hour

    // Initial poll
    this.pollAllNodes();

    logger.info(`Stats collector started with ${pollIntervalMs}ms poll interval`);
  }

  // Stop collecting
  stop(): void {
    if (this.pollInterval) {
      clearInterval(this.pollInterval);
      this.pollInterval = null;
    }
    if (this.saveInterval) {
      clearInterval(this.saveInterval);
      this.saveInterval = null;
    }
    logger.info('Stats collector stopped');
  }

  // Poll stats from all nodes
  private async pollAllNodes(): Promise<void> {
    const nodes = this.nodeManager.getAllNodes();

    for (const node of nodes) {
      try {
        await this.pollNode(node);
      } catch (error) {
        logger.debug(`Failed to poll stats from ${node.id}: ${error}`);
      }
    }
  }

  // Poll stats from a single node
  private async pollNode(node: NodeConfig): Promise<void> {
    const url = `http://${node.host}:${node.runtime.port}/api/stats`;
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 5000);

    try {
      const response = await fetch(url, { signal: controller.signal });
      clearTimeout(timeout);

      if (!response.ok) {
        return;
      }

      const data = await response.json() as {
        total_detections: number;
        total_frames: number;
        by_label: Record<string, number>;
        fps: number;
        uptime_sec: number;
        timestamp: string;
        model_loaded: boolean;
      };

      const stats: NodeStats = {
        nodeId: node.id,
        timestamp: data.timestamp,
        totalDetections: data.total_detections,
        totalFrames: data.total_frames,
        fps: data.fps,
        uptimeSec: data.uptime_sec,
        modelLoaded: data.model_loaded,
        byLabel: data.by_label,
      };

      // Calculate delta since last poll
      const lastStat = this.lastStats.get(node.id);
      let newDetections = 0;

      if (lastStat) {
        newDetections = stats.totalDetections - lastStat.totalDetections;

        // Only process if we got new detections
        if (newDetections > 0) {
          this.processNewDetections(node.id, newDetections, stats);
        }
      }

      // Update last stats
      this.lastStats.set(node.id, stats);

      // Update hourly accumulator
      this.updateAccumulator(node.id, stats, newDetections);

      // Publish to MQTT if connected
      if (this.mqttChannel?.isConnected()) {
        this.mqttChannel.publish(`cira/stats/${node.id}`, {
          ...stats,
          new_detections: newDetections,
        });
      }

      // Emit event for local listeners
      this.emit('stats', stats);

    } catch (error) {
      clearTimeout(timeout);
      // Node might be offline, silently ignore
    }
  }

  // Process new detections and check alert thresholds
  private processNewDetections(nodeId: string, count: number, stats: NodeStats): void {
    // Track recent detections for rate calculation
    const recent = this.recentDetections.get(nodeId) || { count: 0, since: new Date() };
    recent.count += count;

    // Reset tracking every minute
    const now = new Date();
    const elapsed = now.getTime() - recent.since.getTime();
    if (elapsed > 60000) {
      // Calculate rate per minute
      const rate = recent.count;

      // Check defect threshold alert
      if (rate >= this.alertsConfig.defect_threshold) {
        this.triggerAlert(nodeId, 'defect_rate',
          `High defect rate: ${rate}/min exceeds threshold of ${this.alertsConfig.defect_threshold}`);
      }

      // Reset counter
      recent.count = 0;
      recent.since = now;
    }

    this.recentDetections.set(nodeId, recent);

    // Check FPS alert
    if (stats.fps > 0 && stats.fps < this.alertsConfig.fps_min) {
      this.triggerAlert(nodeId, 'low_fps',
        `Low FPS: ${stats.fps.toFixed(1)} below minimum of ${this.alertsConfig.fps_min}`);
    }

    logger.debug(`Node ${nodeId}: +${count} detections (total: ${stats.totalDetections})`);
  }

  // Trigger an alert
  private triggerAlert(nodeId: string, alertType: string, message: string): void {
    logger.warn(`Alert [${nodeId}]: ${message}`);

    // Emit local event
    this.emit('alert', { nodeId, type: alertType, message });

    // Publish to MQTT if enabled
    if (this.mqttChannel?.isConnected() && this.alertsConfig.notify_channels.includes('mqtt')) {
      this.mqttChannel.publishAlert(nodeId, alertType, message);
    }

    // Emit to node manager for WebSocket broadcast
    this.nodeManager.emit('node:alert', nodeId, message);
  }

  // Update hourly accumulator
  private updateAccumulator(nodeId: string, stats: NodeStats, newDetections: number): void {
    const hourStart = this.getHourStart();
    const key = `${nodeId}:${hourStart}`;

    const acc = this.hourlyAccumulator.get(key) || {
      nodeId,
      hourStart,
      detections: 0,
      frames: 0,
      avgFps: 0,
      samples: 0,
    };

    acc.detections += newDetections;
    acc.samples++;
    acc.avgFps = (acc.avgFps * (acc.samples - 1) + stats.fps) / acc.samples;

    this.hourlyAccumulator.set(key, acc);
  }

  // Get current hour start time as ISO string
  private getHourStart(): string {
    const now = new Date();
    now.setMinutes(0, 0, 0);
    return now.toISOString();
  }

  // Save hourly snapshot to disk
  private async saveHourlySnapshot(): Promise<void> {
    const hourStart = this.getHourStart();
    const snapshots: AccumulatedStats[] = [];

    // Collect all accumulators for the current hour
    for (const [, acc] of this.hourlyAccumulator) {
      if (acc.hourStart === hourStart) {
        snapshots.push(acc);
      }
    }

    if (snapshots.length === 0) {
      return;
    }

    // Save to file
    const filename = `stats_${hourStart.replace(/[:.]/g, '-')}.json`;
    const filepath = path.join(this.dataDir, filename);

    try {
      fs.writeFileSync(filepath, JSON.stringify(snapshots, null, 2));
      logger.info(`Saved hourly snapshot: ${filename}`);

      // Publish summary to MQTT
      if (this.mqttChannel?.isConnected()) {
        this.mqttChannel.publish('cira/stats/hourly', {
          hour: hourStart,
          nodes: snapshots,
        });
      }

      // Clear old accumulator entries
      for (const key of this.hourlyAccumulator.keys()) {
        if (!key.includes(hourStart)) {
          this.hourlyAccumulator.delete(key);
        }
      }
    } catch (error) {
      logger.error(`Failed to save hourly snapshot: ${error}`);
    }
  }

  // Get current stats for a node
  getCurrentStats(nodeId: string): NodeStats | undefined {
    return this.lastStats.get(nodeId);
  }

  // Get all current stats
  getAllCurrentStats(): NodeStats[] {
    return Array.from(this.lastStats.values());
  }

  // Get hourly summary for today
  async getDailySummary(): Promise<Record<string, { totalDetections: number; hours: AccumulatedStats[] }>> {
    const today = new Date().toISOString().split('T')[0];
    const summary: Record<string, { totalDetections: number; hours: AccumulatedStats[] }> = {};

    // Read today's files
    const files = fs.readdirSync(this.dataDir).filter(f => f.startsWith(`stats_${today}`));

    for (const file of files) {
      try {
        const content = fs.readFileSync(path.join(this.dataDir, file), 'utf-8');
        const hourlyStats = JSON.parse(content) as AccumulatedStats[];

        for (const stat of hourlyStats) {
          if (!summary[stat.nodeId]) {
            summary[stat.nodeId] = { totalDetections: 0, hours: [] };
          }
          summary[stat.nodeId].totalDetections += stat.detections;
          summary[stat.nodeId].hours.push(stat);
        }
      } catch {
        // Skip invalid files
      }
    }

    return summary;
  }
}

// Factory function
export function createStatsCollector(
  nodeManager: NodeManager,
  mqttChannel: MqttChannel | null,
  alertsConfig: AlertsConfig,
  dataDir: string
): StatsCollector {
  return new StatsCollector(nodeManager, mqttChannel, alertsConfig, dataDir);
}
