import ivm from 'isolated-vm';
import fs from 'fs';
import path from 'path';
import { createLogger } from '../utils/logger.js';
import { SocketType, SOCKET_TYPES, isValidSocketType } from './socket-registry.js';

const logger = createLogger('rule-engine');

// Re-export socket types for convenience
export type { SocketType } from './socket-registry.js';
export { isValidSocketType } from './socket-registry.js';

// ─── Payload ────────────────────────────────────────────────────────────────
// The data object passed into every rule and query execution.
// 'signals' is reserved for Spec C (Universal Signal Ingestion).
// Spec C will populate this with vibration, current, temperature feeds.
// Rules written in Spec A should not reference signals — it will be empty.

export interface RulePayload {
  // Current frame detections from /api/results
  detections: Array<{
    label: string;
    confidence: number;
    x: number;      // normalized [0,1] top-left
    y: number;
    w: number;
    h: number;
  }>;
  frame: {
    number: number;
    timestamp: string;
    width: number;
    height: number;
  };
  // Accumulated stats from StatsCollector
  stats: {
    total_detections: number;
    by_label: Record<string, number>;
    fps: number;
    uptime_sec: number;
    defects_per_hour: number;
  };
  // Hourly breakdown
  hourly: Array<{
    hour: string;
    detections: number;
  }>;
  // Node context
  node: {
    id: string;
    status: string;
  };
  // Reserved for Spec C — Universal Signal Ingestion
  // Will carry vibration, current, audio, temperature snapshots
  // Leave empty in Spec A — do not remove this field
  signals?: Record<string, unknown>;
}

// ─── Actions ─────────────────────────────────────────────────────────────────

export interface RuleAction {
  action: 'pass' | 'reject' | 'alert' | 'log' | 'modbus_write';
  reason?: string;
  // For modbus_write
  register?: number;
  value?: number;
  // For alert
  severity?: 'info' | 'warning' | 'critical';
  message?: string;
}

// ─── Results ─────────────────────────────────────────────────────────────────

export interface RuleResult {
  success: boolean;
  result?: unknown;       // For js_query: the computed answer
  action?: RuleAction;    // For js_rule: the action to take
  socket_type: SocketType; // v3: Required socket type classification
  reads?: string[];       // v3: Payload fields this rule reads
  produces?: string[];    // v3: Action types this rule can produce
  error?: string;
  execution_ms: number;
  code: string;           // Echoed back for agent to include in response
}

// ─── Saved Rule ───────────────────────────────────────────────────────────────

export interface SavedRule {
  id: string;                              // Filename without .js extension
  name: string;                            // Human-readable name
  description: string;                     // What this rule does
  socket_type: SocketType;                 // v3: Signal category (required)
  reads: string[];                         // v3: Payload fields accessed
  produces: RuleAction['action'][];        // v3: Action types returned
  code: string;                            // The JS code
  enabled: boolean;
  created_at: string;                      // ISO timestamp
  created_by: string;                      // 'ai-agent' | 'manual'
  node_id?: string;                        // Scope to specific node. Omit = applies to all nodes.
                                           // Reserved for multi-node deployment (future spec).
  prompt?: string;                         // Original prompt that created the rule
  tags?: string[];                         // Operator-assigned labels e.g. ['reject-logic', 'pcb']
                                           // Used by js_rule_list filter_tag. No semantic meaning.
  signal_type?: string;                    // DEPRECATED. Replaced by socket_type in v3.
                                           // Kept only for loading pre-v3 rule files.
                                           // Do not write this field. Do not read this field.
                                           // Will be removed when all pre-v3 rules are migrated.
}

// Maximum active rules
const MAX_ACTIVE_RULES = 20;
const TOTAL_RULE_CYCLE_MS = 500;

export class RuleEngine {
  private rulesDir: string;

  constructor(rulesDir: string) {
    this.rulesDir = rulesDir;
    this.ensureRulesDir();
  }

