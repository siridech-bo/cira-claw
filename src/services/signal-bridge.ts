/**
 * Signal Bridge Service - Feeds physical sensor data from MODBUS/MQTT to CiRA runtime
 *
 * Spec CX-1: Signal Protocol Bridges
 * (c) CiRA Robotics / KMITL 2026
 */

import * as fs from 'fs';
import * as path from 'path';
import * as net from 'net';
import { NodeManager } from '../nodes/manager.js';
import { MqttChannel } from '../channels/mqtt.js';
import { createLogger } from '../utils/logger.js';

const logger = createLogger('signal-bridge');

// ============================================================================
// Types
// ============================================================================

interface ModbusChannel {
  name: string;
  register: number;
  scale: number;
  offset: number;
}

interface MqttSubscribeChannel {
  name: string;
  json_path: string;
}

interface ModbusSource {
  id: string;
  enabled: boolean;
  protocol: 'modbus_client';
  host: string;
  port: number;
  poll_interval_ms: number;
  batch_size: number;
  node_id: string;
  channels: ModbusChannel[];
}

interface MqttSource {
  id: string;
  enabled: boolean;
  protocol: 'mqtt_subscribe';
  topic: string;
  payload_format: string;
  node_id: string;
  batch_size: number;
  channels: MqttSubscribeChannel[];
}

type SignalSource = ModbusSource | MqttSource;

interface SignalSourcesConfig {
  sources: SignalSource[];
}

export interface SourceStatus {
  id: string;
  protocol: 'modbus_client' | 'mqtt_subscribe';
  enabled: boolean;
  connected: boolean;
  validation_status: 'ok' | 'mismatch' | 'pending' | 'disabled';
  mismatched_channels: string[];
  samples_fed: number;
  batches_sent: number;
  last_feed_at: string | null;
  error: string | null;
}

interface ModbusConnection {
  socket: net.Socket;
  pollInterval: NodeJS.Timeout | null;
  reconnectTimeout: NodeJS.Timeout | null;
  transactionId: number;
  buffers: Map<string, number[]>; // channel name -> samples
  pendingRequests: Map<number, string>; // transactionId -> channel name
}

interface MqttSubscription {
  buffers: Map<string, number[]>; // channel name -> samples
}

// ============================================================================
// SignalBridgeService
// ============================================================================

export class SignalBridgeService {
  private configDir: string;
  private nodeManager: NodeManager;
  private mqttChannel: MqttChannel | null;

  private sources: SignalSource[] = [];
  private statuses: Map<string, SourceStatus> = new Map();
  private modbusConnections: Map<string, ModbusConnection> = new Map();
  private mqttSubscriptions: Map<string, MqttSubscription> = new Map();

  constructor(
    configDir: string,
    nodeManager: NodeManager,
    mqttChannel: MqttChannel | null
  ) {
    this.configDir = configDir;
    this.nodeManager = nodeManager;
    this.mqttChannel = mqttChannel;
  }

  async start(): Promise<void> {
    const configPath = path.join(this.configDir, 'signal-sources.json');

    if (!fs.existsSync(configPath)) {
      logger.info('No signal-sources.json found, bridge idle');
      return;
    }

    try {
      const content = fs.readFileSync(configPath, 'utf-8');
      const config: SignalSourcesConfig = JSON.parse(content);
      this.sources = config.sources || [];
    } catch (err) {
      logger.error(`Failed to parse signal-sources.json: ${err}`);
      return;
    }

    let activeCount = 0;

    for (const source of this.sources) {
      // Initialize status
      this.statuses.set(source.id, {
        id: source.id,
        protocol: source.protocol,
        enabled: source.enabled,
        connected: false,
        validation_status: 'pending',
        mismatched_channels: [],
        samples_fed: 0,
        batches_sent: 0,
        last_feed_at: null,
        error: null,
      });

      if (!source.enabled) {
        this.updateStatus(source.id, { validation_status: 'disabled' });
        continue;
      }

      // Validate channel count
      if (source.channels.length > 16) {
        logger.error(`Source '${source.id}' has ${source.channels.length} channels (max 16)`);
        this.updateStatus(source.id, {
          validation_status: 'disabled',
          error: 'Too many channels (max 16)',
        });
        continue;
      }

      // Validate against model
      const valid = await this.validateSourceAgainstModel(source);

      if (!valid) {
        continue; // Status already updated in validateSourceAgainstModel
      }

      // Start protocol handler
      if (source.protocol === 'modbus_client') {
        this.startModbusClient(source as ModbusSource);
      } else if (source.protocol === 'mqtt_subscribe') {
        this.startMqttSubscriber(source as MqttSource);
      }

      activeCount++;
    }

    logger.info(`Signal bridge started: ${activeCount}/${this.sources.length} sources active`);
  }

