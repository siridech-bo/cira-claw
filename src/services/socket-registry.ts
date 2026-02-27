/**
 * Socket Type Registry
 *
 * The shared socket type source-of-truth for the CiRA CLAW rule system.
 * Every file in Spec A v3 and Spec G imports from this registry.
 *
 * Socket types categorize the signal domain that a rule operates on.
 * This enables the visual Rule Graph editor (Spec G) to show type-compatible
 * connections between nodes.
 */

// ─── Socket Types ─────────────────────────────────────────────────────────────

/**
 * All valid socket types as a readonly tuple.
 * Order matters for priority in inferSocketType().
 */
export const SOCKET_TYPES = [
  'vision.confidence',
  'vision.detection',
  'signal.rate',
  'signal.threshold',
  'system.health',
  'any.boolean',
] as const;

/**
 * TypeScript union type derived from SOCKET_TYPES.
 */
export type SocketType = typeof SOCKET_TYPES[number];

/**
 * Human-readable labels for each socket type.
 */
export const SOCKET_TYPE_LABELS: Record<SocketType, string> = {
  'vision.confidence': 'Confidence Score',
  'vision.detection': 'Detection Count/Label',
  'signal.rate': 'Rate (per hour)',
  'signal.threshold': 'Threshold Value',
  'system.health': 'System Health',
  'any.boolean': 'Boolean (Any)',
};

/**
 * Color codes for each socket type (for UI rendering).
 */
export const SOCKET_TYPE_COLORS: Record<SocketType, string> = {
  'vision.confidence': '#F59E0B', // Amber
  'vision.detection': '#10B981',  // Emerald
  'signal.rate': '#8B5CF6',       // Purple
  'signal.threshold': '#3B82F6',  // Blue
  'system.health': '#EF4444',     // Red
  'any.boolean': '#6B7280',       // Gray
};

// ─── Payload Field Mapping ────────────────────────────────────────────────────

/**
 * Maps payload field paths to their socket types.
 * Used by inferSocketType() to determine socket type from accessed fields.
 *
 * Field paths use dot notation. Array element access is denoted with [].
 * Example: "detections[].confidence" means any element's confidence field.
 */
export const PAYLOAD_FIELD_MAP: Record<string, SocketType> = {
  // vision.confidence — confidence scores
  'detections[].confidence': 'vision.confidence',
  'confidence': 'vision.confidence',

  // vision.detection — detection counts and labels
  'detections': 'vision.detection',
  'detections.length': 'vision.detection',
  'detections[].label': 'vision.detection',
  'detections[].x': 'vision.detection',
  'detections[].y': 'vision.detection',
  'detections[].w': 'vision.detection',
  'detections[].h': 'vision.detection',
  'stats.by_label': 'vision.detection',
  'stats.total_detections': 'vision.detection',

  // signal.rate — rates and per-hour metrics
  'stats.defects_per_hour': 'signal.rate',
  'defects_per_hour': 'signal.rate',
  'hourly': 'signal.rate',
  'hourly[].detections': 'signal.rate',

  // signal.threshold — numeric thresholds (fps, uptime, etc.)
  'stats.fps': 'signal.threshold',
  'stats.uptime_sec': 'signal.threshold',
  'fps': 'signal.threshold',
  'uptime_sec': 'signal.threshold',
  'frame.width': 'signal.threshold',
  'frame.height': 'signal.threshold',

  // system.health — system status and frame info
  'node.status': 'system.health',
  'node.id': 'system.health',
  'frame.number': 'system.health',
  'frame.timestamp': 'system.health',
};

// ─── Type Guards and Inference ────────────────────────────────────────────────

/**
 * Type guard to check if a string is a valid SocketType.
 */
export function isValidSocketType(s: string): s is SocketType {
  return SOCKET_TYPES.includes(s as SocketType);
}

/**
 * Priority order for socket types when multiple types are inferred.
 * Lower index = higher priority.
 */
const SOCKET_TYPE_PRIORITY: SocketType[] = [
  'vision.confidence',  // Most specific, wins over vision.detection
  'vision.detection',
  'signal.rate',
  'signal.threshold',
  'system.health',
  'any.boolean',        // Fallback
];

