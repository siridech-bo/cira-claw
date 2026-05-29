<script setup lang="ts">
import { ref, onMounted, onUnmounted, computed } from 'vue';
import CameraStream from '../components/CameraStream.vue';

interface Node {
  id: string;
  name: string;
  host: string;
  status: string;
  runtime?: {
    port: number;
  };
  metrics?: {
    fps: number | null;
  };
  inference?: {
    defectsPerHour: number;
  };
}

interface NodeStreamInfo {
  mjpeg: string;
  raw_mjpeg: string;
  websocket?: string;
  webrtc?: string;
  raw_webrtc?: string;
}

const nodes = ref<Node[]>([]);
const nodeStreams = ref<Map<string, NodeStreamInfo>>(new Map());
const go2rtcEnabled = ref(false);
const loading = ref(true);
const gridSize = ref<'2x2' | '3x3'>('2x2');
const streamMode = ref<'auto' | 'webrtc' | 'mjpeg'>('auto');

// Track stream component refs for refresh
const streamRefs = ref<Record<string, InstanceType<typeof CameraStream> | null>>({});

let refreshInterval: number | null = null;
let pageRefreshTimer: number | null = null;
let wsConnection: WebSocket | null = null;
const showRefreshWarning = ref(false);
const reloadCountdown = ref(0);

// Client-side fallback timer (35 minutes - longer than server's 25 min cycle)
const CLIENT_FALLBACK_REFRESH_MS = 35 * 60 * 1000;

const onlineNodes = computed(() =>
  nodes.value.filter(n => n.status === 'online')
);

// Connect to WebSocket for server-triggered reloads
function connectWebSocket() {
  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  const wsUrl = `${protocol}//${window.location.host}/ws`;

  wsConnection = new WebSocket(wsUrl);

  wsConnection.onopen = () => {
    console.log('WebSocket connected for reload signals');
  };

  wsConnection.onmessage = (event) => {
    try {
      const msg = JSON.parse(event.data);

      if (msg.type === 'page:reload_warning') {
        // Server is warning us about upcoming reload
        showRefreshWarning.value = true;
        reloadCountdown.value = msg.payload.secondsUntilReload;

        // Start countdown
        const countdownInterval = setInterval(() => {
          reloadCountdown.value--;
          if (reloadCountdown.value <= 0) {
            clearInterval(countdownInterval);
          }
        }, 1000);
      } else if (msg.type === 'page:reload') {
        // Server triggered immediate reload
        console.log('Server triggered page reload:', msg.payload.reason);
        window.location.reload();
      }
    } catch (e) {
      // Ignore non-JSON messages
    }
  };

  wsConnection.onclose = () => {
    console.log('WebSocket disconnected, reconnecting in 5s...');
    setTimeout(connectWebSocket, 5000);
  };

  wsConnection.onerror = (err) => {
    console.error('WebSocket error:', err);
  };
}

// Client-side fallback timer (in case WebSocket fails)
function startFallbackTimer() {
  if (pageRefreshTimer) {
    clearTimeout(pageRefreshTimer);
  }

  pageRefreshTimer = window.setTimeout(() => {
    showRefreshWarning.value = true;
    reloadCountdown.value = 30;

    const countdownInterval = setInterval(() => {
      reloadCountdown.value--;
      if (reloadCountdown.value <= 0) {
        clearInterval(countdownInterval);
        if (showRefreshWarning.value) {
          window.location.reload();
        }
      }
    }, 1000);
  }, CLIENT_FALLBACK_REFRESH_MS - 30000);
}

// Stop timers
function stopTimers() {
  if (pageRefreshTimer) {
    clearTimeout(pageRefreshTimer);
    pageRefreshTimer = null;
  }
  if (wsConnection) {
    wsConnection.close();
    wsConnection = null;
  }
  showRefreshWarning.value = false;
}

onMounted(async () => {
  await fetchNodes();
  // Refresh node status every 10 seconds
  refreshInterval = window.setInterval(fetchNodes, 10000);

  // Connect WebSocket for server-triggered reloads
  connectWebSocket();

  // Client-side fallback timer (35 min, in case WebSocket fails)
  startFallbackTimer();
});

onUnmounted(() => {
  if (refreshInterval) {
    clearInterval(refreshInterval);
  }
  stopTimers();
});


