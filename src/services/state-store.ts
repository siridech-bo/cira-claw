/**
 * State Store — SQLite persistence for Spec G composite rules
 *
 * Uses better-sqlite3 for synchronous operations.
 * Stores composite rule definitions and Rete.js node positions.
 */

import Database from 'better-sqlite3';
import path from 'path';
import fs from 'fs';
import { createLogger } from '../utils/logger.js';
import { SocketType, isValidSocketType } from './socket-registry.js';

const logger = createLogger('state-store');

// ─── Composite Rule Types ────────────────────────────────────────────────────

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
}

/**
 * A node in the composite rule graph.
 * Can be: atomic rule reference, logic gate, or constant.
 */
export interface CompositeNode {
  id: string;                        // Unique within this graph
  type: 'atomic' | 'and' | 'or' | 'not' | 'constant' | 'threshold' | 'output';
  // Position for Rete.js rendering
  position: { x: number; y: number };
  // Type-specific data
  data: AtomicNodeData | GateNodeData | ConstantNodeData | ThresholdNodeData | OutputNodeData;
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

    // Create composite_rules table
    this.db.exec(`
      CREATE TABLE IF NOT EXISTS composite_rules (
        id TEXT PRIMARY KEY,
        name TEXT NOT NULL,
        description TEXT DEFAULT '',
        enabled INTEGER DEFAULT 1,
        created_at TEXT NOT NULL,
        created_by TEXT DEFAULT 'manual',
        nodes_json TEXT NOT NULL,
        connections_json TEXT NOT NULL,
        output_action_json TEXT NOT NULL
      )
    `);

    // Create index on enabled for faster filtering
    this.db.exec(`
      CREATE INDEX IF NOT EXISTS idx_composite_rules_enabled
      ON composite_rules(enabled)
    `);

    logger.info(`State store initialized: ${this.dbPath}`);
  }

  // ─── Composite Rule Operations ─────────────────────────────────────────────

  /**
   * Save a composite rule (insert or update).
   */
  saveCompositeRule(rule: CompositeRule): void {
    const stmt = this.db.prepare(`
      INSERT OR REPLACE INTO composite_rules
      (id, name, description, enabled, created_at, created_by, nodes_json, connections_json, output_action_json)
      VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
    `);

    stmt.run(
      rule.id,
      rule.name,
      rule.description,
      rule.enabled ? 1 : 0,
      rule.created_at,
      rule.created_by,
      JSON.stringify(rule.nodes),
      JSON.stringify(rule.connections),
      JSON.stringify(rule.output_action)
    );

    logger.info(`Saved composite rule: ${rule.id}`);
  }

  /**
   * Load a composite rule by ID.
   */
  getCompositeRule(id: string): CompositeRule | null {
    const stmt = this.db.prepare(`
      SELECT * FROM composite_rules WHERE id = ?
    `);

    const row = stmt.get(id) as CompositeRuleRow | undefined;
    if (!row) {
      return null;
    }

    return this.rowToCompositeRule(row);
  }

  /**
   * Load all composite rules.
   * @param enabledOnly - If true, only return enabled rules
   */
  getAllCompositeRules(enabledOnly = false): CompositeRule[] {
    let sql = 'SELECT * FROM composite_rules';
    if (enabledOnly) {
      sql += ' WHERE enabled = 1';
    }
    sql += ' ORDER BY created_at ASC';

    const stmt = this.db.prepare(sql);
    const rows = stmt.all() as CompositeRuleRow[];

    return rows.map(row => this.rowToCompositeRule(row));
  }

  /**
   * Delete a composite rule by ID.
   * @returns true if deleted, false if not found
   */
  deleteCompositeRule(id: string): boolean {
    const stmt = this.db.prepare(`
      DELETE FROM composite_rules WHERE id = ?
    `);

    const result = stmt.run(id);
    const deleted = result.changes > 0;

    if (deleted) {
      logger.info(`Deleted composite rule: ${id}`);
    }

    return deleted;
  }

  /**
   * Enable or disable a composite rule.
   * @returns true if updated, false if not found
   */
  setCompositeRuleEnabled(id: string, enabled: boolean): boolean {
    const stmt = this.db.prepare(`
      UPDATE composite_rules SET enabled = ? WHERE id = ?
    `);

    const result = stmt.run(enabled ? 1 : 0, id);
    const updated = result.changes > 0;

    if (updated) {
      logger.info(`Composite rule ${id} ${enabled ? 'enabled' : 'disabled'}`);
    }

    return updated;
  }

  /**
   * Update node positions for a composite rule (for Rete.js editor save).
   */
  updateNodePositions(id: string, nodes: CompositeNode[]): boolean {
    const existing = this.getCompositeRule(id);
    if (!existing) {
      return false;
    }

    // Update positions only
    existing.nodes = nodes;
    this.saveCompositeRule(existing);

    return true;
  }

  // ─── Utility ───────────────────────────────────────────────────────────────

  private rowToCompositeRule(row: CompositeRuleRow): CompositeRule {
    return {
      id: row.id,
      name: row.name,
      description: row.description || '',
      enabled: row.enabled === 1,
      created_at: row.created_at,
      created_by: row.created_by || 'manual',
      nodes: JSON.parse(row.nodes_json) as CompositeNode[],
      connections: JSON.parse(row.connections_json) as CompositeConnection[],
      output_action: JSON.parse(row.output_action_json) as OutputAction,
    };
  }

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

// Internal type for database rows
interface CompositeRuleRow {
  id: string;
  name: string;
  description: string;
  enabled: number;
  created_at: string;
  created_by: string;
  nodes_json: string;
  connections_json: string;
  output_action_json: string;
}

// ─── Factory ─────────────────────────────────────────────────────────────────

/**
 * Create a StateStore instance.
 */
export function createStateStore(dbPath: string): StateStore {
  return new StateStore(dbPath);
}