  private ensureRulesDir(): void {
    if (!fs.existsSync(this.rulesDir)) {
      fs.mkdirSync(this.rulesDir, { recursive: true });
      logger.info(`Created rules directory: ${this.rulesDir}`);
    }
  }

  /**
   * Execute a one-shot JavaScript query against the provided payload.
   * Used by the js_query tool for ad-hoc data analysis.
   */
  async executeQuery(code: string, payload: RulePayload): Promise<RuleResult> {
    const startTime = performance.now();

    try {
      // Create isolate with 16MB memory limit
      const isolate = new ivm.Isolate({ memoryLimit: 16 });

      // isolated-vm creates a clean V8 context with no Node.js globals.
      // require, process, fs, and global are unavailable by default.
      // Standard JS builtins (Math, JSON, Date, Array, etc.) are available.
      const context = await isolate.createContext();

      // Wrap code in IIFE and JSON.stringify the result.
      // isolated-vm can only transfer primitives across boundaries,
      // so we serialize the result to a string and parse it back.
      const wrappedCode = `
        const payload = ${JSON.stringify(payload)};
        JSON.stringify((function() { ${code} })());
      `;

      // Compile and run with 100ms timeout
      const script = await isolate.compileScript(wrappedCode);
      const rawResult = await script.run(context, { timeout: 100 });

      // Dispose isolate
      isolate.dispose();

      // Parse the JSON string result back to an object
      const result = rawResult ? JSON.parse(rawResult as string) : undefined;

      const execution_ms = performance.now() - startTime;

      return {
        success: true,
        result,
        socket_type: 'any.boolean', // Queries have no specific socket type
        execution_ms,
        code,
      };
    } catch (error) {
      const execution_ms = performance.now() - startTime;
      const errorMessage = error instanceof Error ? error.message : String(error);

      logger.warn(`Query execution failed: ${errorMessage}`);

      return {
        success: false,
        socket_type: 'any.boolean',
        error: errorMessage,
        execution_ms,
        code,
      };
    }
  }

  /**
   * Evaluate a persistent rule against the provided payload.
   * The rule must export a function that receives payload and returns a RuleAction.
   */
  async evaluateRule(rule: SavedRule, payload: RulePayload): Promise<RuleResult> {
    const startTime = performance.now();

    try {
      // Create isolate with 8MB memory limit for rules
      const isolate = new ivm.Isolate({ memoryLimit: 8 });

      // isolated-vm creates a clean V8 context with no Node.js globals.
      // require, process, fs, and global are unavailable by default.
      // Standard JS builtins (Math, JSON, Date, Array, etc.) are available.
      const context = await isolate.createContext();

      // Wrap code: module.exports pattern with JSON.stringify for result transfer.
      // isolated-vm can only transfer primitives across boundaries,
      // so we serialize the RuleAction to a string and parse it back.
      const wrappedCode = `
        const payload = ${JSON.stringify(payload)};
        const module = { exports: {} };
        ${rule.code}
        JSON.stringify(module.exports(payload));
      `;

      // Compile and run with 50ms timeout for rules
      const script = await isolate.compileScript(wrappedCode);
      const rawResult = await script.run(context, { timeout: 50 });

      // Dispose isolate
      isolate.dispose();

      // Parse the JSON string result back to a RuleAction
      const action = rawResult ? JSON.parse(rawResult as string) as RuleAction : undefined;

      const execution_ms = performance.now() - startTime;

      return {
        success: true,
        action,
        socket_type: rule.socket_type,
        reads: rule.reads,
        produces: rule.produces,
        execution_ms,
        code: rule.code,
      };
    } catch (error) {
      const execution_ms = performance.now() - startTime;
      const errorMessage = error instanceof Error ? error.message : String(error);

      logger.warn(`Rule "${rule.id}" execution failed: ${errorMessage}`);

      return {
        success: false,
        socket_type: rule.socket_type,
        reads: rule.reads,
        produces: rule.produces,
        error: errorMessage,
        execution_ms,
        code: rule.code,
      };
    }
  }

