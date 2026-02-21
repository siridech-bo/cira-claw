<script setup lang="ts">
import { ref, onMounted, computed } from 'vue';
import { useRoute, useRouter } from 'vue-router';
import CameraStream from '../components/CameraStream.vue';

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

interface NodeData {
  id: string;
  name: string;
  type: string;
  host: string;
  status: 'online' | 'offline' | 'error' | 'unknown';
  lastSeen: string | null;
  metrics: NodeMetrics | null;
  inference: NodeInference | null;
  location?: string;
  runtime?: {
    port: number;
    config: string;
  };
  models?: Array<{
    name: string;
    task: string;
    labels: string[];
  }>;
}

const route = useRoute();
const router = useRouter();
const nodeId = computed(() => route.params.id as string);

const node = ref<NodeData | null>(null);
const loading = ref(true);
const error = ref<string | null>(null);
const rebooting = ref(false);

// Model management state
interface ModelInfo {
  name: string;
  path: string;
  type?: string;
  loaded?: boolean;
}

const availableModels = ref<ModelInfo[]>([]);
const selectedModelPath = ref('');
const modelSwitching = ref(false);
const modelError = ref<string | null>(null);
const modelSuccess = ref<string | null>(null);
const streamMode = ref<'auto' | 'mjpeg' | 'polling'>('auto');

onMounted(async () => {
  await fetchNode();
  await fetchModels();
});

async function fetchNode() {
  try {
    loading.value = true;
    error.value = null;

    const response = await fetch(`/api/nodes/${nodeId.value}`);
    if (response.status === 404) {
      error.value = 'Device not found';
      return;
    }
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }

    node.value = await response.json();
  } catch (e) {
    error.value = e instanceof Error ? e.message : 'Failed to fetch device';
  } finally {
    loading.value = false;
  }
}

async function rebootDevice() {
  if (!confirm('Are you sure you want to reboot this device? It will be offline for ~2 minutes.')) {
    return;
  }

  try {
    rebooting.value = true;
    const response = await fetch(`/api/nodes/${nodeId.value}/reboot`, { method: 'POST' });
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }
    alert('Reboot initiated. The device will be offline temporarily.');
  } catch (e) {
    alert('Failed to reboot device');
  } finally {
    rebooting.value = false;
  }
}

async function fetchModels() {
  try {
    const response = await fetch(`/api/nodes/${nodeId.value}/models`);
    if (response.ok) {
      const data = await response.json();
      availableModels.value = data.available || [];
    }
  } catch (e) {
    // Silently fail - models list is optional
  }
}

async function switchModel() {
  if (!selectedModelPath.value) {
    modelError.value = 'Please select a model';
    return;
  }

  try {
    modelSwitching.value = true;
    modelError.value = null;
    modelSuccess.value = null;

    const response = await fetch(`/api/nodes/${nodeId.value}/model`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path: selectedModelPath.value }),
    });

    const data = await response.json();

    if (!response.ok || !data.success) {
      throw new Error(data.message || data.error || 'Failed to switch model');
    }

    modelSuccess.value = `Model loaded: ${data.format || 'success'}`;
    selectedModelPath.value = '';

    // Refresh node data to update model name
    await fetchNode();
    await fetchModels();

    // Clear success message after 5 seconds
    setTimeout(() => { modelSuccess.value = null; }, 5000);
  } catch (e) {
    modelError.value = e instanceof Error ? e.message : 'Failed to switch model';
  } finally {
    modelSwitching.value = false;
  }
}

