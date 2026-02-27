<script setup lang="ts">
import { ref, computed, nextTick, onMounted, onUnmounted } from 'vue';

interface SavedRule {
  id: string;
  name: string;
  description: string;
  socket_type: string;                     // v3: Signal category
  reads: string[];                         // v3: Payload fields accessed
  produces: string[];                      // v3: Action types returned
  code: string;
  enabled: boolean;
  created_at: string;
  created_by: string;
  node_id: string;
  tags?: string[];
  signal_type?: string;                    // Deprecated
}

// Socket type colors for visual distinction
const SOCKET_COLORS: Record<string, string> = {
  'vision.confidence':  '#F59E0B',
  'vision.detection':   '#10B981',
  'signal.rate':        '#8B5CF6',
  'signal.threshold':   '#3B82F6',
  'system.health':      '#EF4444',
  'any.boolean':        '#6B7280',
};

function socketTypeColor(socketType: string): string {
  return SOCKET_COLORS[socketType] ?? '#6B7280';
}

interface ChatResponse {
  type: 'response' | 'error' | 'pong' | 'typing';
  content?: string;
}

// Extracted parameter from code
interface ExtractedParam {
  id: string;
  label: string;
  type: 'number' | 'string' | 'select';
  value: string | number;
  options?: string[]; // For select type
  pattern: RegExp; // Pattern to find/replace in code
  original: string; // Original matched string
}

const rules = ref<SavedRule[]>([]);
const loading = ref(true);
const error = ref<string | null>(null);
const selectedRule = ref<SavedRule | null>(null);
const saving = ref(false);

// Quick Edit state
const quickEditOpen = ref(false);
const extractedParams = ref<ExtractedParam[]>([]);
const editedParams = ref<Record<string, string | number>>({});
const quickEditSaving = ref(false);
const quickEditError = ref<string | null>(null);
const quickEditSuccess = ref(false);

// AI Modification state
const aiPanelOpen = ref(false);
const aiPrompt = ref('');
const aiSending = ref(false);
const aiConnected = ref(false);
const aiResponse = ref<string | null>(null);
const aiError = ref<string | null>(null);
const aiMessagesContainer = ref<HTMLElement | null>(null);

let ws: WebSocket | null = null;
let reconnectTimer: number | null = null;

// =====================
// Parameter Extraction
// =====================