  async stop(): Promise<void> {
    // Stop all MODBUS connections
    for (const [sourceId, conn] of this.modbusConnections) {
      if (conn.pollInterval) clearInterval(conn.pollInterval);
      if (conn.reconnectTimeout) clearTimeout(conn.reconnectTimeout);
      conn.socket.destroy();
      logger.debug(`Stopped MODBUS client: ${sourceId}`);
    }
    this.modbusConnections.clear();

    // Unsubscribe MQTT topics
    if (this.mqttChannel) {
      for (const source of this.sources) {
        if (source.protocol === 'mqtt_subscribe') {
          this.mqttChannel.unsubscribe((source as MqttSource).topic);
        }
      }
    }
    this.mqttSubscriptions.clear();

    logger.info('Signal bridge stopped');
  }

  // ============================================================================
  // Validation
  // ============================================================================

  private async validateSourceAgainstModel(source: SignalSource): Promise<boolean> {
    const node = this.nodeManager.getNode(source.node_id);

    if (!node) {
      logger.warn(`Source '${source.id}' references unknown node '${source.node_id}'`);
      this.updateStatus(source.id, {
        validation_status: 'pending',
        error: `Node '${source.node_id}' not found`,
      });
      return false;
    }

    const runtimePort = node.runtime?.port || 8080;
    const url = `http://${node.host}:${runtimePort}/api/signal/channels`;

    try {
      const response = await fetch(url, {
        signal: AbortSignal.timeout(3000),
      });

      if (!response.ok) {
        this.updateStatus(source.id, {
          validation_status: 'pending',
          error: `Runtime returned HTTP ${response.status}`,
        });
        return false;
      }

      const data = await response.json() as {
        loaded: boolean;
        channels: string[];
      };

      if (!data.loaded) {
        logger.info(`SIGNAL slot empty on node '${source.node_id}' — source '${source.id}' waiting for model load`);
        this.updateStatus(source.id, {
          validation_status: 'pending',
          error: 'SIGNAL slot empty',
        });
        return false;
      }

      // Check channel names
      const modelChannels = new Set(data.channels);
      const mismatched: string[] = [];

      for (const ch of source.channels) {
        const channelName = 'name' in ch ? ch.name : '';
        if (!modelChannels.has(channelName)) {
          mismatched.push(channelName);
        }
      }

      if (mismatched.length > 0) {
        logger.error(
          `Source '${source.id}' channel mismatch: ${mismatched.join(', ')} ` +
          `not found in signal model (model expects: ${data.channels.join(', ')})`
        );
        this.updateStatus(source.id, {
          validation_status: 'mismatch',
          mismatched_channels: mismatched,
          error: `Channels not in model: ${mismatched.join(', ')}`,
        });
        return false;
      }

      this.updateStatus(source.id, {
        validation_status: 'ok',
        mismatched_channels: [],
        error: null,
      });
      return true;
    } catch (err) {
      const message = err instanceof Error ? err.message : String(err);
      logger.warn(`Failed to validate source '${source.id}': ${message}`);
      this.updateStatus(source.id, {
        validation_status: 'pending',
        error: message,
      });
      return false;
    }
  }

  /**
   * Called when a model is loaded on a node - re-validate sources.
   */
  async notifyModelChanged(nodeId: string): Promise<void> {
    logger.debug(`Model changed on node '${nodeId}', re-validating sources`);

    for (const source of this.sources) {
      if (source.node_id !== nodeId) continue;

      const status = this.statuses.get(source.id);
      if (!status || !source.enabled) continue;

      // Re-validate
      const valid = await this.validateSourceAgainstModel(source);

      // If now valid and not already connected, start the handler
      if (valid && !status.connected) {
        if (source.protocol === 'modbus_client') {
          this.startModbusClient(source as ModbusSource);
        } else if (source.protocol === 'mqtt_subscribe') {
          this.startMqttSubscriber(source as MqttSource);
        }
      }
    }
  }

  // ============================================================================
  // MODBUS Client
  // ============================================================================

