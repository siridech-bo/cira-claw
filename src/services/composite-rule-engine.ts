/**
 * Composite Rule Engine — Spec G graph evaluation
 *
 * Evaluates composite rules by traversing the node graph and combining
 * atomic rule results through logic gates.
 */

import { createLogger } from '../utils/logger.js';
import {
  StateStore,
  CompositeRule,
  CompositeNode,
  CompositeConnection,
  AtomicNodeData,
  ThresholdNodeData,
  OutputNodeData,
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

  constructor(stateStore: StateStore) {
    this.stateStore = stateStore;
  }

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
    const composites = this.stateStore.getAllCompositeRules(true);

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
}

// ─── Factory ─────────────────────────────────────────────────────────────────

export function createCompositeRuleEngine(stateStore: StateStore): CompositeRuleEngine {
  return new CompositeRuleEngine(stateStore);
}