function extractParameters(code: string): ExtractedParam[] {
  const params: ExtractedParam[] = [];
  let paramId = 0;

  // Extract numeric thresholds: > N, < N, >= N, <= N, === N, == N
  const thresholdPattern = /(\w+(?:Count|Total|Length|Size)?)\s*(>|<|>=|<=|===|==)\s*(\d+(?:\.\d+)?)/g;
  let match;
  while ((match = thresholdPattern.exec(code)) !== null) {
    const varName = match[1];
    const operator = match[2];
    const value = match[3];
    params.push({
      id: `threshold_${paramId++}`,
      label: `${formatVarName(varName)} ${operator}`,
      type: 'number',
      value: parseFloat(value),
      pattern: new RegExp(`(${escapeRegex(varName)}\\s*${escapeRegex(operator)}\\s*)${escapeRegex(value)}`),
      original: match[0],
    });
  }

  // Extract label comparisons: === 'string' or === "string"
  const labelPattern = /\.label\s*===\s*['"]([^'"]+)['"]/g;
  while ((match = labelPattern.exec(code)) !== null) {
    const value = match[1];
    params.push({
      id: `label_${paramId++}`,
      label: 'Object Label',
      type: 'string',
      value: value,
      pattern: new RegExp(`(\\.label\\s*===\\s*)['"]${escapeRegex(value)}['"]`),
      original: match[0],
    });
  }

  // Extract action type: action: "pass" / "reject" / "alert" / "log"
  const actionPattern = /action:\s*['"](\w+)['"]/g;
  while ((match = actionPattern.exec(code)) !== null) {
    const value = match[1];
    params.push({
      id: `action_${paramId++}`,
      label: 'Action',
      type: 'select',
      value: value,
      options: ['pass', 'reject', 'alert', 'log'],
      pattern: new RegExp(`(action:\\s*)['"]${escapeRegex(value)}['"]`),
      original: match[0],
    });
  }

  // Extract severity: severity: "info" / "warning" / "critical"
  const severityPattern = /severity:\s*['"](\w+)['"]/g;
  while ((match = severityPattern.exec(code)) !== null) {
    const value = match[1];
    params.push({
      id: `severity_${paramId++}`,
      label: 'Severity',
      type: 'select',
      value: value,
      options: ['info', 'warning', 'critical'],
      pattern: new RegExp(`(severity:\\s*)['"]${escapeRegex(value)}['"]`),
      original: match[0],
    });
  }

  return params;
}

function escapeRegex(str: string): string {
  return str.replace(/[.*+?^${}()|[\]\\]/g, '\\$&');
}

function formatVarName(name: string): string {
  // Convert camelCase to Title Case
  return name
    .replace(/([A-Z])/g, ' $1')
    .replace(/^./, s => s.toUpperCase())
    .trim();
}

function applyParameterChanges(code: string, params: ExtractedParam[], edited: Record<string, string | number>): string {
  let newCode = code;

  for (const param of params) {
    const newValue = edited[param.id];
    if (newValue === undefined || newValue === param.value) continue;

    // Build replacement based on type
    let replacement: string;
    if (param.type === 'number') {
      replacement = `$1${newValue}`;
    } else {
      replacement = `$1"${newValue}"`;
    }

    newCode = newCode.replace(param.pattern, replacement);
  }

  return newCode;
}

// Initialize edited params when rule changes
function initEditedParams() {
  if (!selectedRule.value) return;

  const params = extractParameters(selectedRule.value.code);
  extractedParams.value = params;

  // Initialize edited values with current values
  const edited: Record<string, string | number> = {};
  for (const param of params) {
    edited[param.id] = param.value;
  }
  editedParams.value = edited;

  quickEditError.value = null;
  quickEditSuccess.value = false;
}

// Save quick edit changes
async function saveQuickEdit() {
  if (!selectedRule.value || quickEditSaving.value) return;

  const newCode = applyParameterChanges(
    selectedRule.value.code,
    extractedParams.value,
    editedParams.value
  );

  // Check if anything changed
  if (newCode === selectedRule.value.code) {
    quickEditError.value = 'No changes to save';
    return;
  }

  quickEditSaving.value = true;
  quickEditError.value = null;
  quickEditSuccess.value = false;

  try {
    const response = await fetch(`/api/rules/${selectedRule.value.id}`, {
      method: 'PUT',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ code: newCode }),
    });

    const data = await response.json();

    if (!response.ok) {
      throw new Error(data.message || data.error || `HTTP ${response.status}`);
    }

    // Update local state
    selectedRule.value.code = newCode;
    selectedRule.value.created_by = 'manual';

    // Refresh params
    initEditedParams();

    quickEditSuccess.value = true;
    setTimeout(() => { quickEditSuccess.value = false; }, 3000);

  } catch (err) {
    quickEditError.value = err instanceof Error ? err.message : 'Failed to save changes';
  } finally {
    quickEditSaving.value = false;
  }
}

// Reset quick edit to original values
function resetQuickEdit() {
  initEditedParams();
}

// Check if there are unsaved changes
const hasQuickEditChanges = computed(() => {
  for (const param of extractedParams.value) {
    if (editedParams.value[param.id] !== param.value) {
      return true;
    }
  }
  return false;
});

// =====================
// WebSocket for AI Chat
// =====================

function getWebSocketUrl(): string {
  const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
  return `${protocol}//${window.location.host}/chat`;
}

function connectWebSocket() {
  if (ws?.readyState === WebSocket.OPEN) return;

  const url = getWebSocketUrl();
  ws = new WebSocket(url);

  ws.onopen = () => {
    aiConnected.value = true;
    if (reconnectTimer) {
      clearTimeout(reconnectTimer);
      reconnectTimer = null;
    }
  };

  ws.onclose = () => {
    aiConnected.value = false;
    aiSending.value = false;
    reconnectTimer = window.setTimeout(connectWebSocket, 3000);
  };

  ws.onerror = () => {};

  ws.onmessage = (event) => {
    try {
      const response = JSON.parse(event.data) as ChatResponse;
      handleAIResponse(response);
    } catch {
      console.error('Failed to parse WebSocket message');
    }
  };
}

function handleAIResponse(response: ChatResponse) {
  switch (response.type) {
    case 'response':
      aiSending.value = false;
      if (response.content) {
        aiResponse.value = response.content;
        setTimeout(() => {
          fetchRules().then(() => {
            if (selectedRule.value) {
              const updated = rules.value.find(r => r.id === selectedRule.value!.id);
              if (updated) {
                selectedRule.value = updated;
                initEditedParams();
              }
            }
          });
        }, 500);
      }
      scrollAIPanel();
      break;

    case 'error':
      aiSending.value = false;
      aiError.value = response.content || 'An error occurred.';
      scrollAIPanel();
      break;

    case 'typing':
      aiSending.value = true;
      break;

    case 'pong':
      break;
  }
}

function scrollAIPanel() {
  nextTick(() => {
    if (aiMessagesContainer.value) {
      aiMessagesContainer.value.scrollTop = aiMessagesContainer.value.scrollHeight;
    }
  });
}