  private startModbusClient(source: ModbusSource): void {
    const conn: ModbusConnection = {
      socket: new net.Socket(),
      pollInterval: null,
      reconnectTimeout: null,
      transactionId: 0,
      buffers: new Map(),
      pendingRequests: new Map(),
    };

    // Initialize buffers for each channel
    for (const ch of source.channels) {
      conn.buffers.set(ch.name, []);
    }

    this.modbusConnections.set(source.id, conn);

    const connect = () => {
      conn.socket = new net.Socket();

      conn.socket.connect(source.port, source.host, () => {
        logger.info(`MODBUS connected: ${source.id} -> ${source.host}:${source.port}`);
        this.updateStatus(source.id, { connected: true, error: null });

        // Start polling
        conn.pollInterval = setInterval(() => {
          this.pollModbusRegisters(source, conn);
        }, source.poll_interval_ms);
      });

      conn.socket.on('data', (data) => {
        this.handleModbusResponse(source, conn, data);
      });

      conn.socket.on('error', (err) => {
        logger.error(`MODBUS error on '${source.id}': ${err.message}`);
        this.updateStatus(source.id, { connected: false, error: err.message });
      });

      conn.socket.on('close', () => {
        logger.warn(`MODBUS connection closed: ${source.id}`);
        this.updateStatus(source.id, { connected: false });

        if (conn.pollInterval) {
          clearInterval(conn.pollInterval);
          conn.pollInterval = null;
        }

        // Clear pending requests to prevent stale entries from matching new connection's responses
        conn.pendingRequests.clear();

        // Auto-reconnect after 2 seconds
        conn.reconnectTimeout = setTimeout(() => {
          logger.info(`MODBUS reconnecting: ${source.id}`);
          connect();
        }, 2000);
      });
    };

    connect();
  }

  private pollModbusRegisters(source: ModbusSource, conn: ModbusConnection): void {
    // Read each channel's register using FC 03
    for (const ch of source.channels) {
      conn.transactionId = (conn.transactionId + 1) & 0xFFFF;

      // Track which channel this transaction ID is for
      conn.pendingRequests.set(conn.transactionId, ch.name);

      // MODBUS TCP frame: MBAP Header (7 bytes) + PDU
      const request = Buffer.alloc(12);
      request.writeUInt16BE(conn.transactionId, 0);  // Transaction ID
      request.writeUInt16BE(0, 2);                    // Protocol ID
      request.writeUInt16BE(6, 4);                    // Length (6 bytes following)
      request.writeUInt8(1, 6);                       // Unit ID
      request.writeUInt8(0x03, 7);                    // Function Code: Read Holding Registers
      request.writeUInt16BE(ch.register, 8);          // Starting Address
      request.writeUInt16BE(1, 10);                   // Quantity (1 register)

      try {
        conn.socket.write(request);
      } catch (err) {
        logger.debug(`MODBUS write failed for ${source.id}: ${err}`);
      }
    }
  }

  private handleModbusResponse(
    source: ModbusSource,
    conn: ModbusConnection,
    data: Buffer
  ): void {
    // Minimum response: MBAP header (7) + FC (1) + byte count (1) + data (2)
    if (data.length < 11) return;

    const functionCode = data.readUInt8(7);

    // Check for exception
    if (functionCode & 0x80) {
      const exceptionCode = data.readUInt8(8);
      logger.warn(`MODBUS exception ${exceptionCode} on '${source.id}'`);
      return;
    }

    // FC 03 response: FC (1) + byte count (1) + data
    if (functionCode !== 0x03) return;

    const byteCount = data.readUInt8(8);
    if (byteCount < 2) return;

    // Read transaction ID from MBAP header (bytes 0-1)
    const responseTxId = data.readUInt16BE(0);
    const channelName = conn.pendingRequests.get(responseTxId);
    conn.pendingRequests.delete(responseTxId);

    if (!channelName) {
      logger.debug(`Unexpected MODBUS response txId=${responseTxId} on '${source.id}'`);
      return;
    }

    // Find the channel config by name
    const ch = source.channels.find(c => c.name === channelName);
    if (!ch) return;

    // Read register value as signed 16-bit
    const rawValue = data.readInt16BE(9);

    // Apply scale and offset
    const value = rawValue * ch.scale + ch.offset;

    // Add to buffer
    const buffer = conn.buffers.get(channelName);
    if (buffer) {
      buffer.push(value);

      // Check if batch is ready
      const batchSize = source.batch_size || 64;
      if (buffer.length >= batchSize) {
        this.feedBatch(source, channelName, buffer.splice(0, batchSize));
      }
    }
  }

