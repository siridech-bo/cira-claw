<script setup lang="ts">
import { ref, onMounted, computed } from 'vue';

interface Camera {
  id: number;
  name: string;
  path: string;
}

interface Node {
  id: string;
  name: string;
  host: string;
  status: string;
  runtime?: {
    port: number;
  };
}

const nodes = ref<Node[]>([]);
const selectedNode = ref<Node | null>(null);
const cameras = ref<Camera[]>([]);
const cameraRunning = ref(false);
const currentCamera = ref(-1);
const loading = ref(true);
const camerasLoading = ref(false);
const actionLoading = ref(false);
const error = ref<string | null>(null);
const successMsg = ref<string | null>(null);

// Direct connect mode
const directConnectMode = ref(false);
const directHost = ref('localhost');
const directPort = ref(8080);

const onlineNodes = computed(() =>
  nodes.value.filter(n => n.status === 'online')
);

// Get the base URL for API calls
const baseUrl = computed(() => {
  if (directConnectMode.value) {
    return `http://${directHost.value}:${directPort.value}`;
  }
  if (selectedNode.value) {
    return `http://${selectedNode.value.host}:${selectedNode.value.runtime?.port || 8080}`;
  }
  return '';
});

onMounted(async () => {
  await fetchNodes();
});

async function fetchNodes() {
  try {
    loading.value = true;
    error.value = null;
    const response = await fetch('/api/nodes');
    if (!response.ok) throw new Error('Failed to fetch nodes');
    const data = await response.json();
    nodes.value = data.nodes || [];

    // Auto-select first online node
    if (onlineNodes.value.length > 0) {
      selectedNode.value = onlineNodes.value[0];
      await fetchCameras();
    } else if (nodes.value.length === 0) {
      // No nodes configured - suggest direct connect
      directConnectMode.value = true;
    }
  } catch (e) {
    // Coordinator not available - enable direct connect mode
    directConnectMode.value = true;
    error.value = null; // Clear error since we're falling back to direct connect
  } finally {
    loading.value = false;
  }
}

async function connectDirect() {
  if (!directHost.value || !directPort.value) {
    error.value = 'Please enter host and port';
    return;
  }

  // Create a virtual node for direct connection
  selectedNode.value = {
    id: 'direct',
    name: 'Direct Connection',
    host: directHost.value,
    status: 'online',
    runtime: { port: directPort.value }
  };

  await fetchCameras();
}

async function fetchCameras() {
  if (!baseUrl.value) return;

  try {
    camerasLoading.value = true;
    error.value = null;

    const response = await fetch(`${baseUrl.value}/api/cameras`);

    if (!response.ok) throw new Error('Failed to fetch cameras');

    const data = await response.json();
    cameras.value = data.cameras || [];
    cameraRunning.value = data.running || false;
    currentCamera.value = data.current ?? -1;
  } catch (e) {
    error.value = 'Failed to fetch cameras from device. Check if runtime is running.';
    cameras.value = [];
  } finally {
    camerasLoading.value = false;
  }
}

async function startCamera(deviceId: number) {
  if (!baseUrl.value) return;

  try {
    actionLoading.value = true;
    error.value = null;
    successMsg.value = null;

    const response = await fetch(`${baseUrl.value}/api/camera/start`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ device_id: deviceId })
    });

    const data = await response.json();

    if (data.success) {
      successMsg.value = `Camera ${deviceId} started successfully`;
      cameraRunning.value = true;
      currentCamera.value = deviceId;
    } else {
      error.value = data.error || 'Failed to start camera';
    }
  } catch (e) {
    error.value = 'Failed to start camera';
  } finally {
    actionLoading.value = false;
    setTimeout(() => { successMsg.value = null; }, 3000);
  }
}

