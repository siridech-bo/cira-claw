<script setup lang="ts">
import { ref, onMounted } from 'vue';
import NodeCard from '../components/NodeCard.vue';

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

interface Summary {
  total: number;
  online: number;
  offline: number;
  error: number;
  unknown: number;
}

const nodes = ref<Node[]>([]);
const summary = ref<Summary>({ total: 0, online: 0, offline: 0, error: 0, unknown: 0 });
const loading = ref(true);
const error = ref<string | null>(null);

onMounted(async () => {
  await fetchNodes();
});

async function fetchNodes() {
  try {
    loading.value = true;
    error.value = null;

    const response = await fetch('/api/nodes');
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }

    const data = await response.json();
    nodes.value = data.nodes;
    summary.value = data.summary;
  } catch (e) {
    error.value = e instanceof Error ? e.message : 'Failed to fetch nodes';
  } finally {
    loading.value = false;
  }
}
</script>

<template>
  <div class="overview">
    <header class="page-header">
      <h2>Device Overview</h2>
      <button class="refresh-btn" @click="fetchNodes" :disabled="loading">
        {{ loading ? 'Loading...' : 'Refresh' }}
      </button>
    </header>

    <div class="summary-cards" v-if="!loading">
      <div class="summary-card">
        <div class="summary-value">{{ summary.total }}</div>
        <div class="summary-label">Total Devices</div>
      </div>
      <div class="summary-card online">
        <div class="summary-value">{{ summary.online }}</div>
        <div class="summary-label">Online</div>
      </div>
      <div class="summary-card offline">
        <div class="summary-value">{{ summary.offline }}</div>
        <div class="summary-label">Offline</div>
      </div>
      <div class="summary-card error" v-if="summary.error > 0">
        <div class="summary-value">{{ summary.error }}</div>
        <div class="summary-label">Error</div>
      </div>
    </div>

    <div class="error-message" v-if="error">
      <p>Failed to load devices: {{ error }}</p>
      <button @click="fetchNodes">Try Again</button>
    </div>

    <div class="nodes-grid" v-if="!loading && !error">
      <NodeCard
        v-for="node in nodes"
        :key="node.id"
        :node="node"
      />

      <div class="empty-state" v-if="nodes.length === 0">
        <p>No devices registered yet.</p>
        <p>Add devices via the API or wait for mDNS discovery.</p>
      </div>
    </div>
  </div>
</template>

<style scoped>
.overview {
  max-width: 1400px;
  margin: 0 auto;
}

.page-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 24px;
}

.page-header h2 {
  font-size: 1.5rem;
  font-weight: 600;
}

.refresh-btn {
  padding: 8px 16px;
  background: #2563eb;
  color: white;
  border: none;
  border-radius: 6px;
  cursor: pointer;
  font-size: 0.875rem;
}

.refresh-btn:hover {
  background: #1d4ed8;
}

.refresh-btn:disabled {
  background: #94a3b8;
  cursor: not-allowed;
}

.summary-cards {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(150px, 1fr));
  gap: 16px;
  margin-bottom: 32px;
}

.summary-card {
  background: white;
  padding: 20px;
  border-radius: 12px;
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.1);
}

.summary-value {
  font-size: 2rem;
  font-weight: 700;
  color: #1e293b;
}

.summary-label {
  font-size: 0.875rem;
  color: #64748b;
  margin-top: 4px;
}

.summary-card.online .summary-value {
  color: #16a34a;
}

.summary-card.offline .summary-value {
  color: #dc2626;
}

.summary-card.error .summary-value {
  color: #ea580c;
}

.nodes-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(320px, 1fr));
  gap: 20px;
}

.error-message {
  background: #fef2f2;
  border: 1px solid #fecaca;
  color: #dc2626;
  padding: 20px;
  border-radius: 8px;
  text-align: center;
}

.error-message button {
  margin-top: 12px;
  padding: 8px 16px;
  background: #dc2626;
  color: white;
  border: none;
  border-radius: 6px;
  cursor: pointer;
}

.empty-state {
  grid-column: 1 / -1;
  text-align: center;
  padding: 60px 20px;
  color: #64748b;
}

.empty-state p:first-child {
  font-size: 1.125rem;
  margin-bottom: 8px;
}
</style>
