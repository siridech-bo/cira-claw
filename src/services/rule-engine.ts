import ivm from 'isolated-vm';
import fs from 'fs';
import path from 'path';
import { createLogger } from '../utils/logger.js';

const logger = createLogger('rule-engine');

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
  result?: unknown;    // For js_query: the computed answer
  action?: RuleAction; // For js_rule: the action to take
  error?: string;
  execution_ms: number;
  code: string;        // Echoed back for agent to include in response
}

// ─── Saved Rule ───────────────────────────────────────────────────────────────
// This is the format stored in ~/.cira/rules/*.js and the in-memory representation.
//
// 'tags' and 'signal_type' are operational metadata for internal rule management.
// They are optional — existing rules without them remain valid.
// They enable future queries like "list all reject-logic rules" or
// "which rules apply to vibration signals" once Spec C signals are available.
//
// 'node_id' scopes the rule to a specific edge device.

export interface SavedRule {
  id: string;           // Filename without .js extension
  name: string;         // Human-readable name
  description: string;  // What this rule does
  code: string;         // The JS code (AI-generated)
  enabled: boolean;
  created_at: string;   // ISO timestamp
  created_by: string;   // 'ai-agent' | 'manual'
  node_id: string;      // Which node this rule applies to (default: 'local-dev')
  tags?: string[];      // e.g. ['reject-logic', 'defect-rate', 'pcb']
  signal_type?: string; // 'visual' | 'vibration' | 'current' | 'any' (default: 'visual')
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
        execution_ms,
        code,
      };
    } catch (error) {
      const execution_ms = performance.now() - startTime;
      const errorMessage = error instanceof Error ? error.message : String(error);

      logger.warn(`Query execution failed: ${errorMessage}`);

      return {
        success: false,
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
        execution_ms,
        code: rule.code,
      };
    } catch (error) {
      const execution_ms = performance.now() - startTime;
      const errorMessage = error instanceof Error ? error.message : String(error);

      logger.warn(`Rule "${rule.id}" execution failed: ${errorMessage}`);

      return {
        success: false,
        error: errorMessage,
        execution_ms,
        code: rule.code,
      };
    }
  }

  /**
   * Load all rules from the rules directory.
   * Each file has a JSON header comment on line 1.
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
          const meta = JSON.parse(jsonStr) as Omit<SavedRule, 'code'>;

          // Extract code (everything after the first line)
          const code = lines.slice(1).join('\n');

          const rule: SavedRule = {
            ...meta,
            code,
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
   */
  saveRule(rule: SavedRule): void {
    this.ensureRulesDir();

    const filePath = path.join(this.rulesDir, `${rule.id}.js`);

    // Build metadata object (excluding code)
    const meta: Omit<SavedRule, 'code'> = {
      id: rule.id,
      name: rule.name,
      description: rule.description,
      enabled: rule.enabled,
      created_at: rule.created_at,
      created_by: rule.created_by,
      node_id: rule.node_id,
    };

    // Only include tags and signal_type if they are set
    if (rule.tags && rule.tags.length > 0) {
      meta.tags = rule.tags;
    }
    if (rule.signal_type) {
      meta.signal_type = rule.signal_type;
    }

    // Write file: JSON header + code
    const content = `// ${JSON.stringify(meta)}\n${rule.code}`;
    fs.writeFileSync(filePath, content, 'utf-8');

    logger.info(`Saved rule: ${rule.id}`);
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
   * Evaluate all enabled rules against the provided payload.
   * Returns array of actions for the caller to dispatch.
   * If any rule throws or times out, logs error and continues.
   */
  async evaluateAllRules(payload: RulePayload): Promise<RuleAction[]> {
    const startTime = performance.now();
    const actions: RuleAction[] = [];

    // Load enabled rules, sorted by creation date (oldest first)
    let rules = this.loadRules()
      .filter(r => r.enabled)
      .sort((a, b) => new Date(a.created_at).getTime() - new Date(b.created_at).getTime());

    // Limit to MAX_ACTIVE_RULES
    if (rules.length > MAX_ACTIVE_RULES) {
      logger.warn(`Too many active rules (${rules.length}), limiting to ${MAX_ACTIVE_RULES} oldest`);
      rules = rules.slice(0, MAX_ACTIVE_RULES);
    }

    for (const rule of rules) {
      try {
        // Check if we're approaching the total cycle limit
        const elapsed = performance.now() - startTime;
        if (elapsed > TOTAL_RULE_CYCLE_MS) {
          logger.warn(`Rule cycle time exceeded ${TOTAL_RULE_CYCLE_MS}ms, stopping evaluation`);
          break;
        }

        const result = await this.evaluateRule(rule, payload);

        if (result.success && result.action) {
          actions.push(result.action);
        } else if (!result.success) {
          logger.warn(`Rule ${rule.id} failed: ${result.error}`);
        }
      } catch (error) {
        const errorMessage = error instanceof Error ? error.message : String(error);
        logger.error(`Unexpected error evaluating rule ${rule.id}: ${errorMessage}`);
        // Continue to next rule
      }
    }

    const totalMs = performance.now() - startTime;
    if (rules.length > 0) {
      logger.debug(`Evaluated ${rules.length} rules in ${totalMs.toFixed(1)}ms`);
    }

    return actions;
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