// Dismiss refresh warning and postpone
function dismissRefreshWarning() {
  showRefreshWarning.value = false;
  reloadCountdown.value = 0;
  // Restart fallback timer
  startFallbackTimer();
}


// Manual refresh now
function refreshPageNow() {
  window.location.reload();
}

async function fetchNodes() {
  try {
    if (nodes.value.length === 0) {
      loading.value = true;
    }
    const response = await fetch('/api/nodes');
    if (!response.ok) throw new Error('Failed to fetch');
    const data = await response.json();
    nodes.value = data.nodes;

    // Fetch stream URLs for online nodes (includes WebRTC URLs if go2rtc is enabled)
    const onlineNodeIds = data.nodes
      .filter((n: Node) => n.status === 'online')
      .map((n: Node) => n.id);

    await Promise.all(onlineNodeIds.map(fetchStreamUrls));
  } catch (e) {
    console.error('Failed to load nodes:', e);
  } finally {
    loading.value = false;
  }
}

// Fetch stream URLs for a specific node (includes WebRTC if go2rtc enabled)
async function fetchStreamUrls(nodeId: string) {
  try {
    const response = await fetch(`/api/nodes/${nodeId}/stream`);
    if (!response.ok) return;

    const data = await response.json();
    nodeStreams.value.set(nodeId, data.streams);

    // Track if go2rtc is enabled (from any node response)
    if (data.go2rtcEnabled) {
      go2rtcEnabled.value = true;
    }
  } catch (e) {
    console.error(`Failed to fetch stream URLs for ${nodeId}:`, e);
  }
}

// Get WebRTC URL for a node (if available)
function getWebRTCUrl(nodeId: string): string | undefined {
  return nodeStreams.value.get(nodeId)?.webrtc;
}

// Force refresh a specific stream
function refreshStream(nodeId: string) {
  const streamRef = streamRefs.value[nodeId];
  if (streamRef) {
    streamRef.refresh();
  }
}

// Handle stream error
function handleStreamError(nodeId: string, msg: string) {
  console.error(`Stream error for ${nodeId}: ${msg}`);
}
</script>

<template>
  <div class="camera-grid-page">
    <!-- Memory refresh warning -->
    <div class="refresh-warning" v-if="showRefreshWarning">
      <span>
        Page will auto-refresh for memory stability
        <strong v-if="reloadCountdown > 0"> ({{ reloadCountdown }}s)</strong>
      </span>
      <div class="refresh-actions">
        <button class="postpone-btn" @click="dismissRefreshWarning">Postpone</button>
        <button class="refresh-now-btn" @click="refreshPageNow">Refresh Now</button>
      </div>
    </div>

    <header class="page-header">
      <h2>All Cameras</h2>
      <div class="header-controls">
        <div class="mode-selector">
          <label>Mode:</label>
          <select v-model="streamMode">
            <option value="auto">Auto{{ go2rtcEnabled ? ' (WebRTC)' : '' }}</option>
            <option value="webrtc" v-if="go2rtcEnabled">WebRTC</option>
            <option value="mjpeg">MJPEG</option>
          </select>
        </div>
        <div class="grid-controls">
          <button
            :class="{ active: gridSize === '2x2' }"
            @click="gridSize = '2x2'"
          >
            2×2
          </button>
          <button
            :class="{ active: gridSize === '3x3' }"
            @click="gridSize = '3x3'"
          >
            3×3
          </button>
        </div>
      </div>
    </header>

    <div class="loading" v-if="loading">Loading cameras...</div>

    <div
      class="camera-grid"
      :class="gridSize"
      v-else-if="onlineNodes.length > 0"
    >
      <div
        class="camera-cell"
        v-for="node in onlineNodes"
        :key="node.id"
      >
        <CameraStream
          :ref="(el: any) => { streamRefs[node.id] = el }"
          :host="node.host"
          :port="node.runtime?.port || 8080"
          :annotated="true"
          :mode="streamMode"
          :webrtc-url="getWebRTCUrl(node.id)"
          @error="handleStreamError(node.id, $event)"
        />
        <div class="camera-overlay">
          <div class="camera-name">{{ node.name }}</div>
          <div class="camera-stats">
            <span v-if="node.metrics?.fps">FPS: {{ node.metrics.fps }}</span>
            <span v-if="node.inference">D: {{ node.inference.defectsPerHour }}/hr</span>
          </div>
          <button class="refresh-btn" @click="refreshStream(node.id)" title="Refresh stream">
            ↻
          </button>
        </div>
      </div>
    </div>

    <div class="empty-state" v-else>
      <p>No online cameras available.</p>
      <p>Make sure your devices are connected and running.</p>
    </div>
  </div>
