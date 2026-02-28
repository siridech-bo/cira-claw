/**
 * Type definitions for the Rule Graph editor (Spec G)
 *
 * These match the gateway's state-store.ts definitions.
 */

// Re-export SocketType from gateway for convenience
export type { SocketType } from '@gateway/socket-registry';

// ─── Action Types ────────────────────────────────────────────────────────────

export type ActionType = 'pass' | 'reject' | 'alert' | 'log' | 'modbus_write';

export const ACTION_TYPE_LABELS: Record<ActionType, string> = {
  pass: 'Pass (No Action)',
  reject: 'Reject',
  alert: 'Send Alert',
  log: 'Log Message',
  modbus_write: 'MODBUS Write',
};

export const ACTION_TYPE_COLORS: Record<ActionType, string> = {
  pass: '#6B7280',      // Gray
  reject: '#EF4444',    // Red
  alert: '#F59E0B',     // Amber
  log: '#3B82F6',       // Blue
  modbus_write: '#10B981', // Emerald
};

// ─── Threshold Operators ─────────────────────────────────────────────────────

export type ThresholdOperator = '>' | '<' | '>=' | '<=' | '==' | '!=';

export const THRESHOLD_OPERATORS: readonly ThresholdOperator[] = [
  '>',
  '<',
  '>=',
  '<=',
  '==',
  '!=',
] as const;

export const THRESHOLD_OPERATOR_LABELS: Record<ThresholdOperator, string> = {
  '>': 'Greater than',
  '<': 'Less than',
  '>=': 'Greater or equal',
  '<=': 'Less or equal',
  '==': 'Equal to',
  '!=': 'Not equal to',
};

// ─── Payload Fields ──────────────────────────────────────────────────────────

/**
 * Common payload fields that can be used in threshold nodes.
 */
export const PAYLOAD_FIELDS = [
  { path: 'detections.length', label: 'Detection Count', type: 'vision.detection' as SocketType },
  { path: 'stats.total_detections', label: 'Total Detections', type: 'vision.detection' as SocketType },
  { path: 'stats.fps', label: 'FPS', type: 'signal.threshold' as SocketType },
  { path: 'stats.uptime_sec', label: 'Uptime (seconds)', type: 'signal.threshold' as SocketType },
  { path: 'stats.defects_per_hour', label: 'Defects per Hour', type: 'signal.rate' as SocketType },
  { path: 'frame.width', label: 'Frame Width', type: 'signal.threshold' as SocketType },
  { path: 'frame.height', label: 'Frame Height', type: 'signal.threshold' as SocketType },
  { path: 'frame.number', label: 'Frame Number', type: 'system.health' as SocketType },
] as const;

// ─── Node Types ──────────────────────────────────────────────────────────────

export type NodeType = 'atomic' | 'and' | 'or' | 'not' | 'constant' | 'threshold' | 'output' | 'stateful_condition';

export const NODE_TYPE_LABELS: Record<NodeType, string> = {
  atomic: 'Atomic Rule',
  and: 'AND Gate',
  or: 'OR Gate',
  not: 'NOT Gate',
  constant: 'Constant',
  threshold: 'Threshold',
  output: 'Output Action',
  stateful_condition: 'Stateful Condition',
};

export const NODE_TYPE_COLORS: Record<NodeType, string> = {
  atomic: '#6366F1',    // Indigo - matches app theme
  and: '#10B981',       // Emerald
  or: '#F59E0B',        // Amber
  not: '#EF4444',       // Red
  constant: '#6B7280',  // Gray
  threshold: '#3B82F6', // Blue
  output: '#8B5CF6',    // Purple
  stateful_condition: '#F59E0B', // Amber
};

// ─── Composite Rule ──────────────────────────────────────────────────────────

export interface CompositeRule {
  id: string;
  name: string;
  description: string;
  enabled: boolean;
  created_at: string;
  created_by: string;
  nodes: CompositeNode[];
  connections: CompositeConnection[];
  output_action: OutputAction;
}

// ─── Nodes ───────────────────────────────────────────────────────────────────

export interface CompositeNode {
  id: string;
  type: 'atomic' | 'and' | 'or' | 'not' | 'constant' | 'threshold' | 'output' | 'stateful_condition';
  position: { x: number; y: number };
  data: AtomicNodeData | GateNodeData | ConstantNodeData | ThresholdNodeData | OutputNodeData | StatefulConditionNodeData;
}

export interface AtomicNodeData {
  rule_id: string;
  socket_type: SocketType;
  label?: string;
}

export interface GateNodeData {
  gate_type: 'and' | 'or' | 'not';
}

export interface ConstantNodeData {
  value: boolean;
}

export interface ThresholdNodeData {
  operator: ThresholdOperator;
  threshold: number;
  field: string;
}

export interface OutputNodeData {
  action: ActionType;
  severity?: 'info' | 'warning' | 'critical';
  message?: string;
  register?: number;
  value?: number;
}

export interface StatefulConditionNodeData {
  condition: 'count_window' | 'consecutive' | 'rate' | 'sustained' | 'cooldown';
  accepts_socket_type: SocketType;
  count: number;
  window_minutes: number;
}

export type StatefulConditionType = 'count_window' | 'consecutive' | 'rate' | 'sustained' | 'cooldown';

export const STATEFUL_CONDITION_LABELS: Record<StatefulConditionType, string> = {
  count_window: 'Count in Window',
  consecutive: 'Consecutive',
  rate: 'Rate per Minute',
  sustained: 'Sustained',
  cooldown: 'Cooldown',
};

// ─── Connections ─────────────────────────────────────────────────────────────

export interface CompositeConnection {
  id: string;
  source_node: string;
  source_socket: string;
  target_node: string;
  target_socket: string;
}

// ─── Output Action ───────────────────────────────────────────────────────────

export interface OutputAction {
  action: ActionType;
  severity?: 'info' | 'warning' | 'critical';
  message?: string;
  register?: number;
  value?: number;
}

// ─── Atomic Rule (from gateway) ──────────────────────────────────────────────

export interface AtomicRule {
  id: string;
  name: string;
  description: string;
  socket_type: SocketType;
  reads: string[];
  produces: ActionType[];
  code: string;
  enabled: boolean;
  created_at: string;
  created_by: string;
  node_id?: string;
  prompt?: string;
  tags?: string[];
}

// ─── Rule Results ────────────────────────────────────────────────────────────

export interface AtomicRuleResult {
  action?: {
    action: ActionType;
    reason?: string;
    severity?: string;
    message?: string;
    register?: number;
    value?: number;
  };
  socket_type: SocketType;
  reads: string[];
  produces: string[];
  execution_ms: number;
  success: boolean;
  error?: string;
}

export interface CompositeRuleResult {
  triggered: boolean;
  action?: OutputAction;
  node_results: Record<string, boolean>;
  success: boolean;
  error?: string;
  execution_ms: number;
}

// ─── API Responses ───────────────────────────────────────────────────────────

export interface AtomicRulesResponse {
  rules: AtomicRule[];
}

export interface AtomicRuleResultsResponse {
  evaluated_at: string;
  results: Record<string, AtomicRuleResult>;
}

export interface CompositeRulesResponse {
  rules: CompositeRule[];
}

export interface CompositeRuleResultsResponse {
  evaluated_at: string;
  results: Record<string, CompositeRuleResult>;
}