async function stopCamera() {
  if (!baseUrl.value) return;

  try {
    actionLoading.value = true;
    error.value = null;
    successMsg.value = null;

    const response = await fetch(`${baseUrl.value}/api/camera/stop`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' }
    });

    const data = await response.json();

    if (data.success) {
      successMsg.value = 'Camera stopped successfully';
      cameraRunning.value = false;
      currentCamera.value = -1;
    } else {
      error.value = data.error || 'Failed to stop camera';
    }
  } catch (e) {
    error.value = 'Failed to stop camera';
  } finally {
    actionLoading.value = false;
    setTimeout(() => { successMsg.value = null; }, 3000);
  }
}

function selectNode(node: Node) {
  directConnectMode.value = false;
  selectedNode.value = node;
  fetchCameras();
}

function switchToDirectConnect() {
  directConnectMode.value = true;
  selectedNode.value = null;
  cameras.value = [];
}
</script>

<template>
  <div class="camera-manager">
    <header class="page-header">
      <h2>Camera Manager</h2>
      <p class="subtitle">Discover and manage cameras on edge devices</p>
    </header>

    <div class="loading" v-if="loading">Loading devices...</div>

    <div class="content" v-else>
      <!-- Device Selector -->
      <div class="card device-selector">
        <div class="card-header">
          <h3>Select Device</h3>
          <button
            class="mode-toggle"
            :class="{ active: directConnectMode }"
            @click="switchToDirectConnect"
          >
            Direct Connect
          </button>
        </div>

        <!-- Direct Connect Form -->
        <div class="direct-connect" v-if="directConnectMode">
          <p class="direct-hint">Connect directly to a runtime (no coordinator required)</p>
          <div class="direct-form">
            <div class="input-group">
              <label>Host</label>
              <input
                v-model="directHost"
                type="text"
                placeholder="localhost"
                @keyup.enter="connectDirect"
              />
            </div>
            <div class="input-group port">
              <label>Port</label>
              <input
                v-model.number="directPort"
                type="number"
                placeholder="8080"
                @keyup.enter="connectDirect"
              />
            </div>
            <button class="connect-btn" @click="connectDirect" :disabled="camerasLoading">
              {{ camerasLoading ? 'Connecting...' : 'Connect' }}
            </button>
          </div>
        </div>

        <!-- Node List -->
        <div class="device-list" v-else-if="onlineNodes.length > 0">
          <button
            v-for="node in onlineNodes"
            :key="node.id"
            class="device-btn"
            :class="{ active: selectedNode?.id === node.id }"
            @click="selectNode(node)"
          >
            <span class="device-name">{{ node.name }}</span>
            <span class="device-host">{{ node.host }}</span>
          </button>
        </div>
        <div class="empty-state" v-else>
          <p>No online devices available</p>
          <button class="link-btn" @click="switchToDirectConnect">Use Direct Connect</button>
        </div>
      </div>

      <!-- Camera List -->
      <div class="card camera-list" v-if="selectedNode || (directConnectMode && cameras.length > 0)">
        <div class="card-header">
          <h3>Available Cameras</h3>
          <div class="header-actions">
            <span class="connected-to" v-if="selectedNode">
              Connected to: {{ selectedNode.host }}:{{ selectedNode.runtime?.port || 8080 }}
            </span>
            <button class="refresh-btn" @click="fetchCameras" :disabled="camerasLoading">
              {{ camerasLoading ? 'Scanning...' : 'Refresh' }}
            </button>
          </div>
        </div>

        <div class="status-bar" v-if="cameraRunning">
          <span class="status-indicator running"></span>
          <span>Camera {{ currentCamera }} is running</span>
          <button class="stop-btn" @click="stopCamera" :disabled="actionLoading">
            {{ actionLoading ? 'Stopping...' : 'Stop' }}
          </button>
        </div>

        <div class="cameras" v-if="cameras.length > 0">
          <div
            v-for="camera in cameras"
            :key="camera.id"
            class="camera-item"
            :class="{ active: currentCamera === camera.id && cameraRunning }"
          >
            <div class="camera-info">
              <span class="camera-icon">📷</span>
              <div class="camera-details">
                <span class="camera-name">{{ camera.name }}</span>
                <span class="camera-path">{{ camera.path }}</span>
              </div>
            </div>
            <button
              class="start-btn"
              @click="startCamera(camera.id)"
              :disabled="actionLoading || (cameraRunning && currentCamera === camera.id)"
            >
              {{ cameraRunning && currentCamera === camera.id ? 'Running' : 'Start' }}
            </button>
          </div>
        </div>

        <div class="empty-state" v-else-if="!camerasLoading">
          <p>No cameras found on this device</p>
        </div>

        <div class="loading-cameras" v-if="camerasLoading">
          <span class="spinner"></span>
          Scanning for cameras...
        </div>
      </div>

      <!-- Messages -->
      <div class="message success" v-if="successMsg">{{ successMsg }}</div>
      <div class="message error" v-if="error">{{ error }}</div>
    </div>
  </div>
