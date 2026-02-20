import mqtt, { MqttClient, IClientOptions } from 'mqtt';
import { CiraAgent } from '../agent/agent.js';
import { NodeManager } from '../nodes/manager.js';
import { createLogger } from '../utils/logger.js';

const logger = createLogger('mqtt-channel');

export interface MqttConfig {
  broker: string;
  topics: {
    subscribe: string[];
    publish: string[];
  };
  username?: string;
  password?: string;
  clientId?: string;
}

export interface MqttCommand {
  action: string;
  node_id?: string;
  model?: string;
  params?: Record<string, unknown>;
  reply_topic?: string;
}

export class MqttChannel {
  private client: MqttClient | null = null;
  private config: MqttConfig;
  private agent: CiraAgent;
  private nodeManager: NodeManager;
  private connected = false;

  constructor(config: MqttConfig, agent: CiraAgent, nodeManager: NodeManager) {
    this.config = config;
    this.agent = agent;
    this.nodeManager = nodeManager;
  }

  async connect(): Promise<void> {
    const options: IClientOptions = {
      clientId: this.config.clientId || `cira-gateway-${Date.now()}`,
      clean: true,
      connectTimeout: 10000,
      reconnectPeriod: 5000,
    };

    if (this.config.username) {
      options.username = this.config.username;
      options.password = this.config.password;
    }

    return new Promise((resolve, reject) => {
      try {
        this.client = mqtt.connect(this.config.broker, options);

        this.client.on('connect', () => {
          this.connected = true;
          logger.info(`Connected to MQTT broker: ${this.config.broker}`);

          // Subscribe to configured topics
          for (const topic of this.config.topics.subscribe) {
            this.client!.subscribe(topic, (err) => {
              if (err) {
                logger.error(`Failed to subscribe to ${topic}: ${err}`);
              } else {
                logger.debug(`Subscribed to ${topic}`);
              }
            });
          }

          resolve();
        });

        this.client.on('error', (error) => {
          logger.error(`MQTT error: ${error}`);
          if (!this.connected) {
            reject(error);
          }
        });

        this.client.on('close', () => {
          this.connected = false;
          logger.warn('MQTT connection closed');
        });

        this.client.on('reconnect', () => {
          logger.info('Reconnecting to MQTT broker...');
        });

        this.client.on('message', async (topic, payload) => {
          await this.handleMessage(topic, payload);
        });
      } catch (error) {
        reject(error);
      }
    });
  }

  async disconnect(): Promise<void> {
    return new Promise((resolve) => {
      if (this.client) {
        this.client.end(false, {}, () => {
          this.connected = false;
          logger.info('Disconnected from MQTT broker');
          resolve();
        });
      } else {
        resolve();
      }
    });
  }

  private async handleMessage(topic: string, payload: Buffer): Promise<void> {
    logger.debug(`MQTT message on ${topic}: ${payload.toString()}`);

    try {
      const message = JSON.parse(payload.toString()) as MqttCommand;

      // Handle different command topics
      if (topic.startsWith('cira/command/')) {
        await this.handleCommand(topic, message);
      }
    } catch (error) {
      logger.error(`Failed to process MQTT message: ${error}`);
    }
  }

  private async handleCommand(topic: string, command: MqttCommand): Promise<void> {
    const action = command.action || topic.split('/').pop();
    const replyTopic = command.reply_topic || topic.replace('/command/', '/results/');

    logger.info({ command }, `MQTT command: ${action}`);

    let result: unknown;

    try {
      switch (action) {
        case 'status':
          result = {
            nodes: this.nodeManager.getAllStatuses(),
            summary: this.nodeManager.getSummary(),
          };
          break;

        case 'node_status':
          if (!command.node_id) {
            result = { error: 'node_id required' };
          } else {
            const node = this.nodeManager.getNode(command.node_id);
            if (!node) {
              result = { error: `Node '${command.node_id}' not found` };
            } else {
              const status = await this.nodeManager.checkNodeHealth(node);
              result = status;
            }
          }
          break;

        case 'snapshot':
          if (!command.node_id) {
            result = { error: 'node_id required' };
          } else {
            const node = this.nodeManager.getNode(command.node_id);
            if (!node) {
              result = { error: `Node '${command.node_id}' not found` };
            } else {
              const port = node.runtime?.port || 8080;
              const url = `http://${node.host}:${port}/snapshot`;

              try {
                const response = await fetch(url);
                if (response.ok) {
                  result = {
                    node_id: command.node_id,
                    image_url: url,
                    timestamp: new Date().toISOString(),
                  };
                } else {
                  result = { error: `Failed to get snapshot: HTTP ${response.status}` };
                }
              } catch (fetchError) {
                result = { error: `Failed to connect to node: ${fetchError}` };
              }
            }
          }
          break;

        case 'query':
          // Natural language query to agent
          const queryText = (command.params?.query as string) || (command as unknown as { query?: string }).query;
          if (!queryText) {
            result = { error: 'query text required' };
          } else {
            const response = await this.agent.query(queryText, {
              nodeManager: this.nodeManager as unknown as undefined,
            });
            result = { response };
          }
          break;

        default:
          result = { error: `Unknown action: ${action}` };
      }
    } catch (error) {
      result = { error: error instanceof Error ? error.message : 'Unknown error' };
    }

    // Publish result
    this.publish(replyTopic, {
      action,
      timestamp: new Date().toISOString(),
      result,
    });
  }

  // Publish a message
  publish(topic: string, message: unknown): void {
    if (!this.client || !this.connected) {
      logger.warn('Cannot publish: not connected to MQTT broker');
      return;
    }

    const payload = JSON.stringify(message);
    this.client.publish(topic, payload, { qos: 1 }, (err) => {
      if (err) {
        logger.error(`Failed to publish to ${topic}: ${err}`);
      } else {
        logger.debug(`Published to ${topic}`);
      }
    });
  }

  // Publish alert
  publishAlert(nodeId: string, alertType: string, message: string): void {
    this.publish('cira/alerts/node', {
      node_id: nodeId,
      type: alertType,
      message,
      timestamp: new Date().toISOString(),
    });
  }

  // Publish inference result
  publishInferenceResult(nodeId: string, result: unknown): void {
    this.publish(`cira/results/${nodeId}`, {
      node_id: nodeId,
      timestamp: new Date().toISOString(),
      ...result as object,
    });
  }

  isConnected(): boolean {
    return this.connected;
  }
}

export function createMqttChannel(
  config: MqttConfig,
  agent: CiraAgent,
  nodeManager: NodeManager
): MqttChannel {
  return new MqttChannel(config, agent, nodeManager);
}
