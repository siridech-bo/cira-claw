/**
 * Socket type re-exports for the Rule Graph editor (Spec G)
 *
 * Single source of truth: gateway socket registry.
 * All dashboard files import socket types from here.
 * This file ONLY re-exports from the gateway — no local definitions.
 */

// Re-export socket types from gateway
export type { SocketType } from '@gateway/socket-registry';

export {
  SOCKET_TYPES,
  SOCKET_TYPE_LABELS,
  SOCKET_TYPE_COLORS,
  isValidSocketType,
  inferSocketType,
} from '@gateway/socket-registry';

// Re-export types from types.ts for backward compatibility
export type {
  NodeType,
  ActionType,
  ThresholdOperator,
  StatefulConditionType,
} from './types';

export {
  NODE_TYPE_LABELS,
  NODE_TYPE_COLORS,
  ACTION_TYPE_LABELS,
  ACTION_TYPE_COLORS,
  THRESHOLD_OPERATORS,
  THRESHOLD_OPERATOR_LABELS,
  PAYLOAD_FIELDS,
  STATEFUL_CONDITION_LABELS,
} from './types';

// ─── Connection Compatibility ────────────────────────────────────────────────

/**
 * Check if two socket types are compatible for connection.
 * Rules:
 * - 'any.boolean' can connect to anything
 * - Same types can always connect
 * - vision.* can connect to each other
 * - signal.* can connect to each other
 */
export function areSocketsCompatible(source: string, target: string): boolean {
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

export function isValidNodeType(value: string): value is import('./types').NodeType {
  return ['atomic', 'and', 'or', 'not', 'constant', 'threshold', 'output', 'stateful_condition'].includes(value);
}