  // ============================================================================
  // MQTT Subscriber
  // ============================================================================

  private startMqttSubscriber(source: MqttSource): void {
    if (!this.mqttChannel) {
      logger.warn(`MQTT not available, source '${source.id}' disabled`);
      this.updateStatus(source.id, {
        validation_status: 'disabled',
        error: 'MQTT not available',
      });
      return;
    }

    const sub: MqttSubscription = {
      buffers: new Map(),
    };

    // Initialize buffers
    for (const ch of source.channels) {
      sub.buffers.set(ch.name, []);
    }

    this.mqttSubscriptions.set(source.id, sub);

    // Subscribe to topic
    this.mqttChannel.subscribe(source.topic, (_topic, payload) => {
      this.handleMqttMessage(source, sub, payload);
    });

    this.updateStatus(source.id, { connected: true });
    logger.info(`MQTT subscribed: ${source.id} -> ${source.topic}`);
  }

  private handleMqttMessage(
    source: MqttSource,
    sub: MqttSubscription,
    payload: Buffer
  ): void {
    try {
      const data = JSON.parse(payload.toString());

      for (const ch of source.channels) {
        const value = this.extractJsonPath(data, ch.json_path);
        if (typeof value === 'number') {
          const buffer = sub.buffers.get(ch.name);
          if (buffer) {
            buffer.push(value);

            // Check if batch is ready
            const batchSize = source.batch_size || 64;
            if (buffer.length >= batchSize) {
              this.feedBatch(source, ch.name, buffer.splice(0, batchSize));
            }
          }
        }
      }
    } catch (err) {
      logger.debug(`Failed to parse MQTT payload for '${source.id}': ${err}`);
    }
  }

  /**
   * Simple dot-notation JSON path extraction.
   */
  private extractJsonPath(obj: unknown, path: string): unknown {
    const parts = path.split('.');
    let current: unknown = obj;

    for (const part of parts) {
      if (current === null || current === undefined) return undefined;
      if (typeof current !== 'object') return undefined;
      current = (current as Record<string, unknown>)[part];
    }

    return current;
  }

  // ============================================================================
  // Feed Batch
  // ============================================================================

