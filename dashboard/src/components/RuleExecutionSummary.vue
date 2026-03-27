<script setup lang="ts">
import { ref, onMounted, onUnmounted, computed, shallowRef } from 'vue';

interface RuleExecution {
  timestamp: string;
  ruleId: string;
  ruleName: string;
  action: string;
  success: boolean;
  execution_ms: number;
  payload_preview: string;
  error?: string;
}

interface RuleSummary {
  ruleId: string;
  ruleName: string;
  lastAction: string;
  lastTimestamp: string;
  passCount: number;
  alertCount: number;
  rejectCount: number;
  errorCount: number;
  avgMs: number;
}

// Configuration
const MAX_EXECUTIONS = 30;        // Reduced from 50
const MAX_TOTAL_COUNT = 5000;     // Auto-reset threshold
const UPDATE_INTERVAL_MS = 2000;  // Batch update interval (2 seconds)
const AUTO_RESET_MINUTES = 60;    // Auto-reset after 1 hour

// State - use shallowRef for better performance
const executions = shallowRef<RuleExecution[]>([]);
const ruleSummaries = shallowRef<Map<string, RuleSummary>>(new Map());
const isConnected = ref(false);
const isPaused = ref(false);

// Pending updates buffer (not reactive - for batching)
let pendingExecutions: RuleExecution[] = [];
let pendingUpdates = false;

let ws: WebSocket | null = null;
let reconnectTimer: number | null = null;
let updateTimer: number | null = null;
let autoResetTimer: number | null = null;
let startTime = Date.now();

function getWebSocketUrl(): string {
  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  return `${protocol}//${window.location.host}/gateway-ws`;
}

function connect() {
  if (ws && ws.readyState === WebSocket.OPEN) {
    return;
  }

  try {
    ws = new WebSocket(getWebSocketUrl());

    ws.onopen = () => {
      isConnected.value = true;
      ws?.send(JSON.stringify({
        type: 'subscribe',
        channel: 'rule-executions',
      }));
    };

    ws.onmessage = (event) => {
      try {
        const msg = JSON.parse(event.data);
        if (msg.type === 'rule:execution' && msg.payload && !isPaused.value) {
          const exec = msg.payload as RuleExecution;
          queueExecution(exec); // Queue for batched update
        }
      } catch {
        // Ignore parse errors
      }
    };

    ws.onclose = () => {
      isConnected.value = false;
      if (!reconnectTimer) {
        reconnectTimer = window.setTimeout(() => {
          reconnectTimer = null;
          connect();
        }, 3000);
      }
    };

    ws.onerror = () => {
      isConnected.value = false;
    };
  } catch {
    isConnected.value = false;
  }
}

function disconnect() {
  if (reconnectTimer) {
    clearTimeout(reconnectTimer);
    reconnectTimer = null;
  }
  if (ws) {
    ws.close();
    ws = null;
  }
}

// Queue execution for batched update (doesn't trigger Vue reactivity)
function queueExecution(exec: RuleExecution) {
  pendingExecutions.push(exec);
  pendingUpdates = true;
}

// Process pending updates and update Vue state (batched)
function flushUpdates() {
  if (!pendingUpdates || pendingExecutions.length === 0) return;

  // Clone current state for modification
  const newExecutions = [...executions.value];
  const newSummaries = new Map(ruleSummaries.value);

  // Process all pending executions
  for (const exec of pendingExecutions) {
    // Add to executions list
    newExecutions.unshift(exec);

    // Update rule summary
    let summary = newSummaries.get(exec.ruleId);
    if (!summary) {
      summary = {
        ruleId: exec.ruleId,
        ruleName: exec.ruleName,
        lastAction: exec.action,
        lastTimestamp: exec.timestamp,
        passCount: 0,
        alertCount: 0,
        rejectCount: 0,
        errorCount: 0,
        avgMs: exec.execution_ms,
      };
    }

    // Update counts
    summary.lastAction = exec.action;
    summary.lastTimestamp = exec.timestamp;

    switch (exec.action) {
      case 'pass': summary.passCount++; break;
      case 'alert': summary.alertCount++; break;
      case 'reject': summary.rejectCount++; break;
    }
    if (!exec.success) summary.errorCount++;

    // Rolling average for execution time
    const totalExecs = summary.passCount + summary.alertCount + summary.rejectCount;
    summary.avgMs = ((summary.avgMs * (totalExecs - 1)) + exec.execution_ms) / totalExecs;

    newSummaries.set(exec.ruleId, summary);
  }

  // Trim executions to max
  while (newExecutions.length > MAX_EXECUTIONS) {
    newExecutions.pop();
  }

  // Check for auto-reset threshold
  let totalCount = 0;
  for (const s of newSummaries.values()) {
    totalCount += s.passCount + s.alertCount + s.rejectCount;
  }

  if (totalCount >= MAX_TOTAL_COUNT) {
    // Auto-reset to prevent memory issues
    console.log('[RuleExecutionSummary] Auto-reset triggered (max count reached)');
    clearAllInternal();
  } else {
    // Update Vue state (single reactive update)
    executions.value = newExecutions;
    ruleSummaries.value = newSummaries;
  }

  // Clear pending
  pendingExecutions = [];
  pendingUpdates = false;
}