</template>

<style scoped>
.camera-grid-page {
  max-width: 1600px;
  margin: 0 auto;
}

.refresh-warning {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 12px 16px;
  margin-bottom: 16px;
  background: rgba(251, 146, 60, 0.15);
  border: 1px solid #FB923C;
  border-radius: 8px;
  color: #FB923C;
  font-size: 0.875rem;
}

.refresh-actions {
  display: flex;
  gap: 8px;
}

.postpone-btn {
  padding: 6px 12px;
  background: transparent;
  border: 1px solid #FB923C;
  border-radius: 6px;
  color: #FB923C;
  font-size: 0.75rem;
  cursor: pointer;
  transition: all 0.2s;
}

.postpone-btn:hover {
  background: rgba(251, 146, 60, 0.2);
}

.refresh-now-btn {
  padding: 6px 12px;
  background: #FB923C;
  border: none;
  border-radius: 6px;
  color: white;
  font-size: 0.75rem;
  cursor: pointer;
  transition: all 0.2s;
}

.refresh-now-btn:hover {
  background: #F97316;
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

.header-controls {
  display: flex;
  align-items: center;
  gap: 16px;
}

.mode-selector {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 0.875rem;
  color: #94A3B8;
}

.mode-selector select {
  padding: 6px 12px;
  border: 1px solid #334155;
  border-radius: 6px;
  background: #1E293B;
  color: #E2E8F0;
  font-size: 0.875rem;
  cursor: pointer;
}

.mode-selector select:focus {
  outline: none;
  border-color: #6366F1;
}

.grid-controls {
  display: flex;
  gap: 4px;
  background: #1E293B;
  padding: 4px;
  border-radius: 8px;
}

.grid-controls button {
  padding: 8px 16px;
  border: none;
  background: transparent;
  border-radius: 6px;
  cursor: pointer;
  font-size: 0.875rem;
  color: #E2E8F0;
  transition: background 0.2s;
}

.grid-controls button:hover:not(.active) {
  background: #334155;
}

.grid-controls button.active {
  background: #6366F1;
  color: white;
  box-shadow: 0 1px 2px rgba(0, 0, 0, 0.2);
}

.loading {
  text-align: center;
  padding: 60px;
  color: #64748b;
}

.camera-grid {
  display: grid;
  gap: 16px;
}

.camera-grid.2x2 {
  grid-template-columns: repeat(2, 1fr);
}

.camera-grid.3x3 {
  grid-template-columns: repeat(3, 1fr);
}

.camera-cell {
  position: relative;
  background: #0F172A;
  border-radius: 12px;
  overflow: hidden;
  aspect-ratio: 16/9;
}

.camera-overlay {
  position: absolute;
  bottom: 0;
  left: 0;
  right: 0;
  padding: 12px;
  background: linear-gradient(transparent, rgba(0, 0, 0, 0.7));
  color: white;
}

.camera-name {
  font-weight: 600;
  font-size: 0.875rem;
}

.camera-stats {
  display: flex;
  gap: 12px;
  font-size: 0.75rem;
  opacity: 0.8;
  margin-top: 4px;
}

.refresh-btn {
  position: absolute;
  top: 8px;
  right: 8px;
  width: 28px;
  height: 28px;
  border: none;
  border-radius: 50%;
  background: rgba(255, 255, 255, 0.2);
  color: white;
  font-size: 14px;
  cursor: pointer;
  opacity: 0;
  transition: opacity 0.2s;
}

.camera-cell:hover .refresh-btn {
  opacity: 1;
}

.refresh-btn:hover {
  background: rgba(255, 255, 255, 0.4);
}

.empty-state {
  text-align: center;
  padding: 80px 20px;
  color: #64748b;
}

.empty-state p:first-child {
  font-size: 1.125rem;
  margin-bottom: 8px;
}

@media (max-width: 1024px) {
  .camera-grid.3x3 {
    grid-template-columns: repeat(2, 1fr);
  }
}

@media (max-width: 640px) {
  .camera-grid.2x2,
  .camera-grid.3x3 {
    grid-template-columns: 1fr;
  }
}
</style>
