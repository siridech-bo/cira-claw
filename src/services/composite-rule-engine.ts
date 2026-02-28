/**
 * Composite Rule Engine — Spec G graph evaluation
 *
 * Evaluates composite rules by traversing the node graph and combining
 * atomic rule results through logic gates.
 *
 * Composite rules are stored as JSON files in ~/.cira/composite-rules/
 */

import fs from 'fs';
import path from 'path';
import os from 'os';
import { createLogger } from '../utils/logger.js';
import {
  StateStore,
  CompositeRule,
  CompositeNode,
  CompositeConnection,
  AtomicNodeData,
  ThresholdNodeData,
  OutputNodeData,
  StatefulConditionNodeData,
} from './state-store.js';
import { RuleResult, RuleAction, RulePayload } from './rule-engine.js';
import { SocketType } from './socket-registry.js';

const logger = createLogger('composite-rule-engine');

// ─── Result Types ────────────────────────────────────────────────────────────

export interface CompositeRuleResult {
  success: boolean;
  triggered: boolean;                // Did the composite evaluate to true?
  action?: RuleAction;               // Action to execute if triggered
  composite_id: string;
  composite_name: string;
  node_results: Map<string, boolean>; // Per-node evaluation results
  error?: string;
  execution_ms: number;
}

// ─── Composite Rule Engine ───────────────────────────────────────────────────

export class CompositeRuleEngine {
  private stateStore: StateStore;
  private rulesDir: string;

  constructor(stateStore: StateStore, rulesDir?: string) {
    this.stateStore = stateStore;
    this.rulesDir = rulesDir || path.join(os.homedir(), '.cira', 'composite-rules');
    this.ensureRulesDirectory();
  }

  /**
   * Ensure the rules directory exists.
   */
  private ensureRulesDirectory(): void {
    if (!fs.existsSync(this.rulesDir)) {
      fs.mkdirSync(this.rulesDir, { recursive: true });
      logger.info(`Created composite rules directory: ${this.rulesDir}`);
    }
  }

  // ─── File-Based Persistence ─────────────────────────────────────────────────

  /**
   * Load all composite rules from JSON files.
   */
  loadCompositeRules(): CompositeRule[] {
    try {
      const files = fs.readdirSync(this.rulesDir).filter(f => f.endsWith('.json'));
      return files.flatMap(f => {
        try {
          const content = fs.readFileSync(path.join(this.rulesDir, f), 'utf-8');
          return [JSON.parse(content) as CompositeRule];
        } catch (err) {
          logger.warn({ file: f, err }, 'Failed to parse composite rule file, skipping');
          return [];
        }
      });
    } catch (err) {
      logger.warn({ err }, 'Failed to read composite rules directory');
      return [];
    }
  }

  /**
   * Get all composite rules, optionally filtering by enabled status.
   */
  getAllCompositeRules(enabledOnly = false): CompositeRule[] {
    const rules = this.loadCompositeRules();
    if (enabledOnly) {
      return rules.filter(r => r.enabled);
    }
    return rules;
  }

  /**
   * Get a single composite rule by ID.
   */
  getCompositeRule(id: string): CompositeRule | null {
    const filePath = path.join(this.rulesDir, `${id}.json`);
    if (!fs.existsSync(filePath)) {
      return null;
    }

    try {
      const content = fs.readFileSync(filePath, 'utf-8');
      return JSON.parse(content) as CompositeRule;
    } catch (err) {
      logger.warn({ id, err }, 'Failed to read composite rule');
      return null;
    }
  }

  /**
   * Save a composite rule to a JSON file.
   * Validates the graph before saving.
   */
  saveCompositeRule(rule: CompositeRule): void {
    const validation = this.validateGraph(rule);
    if (!validation.valid) {
      throw new Error(validation.error);
    }

    const filePath = path.join(this.rulesDir, `${rule.id}.json`);
    fs.writeFileSync(filePath, JSON.stringify(rule, null, 2), 'utf-8');
    logger.info({ ruleId: rule.id }, 'Composite rule saved');
  }