  private async feedBatch(
    source: SignalSource,
    channelName: string,
    samples: number[]
  ): Promise<void> {
    const node = this.nodeManager.getNode(source.node_id);
    if (!node) {
      logger.warn(`Node '${source.node_id}' not found for feed`);
      return;
    }

    const runtimePort = node.runtime?.port || 8080;
    const url = `http://${node.host}:${runtimePort}/api/signal/feed`;

    // Find channel index
    const channelIdx = source.channels.findIndex(
      (c) => ('name' in c ? c.name : '') === channelName
    );

    if (channelIdx < 0) {
      logger.warn(`Channel '${channelName}' not found in source '${source.id}'`);
      return;
    }

    try {
      const response = await fetch(url, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ channel: channelIdx, samples }),
        signal: AbortSignal.timeout(5000),
      });

      if (response.ok) {
        const status = this.statuses.get(source.id);
        if (status) {
          status.samples_fed += samples.length;
          status.batches_sent += 1;
          status.last_feed_at = new Date().toISOString();
        }
      } else {
        logger.warn(`Feed failed for '${source.id}': HTTP ${response.status}`);
      }
    } catch (err) {
      logger.debug(`Feed error for '${source.id}': ${err}`);
    }
  }

  // ============================================================================
  // Status & Config Management
  // ============================================================================

  private updateStatus(sourceId: string, update: Partial<SourceStatus>): void {
    const status = this.statuses.get(sourceId);
    if (status) {
      Object.assign(status, update);
    }
  }

  getAllStatuses(): SourceStatus[] {
    return Array.from(this.statuses.values());
  }

  getStatus(sourceId: string): SourceStatus | undefined {
    return this.statuses.get(sourceId);
  }

  /**
   * Reload configuration from file.
   */
  async reload(): Promise<void> {
    await this.stop();
    this.sources = [];
    this.statuses.clear();
    await this.start();
  }

  /**
   * Test a source - attempt connection and validate against model.
   */
  async testSource(sourceId: string): Promise<{
    id: string;
    reachable: boolean;
    validation_status: string;
    mismatched_channels: string[];
    model_channels: string[];
    source_channels: string[];
    error?: string;
  }> {
    const source = this.sources.find((s) => s.id === sourceId);
    if (!source) {
      return {
        id: sourceId,
        reachable: false,
        validation_status: 'error',
        mismatched_channels: [],
        model_channels: [],
        source_channels: [],
        error: 'Source not found',
      };
    }

    const node = this.nodeManager.getNode(source.node_id);
    if (!node) {
      return {
        id: sourceId,
        reachable: false,
        validation_status: 'error',
        mismatched_channels: [],
        model_channels: [],
        source_channels: source.channels.map((c) => ('name' in c ? c.name : '')),
        error: `Node '${source.node_id}' not found`,
      };
    }

    // Test reachability
    let reachable = false;
    if (source.protocol === 'modbus_client') {
      reachable = await this.testModbusConnection(
        (source as ModbusSource).host,
        (source as ModbusSource).port
      );
    } else if (source.protocol === 'mqtt_subscribe') {
      reachable = this.mqttChannel?.isConnected() || false;
    }

    // Validate against model
    const runtimePort = node.runtime?.port || 8080;
    const url = `http://${node.host}:${runtimePort}/api/signal/channels`;

    try {
      const response = await fetch(url, { signal: AbortSignal.timeout(3000) });
      if (!response.ok) {
        return {
          id: sourceId,
          reachable,
          validation_status: 'pending',
          mismatched_channels: [],
          model_channels: [],
          source_channels: source.channels.map((c) => ('name' in c ? c.name : '')),
          error: `Runtime HTTP ${response.status}`,
        };
      }

      const data = await response.json() as { loaded: boolean; channels: string[] };

      if (!data.loaded) {
        return {
          id: sourceId,
          reachable,
          validation_status: 'pending',
          mismatched_channels: [],
          model_channels: [],
          source_channels: source.channels.map((c) => ('name' in c ? c.name : '')),
          error: 'SIGNAL slot empty',
        };
      }

      const modelChannels = new Set(data.channels);
      const mismatched: string[] = [];
      const sourceChannels = source.channels.map((c) => ('name' in c ? c.name : ''));

      for (const chName of sourceChannels) {
        if (!modelChannels.has(chName)) {
          mismatched.push(chName);
        }
      }

      return {
        id: sourceId,
        reachable,
        validation_status: mismatched.length > 0 ? 'mismatch' : 'ok',
        mismatched_channels: mismatched,
        model_channels: data.channels,
        source_channels: sourceChannels,
      };
    } catch (err) {
      return {
        id: sourceId,
        reachable,
        validation_status: 'error',
        mismatched_channels: [],
        model_channels: [],
        source_channels: source.channels.map((c) => ('name' in c ? c.name : '')),
        error: err instanceof Error ? err.message : String(err),
      };
    }
  }

  private testModbusConnection(host: string, port: number): Promise<boolean> {
    return new Promise((resolve) => {
      const socket = new net.Socket();
      const timeout = setTimeout(() => {
        socket.destroy();
        resolve(false);
      }, 2000);

      socket.connect(port, host, () => {
        clearTimeout(timeout);
        socket.destroy();
        resolve(true);
      });

      socket.on('error', () => {
        clearTimeout(timeout);
        resolve(false);
      });
    });
  }

  /**
   * Save sources configuration to file.
   */
  saveConfig(sources: SignalSource[]): void {
    const configPath = path.join(this.configDir, 'signal-sources.json');
    const tempPath = configPath + '.tmp';

    const config: SignalSourcesConfig = { sources };
    fs.writeFileSync(tempPath, JSON.stringify(config, null, 2));
    fs.renameSync(tempPath, configPath);
  }

  /**
   * Get all sources configuration.
   */
  getSources(): SignalSource[] {
    return this.sources;
  }

  /**
   * Add or update a source.
   */
  upsertSource(source: SignalSource): void {
    const idx = this.sources.findIndex((s) => s.id === source.id);
    if (idx >= 0) {
      this.sources[idx] = source;
    } else {
      this.sources.push(source);
    }
    this.saveConfig(this.sources);
  }

  /**
   * Remove a source.
   */
  removeSource(sourceId: string): boolean {
    const idx = this.sources.findIndex((s) => s.id === sourceId);
    if (idx < 0) return false;

    this.sources.splice(idx, 1);
    this.saveConfig(this.sources);
    return true;
  }
}

// ============================================================================
// Factory
// ============================================================================

export function createSignalBridgeService(
  configDir: string,
  nodeManager: NodeManager,
  mqttChannel: MqttChannel | null
): SignalBridgeService {
  return new SignalBridgeService(configDir, nodeManager, mqttChannel);
}