  /**
   * Load all rules from the rules directory.
   * Each file has a JSON header comment on line 1.
   *
   * v3 backward compatibility:
   * - Rules created before v3 may lack socket_type, reads, produces
   * - These default to 'any.boolean' and empty arrays
   * - A warning is logged for pre-v3 rules
   */
  loadRules(): SavedRule[] {
    const rules: SavedRule[] = [];

    if (!fs.existsSync(this.rulesDir)) {
      return rules;
    }

    const files = fs.readdirSync(this.rulesDir).filter(f => f.endsWith('.js'));

    for (const file of files) {
      try {
        const filePath = path.join(this.rulesDir, file);
        const content = fs.readFileSync(filePath, 'utf-8');
        const lines = content.split('\n');
        const firstLine = lines[0];

        // Parse JSON header from first line: // {...}
        if (firstLine.startsWith('// {')) {
          const jsonStr = firstLine.slice(3); // Remove "// "
          const meta = JSON.parse(jsonStr) as Record<string, unknown>;

          // Extract code (everything after the first line)
          const code = lines.slice(1).join('\n');

          // v3 backward compatibility: default missing fields
          let socket_type: SocketType = 'any.boolean';
          let reads: string[] = [];
          let produces: RuleAction['action'][] = [];
          let isPreV3 = false;

          if (meta.socket_type && isValidSocketType(meta.socket_type as string)) {
            socket_type = meta.socket_type as SocketType;
          } else if (meta.socket_type) {
            // Invalid socket_type in file — warn and use default
            logger.warn(`Rule ${file} has invalid socket_type: ${meta.socket_type}, defaulting to 'any.boolean'`);
            isPreV3 = true;
          } else {
            isPreV3 = true;
          }

          if (Array.isArray(meta.reads)) {
            reads = meta.reads as string[];
          } else {
            isPreV3 = true;
          }

          if (Array.isArray(meta.produces)) {
            produces = meta.produces as RuleAction['action'][];
          } else {
            isPreV3 = true;
          }

          if (isPreV3) {
            logger.warn(`Rule ${file} predates socket type standard (v3). Consider re-creating via js_rule_create.`);
          }

          const rule: SavedRule = {
            id: meta.id as string,
            name: meta.name as string,
            description: meta.description as string,
            socket_type,
            reads,
            produces,
            code,
            enabled: meta.enabled as boolean ?? true,
            created_at: meta.created_at as string ?? new Date().toISOString(),
            created_by: meta.created_by as string ?? 'unknown',
            node_id: meta.node_id as string,
            prompt: meta.prompt as string,
            tags: meta.tags as string[],
            signal_type: meta.signal_type as string, // Keep for legacy compatibility
          };

          rules.push(rule);
        } else {
          logger.warn(`Rule file ${file} missing JSON header, skipping`);
        }
      } catch (error) {
        const errorMessage = error instanceof Error ? error.message : String(error);
        logger.warn(`Failed to parse rule file ${file}: ${errorMessage}`);
      }
    }

    return rules;
  }

