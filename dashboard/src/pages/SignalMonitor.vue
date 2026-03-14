<script setup lang="ts">
import { ref, onMounted, onUnmounted, computed, watch } from 'vue';
import SignalChannelCard from '../components/SignalChannelCard.vue';
import SignalResultBadge from '../components/SignalResultBadge.vue';

interface Node {
  id: string;
  name: string;
  status: 'online' | 'offline' | 'error' | 'unknown';
  host: string;
}

interface SignalChannel {
  name: string;
  rms: number;
  peak: number;
  mean: number;
  sample_count?: number;
  window_ready?: boolean;
}

interface SignalChannelInfo {
  loaded: boolean;
  num_channels: number;
  window_size: number;
  sample_rate_hz: number;
  channels: string[];
}

interface SignalStats {
  name: string;
  rms: number;
  peak: number;
  mean: number;
}

interface SignalData {
  loaded: boolean;
  num_channels: number;
  window_size: number;
  sample_rate_hz: number;
  channels: SignalChannel[];
  last_result: string | null;
}

interface SlotInfo {
  slot: string;
  loaded: boolean;
  format: string | null;
  name: string | null;
  labels: string[];
  inputType: string | null;
}

const nodes = ref<Node[]>([]);
const selectedNodeId = ref<string | null>(null);
const signalData = ref<SignalData | null>(null);
const slotInfo = ref<SlotInfo | null>(null);
const loading = ref(true);
const error = ref<string | null>(null);

let pollTimer: number | null = null;

// Computed: get online nodes
const onlineNodes = computed(() => nodes.value.filter(n => n.status === 'online'));

// Computed: selected node details
const selectedNode = computed(() => nodes.value.find(n => n.id === selectedNodeId.value));

// Computed: signal channels as array
const channels = computed(() => {
  if (!signalData.value?.channels) return [];
  return signalData.value.channels;
});

onMounted(async () => {
  await fetchNodes();

  // Auto-select first online node
  if (onlineNodes.value.length > 0 && !selectedNodeId.value) {
    selectedNodeId.value = onlineNodes.value[0].id;
  }

  loading.value = false;

  // Start polling for signal data
  pollTimer = window.setInterval(fetchSignalData, 2000);
});

onUnmounted(() => {
  if (pollTimer) {
    clearInterval(pollTimer);
    pollTimer = null;
  }
});

// Watch for node selection changes
watch(selectedNodeId, async (newId) => {
  if (newId) {
    signalData.value = null;
    slotInfo.value = null;
    await fetchSlotInfo();
    await fetchSignalData();
  }
});

async function fetchNodes() {
  try {
    const response = await fetch('/api/nodes', {
      signal: AbortSignal.timeout(2000)
    });
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }
    const data = await response.json();
    nodes.value = data.nodes || [];
  } catch (e) {
    error.value = e instanceof Error ? e.message : 'Failed to fetch nodes';
  }
}

async function fetchSlotInfo() {
  if (!selectedNodeId.value) return;

  try {
    const node = selectedNode.value;
    if (!node || node.status !== 'online') return;

    const response = await fetch(`/api/nodes/${selectedNodeId.value}/slots`, {
      signal: AbortSignal.timeout(2000)
    });

    if (response.ok) {
      const data = await response.json();
      // Find the SIGNAL slot from the slots array
      const signalSlot = data.slots?.find((s: SlotInfo) => s.slot === 'SIGNAL');
      slotInfo.value = signalSlot || null;
    } else {
      slotInfo.value = null;
    }
  } catch (e) {
    // Silent fail - slot info is optional
    slotInfo.value = null;
  }
}

async function fetchSignalData() {
  if (!selectedNodeId.value) return;

  const node = selectedNode.value;
  if (!node || node.status !== 'online') return;

  try {
    // Fetch all signal data in parallel
    const [channelsRes, statsRes, resultRes] = await Promise.all([
      fetch(`/api/nodes/${selectedNodeId.value}/signal/channels`, {
        signal: AbortSignal.timeout(2000)
      }).catch(() => null),
      fetch(`/api/nodes/${selectedNodeId.value}/signals`, {
        signal: AbortSignal.timeout(2000)
      }).catch(() => null),
      fetch(`/api/nodes/${selectedNodeId.value}/signal/result`, {
        signal: AbortSignal.timeout(2000)
      }).catch(() => null),
    ]);

    // Parse channel info
    let channelInfo: SignalChannelInfo = { loaded: false, num_channels: 0, window_size: 0, sample_rate_hz: 0, channels: [] };
    if (channelsRes?.ok) {
      channelInfo = await channelsRes.json();
    }

    // Parse channel stats
    let stats: SignalStats[] = [];
    if (statsRes?.ok) {
      stats = await statsRes.json();
    }

    // Parse inference result
    let lastResult: string | null = null;
    if (resultRes?.ok) {
      const resultData = await resultRes.json();
      // The result endpoint returns the raw JSON string, stringify it if needed
      lastResult = typeof resultData === 'string' ? resultData : JSON.stringify(resultData);
    }

    // Combine into SignalData
    const channels: SignalChannel[] = channelInfo.channels.map((name: string) => {
      const stat = stats.find((s) => s.name === name);
      return {
        name,
        rms: stat?.rms ?? 0,
        peak: stat?.peak ?? 0,
        mean: stat?.mean ?? 0,
      };
    });

    signalData.value = {
      loaded: channelInfo.loaded,
      num_channels: channelInfo.num_channels,
      window_size: channelInfo.window_size,
      sample_rate_hz: channelInfo.sample_rate_hz,
      channels,
      last_result: lastResult,
    };
  } catch (e) {
    // Silent fail on polling - don't show error on every poll
  }
}