  /**
   * Delete a composite rule by ID.
   * @returns true if deleted, false if not found
   */
  deleteCompositeRule(id: string): boolean {
    const filePath = path.join(this.rulesDir, `${id}.json`);
    if (!fs.existsSync(filePath)) {
      return false;
    }

    fs.unlinkSync(filePath);
    logger.info({ ruleId: id }, 'Composite rule deleted');
    return true;
  }

  /**
   * Enable or disable a composite rule.
   * @returns true if updated, false if not found
   */
  setCompositeRuleEnabled(id: string, enabled: boolean): boolean {
    const rule = this.getCompositeRule(id);
    if (!rule) {
      return false;
    }

    rule.enabled = enabled;
    this.saveCompositeRule(rule);
    return true;
  }

  // ─── Evaluation ─────────────────────────────────────────────────────────────

  /**
   * Evaluate all enabled composite rules.
   *
   * @param atomicResults - Results from RuleEngine.evaluateAll()
   * @param payload - The current rule payload (for threshold nodes)
   * @returns Map of composite rule ID to evaluation result
   */
  async evaluateAll(
    atomicResults: Map<string, RuleResult>,
    payload: RulePayload
  ): Promise<Map<string, CompositeRuleResult>> {
    const results = new Map<string, CompositeRuleResult>();

    // Load enabled composite rules
    const composites = this.getAllCompositeRules(true);

    for (const composite of composites) {
      const result = this.evaluateComposite(composite, atomicResults, payload);
      results.set(composite.id, result);
    }

    return results;
  }

  /**
   * Evaluate a single composite rule.
   */
  evaluateComposite(
    composite: CompositeRule,
    atomicResults: Map<string, RuleResult>,
    payload: RulePayload
  ): CompositeRuleResult {
    const startTime = performance.now();
    const nodeResults = new Map<string, boolean>();

    try {
      // Build adjacency map for traversal
      const adjacency = this.buildAdjacencyMap(composite.connections);

      // Find output node(s) - we evaluate backwards from outputs
      const outputNodes = composite.nodes.filter(n => n.type === 'output');

      if (outputNodes.length === 0) {
        return {
          success: false,
          triggered: false,
          composite_id: composite.id,
          composite_name: composite.name,
          node_results: nodeResults,
          error: 'No output node in composite rule',
          execution_ms: performance.now() - startTime,
        };
      }

      // Evaluate each node recursively
      const visited = new Set<string>();
      let triggered = false;
      let outputAction: RuleAction | undefined;

      for (const outputNode of outputNodes) {
        const result = this.evaluateNode(
          outputNode.id,
          composite.id,
          composite.nodes,
          adjacency,
          atomicResults,
          payload,
          nodeResults,
          visited
        );

        if (result) {
          triggered = true;
          const outputData = outputNode.data as OutputNodeData;
          outputAction = {
            action: outputData.action,
            severity: outputData.severity,
            message: outputData.message,
            register: outputData.register,
            value: outputData.value,
          };
        }
      }

      // If no output node triggered, use the composite's default output_action
      if (!triggered && outputNodes.length === 0) {
        outputAction = composite.output_action;
      }

      return {
        success: true,
        triggered,
        action: triggered ? outputAction : undefined,
        composite_id: composite.id,
        composite_name: composite.name,
        node_results: nodeResults,
        execution_ms: performance.now() - startTime,
      };
    } catch (error) {
      const errorMessage = error instanceof Error ? error.message : String(error);
      logger.warn(`Composite rule ${composite.id} evaluation failed: ${errorMessage}`);

      return {
        success: false,
        triggered: false,
        composite_id: composite.id,
        composite_name: composite.name,
        node_results: nodeResults,
        error: errorMessage,
        execution_ms: performance.now() - startTime,
      };
    }
  }