function formatUptime(seconds: number | null | undefined): string {
  if (!seconds) return '--';
  const days = Math.floor(seconds / 86400);
  const hours = Math.floor((seconds % 86400) / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  if (days > 0) return `${days} days, ${hours} hours`;
  return `${hours} hours, ${minutes} minutes`;
}

</script>

<template>
  <div class="device-detail">
    <header class="page-header">
      <button class="back-btn" @click="router.push('/')">← Back</button>
      <h2 v-if="node">{{ node.name }}</h2>
      <h2 v-else>Device Detail</h2>
    </header>

    <div class="loading" v-if="loading">Loading...</div>

    <div class="error-message" v-else-if="error">
      <p>{{ error }}</p>
      <button @click="fetchNode">Try Again</button>
    </div>

    <div class="detail-content" v-else-if="node">
      <div class="streams-section">
        <div class="stream-mode-selector">
          <label>Stream Mode:</label>
          <select v-model="streamMode">
            <option value="auto">Auto (MJPEG with fallback)</option>
            <option value="mjpeg">MJPEG only</option>
            <option value="polling">Polling (file-based)</option>
          </select>
        </div>
        <div class="stream-container">
          <h3>Annotated Feed</h3>
          <CameraStream
            v-if="node.status === 'online'"
            :host="node.host"
            :port="node.runtime?.port || 8080"
            :annotated="true"
            :mode="streamMode"
          />
          <div class="stream-offline" v-else>
            <p>Device is {{ node.status }}</p>
          </div>
        </div>
        <div class="stream-container">
          <h3>Raw Feed</h3>
          <CameraStream
            v-if="node.status === 'online'"
            :host="node.host"
            :port="node.runtime?.port || 8080"
            :annotated="false"
            :mode="streamMode"
          />
          <div class="stream-offline" v-else>
            <p>Device is {{ node.status }}</p>
          </div>
        </div>
      </div>

      <div class="info-section">
        <div class="info-card">
          <h3>Status</h3>
          <div class="status-row">
            <span class="status-indicator" :class="node.status"></span>
            <span>{{ node.status }}</span>
          </div>
          <div class="info-row" v-if="node.lastSeen">
            <span class="label">Last Seen</span>
            <span>{{ new Date(node.lastSeen).toLocaleString() }}</span>
          </div>
        </div>

        <div class="info-card" v-if="node.metrics">
          <h3>Metrics</h3>
          <div class="info-row">
            <span class="label">FPS</span>
            <span>{{ node.metrics.fps ?? '--' }}</span>
          </div>
          <div class="info-row">
            <span class="label">Temperature</span>
            <span :class="{ warning: (node.metrics.temperature ?? 0) > 75 }">
              {{ node.metrics.temperature ? `${node.metrics.temperature}°C` : '--' }}
            </span>
          </div>
          <div class="info-row">
            <span class="label">CPU</span>
            <span>{{ node.metrics.cpuUsage ? `${node.metrics.cpuUsage}%` : '--' }}</span>
          </div>
          <div class="info-row">
            <span class="label">Memory</span>
            <span>{{ node.metrics.memoryUsage ? `${node.metrics.memoryUsage}%` : '--' }}</span>
          </div>
          <div class="info-row">
            <span class="label">Uptime</span>
            <span>{{ formatUptime(node.metrics.uptime) }}</span>
          </div>
        </div>

        <div class="info-card" v-if="node.inference">
          <h3>Inference</h3>
          <div class="info-row">
            <span class="label">Model</span>
            <span>{{ node.inference.modelName || '--' }}</span>
          </div>
          <div class="info-row">
            <span class="label">Total Defects</span>
            <span>{{ node.inference.defectsTotal }}</span>
          </div>
          <div class="info-row">
            <span class="label">Defects/Hour</span>
            <span>{{ node.inference.defectsPerHour }}</span>
          </div>
        </div>

        <div class="info-card model-card">
          <h3>Model Management</h3>
          <div class="info-row">
            <span class="label">Current Model</span>
            <span>{{ node.inference?.modelName || 'None loaded' }}</span>
          </div>

          <div class="model-selector">
            <select
              v-model="selectedModelPath"
              :disabled="modelSwitching || node.status !== 'online'"
            >
              <option value="">Select a model...</option>
              <option
                v-for="model in availableModels"
                :key="model.path"
                :value="model.path"
              >
                {{ model.name }}{{ model.loaded ? ' (current)' : '' }}
              </option>
            </select>

            <button
              class="switch-btn"
              @click="switchModel"
              :disabled="modelSwitching || !selectedModelPath || node.status !== 'online'"
            >
              {{ modelSwitching ? 'Loading...' : 'Switch Model' }}
            </button>
          </div>

          <div class="model-message success" v-if="modelSuccess">
            {{ modelSuccess }}
          </div>
          <div class="model-message error" v-if="modelError">
            {{ modelError }}
          </div>

          <p class="model-hint" v-if="node.status !== 'online'">
            Device must be online to switch models
          </p>
        </div>

        <div class="info-card">
          <h3>Device Info</h3>
          <div class="info-row">
            <span class="label">ID</span>
            <span class="mono">{{ node.id }}</span>
          </div>
          <div class="info-row">
            <span class="label">Type</span>
            <span>{{ node.type }}</span>
          </div>
          <div class="info-row">
            <span class="label">Host</span>
            <span class="mono">{{ node.host }}</span>
          </div>
          <div class="info-row" v-if="node.location">
            <span class="label">Location</span>
            <span>{{ node.location }}</span>
          </div>
        </div>
      </div>

      <div class="actions-section">
        <button @click="fetchNode" class="action-btn">Refresh Status</button>
        <button
          @click="rebootDevice"
          class="action-btn danger"
          :disabled="rebooting || node.status === 'offline'"
        >
          {{ rebooting ? 'Rebooting...' : 'Reboot Device' }}
        </button>
      </div>
    </div>
  </div>
</template>

<style scoped>
.device-detail {
  max-width: 1200px;
  margin: 0 auto;
}

.page-header {
  display: flex;
  align-items: center;
  gap: 16px;
  margin-bottom: 24px;
}

.back-btn {
  padding: 8px 12px;
  background: none;
  border: 1px solid #e2e8f0;
  border-radius: 6px;
  cursor: pointer;
  color: #64748b;
}

.back-btn:hover {
  background: #f1f5f9;
}

.page-header h2 {
  font-size: 1.5rem;
  font-weight: 600;
}

.loading {
  text-align: center;
  padding: 40px;
  color: #64748b;
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

.streams-section {
  display: grid;
  grid-template-columns: repeat(2, 1fr);
  gap: 20px;
  margin-bottom: 24px;
}

.stream-mode-selector {
  grid-column: 1 / -1;
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 0.875rem;
  color: #64748b;
  padding: 12px 16px;
  background: white;
  border-radius: 8px;
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.1);
}

.stream-mode-selector select {
  padding: 6px 12px;
  border: 1px solid #e2e8f0;
  border-radius: 6px;
  background: white;
  font-size: 0.875rem;
  cursor: pointer;
}

.stream-mode-selector select:focus {
  outline: none;
  border-color: #2563eb;
}

.stream-container {
  background: white;
  border-radius: 12px;
  overflow: hidden;
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.1);
}