function sendAIModification() {
  if (!aiPrompt.value.trim() || aiSending.value || !selectedRule.value) return;

  const prompt = aiPrompt.value.trim();
  aiPrompt.value = '';
  aiResponse.value = null;
  aiError.value = null;

  const contextMessage = `I want to modify the rule "${selectedRule.value.name}" (ID: ${selectedRule.value.id}).

Current rule details:
- Name: ${selectedRule.value.name}
- Description: ${selectedRule.value.description}
- Node: ${selectedRule.value.node_id}
- Tags: ${selectedRule.value.tags?.join(', ') || 'none'}

Current code:
\`\`\`javascript
${selectedRule.value.code}
\`\`\`

My modification request: ${prompt}

Please update this rule using the js_rule_create tool. Keep the same rule ID "${selectedRule.value.id}" to overwrite the existing rule.`;

  if (ws?.readyState === WebSocket.OPEN) {
    aiSending.value = true;
    ws.send(JSON.stringify({
      type: 'message',
      content: contextMessage,
    }));
  } else {
    aiError.value = 'Not connected to AI server. Reconnecting...';
    connectWebSocket();
  }
}

function handleAIKeydown(event: KeyboardEvent) {
  if (event.key === 'Enter' && !event.shiftKey) {
    event.preventDefault();
    sendAIModification();
  }
}

function toggleAIPanel() {
  aiPanelOpen.value = !aiPanelOpen.value;
  if (aiPanelOpen.value && !aiConnected.value) {
    connectWebSocket();
  }
}

function clearAIResponse() {
  aiResponse.value = null;
  aiError.value = null;
}

function renderAIContent(content: string): string {
  let html = content;
  html = html.replace(/<script\b[^<]*(?:(?!<\/script>)<[^<]*)*<\/script>/gi, '');
  html = html.replace(
    /```(\w*)\n([\s\S]*?)```/g,
    '<pre style="background:#0F172A;color:#d4d4d4;padding:12px;border-radius:6px;overflow-x:auto;font-size:12px;margin:8px 0;font-family:\'JetBrains Mono\',monospace"><code>$2</code></pre>'
  );
  html = html.replace(/`([^`]+)`/g, '<code style="background:#334155;padding:2px 6px;border-radius:4px;font-size:0.875em">$1</code>');
  return html;
}

// =====================
// Core Functions
// =====================

async function fetchRules() {
  loading.value = true;
  error.value = null;
  try {
    const response = await fetch('/api/rules');
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }
    const data = await response.json();
    rules.value = data.rules || [];
  } catch (err) {
    error.value = err instanceof Error ? err.message : 'Failed to load rules';
  } finally {
    loading.value = false;
  }
}

async function toggleRule(rule: SavedRule) {
  saving.value = true;
  try {
    const response = await fetch(`/api/rules/${rule.id}/toggle`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ enabled: !rule.enabled }),
    });
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }
    rule.enabled = !rule.enabled;
  } catch (err) {
    error.value = err instanceof Error ? err.message : 'Failed to toggle rule';
  } finally {
    saving.value = false;
  }
}

async function deleteRule(rule: SavedRule) {
  if (!confirm(`Delete rule "${rule.name}"?`)) return;

  saving.value = true;
  try {
    const response = await fetch(`/api/rules/${rule.id}`, {
      method: 'DELETE',
    });
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`);
    }
    rules.value = rules.value.filter(r => r.id !== rule.id);
    if (selectedRule.value?.id === rule.id) {
      selectedRule.value = null;
    }
  } catch (err) {
    error.value = err instanceof Error ? err.message : 'Failed to delete rule';
  } finally {
    saving.value = false;
  }
}

function selectRule(rule: SavedRule) {
  selectedRule.value = rule;
  aiResponse.value = null;
  aiError.value = null;
  initEditedParams();
}

function formatDate(dateStr: string): string {
  const date = new Date(dateStr);
  return date.toLocaleDateString([], {
    year: 'numeric',
    month: 'short',
    day: 'numeric',
    hour: '2-digit',
    minute: '2-digit',
  });
}

function getAuthor(createdBy: string): { label: string; icon: string } {
  if (createdBy === 'ai-agent') {
    return { label: 'Claude', icon: '🤖' };
  }
  return { label: 'User', icon: '👤' };
}

const sortedRules = computed(() => {
  return [...rules.value].sort((a, b) =>
    new Date(b.created_at).getTime() - new Date(a.created_at).getTime()
  );
});

const stats = computed(() => ({
  total: rules.value.length,
  enabled: rules.value.filter(r => r.enabled).length,
  byAI: rules.value.filter(r => r.created_by === 'ai-agent').length,
  byUser: rules.value.filter(r => r.created_by !== 'ai-agent').length,
}));