// Start batched update timer
function startUpdateTimer() {
  if (updateTimer) return;
  updateTimer = window.setInterval(flushUpdates, UPDATE_INTERVAL_MS);
}

// Stop batched update timer
function stopUpdateTimer() {
  if (updateTimer) {
    clearInterval(updateTimer);
    updateTimer = null;
  }
}

// Start auto-reset timer
function startAutoResetTimer() {
  if (autoResetTimer) return;
  startTime = Date.now();
  autoResetTimer = window.setInterval(() => {
    const elapsed = Date.now() - startTime;
    if (elapsed >= AUTO_RESET_MINUTES * 60 * 1000) {
      console.log('[RuleExecutionSummary] Auto-reset triggered (time limit)');
      clearAllInternal();
      startTime = Date.now();
    }
  }, 60000); // Check every minute
}

// Stop auto-reset timer
function stopAutoResetTimer() {
  if (autoResetTimer) {
    clearInterval(autoResetTimer);
    autoResetTimer = null;
  }
}

// Internal clear without logging
function clearAllInternal() {
  executions.value = [];
  ruleSummaries.value = new Map();
  pendingExecutions = [];
  pendingUpdates = false;
  startTime = Date.now();
}

function formatTime(timestamp: string): string {
  const date = new Date(timestamp);
  return date.toLocaleTimeString('en-US', {
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    hour12: false,
  });
}

function getActionClass(action: string): string {
  return `action-${action}`;
}

function togglePause() {
  isPaused.value = !isPaused.value;
}

function clearAll() {
  clearAllInternal();
  console.log('[RuleExecutionSummary] Manual reset');
}

// Computed: sorted summaries by last activity
const sortedSummaries = computed(() => {
  const summaries = ruleSummaries.value;
  if (!summaries || summaries.size === 0) return [];
  return Array.from(summaries.values()).sort((a, b) => {
    return new Date(b.lastTimestamp).getTime() - new Date(a.lastTimestamp).getTime();
  });
});

// Computed: overall stats
const overallStats = computed(() => {
  let pass = 0, alert = 0, reject = 0, errors = 0;
  const summaries = ruleSummaries.value;
  if (summaries && summaries.size > 0) {
    for (const s of summaries.values()) {
      pass += s.passCount;
      alert += s.alertCount;
      reject += s.rejectCount;
      errors += s.errorCount;
    }
  }
  return { pass, alert, reject, errors, total: pass + alert + reject };
});

// Computed: limited executions for display (reduce DOM elements)
const displayExecutions = computed(() => {
  return executions.value.slice(0, 8); // Show only last 8
});

onMounted(() => {
  connect();
  startUpdateTimer();
  startAutoResetTimer();
});

onUnmounted(() => {
  disconnect();
  stopUpdateTimer();
  stopAutoResetTimer();
});
</script>