  /**
   * Build an adjacency map from connections.
   * Key: target_node:target_socket
   * Value: Array of { source_node, source_socket }
   */
  private buildAdjacencyMap(
    connections: CompositeConnection[]
  ): Map<string, Array<{ source_node: string; source_socket: string }>> {
    const adjacency = new Map<string, Array<{ source_node: string; source_socket: string }>>();

    for (const conn of connections) {
      const key = `${conn.target_node}:${conn.target_socket}`;
      if (!adjacency.has(key)) {
        adjacency.set(key, []);
      }
      adjacency.get(key)!.push({
        source_node: conn.source_node,
        source_socket: conn.source_socket,
      });
    }

    return adjacency;
  }

  /**
   * Recursively evaluate a node.
   */
  private evaluateNode(
    nodeId: string,
    compositeId: string,
    nodes: CompositeNode[],
    adjacency: Map<string, Array<{ source_node: string; source_socket: string }>>,
    atomicResults: Map<string, RuleResult>,
    payload: RulePayload,
    nodeResults: Map<string, boolean>,
    visited: Set<string>
  ): boolean {
    // Prevent infinite loops
    if (visited.has(nodeId)) {
      return nodeResults.get(nodeId) ?? false;
    }
    visited.add(nodeId);

    // Find the node
    const node = nodes.find(n => n.id === nodeId);
    if (!node) {
      logger.warn(`Node not found: ${nodeId}`);
      nodeResults.set(nodeId, false);
      return false;
    }

    let result: boolean;

    switch (node.type) {
      case 'atomic': {
        const data = node.data as AtomicNodeData;
        const atomicResult = atomicResults.get(data.rule_id);
        // An atomic rule is "true" if it returned action != 'pass'
        // (i.e., it triggered an alert, reject, etc.)
        if (atomicResult && atomicResult.success && atomicResult.action) {
          result = atomicResult.action.action !== 'pass';
        } else {
          result = false;
        }
        break;
      }

      case 'constant': {
        const data = node.data as { value: boolean };
        result = data.value;
        break;
      }

      case 'threshold': {
        const data = node.data as ThresholdNodeData;
        result = this.evaluateThreshold(data, payload);
        break;
      }

      case 'and': {
        // Get all inputs
        const inputs = this.getInputValues(
          nodeId,
          compositeId,
          'input',
          nodes,
          adjacency,
          atomicResults,
          payload,
          nodeResults,
          visited
        );
        // AND: all inputs must be true
        result = inputs.length > 0 && inputs.every(v => v);
        break;
      }

      case 'or': {
        // Get all inputs
        const inputs = this.getInputValues(
          nodeId,
          compositeId,
          'input',
          nodes,
          adjacency,
          atomicResults,
          payload,
          nodeResults,
          visited
        );
        // OR: any input must be true
        result = inputs.some(v => v);
        break;
      }

      case 'not': {
        // Get single input
        const inputs = this.getInputValues(
          nodeId,
          compositeId,
          'input',
          nodes,
          adjacency,
          atomicResults,
          payload,
          nodeResults,
          visited
        );
        // NOT: invert the input
        result = inputs.length > 0 ? !inputs[0] : true;
        break;
      }

      case 'output': {
        // Output node evaluates its input
        const inputs = this.getInputValues(
          nodeId,
          compositeId,
          'input',
          nodes,
          adjacency,
          atomicResults,
          payload,
          nodeResults,
          visited
        );
        result = inputs.length > 0 && inputs.every(v => v);
        break;
      }

      case 'stateful_condition': {
        result = this.evaluateStatefulCondition(
          nodeId,
          compositeId,
          node.data as StatefulConditionNodeData,
          nodes,
          adjacency,
          atomicResults,
          payload,
          nodeResults,
          visited
        );
        break;
      }

      default:
        logger.warn(`Unknown node type: ${node.type}`);
        result = false;
    }

    nodeResults.set(nodeId, result);
    return result;
  }

  /**
   * Get input values for a node from its connected sources.
   */
  private getInputValues(
    nodeId: string,
    compositeId: string,
    socketName: string,
    nodes: CompositeNode[],
    adjacency: Map<string, Array<{ source_node: string; source_socket: string }>>,
    atomicResults: Map<string, RuleResult>,
    payload: RulePayload,
    nodeResults: Map<string, boolean>,
    visited: Set<string>
  ): boolean[] {
    const key = `${nodeId}:${socketName}`;
    const sources = adjacency.get(key) || [];

    return sources.map(source =>
      this.evaluateNode(
        source.source_node,
        compositeId,
        nodes,
        adjacency,
        atomicResults,
        payload,
        nodeResults,
        new Set(visited) // Clone to allow multiple paths
      )
    );
  }