.stream-container h3 {
  padding: 12px 16px;
  background: #f8fafc;
  border-bottom: 1px solid #e2e8f0;
  font-size: 0.875rem;
  font-weight: 600;
}

.stream-image {
  width: 100%;
  height: auto;
  display: block;
}

.stream-offline {
  padding: 60px 20px;
  text-align: center;
  color: #64748b;
  background: #f1f5f9;
}

.info-section {
  display: grid;
  grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
  gap: 20px;
  margin-bottom: 24px;
}

.info-card {
  background: white;
  border-radius: 12px;
  padding: 20px;
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.1);
}

.info-card h3 {
  font-size: 0.875rem;
  font-weight: 600;
  color: #64748b;
  margin-bottom: 16px;
  text-transform: uppercase;
  letter-spacing: 0.05em;
}

.status-row {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 1.125rem;
  font-weight: 500;
  margin-bottom: 12px;
  text-transform: capitalize;
}

.status-indicator {
  width: 12px;
  height: 12px;
  border-radius: 50%;
  background: #94a3b8;
}

.status-indicator.online {
  background: #16a34a;
}

.status-indicator.offline {
  background: #dc2626;
}

.status-indicator.error {
  background: #ea580c;
}

.info-row {
  display: flex;
  justify-content: space-between;
  padding: 8px 0;
  border-bottom: 1px solid #f1f5f9;
}

.info-row:last-child {
  border-bottom: none;
}

.info-row .label {
  color: #64748b;
}

.info-row .warning {
  color: #ea580c;
  font-weight: 500;
}

.info-row .mono {
  font-family: monospace;
  font-size: 0.875rem;
}

.actions-section {
  display: flex;
  gap: 12px;
}

.action-btn {
  padding: 10px 20px;
  background: #2563eb;
  color: white;
  border: none;
  border-radius: 6px;
  cursor: pointer;
  font-size: 0.875rem;
}

.action-btn:hover {
  background: #1d4ed8;
}

.action-btn.danger {
  background: #dc2626;
}

.action-btn.danger:hover {
  background: #b91c1c;
}

.action-btn:disabled {
  background: #94a3b8;
  cursor: not-allowed;
}

/* Model Management Styles */
.model-card {
  background: linear-gradient(135deg, #f8fafc 0%, #f1f5f9 100%);
}

.model-selector {
  display: flex;
  flex-direction: column;
  gap: 12px;
  margin-top: 16px;
}

.model-selector select {
  width: 100%;
  padding: 10px 12px;
  border: 1px solid #e2e8f0;
  border-radius: 6px;
  font-size: 0.875rem;
  background: white;
  cursor: pointer;
}

.model-selector select:focus {
  outline: none;
  border-color: #2563eb;
  box-shadow: 0 0 0 3px rgba(37, 99, 235, 0.1);
}

.model-selector select:disabled {
  background: #f1f5f9;
  cursor: not-allowed;
}

.switch-btn {
  padding: 10px 20px;
  background: #2563eb;
  color: white;
  border: none;
  border-radius: 6px;
  cursor: pointer;
  font-size: 0.875rem;
  font-weight: 500;
  transition: background 0.2s;
}

.switch-btn:hover:not(:disabled) {
  background: #1d4ed8;
}

.switch-btn:disabled {
  background: #94a3b8;
  cursor: not-allowed;
}

.model-message {
  margin-top: 12px;
  padding: 10px 12px;
  border-radius: 6px;
  font-size: 0.875rem;
}

.model-message.success {
  background: #dcfce7;
  color: #166534;
  border: 1px solid #86efac;
}

.model-message.error {
  background: #fef2f2;
  color: #dc2626;
  border: 1px solid #fecaca;
}

.model-hint {
  margin-top: 12px;
  font-size: 0.75rem;
  color: #64748b;
  font-style: italic;
}

@media (max-width: 768px) {
  .streams-section {
    grid-template-columns: 1fr;
  }
}
</style>
