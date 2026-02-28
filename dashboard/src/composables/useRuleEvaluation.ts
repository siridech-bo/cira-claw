/**
 * useRuleEvaluation - Composable for polling rule evaluation results
 *
 * Handles:
 * - Polling atomic rule results
 * - Polling composite rule results
 * - Providing reactive state for visualization
 */

import { ref, computed, readonly, onUnmounted } from 'vue';
import type {
  AtomicRuleResult,
  CompositeRuleResult,
  AtomicRuleResultsResponse,
  CompositeRuleResultsResponse,
} from './types';

// ─── State ───────────────────────────────────────────────────────────────────

const atomicResults = ref<Record<string, AtomicRuleResult>>({});
const compositeResults = ref<Record<string, CompositeRuleResult>>({});
const atomicEvaluatedAt = ref<string>('');
const compositeEvaluatedAt = ref<string>('');
const polling = ref(false);
const error = ref<string | null>(null);

let pollTimer: number | null = null;

// ─── Computed ────────────────────────────────────────────────────────────────

/**
 * Get the result for a specific atomic rule.
 */
function getAtomicResult(ruleId: string): AtomicRuleResult | undefined {
  return atomicResults.value[ruleId];
}

/**
 * Get the result for a specific composite rule.
 */
function getCompositeResult(ruleId: string): CompositeRuleResult | undefined {
  return compositeResults.value[ruleId];
}

/**
 * Check if an atomic rule is currently "active" (triggered non-pass action).
 */
function isAtomicRuleActive(ruleId: string): boolean {
  const result = atomicResults.value[ruleId];
  return result?.success === true && result?.action?.action !== 'pass';
}

/**
 * Check if a composite rule is currently triggered.
 */
function isCompositeRuleTriggered(ruleId: string): boolean {
  const result = compositeResults.value[ruleId];
  return result?.success === true && result?.triggered === true;
}

/**
 * Get all active atomic rules.
 */
const activeAtomicRules = computed(() => {
  return Object.entries(atomicResults.value)
    .filter(([, result]) => result.success && result.action?.action !== 'pass')
    .map(([id, result]) => ({ id, ...result }));
});

/**
 * Get all triggered composite rules.
 */
const triggeredCompositeRules = computed(() => {
  return Object.entries(compositeResults.value)
    .filter(([, result]) => result.success && result.triggered)
    .map(([id, result]) => ({ id, ...result }));
});

// ─── Polling ─────────────────────────────────────────────────────────────────

async function fetchAtomicResults(): Promise<void> {
  try {
    const response = await fetch('/api/rules/results');
    if (!response.ok) {
      throw new Error(`Failed to fetch atomic results: ${response.statusText}`);
    }

    const data: AtomicRuleResultsResponse = await response.json();
    atomicResults.value = data.results || {};
    atomicEvaluatedAt.value = data.evaluated_at || '';
    error.value = null;
  } catch (err) {
    error.value = err instanceof Error ? err.message : 'Failed to fetch atomic results';
  }
}

async function fetchCompositeResults(): Promise<void> {
  try {
    const response = await fetch('/api/composite-rules/results');
    if (!response.ok) {
      throw new Error(`Failed to fetch composite results: ${response.statusText}`);
    }

    const data: CompositeRuleResultsResponse = await response.json();
    compositeResults.value = data.results || {};
    compositeEvaluatedAt.value = data.evaluated_at || '';
    error.value = null;
  } catch (err) {
    error.value = err instanceof Error ? err.message : 'Failed to fetch composite results';
  }
}

async function fetchAll(): Promise<void> {
  await Promise.all([fetchAtomicResults(), fetchCompositeResults()]);
}

function startPolling(intervalMs: number = 2000): void {
  if (polling.value) return;

  polling.value = true;

  // Initial fetch
  fetchAll();

  // Start polling
  pollTimer = window.setInterval(() => {
    fetchAll();
  }, intervalMs);
}

function stopPolling(): void {
  if (pollTimer !== null) {
    clearInterval(pollTimer);
    pollTimer = null;
  }
  polling.value = false;
}

// ─── Cleanup ─────────────────────────────────────────────────────────────────

function cleanup(): void {
  stopPolling();
}

// ─── Export ──────────────────────────────────────────────────────────────────

export function useRuleEvaluation() {
  // Cleanup on unmount if used in component
  onUnmounted(() => {
    cleanup();
  });

  return {
    // State (readonly)
    atomicResults: readonly(atomicResults),
    compositeResults: readonly(compositeResults),
    atomicEvaluatedAt: readonly(atomicEvaluatedAt),
    compositeEvaluatedAt: readonly(compositeEvaluatedAt),
    polling: readonly(polling),
    error: readonly(error),

    // Computed
    activeAtomicRules,
    triggeredCompositeRules,

    // Getters
    getAtomicResult,
    getCompositeResult,
    isAtomicRuleActive,
    isCompositeRuleTriggered,

    // Polling
    fetchAll,
    fetchAtomicResults,
    fetchCompositeResults,
    startPolling,
    stopPolling,
    cleanup,
  };
}