onMounted(() => {
  fetchRules();
});

onUnmounted(() => {
  if (reconnectTimer) clearTimeout(reconnectTimer);
  if (ws) {
    ws.close();
    ws = null;
  }
});
</script>

<template>
  <div class="rules-page">
    <header class="page-header">
      <div class="header-left">
        <h2>Rule Engine</h2>
        <span class="subtitle">Manage inspection rules</span>
      </div>
      <div class="header-stats">
        <div class="stat">
          <span class="stat-value">{{ stats.total }}</span>
          <span class="stat-label">Total</span>
        </div>
        <div class="stat enabled">
          <span class="stat-value">{{ stats.enabled }}</span>
          <span class="stat-label">Active</span>
        </div>
        <div class="stat ai">
          <span class="stat-value">{{ stats.byAI }}</span>
          <span class="stat-label">AI</span>
        </div>
        <div class="stat user">
          <span class="stat-value">{{ stats.byUser }}</span>
          <span class="stat-label">User</span>
        </div>
      </div>
    </header>

    <div class="rules-container">
      <!-- Rules List Panel -->
      <div class="rules-list-panel">
        <div class="panel-header">
          <h3>Rules</h3>
          <button class="refresh-btn" @click="fetchRules" :disabled="loading">
            {{ loading ? '...' : '↻' }}
          </button>
        </div>

        <div v-if="error" class="error-banner">{{ error }}</div>

        <div v-if="loading" class="loading">Loading rules...</div>

        <div v-else-if="rules.length === 0" class="empty-state">
          <div class="empty-icon">📋</div>
          <p>No rules yet</p>
          <p class="hint">Use the Chat to ask the AI to create rules</p>
        </div>

        <div v-else class="rules-list">
          <div
            v-for="rule in sortedRules"
            :key="rule.id"
            class="rule-item"
            :class="{ selected: selectedRule?.id === rule.id, disabled: !rule.enabled }"
            @click="selectRule(rule)"
          >
            <div class="rule-header">
              <span class="rule-author" :title="getAuthor(rule.created_by).label">
                {{ getAuthor(rule.created_by).icon }}
              </span>
              <span class="rule-name">{{ rule.name }}</span>
              <span class="rule-status" :class="{ active: rule.enabled }">
                {{ rule.enabled ? 'ON' : 'OFF' }}
              </span>
            </div>
            <div class="rule-meta">
              <span class="rule-date">{{ formatDate(rule.created_at) }}</span>
              <span v-if="rule.tags?.length" class="rule-tags">
                <span v-for="tag in rule.tags.slice(0, 2)" :key="tag" class="tag">{{ tag }}</span>
              </span>
            </div>
          </div>
        </div>
      </div>

      <!-- Rule Detail Panel -->
      <div class="rule-detail-panel">
        <div v-if="!selectedRule" class="no-selection">
          <div class="empty-icon">📝</div>
          <p>Select a rule to view details</p>
        </div>

        <div v-else class="rule-detail">
          <div class="detail-header">
            <div class="detail-title">
              <span class="author-badge" :class="selectedRule.created_by">
                {{ getAuthor(selectedRule.created_by).icon }}
                {{ getAuthor(selectedRule.created_by).label }}
              </span>
              <h3>{{ selectedRule.name }}</h3>
            </div>
            <div class="detail-actions">
              <button
                class="action-btn toggle-btn"
                :class="{ enabled: selectedRule.enabled }"
                @click="toggleRule(selectedRule)"
                :disabled="saving"
              >
                {{ selectedRule.enabled ? 'Disable' : 'Enable' }}
              </button>
              <button class="action-btn delete-btn" @click="deleteRule(selectedRule)" :disabled="saving">
                Delete
              </button>
            </div>
          </div>

          <div class="detail-description">{{ selectedRule.description }}</div>

          <div class="detail-meta">
            <span class="meta-item"><strong>ID:</strong> {{ selectedRule.id }}</span>
            <span class="meta-item"><strong>Node:</strong> {{ selectedRule.node_id }}</span>
            <span class="meta-item"><strong>Created:</strong> {{ formatDate(selectedRule.created_at) }}</span>
            <span v-if="selectedRule.signal_type" class="meta-item">
              <strong>Signal:</strong> {{ selectedRule.signal_type }}
            </span>
          </div>

          <div v-if="selectedRule.tags?.length" class="detail-tags">
            <span v-for="tag in selectedRule.tags" :key="tag" class="tag">{{ tag }}</span>
          </div>

          <!-- Quick Edit Section -->
          <div class="detail-section quick-edit-section" v-if="extractedParams.length > 0">
            <div class="section-header">
              <h4>Quick Edit</h4>
              <div class="quick-edit-actions" v-if="hasQuickEditChanges">
                <button class="reset-btn" @click="resetQuickEdit" :disabled="quickEditSaving">Reset</button>
                <button class="save-btn" @click="saveQuickEdit" :disabled="quickEditSaving">
                  {{ quickEditSaving ? 'Saving...' : 'Save' }}
                </button>
              </div>
            </div>

            <div v-if="quickEditError" class="quick-edit-error">{{ quickEditError }}</div>
            <div v-if="quickEditSuccess" class="quick-edit-success">Changes saved successfully!</div>

            <div class="params-grid">
              <div v-for="param in extractedParams" :key="param.id" class="param-field">
                <label :for="param.id">{{ param.label }}</label>

                <input
                  v-if="param.type === 'number'"
                  type="number"
                  :id="param.id"
                  v-model.number="editedParams[param.id]"
                  :class="{ changed: editedParams[param.id] !== param.value }"
                />

                <input
                  v-else-if="param.type === 'string'"
                  type="text"
                  :id="param.id"
                  v-model="editedParams[param.id]"
                  :class="{ changed: editedParams[param.id] !== param.value }"
                />

                <select
                  v-else-if="param.type === 'select'"
                  :id="param.id"
                  v-model="editedParams[param.id]"
                  :class="{ changed: editedParams[param.id] !== param.value }"
                >
                  <option v-for="opt in param.options" :key="opt" :value="opt">{{ opt }}</option>
                </select>
              </div>
            </div>
          </div>

          <!-- Socket Metadata Panel — visual rule composition in Rule Graph editor (Spec G) -->
          <div class="detail-section">
            <h4>Socket Metadata</h4>
            <div class="socket-meta-panel">
              <div class="socket-meta-row">
                <span class="socket-meta-label">Signal Type</span>
                <span
                  class="socket-type-badge"
                  :style="{ background: socketTypeColor(selectedRule.socket_type) }"
                >
                  {{ selectedRule.socket_type || 'any.boolean' }}
                </span>
              </div>

              <div class="socket-meta-row" v-if="selectedRule.reads && selectedRule.reads.length">
                <span class="socket-meta-label">Reads</span>
                <div class="socket-field-list">
                  <span
                    v-for="field in selectedRule.reads"
                    :key="field"
                    class="socket-field-tag"
                  >{{ field }}</span>
                </div>
              </div>

              <div class="socket-meta-row" v-if="selectedRule.produces && selectedRule.produces.length">
                <span class="socket-meta-label">Produces</span>
                <div class="socket-field-list">
                  <span
                    v-for="action in selectedRule.produces"
                    :key="action"
                    class="socket-action-tag"
                    :class="'action-' + action"
                  >{{ action }}</span>
                </div>
              </div>

              <div class="socket-meta-hint">
                Visual rule composition → Rule Graph editor (Spec G)
              </div>
            </div>
          </div>

          <!-- Code Viewer -->
          <div class="detail-section">
            <h4>JavaScript Code</h4>
            <div class="code-viewer">
              <pre><code>{{ selectedRule.code }}</code></pre>
            </div>
          </div>

          <!-- AI Modification Section -->
          <div class="detail-section ai-section">
            <button class="ai-toggle-btn" @click="toggleAIPanel">
              <span class="ai-icon">🤖</span>
              <span>Modify with AI</span>
              <span class="ai-status" :class="{ connected: aiConnected }">{{ aiConnected ? '●' : '○' }}</span>
              <span class="expand-icon" :class="{ expanded: aiPanelOpen }">▼</span>
            </button>

            <div class="ai-panel" :class="{ open: aiPanelOpen }">
              <div class="ai-hint">
                Ask Claude to modify this rule. Example: "Add a condition for scratch defects" or "Change action to reject"
              </div>

              <div class="ai-messages" ref="aiMessagesContainer" v-if="aiResponse || aiError || aiSending">
                <div v-if="aiError" class="ai-error">{{ aiError }}</div>
                <div v-if="aiResponse" class="ai-response">
                  <div class="ai-response-header">
                    <span>🤖 Claude</span>
                    <button class="clear-btn" @click="clearAIResponse">×</button>
                  </div>
                  <div class="ai-response-content" v-html="renderAIContent(aiResponse)"></div>
                </div>
                <div v-if="aiSending" class="ai-typing">
                  <span class="typing-dots"><span></span><span></span><span></span></span>
                  <span>Claude is thinking...</span>
                </div>
              </div>

              <div class="ai-input-area">
                <textarea
                  v-model="aiPrompt"
                  @keydown="handleAIKeydown"
                  placeholder="Describe how you want to modify this rule..."
                  rows="2"
                  :disabled="aiSending"
                ></textarea>
                <button
                  class="ai-send-btn"
                  @click="sendAIModification"
                  :disabled="!aiPrompt.trim() || aiSending || !aiConnected"
                >
                  {{ aiSending ? '...' : 'Send' }}
                </button>
              </div>
            </div>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.rules-page {
  max-width: 1400px;
  margin: 0 auto;
  height: calc(100vh - 48px);
  display: flex;
  flex-direction: column;
}