  /**
   * Evaluate a stateful condition node.
   * Manages persistent state across evaluations.
   */
  private evaluateStatefulCondition(
    nodeId: string,
    compositeId: string,
    data: StatefulConditionNodeData,
    nodes: CompositeNode[],
    adjacency: Map<string, Array<{ source_node: string; source_socket: string }>>,
    atomicResults: Map<string, RuleResult>,
    payload: RulePayload,
    nodeResults: Map<string, boolean>,
    visited: Set<string>
  ): boolean {
    // Get input value from connected source
    const inputs = this.getInputValues(
      nodeId,
      compositeId,
      'in',
      nodes,
      adjacency,
      atomicResults,
      payload,
      nodeResults,
      visited
    );
    const inputValue = inputs.length > 0 ? inputs[0] : false;

    // Load existing state
    const existingState = this.stateStore.getState(nodeId, compositeId) || {
      timestamps: [] as number[],
      consecutiveCount: 0,
      lastTriggerTime: 0,
      sustainedStartTime: 0,
    };

    const now = Date.now();
    const windowMs = data.window_minutes * 60 * 1000;
    let result = false;

    switch (data.condition) {
      case 'count_window': {
        // True if input has been true at least `count` times within window
        const timestamps = (existingState.timestamps as number[]).filter(
          (t: number) => now - t < windowMs
        );
        if (inputValue) {
          timestamps.push(now);
        }
        result = timestamps.length >= data.count;
        existingState.timestamps = timestamps;
        break;
      }

      case 'consecutive': {
        // True if input has been true for `count` consecutive evaluations
        let consecutiveCount = existingState.consecutiveCount as number;
        if (inputValue) {
          consecutiveCount++;
        } else {
          consecutiveCount = 0;
        }
        result = consecutiveCount >= data.count;
        existingState.consecutiveCount = consecutiveCount;
        break;
      }

      case 'rate': {
        // True if rate of true inputs exceeds threshold (count per window)
        const timestamps = (existingState.timestamps as number[]).filter(
          (t: number) => now - t < windowMs
        );
        if (inputValue) {
          timestamps.push(now);
        }
        // Rate = count / window_minutes (per minute)
        const ratePerMinute = timestamps.length / data.window_minutes;
        result = ratePerMinute >= data.count;
        existingState.timestamps = timestamps;
        break;
      }

      case 'sustained': {
        // True if input has been continuously true for at least window_minutes
        let sustainedStartTime = existingState.sustainedStartTime as number;
        if (inputValue) {
          if (sustainedStartTime === 0) {
            sustainedStartTime = now;
          }
          result = now - sustainedStartTime >= windowMs;
        } else {
          sustainedStartTime = 0;
          result = false;
        }
        existingState.sustainedStartTime = sustainedStartTime;
        break;
      }

      case 'cooldown': {
        // After triggering, prevents re-triggering for window_minutes
        const lastTriggerTime = existingState.lastTriggerTime as number;
        const cooldownExpired = now - lastTriggerTime >= windowMs;

        if (inputValue && cooldownExpired) {
          result = true;
          existingState.lastTriggerTime = now;
        } else {
          result = false;
        }
        break;
      }

      default:
        logger.warn(`Unknown stateful condition: ${data.condition}`);
        result = false;
    }

    // Save updated state
    this.stateStore.setState(nodeId, compositeId, existingState, result);

    return result;
  }

  /**
   * Evaluate a threshold condition against the payload.
   */
  private evaluateThreshold(data: ThresholdNodeData, payload: RulePayload): boolean {
    const value = this.getPayloadValue(data.field, payload);
    if (value === undefined || typeof value !== 'number') {
      return false;
    }

    switch (data.operator) {
      case '>':
        return value > data.threshold;
      case '<':
        return value < data.threshold;
      case '>=':
        return value >= data.threshold;
      case '<=':
        return value <= data.threshold;
      case '==':
        return value === data.threshold;
      case '!=':
        return value !== data.threshold;
      default:
        return false;
    }
  }

