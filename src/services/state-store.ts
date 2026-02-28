/**
 * State Store — SQLite persistence for Spec G stateful node state
 *
 * Uses better-sqlite3 for synchronous operations.
 * Stores ONLY stateful node state (node_state table).
 * Composite rules are stored as JSON files in ~/.cira/composite-rules/
 */

import Database from 'better-sqlite3';
import path from 'path';
import fs from 'fs';
import { createLogger } from '../utils/logger.js';
import { SocketType } from './socket-registry.js';

const logger = createLogger('state-store');

// ─── Composite Rule Types ────────────────────────────────────────────────────
// These types are exported for use by composite-rule-engine and API routes.
// Storage of CompositeRule is handled by CompositeRuleEngine (JSON files).

/**
 * A composite rule connects multiple atomic rules via logic gates.
 * The Rete.js editor produces/consumes this structure.
 */
export interface CompositeRule {
  id: string;                        // Unique ID (uuid or slug)
  name: string;                      // Human-readable name
  description: string;               // What this composite does
  enabled: boolean;                  // Whether to evaluate
  created_at: string;                // ISO timestamp
  created_by: string;                // 'ai-agent' | 'manual' | 'dashboard'
  // Graph structure
  nodes: CompositeNode[];            // All nodes in the graph
  connections: CompositeConnection[]; // Edges between nodes
  // Output
  output_action: OutputAction;       // What happens when composite fires
  // Optional Spec G v2 fields
  evaluation_mode?: 'logical' | 'stateful';
  max_depth?: number;
  version?: string;
}

/**
 * A node in the composite rule graph.
 * Can be: atomic rule reference, logic gate, constant, threshold, stateful condition, or output.
 */
export interface CompositeNode {
  id: string;                        // Unique within this graph
  type: 'atomic' | 'and' | 'or' | 'not' | 'constant' | 'threshold' | 'output' | 'stateful_condition';
  // Position for Rete.js rendering
  position: { x: number; y: number };
  // Type-specific data
  data: AtomicNodeData | GateNodeData | ConstantNodeData | ThresholdNodeData | OutputNodeData | StatefulConditionNodeData;
}

export interface AtomicNodeData {
  rule_id: string;                   // References SavedRule.id
  socket_type: SocketType;           // Copied from SavedRule for display
  label?: string;                    // Custom label override
}

export interface GateNodeData {
  gate_type: 'and' | 'or' | 'not';
}

export interface ConstantNodeData {
  value: boolean;
}

export interface ThresholdNodeData {
  operator: '>' | '<' | '>=' | '<=' | '==' | '!=';
  threshold: number;
  field: string;                     // Payload field path
}

export interface OutputNodeData {
  action: 'pass' | 'reject' | 'alert' | 'log' | 'modbus_write';
  // For alert
  severity?: 'info' | 'warning' | 'critical';
  message?: string;
  // For modbus_write
  register?: number;
  value?: number;
}

export interface StatefulConditionNodeData {
  condition: 'count_window' | 'consecutive' | 'rate' | 'sustained' | 'cooldown';
  accepts_socket_type: SocketType;
  count: number;
  window_minutes: number;
}

/**
 * A connection between two nodes.
 */
export interface CompositeConnection {
  id: string;
  source_node: string;               // Node ID
  source_socket: string;             // Output socket name
  target_node: string;               // Node ID
  target_socket: string;             // Input socket name
}

/**
 * Output action when composite evaluates to true.
 */
export interface OutputAction {
  action: 'pass' | 'reject' | 'alert' | 'log' | 'modbus_write';
  severity?: 'info' | 'warning' | 'critical';
  message?: string;
  register?: number;
  value?: number;
}

// ─── State Store Class ───────────────────────────────────────────────────────
// ONLY handles node_state table for stateful condition nodes.
// Composite rules are stored as JSON files by CompositeRuleEngine.

export class StateStore {
  private db: Database.Database;
  private dbPath: string;

