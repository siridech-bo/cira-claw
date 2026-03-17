<script setup lang="ts">
import { ref, onMounted, onUnmounted, computed, watch } from 'vue';
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

interface SourceStatus {
  id: string;
  protocol: 'modbus_client' | 'mqtt_subscribe';
  enabled: boolean;
  connected: boolean;
  validation_status: 'ok' | 'mismatch' | 'pending' | 'disabled';
  mismatched_channels: string[];
  samples_fed: number;
  batches_sent: number;
  last_feed_at: string | null;
  error: string | null;
}

interface ChannelDef {
  name: string;
  // MODBUS fields
  register?: number;
  scale?: number;
  offset?: number;
  // MQTT fields
  json_path?: string;
}

const nodes = ref<Node[]>([]);
const selectedNodeId = ref<string | null>(null);
const signalData = ref<SignalData | null>(null);
const slotInfo = ref<SlotInfo | null>(null);
const loading = ref(true);
const error = ref<string | null>(null);

// Sources state
const sources = ref<SourceStatus[]>([])
const sourcesLoading = ref(false)
const showAddForm = ref(false)
const addError = ref<string | null>(null)
const addSuccess = ref<string | null>(null)
const testingId = ref<string | null>(null)
const testResult = ref<Record<string, { status: string; error?: string }>>({})

// Add form state
const form = ref({
  id: '',
  protocol: 'modbus_client' as 'modbus_client' | 'mqtt_subscribe',
  node_id: '',
  batch_size: 64,
  // MODBUS fields
  host: '',
  port: 502,
  poll_interval_ms: 100,
  // MQTT fields
  topic: '',
  // channels (start with one empty)
  channels: [{ name: '', register: 0, scale: 0.001, offset: 0, json_path: '' }] as ChannelDef[]
})

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
  await fetchSources();

  // Auto-select first online node
  if (onlineNodes.value.length > 0 && !selectedNodeId.value) {
    selectedNodeId.value = onlineNodes.value[0].id;
  }

  loading.value = false;

  // Start polling for signal data and sources
  pollTimer = window.setInterval(async () => {
    await fetchSignalData();
    await fetchSources();
  }, 2000);
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
    form.value.node_id = newId;
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
      // Find the signal slot from the slots array (runtime returns lowercase)
      const signalSlot = data.slots?.find((s: SlotInfo) => s.slot === 'signal');
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

function formatVal(v: number): string {
  if (v === 0) return '—';
  if (Math.abs(v) < 0.001) return v.toExponential(2);
  return v.toFixed(4);
}

async function fetchSources() {
  try {
    const r = await fetch('/api/signal-sources', { signal: AbortSignal.timeout(2000) })
    if (r.ok) {
      const data = await r.json()
      sources.value = data.sources || []
    }
  } catch { /* silent */ }
}

async function deleteSource(id: string) {
  if (!confirm(`Remove source '${id}'?`)) return
  try {
    const r = await fetch(`/api/signal-sources/${id}`, { method: 'DELETE', signal: AbortSignal.timeout(3000) })
    if (r.ok) await fetchSources()
  } catch { /* silent */ }
}

async function testSource(id: string) {
  testingId.value = id
  testResult.value[id] = { status: 'testing' }
  try {
    const r = await fetch(`/api/signal-sources/${id}/test`, {
      method: 'POST',
      signal: AbortSignal.timeout(5000)
    })
    if (r.ok) {
      const data = await r.json()
      testResult.value[id] = {
        status: data.validation_status,
        error: data.mismatched_channels?.length
          ? `Mismatch: ${data.mismatched_channels.join(', ')}`
          : data.error
      }
    } else {
      testResult.value[id] = { status: 'error', error: `HTTP ${r.status}` }
    }
  } catch (e) {
    testResult.value[id] = { status: 'error', error: e instanceof Error ? e.message : 'Failed' }
  } finally {
    testingId.value = null
  }
}

function addChannel() {
  form.value.channels.push({ name: '', register: 0, scale: 0.001, offset: 0, json_path: '' })
}

function removeChannel(idx: number) {
  form.value.channels.splice(idx, 1)
}