</template>

<style scoped>
.camera-manager {
  max-width: 900px;
  margin: 0 auto;
}

.page-header {
  margin-bottom: 24px;
}

.page-header h2 {
  font-size: 1.5rem;
  font-weight: 600;
  margin-bottom: 4px;
}

.subtitle {
  color: #64748b;
  font-size: 0.875rem;
}

.loading {
  text-align: center;
  padding: 48px;
  color: #64748b;
}

.content {
  display: flex;
  flex-direction: column;
  gap: 20px;
}

.card {
  background: white;
  border-radius: 12px;
  padding: 20px;
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.1);
}

.card h3 {
  font-size: 1rem;
  font-weight: 600;
  margin-bottom: 16px;
}

.card-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 16px;
}

.card-header h3 {
  margin-bottom: 0;
}

.header-actions {
  display: flex;
  align-items: center;
  gap: 12px;
}

.connected-to {
  font-size: 0.75rem;
  color: #64748b;
  font-family: monospace;
}

.mode-toggle {
  padding: 6px 12px;
  background: #f1f5f9;
  border: 1px solid #e2e8f0;
  border-radius: 6px;
  cursor: pointer;
  font-size: 0.75rem;
  transition: all 0.2s;
}

.mode-toggle:hover {
  background: #e2e8f0;
}

.mode-toggle.active {
  background: #2563eb;
  color: white;
  border-color: #2563eb;
}

.direct-connect {
  padding: 16px;
  background: #f8fafc;
  border-radius: 8px;
}

.direct-hint {
  font-size: 0.875rem;
  color: #64748b;
  margin-bottom: 16px;
}

.direct-form {
  display: flex;
  gap: 12px;
  align-items: flex-end;
}

.input-group {
  display: flex;
  flex-direction: column;
  gap: 4px;
  flex: 1;
}

.input-group.port {
  flex: 0 0 100px;
}

.input-group label {
  font-size: 0.75rem;
  font-weight: 500;
  color: #475569;
}

.input-group input {
  padding: 8px 12px;
  border: 1px solid #e2e8f0;
  border-radius: 6px;
  font-size: 0.875rem;
}

.input-group input:focus {
  outline: none;
  border-color: #2563eb;
  box-shadow: 0 0 0 3px rgba(37, 99, 235, 0.1);
}

.connect-btn {
  padding: 8px 20px;
  background: #2563eb;
  color: white;
  border: none;
  border-radius: 6px;
  cursor: pointer;
  font-size: 0.875rem;
  font-weight: 500;
  white-space: nowrap;
}

.connect-btn:hover:not(:disabled) {
  background: #1d4ed8;
}

.connect-btn:disabled {
  opacity: 0.6;
  cursor: not-allowed;
}

.link-btn {
  margin-top: 8px;
  padding: 6px 12px;
  background: none;
  border: none;
  color: #2563eb;
  cursor: pointer;
  font-size: 0.875rem;
  text-decoration: underline;
}

