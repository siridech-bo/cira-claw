/**
 * Socket type definitions for the Rule Graph editor (Spec G)
 *
 * These match the gateway's socket-registry.ts definitions.
 * Kept in sync manually - the source of truth is the gateway.
 */

// ─── Socket Types ────────────────────────────────────────────────────────────

export type SocketType =
  | 'vision.confidence'
  | 'vision.detection'
  | 'signal.rate'
  | 'signal.threshold'
  | 'system.health'
  | 'any.boolean';

export const SOCKET_TYPES: readonly SocketType[] = [
  'vision.confidence',
  'vision.detection',
  'signal.rate',
  'signal.threshold',
  'system.health',
  'any.boolean',
] as const;

// ─── Labels and Colors ───────────────────────────────────────────────────────

export const SOCKET_TYPE_LABELS: Record<SocketType, string> = {
  'vision.confidence': 'Confidence Score',
  'vision.detection': 'Detection Count/Label',
  'signal.rate': 'Rate (per hour)',
  'signal.threshold': 'Threshold Value',
  'system.health': 'System Health',
  'any.boolean': 'Boolean (Any)',
};

export const SOCKET_TYPE_COLORS: Record<SocketType, string> = {
  'vision.confidence': '#F59E0B', // Amber
  'vision.detection': '#10B981',  // Emerald
  'signal.rate': '#8B5CF6',       // Purple
  'signal.threshold': '#3B82F6',  // Blue
  'system.health': '#EF4444',     // Red
  'any.boolean': '#6B7280',       // Gray
};

// ─── Node Types ──────────────────────────────────────────────────────────────

export type NodeType = 'atomic' | 'and' | 'or' | 'not' | 'constant' | 'threshold' | 'output';

export const NODE_TYPE_LABELS: Record<NodeType, string> = {
  atomic: 'Atomic Rule',
  and: 'AND Gate',
  or: 'OR Gate',
  not: 'NOT Gate',
  constant: 'Constant',
  threshold: 'Threshold',
  output: 'Output Action',
};

export const NODE_TYPE_COLORS: Record<NodeType, string> = {
  atomic: '#6366F1',    // Indigo - matches app theme
  and: '#10B981',       // Emerald
  or: '#F59E0B',        // Amber
  not: '#EF4444',       // Red
  constant: '#6B7280',  // Gray
  threshold: '#3B82F6', // Blue
  output: '#8B5CF6',    // Purple
};

// ─── Connection Compatibility ────────────────────────────────────────────────

/**
 * Check if two socket types are compatible for connection.
 * Rules:
 * - 'any.boolean' can connect to anything
 * - Same types can always connect
 * - vision.* can connect to each other
 * - signal.* can connect to each other
 */
export function areSocketsCompatible(source: SocketType, target: SocketType): boolean {
  // any.boolean is wildcard
  if (source === 'any.boolean' || target === 'any.boolean') {
    return true;
  }

  // Same type always compatible
  if (source === target) {
    return true;
  }

  // vision.* family
  if (source.startsWith('vision.') && target.startsWith('vision.')) {
    return true;
  }

  // signal.* family
  if (source.startsWith('signal.') && target.startsWith('signal.')) {
    return true;
  }

  return false;
}

// ─── Type Guards ─────────────────────────────────────────────────────────────

export function isValidSocketType(value: string): value is SocketType {
  return SOCKET_TYPES.includes(value as SocketType);
}

export function isValidNodeType(value: string): value is NodeType {
  return ['atomic', 'and', 'or', 'not', 'constant', 'threshold', 'output'].includes(value);
}

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