function resetForm() {
  form.value = {
    id: '', protocol: 'modbus_client', node_id: selectedNodeId.value || '',
    batch_size: 64, host: '', port: 502, poll_interval_ms: 100, topic: '',
    channels: [{ name: '', register: 0, scale: 0.001, offset: 0, json_path: '' }]
  }
  addError.value = null
  addSuccess.value = null
}

async function submitSource() {
  addError.value = null
  addSuccess.value = null
  sourcesLoading.value = true

  // Basic validation
  if (!form.value.id.trim()) { addError.value = 'ID is required'; sourcesLoading.value = false; return }
  if (!form.value.node_id) { addError.value = 'Node is required'; sourcesLoading.value = false; return }
  if (form.value.channels.length === 0) { addError.value = 'At least one channel required'; sourcesLoading.value = false; return }
  if (form.value.channels.some(c => !c.name.trim())) { addError.value = 'All channels need a name'; sourcesLoading.value = false; return }
  if (form.value.protocol === 'modbus_client' && !form.value.host.trim()) { addError.value = 'Host is required for MODBUS'; sourcesLoading.value = false; return }
  if (form.value.protocol === 'mqtt_subscribe' && !form.value.topic.trim()) { addError.value = 'Topic is required for MQTT'; sourcesLoading.value = false; return }

  // Build payload
  const payload: Record<string, unknown> = {
    id: form.value.id.trim(),
    protocol: form.value.protocol,
    node_id: form.value.node_id,
    batch_size: form.value.batch_size,
    enabled: true,
    channels: form.value.channels.map(ch => {
      if (form.value.protocol === 'modbus_client') {
        return { name: ch.name.trim(), register: Number(ch.register), scale: Number(ch.scale), offset: Number(ch.offset) }
      } else {
        return { name: ch.name.trim(), json_path: ch.json_path?.trim() || ch.name.trim() }
      }
    })
  }
  if (form.value.protocol === 'modbus_client') {
    payload.host = form.value.host.trim()
    payload.port = Number(form.value.port)
    payload.poll_interval_ms = Number(form.value.poll_interval_ms)
  } else {
    payload.topic = form.value.topic.trim()
    payload.payload_format = 'json'
  }

  try {
    const r = await fetch('/api/signal-sources', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload),
      signal: AbortSignal.timeout(5000)
    })
    const data = await r.json()
    if (r.ok && data.success) {
      addSuccess.value = `Source '${payload.id}' saved`
      showAddForm.value = false
      resetForm()
      await fetchSources()
    } else {
      addError.value = data.message || data.error || 'Save failed'
    }
  } catch (e) {
    addError.value = e instanceof Error ? e.message : 'Request failed'
  } finally {
    sourcesLoading.value = false
  }
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

        <!-- Feature Stats Table -->
        <table class="channel-table" v-if="channels.length > 0">
          <thead>
            <tr>
              <th>Channel</th>
              <th>RMS</th>
              <th>Peak</th>
              <th>Mean</th>
              <th>Buffer</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="ch in channels" :key="ch.name">
              <td class="ch-name">{{ ch.name }}</td>
              <td class="ch-value">{{ formatVal(ch.rms) }}</td>
              <td class="ch-value">{{ formatVal(ch.peak) }}</td>
              <td class="ch-value">{{ formatVal(ch.mean) }}</td>
              <td>
                <span class="ready-dot" :class="ch.window_ready ? 'ready' : 'waiting'">
                  {{ ch.window_ready ? '● Ready' : '○ Filling' }}
                </span>
              </td>
            </tr>
          </tbody>
        </table>
        <div class="muted" v-else>Waiting for signal data...</div>
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

      <!-- Signal Sources Section -->
      <section class="section sources-section">
        <div class="section-header">
          <h3>Signal Sources</h3>
          <button class="add-btn" @click="showAddForm = !showAddForm">
            {{ showAddForm ? '✕ Cancel' : '+ Add Source' }}
          </button>
        </div>

        <!-- Sources list -->
        <div v-if="sources.length === 0 && !showAddForm" class="muted">
          No signal sources configured. Click "+ Add Source" to connect MODBUS or MQTT.
        </div>

        <div class="source-list">
          <div v-for="src in sources" :key="src.id" class="source-row">
            <div class="source-left">
              <span class="source-protocol" :class="src.protocol === 'modbus_client' ? 'modbus' : 'mqtt'">
                {{ src.protocol === 'modbus_client' ? 'MODBUS' : 'MQTT' }}
              </span>
              <span class="source-id">{{ src.id }}</span>
              <span class="source-detail">{{ src.samples_fed ?? 0 }} samples</span>
              <span v-if="src.last_feed_at" class="source-detail">
                · last {{ new Date(src.last_feed_at).toLocaleTimeString() }}
              </span>
            </div>

            <div class="source-right">
              <!-- Validation status -->
              <span class="val-badge" :class="src.validation_status">
                {{ src.validation_status === 'ok' ? '● ok'
                 : src.validation_status === 'mismatch' ? '⚠ mismatch'
                 : src.validation_status === 'pending' ? '◌ pending'
                 : '✕ disabled' }}
              </span>

              <!-- Connected indicator -->
              <span class="conn-dot" :class="src.connected ? 'connected' : 'disconnected'">
                {{ src.connected ? '● live' : '○ off' }}
              </span>

              <!-- Test result inline -->
              <span v-if="testResult[src.id]" class="test-result"
                :class="testResult[src.id].status === 'ok' ? 'ok' : 'err'">
                {{ testResult[src.id].status === 'ok' ? '✓ ok'
                 : testResult[src.id].status === 'testing' ? '…'
                 : '✗ ' + testResult[src.id].error }}
              </span>

              <!-- Error tooltip -->
              <span v-if="src.error && src.validation_status !== 'ok'" class="source-error" :title="src.error">⚠</span>

              <button class="icon-btn" @click="testSource(src.id)"
                :disabled="testingId === src.id" title="Test connection">
                {{ testingId === src.id ? '…' : '⟳' }}
              </button>
              <button class="icon-btn danger" @click="deleteSource(src.id)" title="Remove source">✕</button>
            </div>
          </div>
        </div>

        <!-- Add form -->
        <div v-if="showAddForm" class="add-form">
          <div class="form-row">
            <div class="field">
              <label>Protocol</label>
              <select v-model="form.protocol" @change="resetForm(); form.protocol = ($event.target as HTMLSelectElement).value as 'modbus_client' | 'mqtt_subscribe'">
                <option value="modbus_client">MODBUS TCP</option>
                <option value="mqtt_subscribe">MQTT Subscribe</option>
              </select>
            </div>
            <div class="field">
              <label>Source ID</label>
              <input v-model="form.id" type="text" placeholder="e.g. plc-line1" />
            </div>
            <div class="field">
              <label>Node</label>
              <select v-model="form.node_id">
                <option value="">Select node...</option>
                <option v-for="node in nodes" :key="node.id" :value="node.id">{{ node.name }}</option>
              </select>
            </div>
            <div class="field field-sm">
              <label>Batch Size</label>
              <input v-model.number="form.batch_size" type="number" min="1" max="512" />
            </div>
          </div>

          <!-- MODBUS fields -->
          <div class="form-row" v-if="form.protocol === 'modbus_client'">
            <div class="field field-lg">
              <label>Host</label>
              <input v-model="form.host" type="text" placeholder="192.168.1.127" />
            </div>
            <div class="field field-sm">
              <label>Port</label>
              <input v-model.number="form.port" type="number" min="1" max="65535" />
            </div>
            <div class="field field-sm">
              <label>Poll (ms)</label>
              <input v-model.number="form.poll_interval_ms" type="number" min="10" max="10000" />
            </div>
          </div>

          <!-- MQTT fields -->
          <div class="form-row" v-if="form.protocol === 'mqtt_subscribe'">
            <div class="field field-xl">
              <label>Topic</label>
              <input v-model="form.topic" type="text" placeholder="cira/sensors/system" />
            </div>
          </div>

          <!-- Channels -->
          <div class="channels-editor">
            <div class="channels-header">
              <span class="field-label">Channels</span>
              <button class="add-ch-btn" @click="addChannel" :disabled="form.channels.length >= 16">
                + Add Channel
              </button>
            </div>

            <div class="channel-row" v-for="(ch, idx) in form.channels" :key="idx">
              <div class="field field-sm">
                <input v-model="ch.name" type="text" placeholder="name" />
              </div>

              <!-- MODBUS channel fields -->
              <template v-if="form.protocol === 'modbus_client'">
                <div class="field field-xs">
                  <input v-model.number="ch.register" type="number" min="0" placeholder="reg" title="Register address" />
                </div>
                <div class="field field-xs">
                  <input v-model.number="ch.scale" type="number" step="0.001" placeholder="scale" title="Scale factor (value = raw × scale)" />
                </div>
                <div class="field field-xs">
                  <input v-model.number="ch.offset" type="number" step="0.1" placeholder="offset" title="Offset (value = raw × scale + offset)" />
                </div>
              </template>

              <!-- MQTT channel fields -->
              <template v-if="form.protocol === 'mqtt_subscribe'">
                <div class="field field-lg">
                  <input v-model="ch.json_path" type="text" placeholder="json_path (e.g. cpu_temp)" title="Dot-notation path in JSON payload" />
                </div>
              </template>

              <button class="icon-btn danger" @click="removeChannel(idx)"
                :disabled="form.channels.length === 1" title="Remove channel">✕</button>
            </div>
          </div>

          <!-- Form footer -->
          <div class="form-footer">
            <div class="form-error" v-if="addError">{{ addError }}</div>
            <div class="form-success" v-if="addSuccess">{{ addSuccess }}</div>
            <div class="form-actions">
              <button class="cancel-btn" @click="showAddForm = false; resetForm()">Cancel</button>
              <button class="save-btn" @click="submitSource" :disabled="sourcesLoading">
                {{ sourcesLoading ? 'Saving...' : 'Save & Reload' }}
              </button>
            </div>
          </div>
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