function formatHz(hz: number): string {
  if (hz >= 1000) return `${(hz / 1000).toFixed(1)} kHz`;
  return `${hz} Hz`;
}
</script>

<template>
  <div class="signal-monitor">
    <header class="page-header">
      <h2>Signal Monitor</h2>
      <div class="node-selector">
        <label>Target Device:</label>
        <select v-model="selectedNodeId" :disabled="onlineNodes.length === 0">
          <option v-if="onlineNodes.length === 0" value="">No online devices</option>
          <option v-for="node in onlineNodes" :key="node.id" :value="node.id">
            {{ node.name }} ({{ node.host }})
          </option>
        </select>
      </div>
    </header>

    <!-- Loading State -->
    <div class="loading" v-if="loading">Loading...</div>

    <!-- Error State -->
    <div class="error-message" v-else-if="error">
      <p>{{ error }}</p>
      <button @click="fetchNodes">Try Again</button>
    </div>

    <!-- No Online Nodes -->
    <div class="empty-state" v-else-if="onlineNodes.length === 0">
      <div class="empty-icon">📡</div>
      <h3>No Online Devices</h3>
      <p>No edge devices are currently online. Start a CiRA runtime to begin monitoring signals.</p>
    </div>

    <!-- Main Content -->
    <div class="content" v-else-if="selectedNode">
      <!-- Signal Model Section -->
      <section class="section model-section">
        <div class="section-header">
          <h3>Signal Model</h3>
          <span class="slot-badge" :class="slotInfo?.loaded ? 'loaded' : 'empty'">
            {{ slotInfo?.loaded ? 'LOADED' : 'EMPTY' }}
          </span>
        </div>

        <div class="model-info" v-if="slotInfo?.loaded">
          <div class="model-row">
            <span class="label">Model Name</span>
            <span class="value">{{ slotInfo.name || 'Unknown' }}</span>
          </div>
          <div class="model-row">
            <span class="label">Format</span>
            <span class="value mono">{{ slotInfo.format }}</span>
          </div>
          <div class="model-row" v-if="slotInfo.inputType">
            <span class="label">Input Type</span>
            <span class="value">{{ slotInfo.inputType }}</span>
          </div>
          <div class="model-row" v-if="slotInfo.labels && slotInfo.labels.length > 0">
            <span class="label">Classes</span>
            <span class="value">{{ slotInfo.labels.join(', ') }}</span>
          </div>
        </div>

        <div class="model-empty" v-else>
          <p>No signal model loaded on this device.</p>
          <p class="hint">Load a signal model via Device Detail page or POST /api/model?slot=signal</p>
        </div>
      </section>

      <!-- Signal Buffer Section -->
      <section class="section buffer-section" v-if="signalData?.loaded">
        <div class="section-header">
          <h3>Signal Buffer</h3>
          <div class="buffer-stats">
            <span class="stat">
              <span class="stat-label">Window Size</span>
              <span class="stat-value">{{ signalData.window_size }}</span>
            </span>
            <span class="stat">
              <span class="stat-label">Sample Rate</span>
              <span class="stat-value">{{ formatHz(signalData.sample_rate_hz) }}</span>
            </span>
            <span class="stat">
              <span class="stat-label">Channels</span>
              <span class="stat-value">{{ signalData.num_channels }}</span>
            </span>
          </div>
        </div>

        <!-- Channels Grid -->
        <div class="channels-grid">
          <SignalChannelCard
            v-for="channel in channels"
            :key="channel.name"
            :name="channel.name"
            :rms="channel.rms"
            :peak="channel.peak"
            :mean="channel.mean"
            :windowReady="channel.window_ready"
            :sampleCount="channel.sample_count"
          />
        </div>
      </section>

      <!-- No Signal Buffer -->
      <section class="section buffer-section" v-else>
        <div class="section-header">
          <h3>Signal Buffer</h3>
        </div>
        <div class="buffer-empty">
          <p>No signal buffer active on this device.</p>
          <p class="hint">Signal buffers are created when a signal model is loaded with compatible input_type.</p>
        </div>
      </section>

      <!-- Inference Results Section -->
      <section class="section results-section">
        <div class="section-header">
          <h3>Last Inference Result</h3>
        </div>

        <div class="result-container" v-if="signalData?.loaded">
          <SignalResultBadge
            :resultJson="signalData.last_result"
            :timestamp="null"
          />
        </div>

        <div class="result-empty" v-else>
          <p>No inference results available.</p>
        </div>
      </section>
    </div>
  </div>
