<script setup lang="ts">
import { ref, onMounted, onUnmounted, watch } from 'vue';

interface Props {
  ruleId: string;
}

const props = defineProps<Props>();

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

// State
const executions = ref<RuleExecution[]>([]);
const loading = ref(true);
const error = ref<string | null>(null);
const total = ref(0);

// Auto-refresh
const autoRefresh = ref(false);
const refreshInterval = ref(10); // seconds
let refreshTimer: number | null = null;

function startAutoRefresh() {
  stopAutoRefresh();
  if (autoRefresh.value) {
    refreshTimer = window.setInterval(() => {
      fetchHistory();
    }, refreshInterval.value * 1000);
  }
}

function stopAutoRefresh() {
  if (refreshTimer) {
    clearInterval(refreshTimer);
    refreshTimer = null;
  }
}

function toggleAutoRefresh() {
  autoRefresh.value = !autoRefresh.value;
  if (autoRefresh.value) {
    startAutoRefresh();
  } else {
    stopAutoRefresh();
  }
}

async function fetchHistory() {
  loading.value = true;
  error.value = null;

  try {
    const response = await fetch(`/api/rules/${props.ruleId}/executions`);
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }
    const data = await response.json() as { executions: RuleExecution[]; total: number };
    executions.value = data.executions;
    total.value = data.total;
  } catch (e) {
    error.value = e instanceof Error ? e.message : 'Failed to fetch history';
    executions.value = [];
  } finally {
    loading.value = false;
  }
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

function formatDate(timestamp: string): string {
  const date = new Date(timestamp);
  return date.toLocaleDateString('en-US', {
    month: 'short',
    day: 'numeric',
  });
}

function getActionClass(action: string): string {
  return `action-${action}`;
}

// Compute stats
function getStats() {
  const stats = {
    total: executions.value.length,
    pass: 0,
    alert: 0,
    reject: 0,
    other: 0,
    avgMs: 0,
    errors: 0,
  };

  let totalMs = 0;
  for (const exec of executions.value) {
    totalMs += exec.execution_ms;
    if (!exec.success) stats.errors++;

    switch (exec.action) {
      case 'pass': stats.pass++; break;
      case 'alert': stats.alert++; break;
      case 'reject': stats.reject++; break;
      default: stats.other++; break;
    }
  }

  stats.avgMs = stats.total > 0 ? totalMs / stats.total : 0;
  return stats;
}

// Watch for ruleId changes
watch(() => props.ruleId, () => {
  fetchHistory();
  // Restart auto-refresh for new rule
  if (autoRefresh.value) {
    startAutoRefresh();
  }
});

onMounted(() => {
  fetchHistory();
});

onUnmounted(() => {
  stopAutoRefresh();
});
</script>

<template>
  <div class="execution-history">
    <!-- Stats Summary -->
    <div class="history-stats" v-if="!loading && executions.length > 0">
      <div class="stat-item">
        <span class="stat-value">{{ getStats().total }}</span>
        <span class="stat-label">Total</span>
      </div>
      <div class="stat-item pass">
        <span class="stat-value">{{ getStats().pass }}</span>
        <span class="stat-label">Pass</span>
      </div>
      <div class="stat-item alert">
        <span class="stat-value">{{ getStats().alert }}</span>
        <span class="stat-label">Alert</span>
      </div>
      <div class="stat-item reject">
        <span class="stat-value">{{ getStats().reject }}</span>
        <span class="stat-label">Reject</span>
      </div>
      <div class="stat-item">
        <span class="stat-value">{{ getStats().avgMs.toFixed(1) }}ms</span>
        <span class="stat-label">Avg Time</span>
      </div>
      <div class="stat-actions">
        <button
          class="auto-refresh-btn"
          :class="{ active: autoRefresh }"
          @click="toggleAutoRefresh"
          :title="autoRefresh ? 'Stop auto-refresh' : 'Start auto-refresh (every ' + refreshInterval + 's)'"
        >
          {{ autoRefresh ? 'Auto: ON' : 'Auto: OFF' }}
        </button>
        <button class="refresh-btn" @click="fetchHistory" :disabled="loading">
          {{ loading ? 'Loading...' : 'Refresh' }}
        </button>
      </div>
    </div>

    <!-- Loading State -->
    <div class="loading-state" v-if="loading">
      <span class="spinner"></span>
      <span>Loading history...</span>
    </div>

    <!-- Error State -->
    <div class="error-state" v-else-if="error">
      <span class="error-icon">⚠️</span>
      <span>{{ error }}</span>
      <button class="retry-btn" @click="fetchHistory">Retry</button>
    </div>

    <!-- Empty State -->
    <div class="empty-state" v-else-if="executions.length === 0">
      <div class="empty-icon">📋</div>
      <div class="empty-text">No execution history yet</div>
      <div class="empty-hint">
        History will be recorded when the rule is evaluated
      </div>
    </div>

    <!-- History List -->
    <div class="history-list" v-else>
      <table class="history-table">
        <thead>
          <tr>
            <th class="col-date">Date</th>
            <th class="col-time">Time</th>
            <th class="col-action">Result</th>
            <th class="col-payload">Payload</th>
            <th class="col-ms">Ms</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="exec in executions" :key="exec.timestamp" :class="{ error: !exec.success }">
            <td class="col-date">{{ formatDate(exec.timestamp) }}</td>
            <td class="col-time">{{ formatTime(exec.timestamp) }}</td>
            <td class="col-action">
              <span class="action-badge" :class="getActionClass(exec.action)">
                {{ exec.action }}
              </span>
            </td>
            <td class="col-payload">
              <span class="payload-text" :title="exec.payload_preview">
                {{ exec.payload_preview }}
              </span>
              <span v-if="exec.error" class="error-text" :title="exec.error">
                {{ exec.error }}
              </span>
            </td>
            <td class="col-ms">{{ exec.execution_ms.toFixed(1) }}</td>
          </tr>
        </tbody>
      </table>
    </div>
  </div>
