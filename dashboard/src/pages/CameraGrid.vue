<script setup lang="ts">
import { ref, onMounted, onUnmounted, computed } from 'vue';

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

const nodes = ref<Node[]>([]);
const loading = ref(true);
const gridSize = ref<'2x2' | '3x3'>('2x2');

// Track stream reload keys to force refresh on error
const streamKeys = ref<Record<string, number>>({});

let refreshInterval: number | null = null;

const onlineNodes = computed(() =>
  nodes.value.filter(n => n.status === 'online')
);

onMounted(async () => {
  await fetchNodes();
  // Refresh node status every 10 seconds
  refreshInterval = window.setInterval(fetchNodes, 10000);
});

onUnmounted(() => {
  if (refreshInterval) {
    clearInterval(refreshInterval);
  }
});

async function fetchNodes() {
  try {
    if (nodes.value.length === 0) {
      loading.value = true;
    }
    const response = await fetch('/api/nodes');
    if (!response.ok) throw new Error('Failed to fetch');
    const data = await response.json();
    nodes.value = data.nodes;

    // Initialize stream keys for new nodes
    for (const node of data.nodes) {
      if (!(node.id in streamKeys.value)) {
        streamKeys.value[node.id] = Date.now();
      }
    }
  } catch (e) {
    console.error('Failed to load nodes:', e);
  } finally {
    loading.value = false;
  }
}

function getStreamUrl(node: Node): string {
  const key = streamKeys.value[node.id] || Date.now();
  return `http://${node.host}:${node.runtime?.port || 8080}/stream/annotated?_t=${key}`;
}

// Handle image load error - reload after a delay
function handleStreamError(nodeId: string) {
  console.log(`Stream error for ${nodeId}, will retry...`);
  setTimeout(() => {
    streamKeys.value[nodeId] = Date.now();
  }, 2000);
}

// Force refresh a specific stream
function refreshStream(nodeId: string) {
  streamKeys.value[nodeId] = Date.now();
}
</script>

<template>
  <div class="camera-grid-page">
    <header class="page-header">
      <h2>All Cameras</h2>
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
        :key="node.id + '-' + streamKeys[node.id]"
      >
        <img
          :src="getStreamUrl(node)"
          :alt="node.name"
          class="camera-feed"
          @error="handleStreamError(node.id)"
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

.grid-controls {
  display: flex;
  gap: 4px;
  background: #e2e8f0;
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
  color: #64748b;
}

.grid-controls button.active {
  background: white;
  color: #1e293b;
  box-shadow: 0 1px 2px rgba(0, 0, 0, 0.1);
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
  background: #1e293b;
  border-radius: 12px;
  overflow: hidden;
  aspect-ratio: 16/9;
}

.camera-feed {
  width: 100%;
  height: 100%;
  object-fit: cover;
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