  /**
   * Get a value from the payload using dot notation path.
   */
  private getPayloadValue(path: string, payload: RulePayload): unknown {
    const parts = path.split('.');
    let current: unknown = payload;

    for (const part of parts) {
      if (current === null || current === undefined) {
        return undefined;
      }

      // Handle array access: field[0]
      const arrayMatch = part.match(/^(\w+)\[(\d+)\]$/);
      if (arrayMatch) {
        const [, field, index] = arrayMatch;
        current = (current as Record<string, unknown>)[field];
        if (Array.isArray(current)) {
          current = current[parseInt(index, 10)];
        } else {
          return undefined;
        }
      } else {
        current = (current as Record<string, unknown>)[part];
      }
    }

    return current;
  }

  /**
   * Get the state store for direct access.
   */
  getStateStore(): StateStore {
    return this.stateStore;
  }

  /**
   * Get the rules directory path.
   */
  getRulesDir(): string {
    return this.rulesDir;
  }

  /**
   * Validate a composite rule graph for cycles and structural issues.
   * @returns { valid: boolean; error?: string }
   */
  validateGraph(rule: CompositeRule): { valid: boolean; error?: string } {
    // Check for empty graph
    if (!rule.nodes || rule.nodes.length === 0) {
      return { valid: true }; // Empty graph is technically valid
    }

    // Build adjacency list for cycle detection (source -> targets)
    const adjacency = new Map<string, string[]>();
    for (const node of rule.nodes) {
      adjacency.set(node.id, []);
    }

    for (const conn of rule.connections) {
      if (!adjacency.has(conn.source_node)) {
        adjacency.set(conn.source_node, []);
      }
      adjacency.get(conn.source_node)!.push(conn.target_node);
    }

    // DFS-based cycle detection
    const WHITE = 0; // Unvisited
    const GRAY = 1;  // Currently visiting (in recursion stack)
    const BLACK = 2; // Finished visiting

    const color = new Map<string, number>();
    for (const node of rule.nodes) {
      color.set(node.id, WHITE);
    }

    const hasCycle = (nodeId: string, path: string[]): string[] | null => {
      color.set(nodeId, GRAY);
      path.push(nodeId);

      const neighbors = adjacency.get(nodeId) || [];
      for (const neighbor of neighbors) {
        if (color.get(neighbor) === GRAY) {
          // Found a cycle - return the path
          const cycleStart = path.indexOf(neighbor);
          return path.slice(cycleStart).concat(neighbor);
        }
        if (color.get(neighbor) === WHITE) {
          const cycle = hasCycle(neighbor, [...path]);
          if (cycle) return cycle;
        }
      }

      color.set(nodeId, BLACK);
      return null;
    };

    // Check each unvisited node
    for (const node of rule.nodes) {
      if (color.get(node.id) === WHITE) {
        const cycle = hasCycle(node.id, []);
        if (cycle) {
          return {
            valid: false,
            error: `Cycle detected: ${cycle.join(' → ')}`,
          };
        }
      }
    }

    // Check that all connections reference valid nodes
    const nodeIds = new Set(rule.nodes.map(n => n.id));
    for (const conn of rule.connections) {
      if (!nodeIds.has(conn.source_node)) {
        return {
          valid: false,
          error: `Connection references non-existent source node: ${conn.source_node}`,
        };
      }
      if (!nodeIds.has(conn.target_node)) {
        return {
          valid: false,
          error: `Connection references non-existent target node: ${conn.target_node}`,
        };
      }
    }

    return { valid: true };
  }
}

// ─── Factory ─────────────────────────────────────────────────────────────────

export function createCompositeRuleEngine(stateStore: StateStore, rulesDir?: string): CompositeRuleEngine {
  return new CompositeRuleEngine(stateStore, rulesDir);
}