.link-btn:hover {
  color: #1d4ed8;
}

.device-list {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
}

.device-btn {
  display: flex;
  flex-direction: column;
  align-items: flex-start;
  padding: 12px 16px;
  background: #f1f5f9;
  border: 2px solid transparent;
  border-radius: 8px;
  cursor: pointer;
  transition: all 0.2s;
}

.device-btn:hover {
  background: #e2e8f0;
}

.device-btn.active {
  background: #eff6ff;
  border-color: #2563eb;
}

.device-name {
  font-weight: 600;
  font-size: 0.875rem;
}

.device-host {
  font-size: 0.75rem;
  color: #64748b;
}

.status-bar {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 12px 16px;
  background: #f0fdf4;
  border-radius: 8px;
  margin-bottom: 16px;
  font-size: 0.875rem;
}

.status-indicator {
  width: 10px;
  height: 10px;
  border-radius: 50%;
}

.status-indicator.running {
  background: #22c55e;
  animation: pulse 2s infinite;
}

@keyframes pulse {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.5; }
}

.stop-btn {
  margin-left: auto;
  padding: 6px 16px;
  background: #ef4444;
  color: white;
  border: none;
  border-radius: 6px;
  cursor: pointer;
  font-size: 0.875rem;
  font-weight: 500;
}

.stop-btn:hover:not(:disabled) {
  background: #dc2626;
}

.stop-btn:disabled {
  opacity: 0.6;
  cursor: not-allowed;
}

.cameras {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.camera-item {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 16px;
  background: #f8fafc;
  border: 1px solid #e2e8f0;
  border-radius: 8px;
  transition: all 0.2s;
}

.camera-item:hover {
  background: #f1f5f9;
}

.camera-item.active {
  background: #f0fdf4;
  border-color: #22c55e;
}

.camera-info {
  display: flex;
  align-items: center;
  gap: 12px;
}

.camera-icon {
  font-size: 1.5rem;
}

.camera-details {
  display: flex;
  flex-direction: column;
}

.camera-name {
  font-weight: 600;
  font-size: 0.875rem;
}

.camera-path {
  font-size: 0.75rem;
  color: #64748b;
  font-family: monospace;
}

.start-btn {
  padding: 8px 20px;
  background: #2563eb;
  color: white;
  border: none;
  border-radius: 6px;
  cursor: pointer;
  font-size: 0.875rem;
  font-weight: 500;
}

.start-btn:hover:not(:disabled) {
  background: #1d4ed8;
}

.start-btn:disabled {
  opacity: 0.6;
  cursor: not-allowed;
}

.refresh-btn {
  padding: 6px 16px;
  background: #f1f5f9;
  border: 1px solid #e2e8f0;
  border-radius: 6px;
  cursor: pointer;
  font-size: 0.875rem;
}

.refresh-btn:hover:not(:disabled) {
  background: #e2e8f0;
}

.refresh-btn:disabled {
  opacity: 0.6;
  cursor: not-allowed;
}

.empty-state {
  text-align: center;
  padding: 32px;
  color: #64748b;
}

.loading-cameras {
  display: flex;
  align-items: center;
  justify-content: center;
  gap: 12px;
  padding: 32px;
  color: #64748b;
}

.spinner {
  width: 20px;
  height: 20px;
  border: 2px solid #e2e8f0;
  border-top-color: #2563eb;
  border-radius: 50%;
  animation: spin 1s linear infinite;
}

@keyframes spin {
  to { transform: rotate(360deg); }
}

.message {
  padding: 12px 16px;
  border-radius: 8px;
  font-size: 0.875rem;
}

.message.success {
  background: #f0fdf4;
  color: #166534;
  border: 1px solid #bbf7d0;
}

.message.error {
  background: #fef2f2;
  color: #991b1b;
  border: 1px solid #fecaca;
}
</style>