<template>
  <div class="rule-summary">
    <div class="summary-header">
      <h3>Rule Execution Monitor</h3>
      <div class="header-controls">
        <span class="connection-status" :class="{ connected: isConnected }">
          <span class="status-dot"></span>
          {{ isConnected ? 'Live' : 'Disconnected' }}
        </span>
        <button class="control-btn" @click="togglePause" :class="{ active: isPaused }">
          {{ isPaused ? 'Resume' : 'Pause' }}
        </button>
        <button class="control-btn reset" @click="clearAll" title="Reset all counters and activity">Reset</button>
      </div>
    </div>

    <!-- Overall Stats -->
    <div class="overall-stats" v-if="overallStats.total > 0">
      <div class="stat-chip">
        <span class="stat-value">{{ overallStats.total }}</span>
        <span class="stat-label">Total</span>
      </div>
      <div class="stat-chip pass">
        <span class="stat-value">{{ overallStats.pass }}</span>
        <span class="stat-label">Pass</span>
      </div>
      <div class="stat-chip alert">
        <span class="stat-value">{{ overallStats.alert }}</span>
        <span class="stat-label">Alert</span>
      </div>
      <div class="stat-chip reject">
        <span class="stat-value">{{ overallStats.reject }}</span>
        <span class="stat-label">Reject</span>
      </div>
      <div class="stat-chip error" v-if="overallStats.errors > 0">
        <span class="stat-value">{{ overallStats.errors }}</span>
        <span class="stat-label">Errors</span>
      </div>
    </div>

    <!-- Rule Summaries -->
    <div class="rule-cards" v-if="sortedSummaries.length > 0">
      <div
        v-for="summary in sortedSummaries"
        :key="summary.ruleId"
        class="rule-card"
        :class="getActionClass(summary.lastAction)"
      >
        <div class="rule-info">
          <span class="rule-name">{{ summary.ruleName }}</span>
          <span class="rule-time">{{ formatTime(summary.lastTimestamp) }}</span>
        </div>
        <div class="rule-stats">
          <span class="action-badge" :class="getActionClass(summary.lastAction)">
            {{ summary.lastAction }}
          </span>
          <span class="mini-stat" v-if="summary.passCount > 0">
            <span class="count pass">{{ summary.passCount }}</span> pass
          </span>
          <span class="mini-stat" v-if="summary.alertCount > 0">
            <span class="count alert">{{ summary.alertCount }}</span> alert
          </span>
          <span class="mini-stat" v-if="summary.rejectCount > 0">
            <span class="count reject">{{ summary.rejectCount }}</span> reject
          </span>
          <span class="avg-ms">{{ summary.avgMs.toFixed(1) }}ms</span>
        </div>
      </div>
    </div>

    <!-- Empty State -->
    <div class="empty-state" v-else>
      <div class="empty-text">
        {{ isConnected ? 'Waiting for rule executions...' : 'Connecting...' }}
      </div>
      <div class="empty-hint">
        Rule execution results will appear here in real-time
      </div>
    </div>

    <!-- Recent Executions Feed -->
    <div class="recent-feed" v-if="executions.length > 0">
      <div class="feed-header">Recent Activity</div>
      <div class="feed-list">
        <div
          v-for="exec in displayExecutions"
          :key="exec.timestamp + exec.ruleId"
          class="feed-item"
          :class="{ error: !exec.success }"
        >
          <span class="feed-time">{{ formatTime(exec.timestamp) }}</span>
          <span class="feed-rule">{{ exec.ruleName }}</span>
          <span class="action-badge small" :class="getActionClass(exec.action)">
            {{ exec.action }}
          </span>
          <span class="feed-payload">{{ exec.payload_preview }}</span>
        </div>
      </div>
    </div>

    <!-- Auto-reset info -->
    <div class="auto-reset-info" v-if="overallStats.total > 0">
      Auto-resets hourly or at 5000 executions
    </div>
  </div>
</template>

<style scoped>
.rule-summary {
  background: #1E293B;
  border-radius: 12px;
  padding: 20px;
  margin-bottom: 24px;
}

.summary-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 16px;
}

.summary-header h3 {
  font-size: 1rem;
  font-weight: 600;
  color: #E2E8F0;
  margin: 0;
}

.header-controls {
  display: flex;
  align-items: center;
  gap: 12px;
}

.connection-status {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 12px;
  color: #94a3b8;
}

.status-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: #EF4444;
}

.connection-status.connected .status-dot {
  background: #10B981;
  animation: pulse 2s infinite;
}

.connection-status.connected {
  color: #10B981;
}

@keyframes pulse {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.5; }
}

.control-btn {
  padding: 5px 12px;
  font-size: 11px;
  font-weight: 500;
  color: #94a3b8;
  background: #0F172A;
  border: 1px solid #334155;
  border-radius: 4px;
  cursor: pointer;
  transition: all 0.2s;
}