  constructor(dbPath: string) {
    this.dbPath = dbPath;
    this.ensureDirectory();
    this.db = new Database(dbPath);
    this.init();
  }

  private ensureDirectory(): void {
    const dir = path.dirname(this.dbPath);
    if (!fs.existsSync(dir)) {
      fs.mkdirSync(dir, { recursive: true });
      logger.info(`Created state directory: ${dir}`);
    }
  }

  private init(): void {
    // Enable WAL mode for better concurrent access
    this.db.pragma('journal_mode = WAL');

    // Create node_state table for stateful condition nodes
    this.db.exec(`
      CREATE TABLE IF NOT EXISTS node_state (
        node_id TEXT NOT NULL,
        composite_rule_id TEXT NOT NULL,
        state_json TEXT NOT NULL,
        last_result INTEGER DEFAULT 0,
        updated_at TEXT NOT NULL,
        PRIMARY KEY (node_id, composite_rule_id)
      )
    `);

    logger.info(`State store initialized: ${this.dbPath}`);
  }

  // ─── Stateful Node State Operations ─────────────────────────────────────────

  /**
   * Get state for a specific node in a composite rule.
   */
  getState(nodeId: string, compositeRuleId: string): Record<string, unknown> | null {
    const stmt = this.db.prepare(`
      SELECT state_json FROM node_state WHERE node_id = ? AND composite_rule_id = ?
    `);
    const row = stmt.get(nodeId, compositeRuleId) as { state_json: string } | undefined;
    if (!row) {
      return null;
    }
    return JSON.parse(row.state_json) as Record<string, unknown>;
  }

  /**
   * Set state for a specific node in a composite rule.
   */
  setState(nodeId: string, compositeRuleId: string, state: Record<string, unknown>, lastResult: boolean): void {
    const stmt = this.db.prepare(`
      INSERT OR REPLACE INTO node_state (node_id, composite_rule_id, state_json, last_result, updated_at)
      VALUES (?, ?, ?, ?, ?)
    `);
    stmt.run(nodeId, compositeRuleId, JSON.stringify(state), lastResult ? 1 : 0, new Date().toISOString());
  }

  /**
   * Clear state for a specific node.
   */
  clearState(nodeId: string, compositeRuleId: string): void {
    const stmt = this.db.prepare(`
      DELETE FROM node_state WHERE node_id = ? AND composite_rule_id = ?
    `);
    stmt.run(nodeId, compositeRuleId);
  }

  /**
   * Get all states for a composite rule (for debugging/API).
   */
  getAllStates(compositeRuleId: string): Array<{ nodeId: string; state: Record<string, unknown>; lastResult: boolean }> {
    const stmt = this.db.prepare(`
      SELECT node_id, state_json, last_result FROM node_state WHERE composite_rule_id = ?
    `);
    const rows = stmt.all(compositeRuleId) as Array<{ node_id: string; state_json: string; last_result: number }>;
    return rows.map(row => ({
      nodeId: row.node_id,
      state: JSON.parse(row.state_json) as Record<string, unknown>,
      lastResult: row.last_result === 1,
    }));
  }

  /**
   * Clear all states for a composite rule (reset all stateful nodes).
   */
  clearAllStates(compositeRuleId: string): void {
    const stmt = this.db.prepare(`
      DELETE FROM node_state WHERE composite_rule_id = ?
    `);
    stmt.run(compositeRuleId);
    logger.info(`Cleared all states for composite rule: ${compositeRuleId}`);
  }

  // ─── Utility ───────────────────────────────────────────────────────────────

  /**
   * Close the database connection.
   */
  close(): void {
    this.db.close();
    logger.info('State store closed');
  }

  /**
   * Get database path.
   */
  getDbPath(): string {
    return this.dbPath;
  }
}

// ─── Factory ─────────────────────────────────────────────────────────────────

/**
 * Create a StateStore instance.
 */
export function createStateStore(dbPath: string): StateStore {
  return new StateStore(dbPath);
}
