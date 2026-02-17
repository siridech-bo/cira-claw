<script setup lang="ts">
import { computed } from 'vue';

interface NodeMetrics {
  fps: number | null;
  temperature: number | null;
  cpuUsage: number | null;
  memoryUsage: number | null;
  uptime: number | null;
}

interface NodeInference {
  modelName: string | null;
  defectsTotal: number;
  defectsPerHour: number;
  lastDefect: string | null;
}

interface Node {
  id: string;
  name: string;
  type: string;
  host: string;
  status: 'online' | 'offline' | 'error' | 'unknown';
  lastSeen: string | null;
  metrics: NodeMetrics | null;
  inference: NodeInference | null;
  location?: string;
}

const props = defineProps<{
  node: Node;
}>();

const statusColor = computed(() => {
  switch (props.node.status) {
    case 'online': return '#16a34a';
    case 'offline': return '#dc2626';
    case 'error': return '#ea580c';
    default: return '#94a3b8';
  }
});

const statusIcon = computed(() => {
  switch (props.node.status) {
    case 'online': return '🟢';
    case 'offline': return '🔴';
    case 'error': return '🟡';
    default: return '⚪';
  }
});

const deviceIcon = computed(() => {
  switch (props.node.type) {
    case 'jetson-nano':
    case 'jetson-nx':
    case 'jetson-agx':
      return '🖥️';
    case 'raspberry-pi':
      return '🍓';
    default:
      return '📟';
  }
});

function formatUptime(seconds: number | null | undefined): string {
  if (!seconds) return '--';
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  if (days > 0) return `${days}d ${hours}h`;
  const minutes = Math.floor((seconds % 3600) / 60);
  return `${hours}h ${minutes}m`;
}
</script>

<template>
  <router-link :to="`/device/${node.id}`" class="node-card">
    <div class="card-header">
      <div class="device-info">
        <span class="device-icon">{{ deviceIcon }}</span>
        <div>
          <h3 class="device-name">{{ node.name }}</h3>
          <span class="device-type">{{ node.type }}</span>
        </div>
      </div>
      <div class="status-badge" :style="{ backgroundColor: statusColor }">
        {{ node.status }}
      </div>
    </div>

    <div class="card-body">
      <div class="metrics" v-if="node.status === 'online' && node.metrics">
        <div class="metric">
          <span class="metric-label">FPS</span>
          <span class="metric-value">{{ node.metrics.fps ?? '--' }}</span>
        </div>
        <div class="metric">
          <span class="metric-label">Temp</span>
          <span class="metric-value" :class="{ warning: (node.metrics.temperature ?? 0) > 75 }">
            {{ node.metrics.temperature ? `${node.metrics.temperature}°C` : '--' }}
          </span>
        </div>
        <div class="metric">
          <span class="metric-label">Defects/hr</span>
          <span class="metric-value">{{ node.inference?.defectsPerHour ?? '--' }}</span>
        </div>
      </div>

      <div class="offline-message" v-else-if="node.status === 'offline'">
        <p>Device is offline</p>
        <p class="last-seen" v-if="node.lastSeen">
          Last seen: {{ new Date(node.lastSeen).toLocaleString() }}
        </p>
      </div>

      <div class="unknown-message" v-else>
        <p>Status unknown</p>
      </div>
    </div>

    <div class="card-footer">
      <span class="host">{{ node.host }}</span>
      <span class="uptime" v-if="node.metrics?.uptime">
        Uptime: {{ formatUptime(node.metrics.uptime) }}
      </span>
    </div>
  </router-link>
</template>

<style scoped>
.node-card {
  display: block;
  background: white;
  border-radius: 12px;
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.1);
  text-decoration: none;
  color: inherit;
  transition: transform 0.2s, box-shadow 0.2s;
  overflow: hidden;
}

.node-card:hover {
  transform: translateY(-2px);
  box-shadow: 0 4px 12px rgba(0, 0, 0, 0.15);
}

.card-header {
  display: flex;
  justify-content: space-between;
  align-items: flex-start;
  padding: 16px;
  background: #f8fafc;
  border-bottom: 1px solid #e2e8f0;
}

.device-info {
  display: flex;
  align-items: center;
  gap: 12px;
}

.device-icon {
  font-size: 1.5rem;
}

.device-name {
  font-size: 1rem;
  font-weight: 600;
  margin: 0;
}

.device-type {
  font-size: 0.75rem;
  color: #64748b;
}

.status-badge {
  padding: 4px 10px;
  border-radius: 20px;
  font-size: 0.75rem;
  font-weight: 500;
  color: white;
  text-transform: capitalize;
}

.card-body {
  padding: 16px;
}

.metrics {
  display: grid;
  grid-template-columns: repeat(3, 1fr);
  gap: 16px;
}

.metric {
  text-align: center;
}

.metric-label {
  display: block;
  font-size: 0.75rem;
  color: #64748b;
  margin-bottom: 4px;
}

.metric-value {
  display: block;
  font-size: 1.25rem;
  font-weight: 600;
  color: #1e293b;
}

.metric-value.warning {
  color: #ea580c;
}

.offline-message,
.unknown-message {
  text-align: center;
  color: #64748b;
  padding: 12px 0;
}

.last-seen {
  font-size: 0.75rem;
  margin-top: 4px;
}

.card-footer {
  display: flex;
  justify-content: space-between;
  padding: 12px 16px;
  background: #f8fafc;
  border-top: 1px solid #e2e8f0;
  font-size: 0.75rem;
  color: #64748b;
}

.host {
  font-family: monospace;
}
</style>