.control-btn:hover {
  background: #334155;
  color: #e2e8f0;
}

.control-btn.active {
  background: #F59E0B;
  color: #0F172A;
  border-color: #F59E0B;
}

.control-btn.reset {
  border-color: #7f1d1d;
  color: #fca5a5;
}

.control-btn.reset:hover {
  background: #991b1b;
  border-color: #991b1b;
  color: white;
}

.overall-stats {
  display: flex;
  gap: 12px;
  margin-bottom: 16px;
  flex-wrap: wrap;
}

.stat-chip {
  display: flex;
  flex-direction: column;
  align-items: center;
  padding: 8px 16px;
  background: #0F172A;
  border-radius: 8px;
  min-width: 60px;
}

.stat-chip .stat-value {
  font-size: 18px;
  font-weight: 600;
  color: #E2E8F0;
}

.stat-chip .stat-label {
  font-size: 10px;
  color: #64748b;
  text-transform: uppercase;
}

.stat-chip.pass .stat-value { color: #4ade80; }
.stat-chip.alert .stat-value { color: #fb923c; }
.stat-chip.reject .stat-value { color: #f87171; }
.stat-chip.error .stat-value { color: #ef4444; }

.rule-cards {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(280px, 1fr));
  gap: 12px;
  margin-bottom: 16px;
}

.rule-card {
  background: #0F172A;
  border-radius: 8px;
  padding: 12px;
  border-left: 3px solid #334155;
}

.rule-card.action-pass { border-left-color: #4ade80; }
.rule-card.action-alert { border-left-color: #fb923c; }
.rule-card.action-reject { border-left-color: #f87171; }

.rule-info {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 8px;
}

.rule-name {
  font-size: 13px;
  font-weight: 500;
  color: #E2E8F0;
}

.rule-time {
  font-size: 11px;
  color: #64748b;
}

.rule-stats {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-wrap: wrap;
}

.action-badge {
  display: inline-block;
  padding: 2px 8px;
  font-size: 10px;
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.05em;
  border-radius: 4px;
}

.action-badge.small {
  padding: 1px 6px;
  font-size: 9px;
}

.action-pass    { background: #14532d; color: #4ade80; }
.action-reject  { background: #450a0a; color: #f87171; }
.action-alert   { background: #451a03; color: #fb923c; }
.action-log     { background: #1e1b4b; color: #818cf8; }

.mini-stat {
  font-size: 11px;
  color: #64748b;
}

.mini-stat .count {
  font-weight: 600;
}

.mini-stat .count.pass { color: #4ade80; }
.mini-stat .count.alert { color: #fb923c; }
.mini-stat .count.reject { color: #f87171; }

.avg-ms {
  font-size: 11px;
  color: #64748b;
  margin-left: auto;
}

.empty-state {
  padding: 40px 20px;
  text-align: center;
}

.empty-text {
  font-size: 14px;
  color: #94a3b8;
  margin-bottom: 8px;
}

.empty-hint {
  font-size: 12px;
  color: #64748b;
}

.recent-feed {
  border-top: 1px solid #334155;
  padding-top: 12px;
}

.feed-header {
  font-size: 11px;
  font-weight: 600;
  color: #64748b;
  text-transform: uppercase;
  letter-spacing: 0.05em;
  margin-bottom: 8px;
}

.feed-list {
  max-height: 160px;
  overflow-y: auto;
}

.feed-item {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 6px 0;
  font-size: 12px;
  border-bottom: 1px solid #1E293B;
}

.feed-item.error {
  background: rgba(239, 68, 68, 0.1);
}

.feed-time {
  color: #64748b;
  font-family: 'JetBrains Mono', monospace;
  font-size: 11px;
  min-width: 65px;
}

.feed-rule {
  color: #E2E8F0;
  font-weight: 500;
  min-width: 120px;
  max-width: 150px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.feed-payload {
  color: #64748b;
  font-family: 'JetBrains Mono', monospace;
  font-size: 11px;
  flex: 1;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.auto-reset-info {
  margin-top: 12px;
  padding-top: 8px;
  border-top: 1px solid #334155;
  font-size: 10px;
  color: #475569;
  text-align: center;
}
</style>
