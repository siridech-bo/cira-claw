/**
 * Type definitions for the Rule Graph editor (Spec G)
 *
 * These match the gateway's state-store.ts definitions.
 */

import type { SocketType, ActionType, ThresholdOperator } from './sockets';

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
  type: 'atomic' | 'and' | 'or' | 'not' | 'constant' | 'threshold' | 'output';
  position: { x: number; y: number };
  data: AtomicNodeData | GateNodeData | ConstantNodeData | ThresholdNodeData | OutputNodeData;
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
