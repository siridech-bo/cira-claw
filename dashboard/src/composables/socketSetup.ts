/**
 * Socket Setup for Rete.js
 *
 * Creates Rete.js socket instances from the gateway socket registry.
 * Handles socket compatibility checks for visual connection validation.
 */

import { ClassicPreset } from 'rete';
import { SOCKET_TYPES } from '@gateway/socket-registry';
import type { SocketType } from '@gateway/socket-registry';

// One singleton socket instance per type — shared across all nodes of the same type.
// Rete uses reference equality for socket compatibility — singletons are required.
export const RETE_SOCKETS = Object.fromEntries(
  SOCKET_TYPES.map(t => [t, new ClassicPreset.Socket(t)])
) as Record<SocketType, ClassicPreset.Socket>;

// Special sockets for Spec G-internal use
export const TIME_WINDOW_SOCKET = new ClassicPreset.Socket('time.window');
export const BOOLEAN_ANY_SOCKET = new ClassicPreset.Socket('boolean.any');
export const CONTEXT_SOCKET = new ClassicPreset.Socket('pipeline.context');

/**
 * Connection compatibility rules:
 * - Any typed socket (vision.*, signal.*, system.*, any.boolean) → BOOLEAN_ANY_SOCKET: ALLOWED
 * - Same socket type → same socket type: ALLOWED
 * - CONTEXT_SOCKET → CONTEXT_SOCKET only: ALLOWED
 * - Cross-typed (e.g. vision.detection → signal.threshold): BLOCKED
 * - vision.detection → vision.confidence: BLOCKED (different types, not a family)
 */
export function isCompatible(
  output: ClassicPreset.Socket,
  input: ClassicPreset.Socket
): boolean {
  const typed = new Set<ClassicPreset.Socket>(Object.values(RETE_SOCKETS));
  const isTypedBool = (s: ClassicPreset.Socket) =>
    typed.has(s) || s === BOOLEAN_ANY_SOCKET || s === TIME_WINDOW_SOCKET;

  // Context socket only connects to itself
  if (output === CONTEXT_SOCKET || input === CONTEXT_SOCKET) {
    return output === input;
  }

  // Any typed boolean output → BOOLEAN_ANY_SOCKET input (operator/action nodes)
  if (isTypedBool(output) && input === BOOLEAN_ANY_SOCKET) return true;

  // Same socket → same socket
  if (output === input) return true;

  return false;
}

/**
 * Get socket by name string (for serialization/deserialization)
 */
export function getSocketByName(name: string): ClassicPreset.Socket {
  if (name === 'time.window') return TIME_WINDOW_SOCKET;
  if (name === 'boolean.any') return BOOLEAN_ANY_SOCKET;
  if (name === 'pipeline.context') return CONTEXT_SOCKET;
  return RETE_SOCKETS[name as SocketType] ?? BOOLEAN_ANY_SOCKET;
}