.result-container {
  background: #0F172A;
  border-radius: 8px;
  padding: 16px;
}

/* Channel Stats Table */
.channel-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 0.875rem;
}
.channel-table th {
  text-align: left;
  padding: 8px 12px;
  color: #64748B;
  font-size: 0.75rem;
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.04em;
  border-bottom: 1px solid #334155;
}
.channel-table td {
  padding: 10px 12px;
  border-bottom: 1px solid #1E293B;
  color: #E2E8F0;
}
.channel-table tbody tr:last-child td { border-bottom: none; }
.channel-table tbody tr:hover { background: rgba(99,102,241,0.05); }
.ch-name {
  font-family: 'JetBrains Mono', monospace;
  color: #22D3EE;
  font-weight: 500;
}
.ch-value {
  font-family: 'JetBrains Mono', monospace;
  font-size: 0.8125rem;
}
.ready-dot { font-size: 0.75rem; font-weight: 600; }
.ready-dot.ready  { color: #10B981; }
.ready-dot.waiting { color: #F59E0B; }
.muted { color: #64748B; font-size: 0.875rem; padding: 12px 0; }

/* Sources section */
.source-list { display: flex; flex-direction: column; gap: 8px; margin-bottom: 12px; }
.source-row {
  display: flex; justify-content: space-between; align-items: center;
  padding: 10px 14px; background: #0F172A; border-radius: 8px;
  border: 1px solid #1E293B; gap: 12px; flex-wrap: wrap;
}
.source-left { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }
.source-right { display: flex; align-items: center; gap: 8px; flex-shrink: 0; }
.source-protocol {
  padding: 2px 8px; border-radius: 4px; font-size: 0.65rem;
  font-weight: 700; text-transform: uppercase;
}
.source-protocol.modbus { background: #1e3a5f; color: #22D3EE; }
.source-protocol.mqtt   { background: #2d1b69; color: #A78BFA; }
.source-id { font-family: monospace; font-size: 0.875rem; color: #E2E8F0; font-weight: 500; }
.source-detail { font-size: 0.75rem; color: #64748B; }
.source-error { color: #F59E0B; cursor: help; }
.val-badge { font-size: 0.7rem; font-weight: 600; }
.val-badge.ok       { color: #10B981; }
.val-badge.mismatch { color: #EF4444; }
.val-badge.pending  { color: #F59E0B; }
.val-badge.disabled { color: #64748B; }
.conn-dot { font-size: 0.7rem; }
.conn-dot.connected    { color: #10B981; }
.conn-dot.disconnected { color: #475569; }
.test-result { font-size: 0.7rem; font-weight: 600; }
.test-result.ok  { color: #10B981; }
.test-result.err { color: #EF4444; }
.add-btn {
  padding: 6px 14px; background: #6366F1; color: white;
  border: none; border-radius: 6px; cursor: pointer; font-size: 0.8125rem;
}
.add-btn:hover { background: #4F46E5; }
.icon-btn {
  width: 28px; height: 28px; background: #1E293B; border: 1px solid #334155;
  border-radius: 4px; cursor: pointer; color: #94A3B8; font-size: 0.875rem;
  display: flex; align-items: center; justify-content: center;
}
.icon-btn:hover { background: #334155; color: #E2E8F0; }
.icon-btn.danger:hover { background: rgba(239,68,68,0.15); color: #EF4444; border-color: #EF4444; }
.icon-btn:disabled { opacity: 0.4; cursor: not-allowed; }

/* Add form */
.add-form {
  margin-top: 16px; padding: 16px; background: #0F172A;
  border-radius: 8px; border: 1px solid #334155;
  display: flex; flex-direction: column; gap: 12px;
}
.form-row { display: flex; gap: 12px; flex-wrap: wrap; align-items: flex-end; }
.field { display: flex; flex-direction: column; gap: 4px; flex: 1; min-width: 120px; }
.field-sm  { flex: 0 0 90px; }
.field-xs  { flex: 0 0 75px; }
.field-lg  { flex: 0 0 200px; }
.field-xl  { flex: 1; min-width: 200px; }
.field label, .field-label {
  font-size: 0.7rem; font-weight: 600; color: #64748B; text-transform: uppercase;
}
.field input, .field select {
  padding: 7px 10px; background: #1E293B; border: 1px solid #334155;
  border-radius: 5px; color: #E2E8F0; font-size: 0.8125rem;
}
.field input:focus, .field select:focus {
  outline: none; border-color: #6366F1;
}
.field input::placeholder { color: #475569; }
.channels-editor { display: flex; flex-direction: column; gap: 8px; }
.channels-header { display: flex; justify-content: space-between; align-items: center; }
.add-ch-btn {
  padding: 4px 10px; background: transparent; border: 1px solid #334155;
  border-radius: 4px; color: #94A3B8; cursor: pointer; font-size: 0.75rem;
}
.add-ch-btn:hover { border-color: #6366F1; color: #6366F1; }
.add-ch-btn:disabled { opacity: 0.3; cursor: not-allowed; }
.channel-row { display: flex; gap: 8px; align-items: flex-end; }
.channel-row .field input { font-family: monospace; font-size: 0.8rem; }
.form-footer { display: flex; flex-direction: column; gap: 8px; }
.form-actions { display: flex; justify-content: flex-end; gap: 10px; }
.cancel-btn {
  padding: 8px 18px; background: transparent; border: 1px solid #334155;
  border-radius: 6px; color: #94A3B8; cursor: pointer; font-size: 0.875rem;
}
.cancel-btn:hover { border-color: #94A3B8; color: #E2E8F0; }
.save-btn {
  padding: 8px 18px; background: #6366F1; color: white;
  border: none; border-radius: 6px; cursor: pointer; font-size: 0.875rem; font-weight: 500;
}
.save-btn:hover:not(:disabled) { background: #4F46E5; }
.save-btn:disabled { background: #334155; color: #64748B; cursor: not-allowed; }
.form-error { color: #EF4444; font-size: 0.8125rem; }
.form-success { color: #10B981; font-size: 0.8125rem; }

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

  .channel-table {
    font-size: 0.8rem;
  }

  .channel-table th,
  .channel-table td {
    padding: 8px 8px;
  }

  .source-row {
    flex-direction: column;
    align-items: flex-start;
  }

  .source-right {
    width: 100%;
    justify-content: flex-end;
    margin-top: 8px;
  }
}
</style>
