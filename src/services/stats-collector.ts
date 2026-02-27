import { EventEmitter } from 'events';
import { NodeConfig, AlertsConfig } from '../utils/config-schema.js';
import { NodeManager } from '../nodes/manager.js';
import { MqttChannel } from '../channels/mqtt.js';
import { createLogger } from '../utils/logger.js';
import { RuleEngine, RulePayload, RuleAction, RuleResult, SocketType } from './rule-engine.js';
import { CompositeRuleEngine, CompositeRuleResult } from './composite-rule-engine.js';
import { ActionRunner, ActionContext } from './action-runner.js';
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

  // Rule engine for automated rule evaluation
  private ruleEngine: RuleEngine | null = null;

  // v3: Cache for rule results (used by /api/rules/results)
  private lastRuleResults: Map<string, RuleResult> | null = null;
  private lastRuleResultsAt: string = '';

  // Spec G: Composite rule engine and action runner
  private compositeRuleEngine: CompositeRuleEngine | null = null;
  private actionRunner: ActionRunner | null = null;
  private lastCompositeResults: Map<string, CompositeRuleResult> | null = null;

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

      // TEMPORARY — Spec D (Heartbeat Scheduler) replaces this
      // with configurable scheduling. Remove this block when Spec D is implemented.
      if (this.ruleEngine) {
        const payload = await this.buildPayload(node.id);
        if (payload) {
          // v3: Use evaluateAll() and cache results for /api/rules/results
          const results = await this.ruleEngine.evaluateAll(payload);

          // Cache results
          this.lastRuleResults = results;
          this.lastRuleResultsAt = new Date().toISOString();

          // Dispatch non-pass actions for atomic rules
          for (const [ruleId, result] of results) {
            if (result.success && result.action && result.action.action !== 'pass') {
              this.handleRuleAction(node.id, ruleId, result.action, false);
            }
          }

          // Spec G: Evaluate composite rules using atomic results
          if (this.compositeRuleEngine) {
            const compositeResults = await this.compositeRuleEngine.evaluateAll(results, payload);
            this.lastCompositeResults = compositeResults;

            // Execute triggered composite actions
            for (const [compositeId, compositeResult] of compositeResults) {
              if (compositeResult.success && compositeResult.triggered && compositeResult.action) {
                this.handleRuleAction(node.id, compositeId, compositeResult.action, true);
              }
            }
          }
        }
      }

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

  /**
   * Set the rule engine for automated rule evaluation.
   * Called from index.ts after both StatsCollector and RuleEngine are created.
   */
  setRuleEngine(engine: RuleEngine): void {
    this.ruleEngine = engine;
  }

  /**
   * Set the composite rule engine for Spec G graph evaluation.
   */
  setCompositeRuleEngine(engine: CompositeRuleEngine): void {
    this.compositeRuleEngine = engine;
  }

  /**
   * Set the action runner for executing rule actions.
   */
  setActionRunner(runner: ActionRunner): void {
    this.actionRunner = runner;
  }

  /**
   * Get the composite rule engine for API access.
   */
  getCompositeRuleEngine(): CompositeRuleEngine | null {
    return this.compositeRuleEngine;
  }

  /**
   * Get cached composite rule results for /api/rules/composite/results.
   */
  getCompositeRuleResults(): {
    evaluated_at: string;
    results: Record<string, {
      triggered: boolean;
      action?: RuleAction;
      node_results: Record<string, boolean>;
      success: boolean;
      error?: string;
      execution_ms: number;
    }>;
  } {
    if (!this.lastCompositeResults) {
      return { evaluated_at: '', results: {} };
    }

    const out: Record<string, {
      triggered: boolean;
      action?: RuleAction;
      node_results: Record<string, boolean>;
      success: boolean;
      error?: string;
      execution_ms: number;
    }> = {};

    for (const [id, result] of this.lastCompositeResults) {
      out[id] = {
        triggered: result.triggered,
        action: result.action,
        node_results: Object.fromEntries(result.node_results),
        success: result.success,
        error: result.error,
        execution_ms: result.execution_ms,
      };
    }

    return { evaluated_at: this.lastRuleResultsAt, results: out };
  }

  /**
   * Build a RulePayload for the given node.
   * Used by js_query tool and rule evaluation.
   */
  async buildPayload(nodeId: string): Promise<RulePayload | null> {
    const stats = this.getCurrentStats(nodeId);
    if (!stats) return null;

    let detections: Array<{
      label: string;
      confidence: number;
      x: number;
      y: number;
      w: number;
      h: number;
    }> = [];
    let frameInfo = { number: 0, timestamp: new Date().toISOString(), width: 640, height: 480 };

    try {
      const node = this.nodeManager.getNode(nodeId);
      if (node?.runtime?.port) {
        const resp = await fetch(`http://${node.host || 'localhost'}:${node.runtime.port}/api/results`);
        if (resp.ok) {
          const data = await resp.json() as {
            detections?: Array<{
              label?: string;
              confidence?: number;
              x?: number;
              y?: number;
              w?: number;
              h?: number;
            }>;
            frame_number?: number;
          };
          detections = (data.detections || []).map((d) => ({
            label: d.label || 'unknown',
            confidence: d.confidence || 0,
            x: d.x || 0,
            y: d.y || 0,
            w: d.w || 0,
            h: d.h || 0,
          }));
          frameInfo.number = data.frame_number || 0;
        }
      }
    } catch {
      // Runtime not reachable — use empty detections
    }

    const daily = await this.getDailySummary();
    const nodeSummary = daily[nodeId];
    const hourly = nodeSummary?.hours?.map((h) => ({
      hour: h.hourStart,
      detections: h.detections,
    })) || [];

    // Calculate defects per hour (detections in last hour)
    const now = new Date();
    const recentDetections = this.recentDetections.get(nodeId);
    let defectsPerHour = 0;
    if (recentDetections) {
      const elapsed = (now.getTime() - recentDetections.since.getTime()) / 1000 / 3600;
      if (elapsed > 0) {
        defectsPerHour = recentDetections.count / elapsed;
      }
    }

    return {
      detections,
      frame: frameInfo,
      stats: {
        total_detections: stats.totalDetections,
        by_label: stats.byLabel,
        fps: stats.fps,
        uptime_sec: stats.uptimeSec,
        defects_per_hour: defectsPerHour,
      },
      hourly,
      node: { id: nodeId, status: stats.modelLoaded ? 'running' : 'offline' },
      // signals: {} — reserved for Spec C, leave undefined
    };
  }

  /**
   * Handle a rule action by dispatching to appropriate channels.
   */
  /**
   * Get cached rule evaluation results for /api/rules/results endpoint.
   * Returns the results from the most recent evaluation cycle.
   *
   * Used by Spec G's composite evaluator to read atomic rule states.
   */
  getRuleResults(): {
    evaluated_at: string;
    results: Record<string, {
      action: RuleAction | undefined;
      socket_type: SocketType;
      reads: string[];
      produces: string[];
      execution_ms: number;
      success: boolean;
      error?: string;
    }>;
  } {
    if (!this.lastRuleResults) {
      return { evaluated_at: '', results: {} };
    }

    const out: Record<string, {
      action: RuleAction | undefined;
      socket_type: SocketType;
      reads: string[];
      produces: string[];
      execution_ms: number;
      success: boolean;
      error?: string;
    }> = {};

    for (const [id, result] of this.lastRuleResults) {
      out[id] = {
        action: result.action,
        socket_type: result.socket_type,
        reads: result.reads ?? [],
        produces: result.produces ?? [],
        execution_ms: result.execution_ms,
        success: result.success,
        error: result.error,
      };
    }

    return { evaluated_at: this.lastRuleResultsAt, results: out };
  }

  /**
   * Handle a rule action by dispatching to appropriate channels.
   * If ActionRunner is available, delegate to it for full action handling.
   */
  private handleRuleAction(nodeId: string, ruleId: string, action: RuleAction, isComposite: boolean): void {
    // If ActionRunner is available, use it for full action handling
    if (this.actionRunner) {
      const context: ActionContext = {
        ruleId,
        ruleName: ruleId, // Could be looked up from rule engine if needed
        nodeId,
        isComposite,
      };
      this.actionRunner.execute(action, context).catch((err) => {
        logger.error(`Action runner error: ${err}`);
      });
      return;
    }

    // Fallback: basic handling without ActionRunner
    switch (action.action) {
      case 'reject':
      case 'alert':
        this.emit('rule-action', { nodeId, ruleId, isComposite, ...action });
        logger.warn(`Rule action [${action.action}]: ${action.reason || action.message}`);
        break;
      case 'modbus_write':
        if (action.register !== undefined && action.value !== undefined) {
          this.emit('modbus-write', { register: action.register, value: action.value });
        }
        break;
      case 'log':
        logger.info(`Rule log: ${action.message}`);
        break;
      case 'pass':
        // No action needed
        break;
    }
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