</template>

<style scoped>
.signal-monitor {
  max-width: 1400px;
  margin: 0 auto;
}

.page-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 24px;
  flex-wrap: wrap;
  gap: 16px;
}

.page-header h2 {
  font-size: 1.5rem;
  font-weight: 600;
  color: #E2E8F0;
}

.node-selector {
  display: flex;
  align-items: center;
  gap: 12px;
}

.node-selector label {
  font-size: 0.875rem;
  color: #94A3B8;
}

.node-selector select {
  padding: 8px 12px;
  border: 1px solid #334155;
  border-radius: 6px;
  background: #1E293B;
  color: #E2E8F0;
  font-size: 0.875rem;
  min-width: 200px;
  cursor: pointer;
}

.node-selector select:focus {
  outline: none;
  border-color: #6366F1;
}

.node-selector select:disabled {
  background: #334155;
  cursor: not-allowed;
}

.loading {
  text-align: center;
  padding: 60px 20px;
  color: #94A3B8;
}

.error-message {
  background: rgba(239, 68, 68, 0.1);
  border: 1px solid #EF4444;
  color: #EF4444;
  padding: 20px;
  border-radius: 8px;
  text-align: center;
}

.error-message button {
  margin-top: 12px;
  padding: 8px 16px;
  background: #EF4444;
  color: white;
  border: none;
  border-radius: 6px;
  cursor: pointer;
}

.empty-state {
  text-align: center;
  padding: 80px 20px;
  color: #64748B;
}

.empty-icon {
  font-size: 4rem;
  margin-bottom: 16px;
}

.empty-state h3 {
  font-size: 1.25rem;
  color: #94A3B8;
  margin-bottom: 8px;
}

.content {
  display: flex;
  flex-direction: column;
  gap: 24px;
}

.section {
  background: #1E293B;
  border: 1px solid #334155;
  border-radius: 12px;
  padding: 20px;
}

.section-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 16px;
  flex-wrap: wrap;
  gap: 12px;
}

.section-header h3 {
  font-size: 1rem;
  font-weight: 600;
  color: #E2E8F0;
  text-transform: uppercase;
  letter-spacing: 0.05em;
}

.slot-badge {
  padding: 4px 12px;
  border-radius: 4px;
  font-size: 0.75rem;
  font-weight: 700;
  text-transform: uppercase;
}

.slot-badge.loaded {
  background: rgba(16, 185, 129, 0.2);
  color: #10B981;
}

.slot-badge.empty {
  background: rgba(148, 163, 184, 0.2);
  color: #94A3B8;
}

.model-info {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.model-row {
  display: flex;
  justify-content: space-between;
  padding: 8px 0;
  border-bottom: 1px solid #334155;
}

.model-row:last-child {
  border-bottom: none;
}

.model-row .label {
  color: #64748B;
  font-size: 0.875rem;
}

.model-row .value {
  color: #E2E8F0;
  font-size: 0.875rem;
}

.model-row .value.mono {
  font-family: 'SF Mono', 'Monaco', 'Consolas', monospace;
}

.model-empty,
.buffer-empty,
.result-empty {
  text-align: center;
  padding: 24px;
  color: #64748B;
}

.hint {
  font-size: 0.75rem;
  color: #475569;
  margin-top: 8px;
}

.buffer-stats {
  display: flex;
  gap: 24px;
}

.stat {
  display: flex;
  flex-direction: column;
  gap: 2px;
}

.stat-label {
  font-size: 0.625rem;
  font-weight: 600;
  color: #64748B;
  text-transform: uppercase;
}

.stat-value {
  font-size: 0.875rem;
  font-weight: 500;
  color: #22D3EE;
  font-family: 'SF Mono', 'Monaco', 'Consolas', monospace;
}

.channels-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(240px, 1fr));
  gap: 16px;
}

.result-container {
  background: #0F172A;
  border-radius: 8px;
  padding: 16px;
}

@media (max-width: 768px) {
  .page-header {
    flex-direction: column;
    align-items: flex-start;
  }

  .node-selector {
    width: 100%;
  }

  .node-selector select {
    flex: 1;
  }

  .buffer-stats {
    flex-wrap: wrap;
    gap: 16px;
  }

  .channels-grid {
    grid-template-columns: 1fr;
  }
}
</style>