.page-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 16px;
}

.header-left h2 {
  font-size: 1.5rem;
  font-weight: 600;
  margin: 0;
}

.header-left .subtitle {
  font-size: 0.875rem;
  color: #94a3b8;
}

.header-stats {
  display: flex;
  gap: 16px;
}

.stat {
  display: flex;
  flex-direction: column;
  align-items: center;
  padding: 8px 16px;
  background: #1E293B;
  border-radius: 8px;
}

.stat-value {
  font-size: 1.25rem;
  font-weight: 600;
  color: #E2E8F0;
}

.stat-label {
  font-size: 0.75rem;
  color: #94a3b8;
}

.stat.enabled .stat-value { color: #22c55e; }
.stat.ai .stat-value { color: #6366F1; }
.stat.user .stat-value { color: #22D3EE; }

.rules-container {
  flex: 1;
  display: grid;
  grid-template-columns: 360px 1fr;
  gap: 16px;
  overflow: hidden;
}

.rules-list-panel {
  background: #1E293B;
  border-radius: 12px;
  display: flex;
  flex-direction: column;
  overflow: hidden;
}

.panel-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 16px;
  border-bottom: 1px solid #334155;
}

.panel-header h3 {
  font-size: 1rem;
  font-weight: 600;
  margin: 0;
}

.refresh-btn {
  width: 32px;
  height: 32px;
  border: none;
  background: #334155;
  color: #E2E8F0;
  border-radius: 6px;
  cursor: pointer;
  font-size: 1rem;
}

.refresh-btn:hover { background: #475569; }

.error-banner {
  padding: 12px 16px;
  background: #7f1d1d;
  color: #fca5a5;
  font-size: 0.875rem;
}

.loading, .empty-state {
  padding: 32px;
  text-align: center;
  color: #94a3b8;
}

.empty-icon {
  font-size: 2rem;
  margin-bottom: 8px;
}

.hint {
  font-size: 0.875rem;
  color: #64748b;
}

.rules-list {
  flex: 1;
  overflow-y: auto;
  padding: 8px;
}

.rule-item {
  padding: 12px;
  border-radius: 8px;
  cursor: pointer;
  margin-bottom: 4px;
  transition: background 0.2s;
}

.rule-item:hover { background: #334155; }
.rule-item.selected { background: #6366F1; }
.rule-item.disabled { opacity: 0.6; }

.rule-header {
  display: flex;
  align-items: center;
  gap: 8px;
}

.rule-author { font-size: 1rem; }

.rule-name {
  flex: 1;
  font-weight: 500;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
}

.rule-status {
  font-size: 0.625rem;
  font-weight: 600;
  padding: 2px 6px;
  border-radius: 4px;
  background: #475569;
  color: #94a3b8;
}

.rule-status.active {
  background: #166534;
  color: #86efac;
}

.rule-meta {
  display: flex;
  align-items: center;
  gap: 8px;
  margin-top: 4px;
  font-size: 0.75rem;
  color: #94a3b8;
}

.rule-item.selected .rule-meta { color: rgba(255, 255, 255, 0.7); }

.rule-tags { display: flex; gap: 4px; }

.tag {
  padding: 2px 6px;
  background: #334155;
  border-radius: 4px;
  font-size: 0.625rem;
}

.rule-item.selected .tag { background: rgba(255, 255, 255, 0.2); }

.rule-detail-panel {
  background: #1E293B;
  border-radius: 12px;
  overflow: hidden;
  display: flex;
  flex-direction: column;
}

.no-selection {
  flex: 1;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  color: #94a3b8;
}

.rule-detail {
  flex: 1;
  overflow-y: auto;
  padding: 20px;
}

.detail-header {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  margin-bottom: 16px;
}

.detail-title h3 {
  font-size: 1.25rem;
  font-weight: 600;
  margin: 8px 0 0 0;
}

.author-badge {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  padding: 4px 10px;
  border-radius: 16px;
  font-size: 0.75rem;
  font-weight: 500;
}

.author-badge.ai-agent { background: #312e81; color: #a5b4fc; }
.author-badge.manual { background: #164e63; color: #67e8f9; }

.detail-actions { display: flex; gap: 8px; }

.action-btn {
  padding: 8px 16px;
  border: none;
  border-radius: 6px;
  font-weight: 500;
  cursor: pointer;
  transition: background 0.2s;
}

.toggle-btn { background: #166534; color: white; }
.toggle-btn.enabled { background: #854d0e; }
.toggle-btn:hover { opacity: 0.9; }
.delete-btn { background: #7f1d1d; color: #fca5a5; }
.delete-btn:hover { background: #991b1b; }
.action-btn:disabled { opacity: 0.5; cursor: not-allowed; }

.detail-description {
  color: #CBD5E1;
  line-height: 1.6;
  margin-bottom: 16px;
}

.detail-meta {
  display: flex;
  flex-wrap: wrap;
  gap: 16px;
  font-size: 0.875rem;
  color: #94a3b8;
  margin-bottom: 12px;
}

.meta-item strong { color: #CBD5E1; }

.detail-tags {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
  margin-bottom: 16px;
}

.detail-tags .tag {
  padding: 4px 10px;
  background: #334155;
  border-radius: 6px;
  font-size: 0.75rem;
  color: #22D3EE;
}

.detail-section { margin-top: 20px; }

.detail-section h4 {
  font-size: 0.875rem;
  font-weight: 600;
  color: #94a3b8;
  margin: 0 0 12px 0;
  text-transform: uppercase;
  letter-spacing: 0.05em;
}

/* Quick Edit Section */
.quick-edit-section {
  background: #0F172A;
  border-radius: 8px;
  padding: 16px;
}

.section-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 12px;
}

.section-header h4 { margin: 0; }

.quick-edit-actions {
  display: flex;
  gap: 8px;
}

.reset-btn, .save-btn {
  padding: 6px 14px;
  border: none;
  border-radius: 6px;
  font-size: 0.8rem;
  font-weight: 500;
  cursor: pointer;
}

.reset-btn {
  background: #334155;
  color: #94a3b8;
}

.reset-btn:hover { background: #475569; }

.save-btn {
  background: #166534;
  color: white;
}

.save-btn:hover { background: #15803d; }
.save-btn:disabled, .reset-btn:disabled { opacity: 0.5; cursor: not-allowed; }

.quick-edit-error {
  padding: 8px 12px;
  background: #7f1d1d;
  color: #fca5a5;
  border-radius: 6px;
  font-size: 0.8rem;
  margin-bottom: 12px;
}

.quick-edit-success {
  padding: 8px 12px;
  background: #166534;
  color: #86efac;
  border-radius: 6px;
  font-size: 0.8rem;
  margin-bottom: 12px;
}

.params-grid {
  display: grid;
  grid-template-columns: repeat(auto-fill, minmax(180px, 1fr));
  gap: 12px;
}

.param-field {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.param-field label {
  font-size: 0.75rem;
  color: #94a3b8;
  font-weight: 500;
}

.param-field input,
.param-field select {
  padding: 8px 12px;
  border: 1px solid #334155;
  border-radius: 6px;
  background: #1E293B;
  color: #E2E8F0;
  font-size: 0.9rem;
  font-family: 'JetBrains Mono', monospace;
}

.param-field input:focus,
.param-field select:focus {
  outline: none;
  border-color: #6366F1;
}

.param-field input.changed,
.param-field select.changed {
  border-color: #22D3EE;
  background: #164e63;
}

/* Socket Metadata Panel */
.socket-meta-panel {
  padding: 16px;
  background: #0F172A;
  border-radius: 8px;
  border: 1px solid #334155;
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.socket-meta-row {
  display: flex;
  align-items: flex-start;
  gap: 12px;
}

.socket-meta-label {
  font-size: 11px;
  font-weight: 600;
  text-transform: uppercase;
  letter-spacing: 0.05em;
  color: #64748b;
  min-width: 80px;
  padding-top: 3px;
}

.socket-type-badge {
  font-size: 12px;
  font-weight: 600;
  color: #ffffff;
  padding: 3px 10px;
  border-radius: 12px;
  letter-spacing: 0.02em;
}

.socket-field-list {
  display: flex;
  flex-wrap: wrap;
  gap: 6px;
}

.socket-field-tag {
  font-size: 11px;
  font-family: 'JetBrains Mono', Consolas, monospace;
  color: #94a3b8;
  background: #1E293B;
  border: 1px solid #334155;
  padding: 2px 8px;
  border-radius: 4px;
}

.socket-action-tag {
  font-size: 11px;
  font-weight: 600;
  padding: 2px 10px;
  border-radius: 4px;
  text-transform: uppercase;
  letter-spacing: 0.05em;
}

.action-pass    { background: #14532d; color: #4ade80; }
.action-reject  { background: #450a0a; color: #f87171; }
.action-alert   { background: #451a03; color: #fb923c; }
.action-log     { background: #1e1b4b; color: #818cf8; }
.action-modbus_write { background: #042f2e; color: #2dd4bf; }

.socket-meta-hint {
  font-size: 11px;
  color: #475569;
  font-style: italic;
  padding-top: 4px;
  border-top: 1px solid #1E293B;
}

.code-viewer {
  background: #0F172A;
  border-radius: 8px;
  overflow: hidden;
}

.code-viewer pre {
  margin: 0;
  padding: 16px;
  overflow-x: auto;
  font-family: 'JetBrains Mono', 'Fira Code', monospace;
  font-size: 13px;
  line-height: 1.6;
}

.code-viewer code {
  color: #E2E8F0;
  white-space: pre;
}

/* AI Section */
.ai-section {
  margin-top: 24px;
  border-top: 1px solid #334155;
  padding-top: 20px;
}

.ai-toggle-btn {
  display: flex;
  align-items: center;
  gap: 8px;
  width: 100%;
  padding: 12px 16px;
  background: linear-gradient(135deg, #312e81 0%, #1e1b4b 100%);
  border: 1px solid #4338ca;
  border-radius: 8px;
  color: #a5b4fc;
  font-size: 0.9rem;
  font-weight: 500;
  cursor: pointer;
  transition: all 0.2s;
}

.ai-toggle-btn:hover {
  background: linear-gradient(135deg, #3730a3 0%, #312e81 100%);
  border-color: #6366f1;
}

.ai-icon { font-size: 1.25rem; }
.ai-status { margin-left: auto; font-size: 0.75rem; }
.ai-status.connected { color: #22c55e; }
.expand-icon { font-size: 0.75rem; transition: transform 0.2s; }
.expand-icon.expanded { transform: rotate(180deg); }

.ai-panel {
  max-height: 0;
  overflow: hidden;
  transition: max-height 0.3s ease-out;
}

.ai-panel.open { max-height: 500px; }

.ai-hint {
  padding: 12px 16px;
  font-size: 0.8rem;
  color: #94a3b8;
  background: #0F172A;
  border-radius: 8px;
  margin-top: 12px;
}

.ai-messages {
  margin-top: 12px;
  max-height: 200px;
  overflow-y: auto;
}

.ai-error {
  padding: 12px 16px;
  background: #7f1d1d;
  color: #fca5a5;
  border-radius: 8px;
  font-size: 0.875rem;
}

.ai-response {
  background: #0F172A;
  border-radius: 8px;
  overflow: hidden;
}

.ai-response-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 10px 16px;
  background: #1e1b4b;
  color: #a5b4fc;
  font-size: 0.8rem;
  font-weight: 500;
}

.clear-btn {
  background: none;
  border: none;
  color: #94a3b8;
  font-size: 1.25rem;
  cursor: pointer;
  padding: 0 4px;
}

.clear-btn:hover { color: #E2E8F0; }

.ai-response-content {
  padding: 12px 16px;
  font-size: 0.875rem;
  line-height: 1.6;
  color: #CBD5E1;
  white-space: pre-wrap;
}

.ai-typing {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 12px 16px;
  color: #94a3b8;
  font-size: 0.875rem;
}

.typing-dots { display: flex; gap: 4px; }

.typing-dots span {
  width: 6px;
  height: 6px;
  border-radius: 50%;
  background: #6366F1;
  animation: bounce 1.4s infinite ease-in-out both;
}

.typing-dots span:nth-child(1) { animation-delay: -0.32s; }
.typing-dots span:nth-child(2) { animation-delay: -0.16s; }

@keyframes bounce {
  0%, 80%, 100% { transform: scale(0); }
  40% { transform: scale(1); }
}

.ai-input-area {
  display: flex;
  gap: 8px;
  margin-top: 12px;
}

.ai-input-area textarea {
  flex: 1;
  padding: 10px 14px;
  border: 1px solid #334155;
  border-radius: 8px;
  resize: none;
  font-family: inherit;
  font-size: 0.875rem;
  line-height: 1.5;
  background: #0F172A;
  color: #E2E8F0;
}

.ai-input-area textarea:focus {
  outline: none;
  border-color: #6366F1;
}

.ai-input-area textarea::placeholder { color: #64748b; }

.ai-send-btn {
  padding: 10px 20px;
  background: #6366F1;
  color: white;
  border: none;
  border-radius: 8px;
  cursor: pointer;
  font-weight: 500;
  white-space: nowrap;
}

.ai-send-btn:hover { background: #4f46e5; }
.ai-send-btn:disabled { background: #475569; cursor: not-allowed; }
</style>
