/**
 * Action Runner — Executes rule actions (Spec G)
 *
 * When a composite or atomic rule triggers, the action runner
 * handles the side effects: logging, alerting, MODBUS writes, etc.
 */

import { createLogger } from '../utils/logger.js';
import { RuleAction } from './rule-engine.js';
import { MqttChannel } from '../channels/mqtt.js';
import { ModbusServer } from './modbus-server.js';

const logger = createLogger('action-runner');

// ─── Action Result ───────────────────────────────────────────────────────────

export interface ActionResult {
  success: boolean;
  action: RuleAction['action'];
  message?: string;
  error?: string;
}

// ─── Action Runner ───────────────────────────────────────────────────────────

export class ActionRunner {
  private mqttChannel: MqttChannel | null;
  private modbusServer: ModbusServer | null;

  constructor(
    mqttChannel: MqttChannel | null = null,
    modbusServer: ModbusServer | null = null
  ) {
    this.mqttChannel = mqttChannel;
    this.modbusServer = modbusServer;
  }

  /**
   * Execute a rule action.
   */
  async execute(action: RuleAction, context: ActionContext): Promise<ActionResult> {
    try {
      switch (action.action) {
        case 'pass':
          // No action needed for pass
          return { success: true, action: 'pass' };

        case 'reject':
          return await this.executeReject(action, context);

        case 'alert':
          return await this.executeAlert(action, context);

        case 'log':
          return await this.executeLog(action, context);

        case 'modbus_write':
          return await this.executeModbusWrite(action, context);

        default:
          logger.warn(`Unknown action type: ${(action as RuleAction).action}`);
          return {
            success: false,
            action: action.action,
            error: `Unknown action type: ${action.action}`,
          };
      }
    } catch (error) {
      const errorMessage = error instanceof Error ? error.message : String(error);
      logger.error(`Action execution failed: ${errorMessage}`);
      return {
        success: false,
        action: action.action,
        error: errorMessage,
      };
    }
  }

  /**
   * Execute a reject action.
   * Logs the rejection and optionally publishes to MQTT.
   */
  private async executeReject(action: RuleAction, context: ActionContext): Promise<ActionResult> {
    const message = action.reason || 'Item rejected by rule';

    logger.warn({
      type: 'reject',
      rule: context.ruleId,
      node: context.nodeId,
      reason: message,
    }, `REJECT: ${message}`);

    // Publish to MQTT if available
    if (this.mqttChannel?.isConnected()) {
      await this.mqttChannel.publish('cira/rules/reject', {
        timestamp: new Date().toISOString(),
        rule_id: context.ruleId,
        rule_name: context.ruleName,
        node_id: context.nodeId,
        reason: message,
      });
    }

    return {
      success: true,
      action: 'reject',
      message,
    };
  }

  /**
   * Execute an alert action.
   * Logs with severity level and optionally publishes to MQTT.
   */
  private async executeAlert(action: RuleAction, context: ActionContext): Promise<ActionResult> {
    const severity = action.severity || 'info';
    const message = action.message || action.reason || 'Alert triggered by rule';

    const logData = {
      type: 'alert',
      severity,
      rule: context.ruleId,
      node: context.nodeId,
      message,
    };

    // Log with appropriate level
    switch (severity) {
      case 'critical':
        logger.error(logData, `CRITICAL ALERT: ${message}`);
        break;
      case 'warning':
        logger.warn(logData, `WARNING: ${message}`);
        break;
      default:
        logger.info(logData, `ALERT: ${message}`);
    }

    // Publish to MQTT if available
    if (this.mqttChannel?.isConnected()) {
      await this.mqttChannel.publish('cira/rules/alert', {
        timestamp: new Date().toISOString(),
        rule_id: context.ruleId,
        rule_name: context.ruleName,
        node_id: context.nodeId,
        severity,
        message,
      });
    }

    return {
      success: true,
      action: 'alert',
      message,
    };
  }

  /**
   * Execute a log action.
   * Simply logs the message for auditing purposes.
   */
  private async executeLog(action: RuleAction, context: ActionContext): Promise<ActionResult> {
    const message = action.reason || action.message || 'Rule logged';

    logger.info({
      type: 'rule_log',
      rule: context.ruleId,
      node: context.nodeId,
    }, `RULE LOG: ${message}`);

    return {
      success: true,
      action: 'log',
      message,
    };
  }

  /**
   * Execute a MODBUS write action.
   * Writes a value to a holding register.
   */
  private async executeModbusWrite(action: RuleAction, context: ActionContext): Promise<ActionResult> {
    if (!this.modbusServer) {
      logger.warn(`MODBUS write requested but server not available`);
      return {
        success: false,
        action: 'modbus_write',
        error: 'MODBUS server not available',
      };
    }

    const register = action.register;
    const value = action.value;

    if (register === undefined || value === undefined) {
      return {
        success: false,
        action: 'modbus_write',
        error: 'Missing register or value for MODBUS write',
      };
    }

    try {
      // Write to holding register
      this.modbusServer.setHoldingRegister(register, value);

      logger.info({
        type: 'modbus_write',
        rule: context.ruleId,
        register,
        value,
      }, `MODBUS write: register ${register} = ${value}`);

      return {
        success: true,
        action: 'modbus_write',
        message: `Wrote ${value} to register ${register}`,
      };
    } catch (error) {
      const errorMessage = error instanceof Error ? error.message : String(error);
      return {
        success: false,
        action: 'modbus_write',
        error: errorMessage,
      };
    }
  }

  /**
   * Update MQTT channel reference.
   */
  setMqttChannel(channel: MqttChannel | null): void {
    this.mqttChannel = channel;
  }

  /**
   * Update MODBUS server reference.
   */
  setModbusServer(server: ModbusServer | null): void {
    this.modbusServer = server;
  }
}

// ─── Context Types ───────────────────────────────────────────────────────────

export interface ActionContext {
  ruleId: string;           // ID of the rule that triggered
  ruleName: string;         // Human-readable name
  nodeId: string;           // Edge node ID
  isComposite: boolean;     // true if from composite rule
}

// ─── Factory ─────────────────────────────────────────────────────────────────

export function createActionRunner(
  mqttChannel: MqttChannel | null = null,
  modbusServer: ModbusServer | null = null
): ActionRunner {
  return new ActionRunner(mqttChannel, modbusServer);
}