</template>

<style scoped>
.execution-history {
  background: #0F172A;
  border-radius: 8px;
  overflow: hidden;
}

.history-stats {
  display: flex;
  align-items: center;
  gap: 16px;
  padding: 12px 16px;
  background: #1E293B;
  border-bottom: 1px solid #334155;
  flex-wrap: wrap;
}

.stat-item {
  display: flex;
  flex-direction: column;
  align-items: center;
  padding: 4px 12px;
  min-width: 50px;
}

.stat-value {
  font-size: 16px;
  font-weight: 600;
  color: #e2e8f0;
}

.stat-label {
  font-size: 10px;
  color: #64748b;
  text-transform: uppercase;
}

.stat-item.pass .stat-value { color: #4ade80; }
.stat-item.alert .stat-value { color: #fb923c; }
.stat-item.reject .stat-value { color: #f87171; }

.stat-actions {
  margin-left: auto;
  display: flex;
  gap: 8px;
}

.auto-refresh-btn {
  padding: 6px 12px;
  font-size: 11px;
  font-weight: 500;
  color: #94a3b8;
  background: #0F172A;
  border: 1px solid #334155;
  border-radius: 4px;
  cursor: pointer;
  transition: all 0.2s;
}

.auto-refresh-btn:hover {
  background: #1E293B;
}

.auto-refresh-btn.active {
  background: #10B981;
  color: white;
  border-color: #10B981;
}

.refresh-btn {
  padding: 6px 12px;
  font-size: 12px;
  font-weight: 500;
  color: #94a3b8;
  background: #0F172A;
  border: 1px solid #334155;
  border-radius: 4px;
  cursor: pointer;
  transition: all 0.2s;
}

.refresh-btn:hover:not(:disabled) {
  background: #1E293B;
  color: #e2e8f0;
}

.refresh-btn:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.loading-state,
.error-state {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 12px;
  padding: 40px 20px;
  color: #94a3b8;
}

.spinner {
  width: 20px;
  height: 20px;
  border: 2px solid #334155;
  border-top-color: #3b82f6;
  border-radius: 50%;
  animation: spin 1s linear infinite;
}

@keyframes spin {
  to { transform: rotate(360deg); }
}

.error-state {
  color: #f87171;
}

.error-icon {
  font-size: 20px;
}

.retry-btn {
  padding: 6px 12px;
  font-size: 12px;
  color: white;
  background: #3b82f6;
  border: none;
  border-radius: 4px;
  cursor: pointer;
}

.retry-btn:hover {
  background: #2563eb;
}

.empty-state {
  padding: 40px 20px;
  text-align: center;
}

.empty-icon {
  font-size: 32px;
  margin-bottom: 12px;
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

.history-list {
  max-height: 300px;
  overflow-y: auto;
}

.history-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 12px;
}

.history-table th {
  position: sticky;
  top: 0;
  background: #1E293B;
  padding: 8px 12px;
  text-align: left;
  font-weight: 600;
  color: #64748b;
  text-transform: uppercase;
  letter-spacing: 0.05em;
}

.history-table td {
  padding: 8px 12px;
  border-top: 1px solid #1E293B;
  color: #e2e8f0;
}

.history-table tr:hover {
  background: rgba(59, 130, 246, 0.05);
}

.history-table tr.error {
  background: rgba(239, 68, 68, 0.1);
}

.col-date { width: 60px; color: #64748b; }
.col-time { width: 70px; }
.col-action { width: 80px; }
.col-payload { flex: 1; }
.col-ms { width: 50px; text-align: right; }

.action-badge {
  display: inline-block;
  padding: 2px 8px;
  font-size: 10px;
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.05em;
  border-radius: 4px;
}

.action-pass    { background: #14532d; color: #4ade80; }
.action-reject  { background: #450a0a; color: #f87171; }
.action-alert   { background: #451a03; color: #fb923c; }
.action-log     { background: #1e1b4b; color: #818cf8; }
.action-modbus_write { background: #042f2e; color: #2dd4bf; }

.payload-text {
  font-family: 'JetBrains Mono', monospace;
  font-size: 11px;
  color: #94a3b8;
  max-width: 180px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
  display: inline-block;
}

.error-text {
  font-size: 10px;
  color: #f87171;
  margin-left: 8px;
}
</style>