  /**
   * Save a rule to disk with JSON header format.
   *
   * v3 validation:
   * - socket_type must be a valid SocketType
   * - reads must be a non-empty array (warning if empty)
   * - produces must be a non-empty array (error if empty)
   * - id must be filename-safe (lowercase-hyphenated)
   */
  saveRule(rule: SavedRule): void {
    this.ensureRulesDir();

    // Validate socket_type
    if (!isValidSocketType(rule.socket_type)) {
      throw new Error(`Invalid socket_type: ${rule.socket_type}. Must be one of: ${SOCKET_TYPES.join(', ')}`);
    }

    // Validate id is filename-safe
    if (!/^[a-z0-9-]+$/.test(rule.id)) {
      throw new Error(`Invalid rule id: ${rule.id}. Must be lowercase letters, numbers, and hyphens only.`);
    }

    // Validate reads
    if (!Array.isArray(rule.reads)) {
      throw new Error(`Missing 'reads' field. Must be an array of payload field paths.`);
    }

    // Validate produces
    if (!Array.isArray(rule.produces) || rule.produces.length === 0) {
      throw new Error(`Missing or empty 'produces' field. Must be a non-empty array of action types.`);
    }

    const filePath = path.join(this.rulesDir, `${rule.id}.js`);

    // Build metadata object (excluding code)
    const meta: Record<string, unknown> = {
      id: rule.id,
      name: rule.name,
      description: rule.description,
      socket_type: rule.socket_type,
      reads: rule.reads,
      produces: rule.produces,
      enabled: rule.enabled,
      created_at: rule.created_at,
      created_by: rule.created_by,
    };

    // Optional fields
    if (rule.node_id) {
      meta.node_id = rule.node_id;
    }
    if (rule.prompt) {
      meta.prompt = rule.prompt;
    }
    if (rule.tags && rule.tags.length > 0) {
      meta.tags = rule.tags;
    }

    // Write file: JSON header + code
    const content = `// ${JSON.stringify(meta)}\n${rule.code}`;
    fs.writeFileSync(filePath, content, 'utf-8');

    logger.info(`Saved rule: ${rule.id} [${rule.socket_type}]`);
  }

  /**
   * Delete a rule from disk.
   * Returns false if rule not found.
   */
  deleteRule(id: string): boolean {
    const filePath = path.join(this.rulesDir, `${id}.js`);

    if (!fs.existsSync(filePath)) {
      return false;
    }

    fs.unlinkSync(filePath);
    logger.info(`Deleted rule: ${id}`);
    return true;
  }

  /**
   * Enable or disable a rule by updating its JSON header.
   * Returns false if rule not found.
   */
  enableRule(id: string, enabled: boolean): boolean {
    const rules = this.loadRules();
    const rule = rules.find(r => r.id === id);

    if (!rule) {
      return false;
    }

    rule.enabled = enabled;
    this.saveRule(rule);

    logger.info(`Rule ${id} ${enabled ? 'enabled' : 'disabled'}`);
    return true;
  }

  /**
   * Evaluate all enabled rules and return a Map of rule ID to RuleResult.
   * This is the v3 API used by StatsCollector.getRuleResults().
   *
   * Hard cap: evaluates at most MAX_ACTIVE_RULES rules.
   * Warns if total cycle time exceeds TOTAL_RULE_CYCLE_MS.
   */
  async evaluateAll(payload: RulePayload): Promise<Map<string, RuleResult>> {
    const startTime = performance.now();
    const results = new Map<string, RuleResult>();

    // Load enabled rules, sorted by creation date (oldest first)
    let rules = this.loadRules()
      .filter(r => r.enabled)
      .sort((a, b) => new Date(a.created_at).getTime() - new Date(b.created_at).getTime());

    // Hard cap at MAX_ACTIVE_RULES
    if (rules.length > MAX_ACTIVE_RULES) {
      logger.warn(`Rule cap exceeded: ${rules.length} enabled rules found, only evaluating first ${MAX_ACTIVE_RULES}`);
      rules = rules.slice(0, MAX_ACTIVE_RULES);
    }

    for (const rule of rules) {
      const result = await this.evaluateRule(rule, payload);
      results.set(rule.id, result);

      if (!result.success) {
        logger.error({ ruleId: rule.id, error: result.error }, 'Rule evaluation error');
      }
    }

    const totalMs = performance.now() - startTime;
    if (totalMs > TOTAL_RULE_CYCLE_MS) {
      logger.warn({ totalMs, ruleCount: rules.length }, `Rule cycle exceeded ${TOTAL_RULE_CYCLE_MS}ms budget`);
    }

    return results;
  }

  /**
   * Get the rules directory path.
   */
  getRulesDir(): string {
    return this.rulesDir;
  }
}

/**
 * Factory function to create a RuleEngine instance.
 */
export function createRuleEngine(rulesDir: string): RuleEngine {
  return new RuleEngine(rulesDir);
}