/**
 * Infer the socket type from an array of payload field paths.
 *
 * @param fields - Array of payload field paths (dot notation)
 * @returns The inferred socket type
 *
 * Decision logic:
 * 1. Map each path through PAYLOAD_FIELD_MAP to get a SocketType
 * 2. If all paths map to the same type, return that type
 * 3. If they map to multiple types, return the highest priority one
 * 4. If no paths match, return 'any.boolean'
 */
export function inferSocketType(fields: string[]): SocketType {
  if (fields.length === 0) {
    return 'any.boolean';
  }

  const inferredTypes = new Set<SocketType>();

  for (const field of fields) {
    // Normalize the field path for matching
    const normalizedField = normalizeFieldPath(field);

    // Try exact match first
    if (normalizedField in PAYLOAD_FIELD_MAP) {
      inferredTypes.add(PAYLOAD_FIELD_MAP[normalizedField]);
      continue;
    }

    // Try partial matches (for array access patterns)
    for (const [pattern, socketType] of Object.entries(PAYLOAD_FIELD_MAP)) {
      if (fieldMatchesPattern(normalizedField, pattern)) {
        inferredTypes.add(socketType);
        break;
      }
    }
  }

  if (inferredTypes.size === 0) {
    return 'any.boolean';
  }

  if (inferredTypes.size === 1) {
    return Array.from(inferredTypes)[0];
  }

  // Multiple types inferred — return highest priority
  for (const socketType of SOCKET_TYPE_PRIORITY) {
    if (inferredTypes.has(socketType)) {
      return socketType;
    }
  }

  return 'any.boolean';
}

/**
 * Normalize a field path for matching.
 * Converts array indices like [0], [1] to [].
 */
function normalizeFieldPath(field: string): string {
  // Replace [N] with []
  return field.replace(/\[\d+\]/g, '[]');
}

/**
 * Check if a field path matches a pattern.
 * Handles array access patterns like "detections[0].label" matching "detections[].label".
 */
function fieldMatchesPattern(field: string, pattern: string): boolean {
  // Direct match
  if (field === pattern) {
    return true;
  }

  // Check if field starts with pattern (for partial matches)
  if (field.startsWith(pattern)) {
    return true;
  }

  // Check if pattern is a prefix of field with array access
  const fieldParts = field.split('.');
  const patternParts = pattern.split('.');

  if (fieldParts.length < patternParts.length) {
    return false;
  }

  for (let i = 0; i < patternParts.length; i++) {
    const fieldPart = fieldParts[i].replace(/\[\d+\]/g, '[]');
    const patternPart = patternParts[i];

    if (fieldPart !== patternPart) {
      return false;
    }
  }

  return true;
}

// ─── Test Assertions (for development verification) ──────────────────────────

if (process.env.NODE_ENV === 'test') {
  // These assertions verify the inferSocketType function works correctly
  const assert = (condition: boolean, message: string) => {
    if (!condition) {
      throw new Error(`Socket registry assertion failed: ${message}`);
    }
  };

  assert(
    inferSocketType(['detections.length']) === 'vision.detection',
    'detections.length should infer vision.detection'
  );
  assert(
    inferSocketType(['detections[].confidence']) === 'vision.confidence',
    'detections[].confidence should infer vision.confidence'
  );
  assert(
    inferSocketType(['stats.fps']) === 'signal.threshold',
    'stats.fps should infer signal.threshold'
  );
  assert(
    inferSocketType(['stats.defects_per_hour']) === 'signal.rate',
    'stats.defects_per_hour should infer signal.rate'
  );
  assert(
    inferSocketType(['node.status']) === 'system.health',
    'node.status should infer system.health'
  );
  assert(
    inferSocketType([]) === 'any.boolean',
    'empty array should infer any.boolean'
  );
  assert(
    inferSocketType(['detections[].confidence', 'detections.length']) === 'vision.confidence',
    'confidence should win over detection (priority)'
  );

  console.log('Socket registry tests passed');
}
