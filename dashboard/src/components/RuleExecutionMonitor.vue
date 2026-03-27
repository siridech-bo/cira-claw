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
const isPaused = ref(false);
const isConnected = ref(false);

let ws: WebSocket | null = null;
let reconnectTimer: number | null = null;

function getWebSocketUrl(): string {
  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  // Use gateway-ws path which proxies to gateway's /ws endpoint
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
      // Subscribe to rule-executions channel
      ws?.send(JSON.stringify({
        type: 'subscribe',
        channel: 'rule-executions',
      }));
    };

    ws.onmessage = (event) => {
      try {
        const msg = JSON.parse(event.data);
        if (msg.type === 'rule:execution' && msg.payload) {
          const exec = msg.payload as RuleExecution;
          // Only show executions for our rule
          if (exec.ruleId === props.ruleId && !isPaused.value) {
            executions.value.unshift(exec);
            // Keep max 100 entries
            if (executions.value.length > 100) {
              executions.value.pop();
            }
          }
        }
      } catch {
        // Ignore parse errors
      }
    };

    ws.onclose = () => {
      isConnected.value = false;
      // Auto-reconnect after 3 seconds
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

function togglePause() {
  isPaused.value = !isPaused.value;
}

function clear() {
  executions.value = [];
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

// Watch for ruleId changes
watch(() => props.ruleId, () => {
  // Clear executions when rule changes
  executions.value = [];
});

onMounted(() => {
  connect();
});

onUnmounted(() => {
  disconnect();
});
</script>

<template>
  <div class="execution-monitor">
    <div class="monitor-header">
      <div class="connection-status" :class="{ connected: isConnected }">
        <span class="status-dot"></span>
        <span>{{ isConnected ? 'Live' : 'Disconnected' }}</span>
      </div>
      <div class="monitor-controls">
        <button class="control-btn" @click="togglePause" :class="{ active: isPaused }">
          {{ isPaused ? 'Resume' : 'Pause' }}
        </button>
        <button class="control-btn" @click="clear">Clear</button>
      </div>
    </div>

    <div class="execution-list" v-if="executions.length > 0">
      <table class="execution-table">
        <thead>
          <tr>
            <th class="col-time">Time</th>
            <th class="col-action">Result</th>
            <th class="col-payload">Payload</th>
            <th class="col-ms">Ms</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="exec in executions" :key="exec.timestamp" :class="{ error: !exec.success }">
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

    <div class="empty-state" v-else>
      <div class="empty-icon">📊</div>
      <div class="empty-text">
        {{ isConnected ? 'Waiting for rule executions...' : 'Connecting...' }}
      </div>
      <div class="empty-hint">
        Executions will appear here in real-time when the rule is evaluated
      </div>
    </div>
  </div>
</template>

<style scoped>
.execution-monitor {
  background: #0F172A;
  border-radius: 8px;
  overflow: hidden;
}

.monitor-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 12px 16px;
  background: #1E293B;
  border-bottom: 1px solid #334155;
}

.connection-status {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 12px;
  font-weight: 500;
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

@keyframes pulse {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.5; }
}

.connection-status.connected {
  color: #10B981;
}

.monitor-controls {
  display: flex;
  gap: 8px;
}

.control-btn {
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

.control-btn:hover {
  background: #1E293B;
  color: #e2e8f0;
}

.control-btn.active {
  background: #F59E0B;
  color: #0F172A;
  border-color: #F59E0B;
}

.execution-list {
  max-height: 300px;
  overflow-y: auto;
}

.execution-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 12px;
}

.execution-table th {
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

.execution-table td {
  padding: 8px 12px;
  border-top: 1px solid #1E293B;
  color: #e2e8f0;
}

.execution-table tr:hover {
  background: rgba(59, 130, 246, 0.05);
}

.execution-table tr.error {
  background: rgba(239, 68, 68, 0.1);
}

.col-time { width: 80px; }
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
  max-width: 200px;
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
</style>
