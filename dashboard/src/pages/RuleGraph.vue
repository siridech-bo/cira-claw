<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted, watch, nextTick } from 'vue';
import { SOCKET_TYPE_COLORS } from '@gateway/socket-registry';
import RuleGraphSidebar from '../components/rule-graph/RuleGraphSidebar.vue';
import { useRuleEditor, createReteEditor } from '../composables/useRuleEditor';
import { useRuleEvaluation } from '../composables/useRuleEvaluation';
import type { CompositeNode, CompositeConnection } from '../composables/types';
import type { NodeType } from '../composables/sockets';

const {
  compositeRules,
  atomicRules,
  currentRule,
  currentRuleId,
  loading,
  error,
  dirty,
  loadAll,
  saveCompositeRule,
  deleteCompositeRule,
  selectRule,
  createNewRule,
  updateCurrentRule,
  addNode,
  updateNode,
  removeNode,
  undoDeleteNode,
  canUndo,
  clearUndoStack,
  addConnection,
  removeConnection,
  generateNodeId,
  generateConnectionId,
  setReteEditor,
  syncRuleToEditor,
  zoomToFit,
  reteArea,
} = useRuleEditor();

const { startPolling, stopPolling } = useRuleEvaluation();

const selectedNodeId = ref<string | null>(null);
const panelOpen = ref(false);
const atomicSearch = ref('');

// Save button status
const saveStatus = ref<'idle' | 'saving' | 'saved' | 'error'>('idle');
let saveStatusTimer: ReturnType<typeof setTimeout> | null = null;

// Inline rule name editing
const editingRuleName = ref(false);
const ruleNameInput = ref('');
const ruleNameInputRef = ref<HTMLInputElement | null>(null);

// Filter atomic rules by search text
const filteredAtomicRules = computed(() => {
  const enabled = atomicRules.value.filter(r => r.enabled);
  if (!atomicSearch.value.trim()) return enabled;
  const search = atomicSearch.value.toLowerCase();
  return enabled.filter(r => r.name.toLowerCase().includes(search));
});

const selectedNode = computed(() => {
  if (!currentRule.value || !selectedNodeId.value) return null;
  return currentRule.value.nodes.find(n => n.id === selectedNodeId.value) || null;
});

// Rete.js canvas ref
const canvasRef = ref<HTMLElement | null>(null);
let reteDestroy: (() => void) | null = null;

// Guard against concurrent initialization
let _initializingEditor = false;

// Update visual selection state on node elements
// Uses Rete's nodeViews API to get the actual DOM element (not DOM queries)
function updateNodeSelectionVisual(oldId: string | null, newId: string | null): void {
  const area = reteArea.value;
  if (!area) {
    return;
  }

  // Remove selection from old node
  if (oldId) {
    const oldNodeView = area.nodeViews.get(oldId);
    if (oldNodeView?.element) {
      const el = oldNodeView.element as HTMLElement;
      el.classList.remove('rete-node-selected');
      el.style.outline = '';
      el.style.outlineOffset = '';
      el.style.boxShadow = '';
    }
  }

  // Add selection to new node
  if (newId) {
    const newNodeView = area.nodeViews.get(newId);

    if (newNodeView?.element) {
      const el = newNodeView.element as HTMLElement;
      el.classList.add('rete-node-selected');
      // Set inline style for immediate visual feedback
      el.style.outline = '3px solid #22D3EE';
      el.style.outlineOffset = '3px';
      el.style.boxShadow = '0 0 20px 4px rgba(34, 211, 238, 0.6)';
      el.style.borderRadius = '12px';
    }
  }
}

// Initialize Rete editor when canvas becomes available
async function initReteEditor() {
  // Prevent double initialization
  if (_initializingEditor) {
    return;
  }
  _initializingEditor = true;

  try {
    // Destroy previous editor if exists
    if (reteDestroy) {
      reteDestroy();
      reteDestroy = null;
      setReteEditor(null, null);
    }

    // Wait for DOM to update - more aggressive waiting
    await nextTick();
    await nextTick();
    await new Promise<void>(resolve => requestAnimationFrame(() => resolve()));
    await new Promise<void>(resolve => setTimeout(resolve, 50));

    if (!canvasRef.value) {
      return;
    }

    const container = canvasRef.value;

    // Wait for container to have non-zero dimensions (up to 2 seconds)
    for (let i = 0; i < 40; i++) {
      if (container.clientWidth > 0 && container.clientHeight > 0) break;
      await new Promise<void>(resolve => setTimeout(resolve, 50));
    }

    if (container.clientWidth === 0 || container.clientHeight === 0) {
      return;
    }

    const { editor, area, destroy } = await createReteEditor(container);
    setReteEditor(editor, area);
    reteDestroy = destroy;

    // Handle node selection when clicked
    area.addPipe((context) => {
      if (context.type === 'nodepicked') {
        const newId = context.data.id;
        // Update visual selection
        updateNodeSelectionVisual(selectedNodeId.value, newId);
        selectedNodeId.value = newId;
      }
      return context;
    });

    // Sync node positions back to Vue state when dragged
    area.addPipe((context) => {
      if (context.type === 'nodetranslated') {
        updateNode(context.data.id, {
          position: {
            x: context.data.position.x,
            y: context.data.position.y,
          },
        });
      }
      return context;
    });

    // Sync existing nodes from the rule to the editor
    await syncRuleToEditor();

    // Additional delayed zoom to ensure all nodes are visible
    setTimeout(async () => {
      await zoomToFit();
    }, 300);
  } catch (err) {
    console.error('[RuleGraph] Editor initialization failed:', err);
  } finally {
    _initializingEditor = false;
  }
}

// NOTE: Removed watch on currentRuleId to prevent double initialization.
// Editor initialization is now explicitly called in onSelectRule, onCreateRule, and onMounted.

// Lifecycle
onMounted(async () => {
  await loadAll();
  startPolling(2000);
  // Add keyboard shortcuts
  window.addEventListener('keydown', handleKeyDown);

  // If a rule was already selected (e.g., returning to this page), reinitialize the editor
  if (currentRuleId.value) {
    await nextTick();
    await initReteEditor();
  }
});

onUnmounted(() => {
  stopPolling();
  // Remove keyboard shortcuts
  window.removeEventListener('keydown', handleKeyDown);
  // MANDATORY — leaks memory if skipped
  reteDestroy?.();
  setReteEditor(null, null);
});

// Watch for programmatic selection changes (e.g., clicking close button on sidebar)
watch(selectedNodeId, (newId, oldId) => {
  updateNodeSelectionVisual(oldId, newId);
});

// Rule list operations
async function onSelectRule(id: string) {
  selectRule(id);
  selectedNodeId.value = null;
  clearUndoStack(); // Clear undo stack when switching rules
  // Explicitly initialize editor after selecting a rule
  await nextTick();
  await initReteEditor();
}

// Select rule from panel and close panel after editor is ready
async function onSelectRuleAndClosePanel(id: string) {
  // Close panel first to ensure canvas has full width
  panelOpen.value = false;
  // Wait for panel to close and layout to settle
  await nextTick();
  await new Promise<void>(resolve => setTimeout(resolve, 100));
  // Now select the rule and initialize editor
  await onSelectRule(id);
}

async function onCreateRule() {
  createNewRule();
  selectedNodeId.value = null;
  clearUndoStack();
  // Initialize Rete editor for the new rule
  await nextTick();
  await initReteEditor();
}

async function onSaveRule() {
  if (!currentRule.value) return;

  // Clear any pending status timer
  if (saveStatusTimer) {
    clearTimeout(saveStatusTimer);
    saveStatusTimer = null;
  }

  saveStatus.value = 'saving';

  try {
    await saveCompositeRule(currentRule.value);
    saveStatus.value = 'saved';
    // Reset to idle after 2 seconds
    saveStatusTimer = setTimeout(() => {
      saveStatus.value = 'idle';
    }, 2000);
  } catch {
    saveStatus.value = 'error';
    // Reset to idle after 3 seconds
    saveStatusTimer = setTimeout(() => {
      saveStatus.value = 'idle';
    }, 3000);
  }
}

async function onDeleteRule() {
  if (!currentRuleId.value) return;

  if (confirm('Are you sure you want to delete this rule?')) {
    await deleteCompositeRule(currentRuleId.value);
  }
}

// Node operations
function onSelectNode(nodeId: string | null) {
  selectedNodeId.value = nodeId;
}

async function onDeleteNode(nodeId: string) {
  await removeNode(nodeId);
  if (selectedNodeId.value === nodeId) {
    selectedNodeId.value = null;
  }
}

// Delete selected node
async function onDeleteSelectedNode() {
  if (selectedNodeId.value) {
    await onDeleteNode(selectedNodeId.value);
  }
}

// Undo last deleted node
async function onUndoDelete() {
  const restoredNode = await undoDeleteNode();
  if (restoredNode) {
    selectedNodeId.value = restoredNode.id;
  }
}

function onMoveNode(nodeId: string, x: number, y: number) {
  updateNode(nodeId, { position: { x, y } });
}

// Zoom to fit all nodes in view
async function onZoomToFit() {
  await zoomToFit();
}

// Keyboard shortcuts
function handleKeyDown(e: KeyboardEvent) {
  // Don't trigger shortcuts when typing in input fields
  const target = e.target as HTMLElement;
  if (target.tagName === 'INPUT' || target.tagName === 'TEXTAREA' || target.isContentEditable) {
    return;
  }

  // Delete key - delete selected node
  if (e.key === 'Delete' || e.key === 'Backspace') {
    if (selectedNodeId.value && currentRule.value) {
      e.preventDefault();
      onDeleteSelectedNode();
    }
  }

  // Ctrl+Z or Cmd+Z - undo delete
  if ((e.ctrlKey || e.metaKey) && e.key === 'z' && !e.shiftKey) {
    if (canUndo() && currentRule.value) {
      e.preventDefault();
      onUndoDelete();
    }
  }
}

async function onAddNode(type: NodeType, data?: unknown) {
  if (!currentRule.value) return;

  // Build appropriate data for each node type
  let nodeData: CompositeNode['data'];

  if (data) {
    // Data was provided (from sidebar config or atomic rule selection)
    nodeData = data as CompositeNode['data'];
  } else {
    // No data provided - use defaults based on type
    switch (type) {
      case 'and':
      case 'or':
      case 'not':
        nodeData = { gate_type: type };
        break;

      case 'atomic': {
        // Should not happen - atomic rules are added via onAddAtomicRule
        alert('Please select an atomic rule from the list.');
        return;
      }

      case 'output':
        nodeData = { action: 'alert', severity: 'warning' };
        break;

      case 'stateful_condition':
        nodeData = {
          condition: 'count_window',
          accepts_socket_type: 'any.boolean',
          count: 3,
          window_minutes: 5,
        };
        break;

      default:
        nodeData = { gate_type: type };
    }
  }

  const node: CompositeNode = {
    id: generateNodeId(),
    type: type as CompositeNode['type'],
    position: { x: 150 + Math.random() * 300, y: 100 + Math.random() * 200 },
    data: nodeData,
  };

  await addNode(node);
}

// Add a specific atomic rule node
async function onAddAtomicRule(rule: { id: string; name: string; socket_type?: string }) {
  if (!currentRule.value) return;

  const nodeData = {
    rule_id: rule.id,
    label: rule.name,
    socket_type: rule.socket_type || 'any.boolean',
  };

  const node: CompositeNode = {
    id: generateNodeId(),
    type: 'atomic' as CompositeNode['type'],
    position: { x: 150 + Math.random() * 300, y: 100 + Math.random() * 200 },
    data: nodeData,
  };

  await addNode(node);
}

function onUpdateNode(nodeId: string, updates: Partial<CompositeNode>) {
  updateNode(nodeId, updates);
}

function onUpdateRule(updates: { name?: string; description?: string; enabled?: boolean }) {
  updateCurrentRule(updates);
}

// Inline rule name editing
async function startEditingRuleName() {
  if (!currentRule.value) return;
  ruleNameInput.value = currentRule.value.name;
  editingRuleName.value = true;
  await nextTick();
  ruleNameInputRef.value?.focus();
  ruleNameInputRef.value?.select();
}

function confirmRuleName() {
  if (ruleNameInput.value.trim()) {
    onUpdateRule({ name: ruleNameInput.value.trim() });
  }
  editingRuleName.value = false;
}

function cancelEditingRuleName() {
  editingRuleName.value = false;
}

function handleRuleNameKeydown(e: KeyboardEvent) {
  if (e.key === 'Enter') {
    e.preventDefault();
    confirmRuleName();
  } else if (e.key === 'Escape') {
    e.preventDefault();
    cancelEditingRuleName();
  }
}

// Connection operations
function onAddConnection(source: string, sourceSocket: string, target: string, targetSocket: string) {
  const connection: CompositeConnection = {
    id: generateConnectionId(),
    source_node: source,
    source_socket: sourceSocket,
    target_node: target,
    target_socket: targetSocket,
  };
  addConnection(connection);
}

function onDeleteConnection(connectionId: string) {
  removeConnection(connectionId);
}
</script>

<template>
  <div class="rg-page">

    <!-- ── Toolbar ─────────────────────────────────────────────────────── -->
    <div class="rg-toolbar">
      <div class="rg-toolbar-left">
        <button class="rg-btn rg-btn-ghost" @click="panelOpen = !panelOpen" title="Toggle rule list">
          ☰
        </button>
        <span class="rg-title">Rule Graph</span>
        <span class="rg-divider">·</span>
        <template v-if="currentRule">
          <input
            v-if="editingRuleName"
            ref="ruleNameInputRef"
            v-model="ruleNameInput"
            type="text"
            class="rg-rule-name-input"
            @blur="confirmRuleName"
            @keydown="handleRuleNameKeydown"
          />
          <span
            v-else
            class="rg-rule-name rg-rule-name--editable"
            @click="startEditingRuleName"
            title="Click to rename"
          >
            {{ currentRule.name }}<span v-if="dirty" class="rg-dirty"> *</span>
          </span>
        </template>
        <span v-else class="rg-no-rule">No rule selected</span>
        <!-- Enable/disable toggle -->
        <label v-if="currentRule" class="rg-toggle" :class="{ 'rg-toggle--on': currentRule.enabled }">
          <input
            type="checkbox"
            :checked="currentRule.enabled"
            @change="onUpdateRule({ enabled: !currentRule.enabled })"
          />
          <span class="rg-toggle-track">
            <span class="rg-toggle-thumb"></span>
          </span>
          <span class="rg-toggle-label">{{ currentRule.enabled ? 'ON' : 'OFF' }}</span>
        </label>
        <span v-if="currentRule" class="rg-meta">
          {{ currentRule.nodes.length }} nodes · {{ currentRule.connections.length }} connections
        </span>
      </div>
      <div class="rg-toolbar-right">
        <button
          v-if="currentRule"
          class="rg-btn rg-btn-ghost"
          @click="onZoomToFit"
          title="Zoom to fit all nodes"
        >
          ⊡ Fit
        </button>
        <button
          v-if="currentRule"
          class="rg-btn rg-btn-ghost rg-btn-undo"
          :disabled="!canUndo()"
          @click="onUndoDelete"
          title="Undo delete (Ctrl+Z)"
        >
          ↶ Undo
        </button>
        <button class="rg-btn rg-btn-secondary" @click="onCreateRule">+ New</button>
        <button
          class="rg-btn rg-btn-save"
          :class="{
            'rg-btn-save--saving': saveStatus === 'saving',
            'rg-btn-save--saved': saveStatus === 'saved',
            'rg-btn-save--error': saveStatus === 'error',
          }"
          :disabled="!dirty && saveStatus === 'idle'"
          @click="onSaveRule"
        >
          <span v-if="saveStatus === 'saving'">Saving...</span>
          <span v-else-if="saveStatus === 'saved'">Saved</span>
          <span v-else-if="saveStatus === 'error'">Error</span>
          <span v-else>Save</span>
        </button>
        <button v-if="currentRule" class="rg-btn rg-btn-danger" @click="onDeleteRule">Delete</button>
      </div>
    </div>

    <!-- Error banner -->
    <div v-if="error" class="rg-error-banner">{{ error }}</div>

    <!-- ── Canvas: fills everything between toolbar and status bar ─────── -->
    <div class="rg-canvas-wrap">
      <div ref="canvasRef" class="rg-canvas" v-show="currentRule"></div>
      <div v-if="!currentRule" class="rg-empty-canvas">
        <div class="rg-empty-icon">⬡</div>
        <div class="rg-empty-title">No rule selected</div>
        <div class="rg-empty-sub">Select a composite rule from the list or create a new one</div>
        <button class="rg-btn rg-btn-primary" @click="onCreateRule">+ New Composite Rule</button>
      </div>
    </div>

    <!-- ── Floating left panel: rule list + add node ─────────────────── -->
    <Transition name="panel-left">
      <div v-if="panelOpen" class="rg-panel rg-panel-left">
        <div class="rg-panel-header">
          <span class="rg-panel-title">COMPOSITE RULES</span>
          <button class="rg-panel-close" @click="panelOpen = false">×</button>
        </div>

        <div class="rg-rule-list">
          <div
            v-for="rule in compositeRules"
            :key="rule.id"
            class="rg-rule-item"
            :class="{ 'rg-rule-item--active': rule.id === currentRuleId }"
            @click="onSelectRuleAndClosePanel(rule.id)"
          >
            <div class="rg-rule-item-name">{{ rule.name }}</div>
            <div class="rg-rule-item-meta">
              {{ rule.nodes.length }}n · {{ rule.connections.length }}c
            </div>
            <span class="rg-rule-badge" :class="rule.enabled ? 'rg-badge-on' : 'rg-badge-off'">
              {{ rule.enabled ? 'ON' : 'OFF' }}
            </span>
          </div>
          <div v-if="compositeRules.length === 0" class="rg-list-empty">
            No rules yet
          </div>
        </div>

        <div class="rg-panel-divider"></div>

        <div class="rg-panel-header">
          <span class="rg-panel-title">ATOMIC RULES</span>
          <span class="rg-atomic-count">{{ filteredAtomicRules.length }}</span>
        </div>
        <div v-if="currentRule" class="rg-atomic-section">
          <input
            v-model="atomicSearch"
            type="text"
            class="rg-atomic-search"
            placeholder="Search rules..."
          />
          <div class="rg-atomic-list">
            <div
              v-for="rule in filteredAtomicRules"
              :key="rule.id"
              class="rg-atomic-item"
              @click="onAddAtomicRule(rule)"
              :title="`Add ${rule.name} to graph`"
            >
              <span class="rg-atomic-name">{{ rule.name }}</span>
              <span
                class="rg-atomic-socket"
                :style="{ background: (SOCKET_TYPE_COLORS as Record<string, string>)[rule.socket_type] || '#6B7280' }"
              >
                {{ rule.socket_type || 'any.boolean' }}
              </span>
            </div>
            <div v-if="filteredAtomicRules.length === 0 && atomicSearch" class="rg-list-empty">
              No matches for "{{ atomicSearch }}"
            </div>
            <div v-else-if="filteredAtomicRules.length === 0" class="rg-list-empty">
              No enabled atomic rules
            </div>
          </div>
        </div>

        <div class="rg-panel-divider"></div>

        <div class="rg-panel-header">
          <span class="rg-panel-title">LOGIC NODES</span>
        </div>
        <div v-if="currentRule" class="rg-node-buttons">
          <button class="rg-node-btn rg-node-and"      @click="onAddNode('and')">AND Gate</button>
          <button class="rg-node-btn rg-node-or"       @click="onAddNode('or')">OR Gate</button>
          <button class="rg-node-btn rg-node-not"      @click="onAddNode('not')">NOT Gate</button>
          <button class="rg-node-btn rg-node-stateful" @click="onAddNode('stateful_condition')">Stateful</button>
          <button class="rg-node-btn rg-node-output"   @click="onAddNode('output')">Output Action</button>
        </div>
        <div v-else class="rg-list-empty">Select a rule first</div>
      </div>
    </Transition>

    <!-- ── Floating right panel: node config (only when node selected) ── -->
    <Transition name="panel-right">
      <div v-if="selectedNode" class="rg-panel rg-panel-right">
        <button class="rg-panel-close rg-panel-close-abs" @click="selectedNodeId = null">×</button>
        <RuleGraphSidebar
          :rule="currentRule"
          :selected-node="selectedNode"
          :atomic-rules="atomicRules"
          :dirty="dirty"
          @add-node="onAddNode"
          @update-node="onUpdateNode"
          @update-rule="onUpdateRule"
        />
        <!-- Delete node button -->
        <div class="rg-node-actions">
          <button class="rg-btn rg-btn-node-delete" @click="onDeleteSelectedNode" title="Delete node (Del)">
            🗑 Delete Node
          </button>
        </div>
      </div>
    </Transition>

    <!-- ── Status bar ──────────────────────────────────────────────────── -->
    <div class="rg-statusbar">
      <div class="rg-legend">
        <span
          v-for="(color, type) in SOCKET_TYPE_COLORS"
          :key="type"
          class="rg-legend-item"
        >
          <span class="rg-legend-dot" :style="{ background: color }"></span>
          <span class="rg-legend-label">{{ type }}</span>
        </span>
      </div>
    </div>

  </div>
</template>

<style scoped>
/* ─── Page shell ──────────────────────────────────────────────────────────── */
.rg-page {
  display: flex;
  flex-direction: column;
  height: calc(100vh - 48px);
  background: #0B1120;
  position: relative;
  overflow: hidden;
}

/* ─── Toolbar ─────────────────────────────────────────────────────────────── */
.rg-toolbar {
  height: 48px;
  min-height: 48px;
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 0 16px;
  background: #0F172A;
  border-bottom: 1px solid #1E293B;
  z-index: 10;
  gap: 8px;
  flex-shrink: 0;
}

.rg-toolbar-left {
  display: flex;
  align-items: center;
  gap: 10px;
  overflow: hidden;
  min-width: 0;
}

.rg-toolbar-right {
  display: flex;
  align-items: center;
  gap: 8px;
  flex-shrink: 0;
}

.rg-title {
  font-size: 14px;
  font-weight: 700;
  color: #F1F5F9;
  letter-spacing: 0.02em;
  white-space: nowrap;
  flex-shrink: 0;
}

.rg-divider { color: #334155; flex-shrink: 0; }

.rg-rule-name {
  font-size: 13px;
  color: #94A3B8;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  max-width: 220px;
}

.rg-rule-name--editable {
  cursor: pointer;
  padding: 2px 6px;
  margin: -2px -6px;
  border-radius: 4px;
  transition: background 0.15s, color 0.15s;
}

.rg-rule-name--editable:hover {
  background: rgba(99, 102, 241, 0.15);
  color: #E2E8F0;
}

.rg-rule-name-input {
  font-size: 13px;
  color: #E2E8F0;
  background: #1E293B;
  border: 1px solid #6366F1;
  border-radius: 4px;
  padding: 2px 8px;
  width: 200px;
  max-width: 220px;
  outline: none;
  font-family: inherit;
}

.rg-dirty { color: #F59E0B; }

.rg-no-rule {
  font-size: 12px;
  color: #475569;
  white-space: nowrap;
}

.rg-meta {
  font-size: 11px;
  color: #475569;
  white-space: nowrap;
  flex-shrink: 0;
}

/* ─── Toggle switch ──────────────────────────────────────────────────────── */
.rg-toggle {
  display: flex;
  align-items: center;
  gap: 6px;
  cursor: pointer;
  flex-shrink: 0;
}

.rg-toggle input {
  display: none;
}

.rg-toggle-track {
  width: 32px;
  height: 18px;
  background: #334155;
  border-radius: 9px;
  position: relative;
  transition: background 0.2s;
}

.rg-toggle--on .rg-toggle-track {
  background: #10B981;
}

.rg-toggle-thumb {
  position: absolute;
  top: 2px;
  left: 2px;
  width: 14px;
  height: 14px;
  background: #fff;
  border-radius: 50%;
  transition: transform 0.2s;
}

.rg-toggle--on .rg-toggle-thumb {
  transform: translateX(14px);
}

.rg-toggle-label {
  font-size: 10px;
  font-weight: 700;
  letter-spacing: 0.05em;
  color: #475569;
}

.rg-toggle--on .rg-toggle-label {
  color: #10B981;
}

/* ─── Buttons ─────────────────────────────────────────────────────────────── */
.rg-btn {
  height: 30px;
  padding: 0 12px;
  border: none;
  border-radius: 5px;
  font-size: 12px;
  font-weight: 600;
  cursor: pointer;
  transition: opacity 0.15s, transform 0.1s;
  white-space: nowrap;
  font-family: inherit;
}

.rg-btn:hover:not(:disabled) { opacity: 0.82; }
.rg-btn:active:not(:disabled) { transform: scale(0.97); }
.rg-btn:disabled { opacity: 0.3; cursor: not-allowed; }

.rg-btn-ghost {
  background: transparent;
  color: #94A3B8;
  border: 1px solid #334155;
  padding: 0 10px;
  font-size: 14px;
}

.rg-btn-secondary {
  background: #1E293B;
  color: #94A3B8;
  border: 1px solid #334155;
}

.rg-btn-primary {
  background: #6366F1;
  color: #fff;
}

.rg-btn-save {
  background: #6366F1;
  color: #fff;
  min-width: 72px;
  transition: background 0.2s, opacity 0.15s;
}

.rg-btn-save--saving {
  background: #818CF8;
  cursor: wait;
}

.rg-btn-save--saved {
  background: #10B981;
}

.rg-btn-save--error {
  background: #EF4444;
}

.rg-btn-danger {
  background: transparent;
  color: #EF4444;
  border: 1px solid #EF444440;
}

.rg-btn-danger:hover:not(:disabled) {
  background: #EF444418;
  opacity: 1;
}

.rg-btn-undo {
  font-size: 11px;
}

.rg-btn-node-delete {
  width: 100%;
  background: rgba(239, 68, 68, 0.1);
  color: #EF4444;
  border: 1px solid rgba(239, 68, 68, 0.3);
}

.rg-btn-node-delete:hover:not(:disabled) {
  background: rgba(239, 68, 68, 0.2);
  opacity: 1;
}

.rg-node-actions {
  padding: 12px;
  border-top: 1px solid #1E293B;
  margin-top: auto;
  flex-shrink: 0;
}

/* ─── Canvas ──────────────────────────────────────────────────────────────── */
.rg-canvas-wrap {
  flex: 1;
  position: relative;
  overflow: hidden;
  background-color: #080E1A;
  background-image:
    linear-gradient(#1E293B55 1px, transparent 1px),
    linear-gradient(90deg, #1E293B55 1px, transparent 1px);
  background-size: 28px 28px;
}

.rg-canvas {
  width: 100%;
  height: 100%;
  position: absolute;
  top: 0;
  left: 0;
}

/* Rete.js internal elements */
.rg-canvas :deep(> div) {
  width: 100%;
  height: 100%;
}

/* ─── Selected node highlight ─────────────────────────────────────────────── */
.rg-canvas :deep(.rete-node-selected) {
  outline: 3px solid #22D3EE !important;
  outline-offset: 3px;
  border-radius: 12px;
  box-shadow: 0 0 20px 4px rgba(34, 211, 238, 0.5), 0 0 40px 8px rgba(34, 211, 238, 0.25) !important;
}

/* ─── Empty state ─────────────────────────────────────────────────────────── */
.rg-empty-canvas {
  position: absolute;
  inset: 0;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  gap: 12px;
}

.rg-empty-icon {
  font-size: 52px;
  opacity: 0.1;
  line-height: 1;
}

.rg-empty-title {
  font-size: 16px;
  font-weight: 600;
  color: #334155;
}

.rg-empty-sub {
  font-size: 13px;
  color: #1E293B;
  text-align: center;
  max-width: 260px;
  line-height: 1.5;
}

/* ─── Error banner ────────────────────────────────────────────────────────── */
.rg-error-banner {
  background: #7F1D1D;
  color: #FCA5A5;
  font-size: 12px;
  padding: 8px 16px;
  border-bottom: 1px solid #EF444440;
  flex-shrink: 0;
}

/* ─── Floating panels ─────────────────────────────────────────────────────── */
.rg-panel {
  position: absolute;
  top: 12px;
  bottom: 44px;
  width: 244px;
  background: rgba(9, 14, 26, 0.93);
  backdrop-filter: blur(16px);
  -webkit-backdrop-filter: blur(16px);
  border: 1px solid #1E293B;
  border-radius: 10px;
  display: flex;
  flex-direction: column;
  overflow: hidden;
  z-index: 20;
  box-shadow: 0 8px 40px rgba(0, 0, 0, 0.5), 0 0 0 1px rgba(255,255,255,0.04);
}

.rg-panel-left  { left: 12px; }
.rg-panel-right { right: 12px; width: 264px; position: absolute; top: 12px; bottom: 44px; }

.rg-panel-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 10px 14px 8px;
  flex-shrink: 0;
}

.rg-panel-title {
  font-size: 9px;
  font-weight: 800;
  letter-spacing: 0.14em;
  color: #334155;
  text-transform: uppercase;
}

.rg-panel-close {
  background: none;
  border: none;
  color: #334155;
  cursor: pointer;
  font-size: 18px;
  line-height: 1;
  padding: 0 4px;
  transition: color 0.15s;
  font-family: inherit;
}

.rg-panel-close:hover { color: #94A3B8; }

.rg-panel-close-abs {
  position: absolute;
  top: 10px;
  right: 10px;
  z-index: 1;
}

.rg-panel-divider {
  height: 1px;
  background: #1E293B;
  flex-shrink: 0;
}

/* ─── Rule list ───────────────────────────────────────────────────────────── */
.rg-rule-list {
  flex: 1;
  overflow-y: auto;
  padding: 4px 8px;
  display: flex;
  flex-direction: column;
  gap: 3px;
  min-height: 0;
}

.rg-rule-item {
  padding: 10px 12px;
  border-radius: 7px;
  cursor: pointer;
  border: 1px solid transparent;
  transition: background 0.12s, border-color 0.12s;
  position: relative;
}

.rg-rule-item:hover { background: #0F172A; border-color: #1E293B; }

.rg-rule-item--active {
  background: rgba(99, 102, 241, 0.1);
  border-color: rgba(99, 102, 241, 0.35);
}

.rg-rule-item-name {
  font-size: 13px;
  font-weight: 600;
  color: #CBD5E1;
  padding-right: 40px;
  line-height: 1.3;
  margin-bottom: 3px;
}

.rg-rule-item-meta {
  font-size: 10px;
  color: #334155;
}

.rg-rule-badge {
  position: absolute;
  top: 10px;
  right: 10px;
  font-size: 9px;
  font-weight: 700;
  padding: 2px 6px;
  border-radius: 4px;
  letter-spacing: 0.06em;
}

.rg-badge-on  { background: #052E16; color: #4ADE80; }
.rg-badge-off { background: #0F172A; color: #334155; }

.rg-list-empty {
  font-size: 12px;
  color: #1E293B;
  text-align: center;
  padding: 16px 12px;
}

/* ─── Atomic rules list ───────────────────────────────────────────────────── */
.rg-atomic-count {
  font-size: 10px;
  font-weight: 600;
  color: #64748B;
  background: #1E293B;
  padding: 2px 6px;
  border-radius: 4px;
}

.rg-atomic-section {
  display: flex;
  flex-direction: column;
  gap: 6px;
  padding: 0 8px;
}

.rg-atomic-search {
  width: 100%;
  height: 28px;
  padding: 0 10px;
  border: 1px solid #334155;
  border-radius: 5px;
  background: #0F172A;
  color: #E2E8F0;
  font-size: 11px;
  font-family: inherit;
  outline: none;
  transition: border-color 0.15s;
}

.rg-atomic-search::placeholder {
  color: #475569;
}

.rg-atomic-search:focus {
  border-color: #6366F1;
}

.rg-atomic-list {
  display: flex;
  flex-direction: column;
  gap: 3px;
  max-height: 140px;
  overflow-y: auto;
}

.rg-atomic-item {
  padding: 8px 10px;
  border-radius: 6px;
  cursor: pointer;
  border: 1px solid rgba(99, 102, 241, 0.25);
  background: rgba(99, 102, 241, 0.08);
  transition: background 0.12s, border-color 0.12s, transform 0.1s;
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 8px;
}

.rg-atomic-item:hover {
  background: rgba(99, 102, 241, 0.16);
  border-color: rgba(99, 102, 241, 0.4);
}

.rg-atomic-item:active {
  transform: scale(0.98);
}

.rg-atomic-name {
  font-size: 11px;
  font-weight: 600;
  color: #A5B4FC;
  white-space: nowrap;
  overflow: hidden;
  text-overflow: ellipsis;
  flex: 1;
  min-width: 0;
}

.rg-atomic-socket {
  font-size: 9px;
  font-weight: 600;
  color: #fff;
  padding: 2px 6px;
  border-radius: 4px;
  white-space: nowrap;
  flex-shrink: 0;
}

/* ─── Add node buttons ────────────────────────────────────────────────────── */
.rg-node-buttons {
  padding: 8px;
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 5px;
  flex-shrink: 0;
}

.rg-node-btn {
  padding: 9px 6px;
  border-radius: 6px;
  border: 1px solid;
  font-size: 11px;
  font-weight: 600;
  cursor: pointer;
  transition: opacity 0.12s, transform 0.1s;
  text-align: center;
  font-family: inherit;
  letter-spacing: 0.01em;
}

.rg-node-btn:hover  { opacity: 0.78; }
.rg-node-btn:active { transform: scale(0.96); }

.rg-node-atomic   { background: rgba(99,102,241,0.12);  border-color: rgba(99,102,241,0.4);  color: #A5B4FC; }
.rg-node-and      { background: rgba(16,185,129,0.12);  border-color: rgba(16,185,129,0.4);  color: #6EE7B7; }
.rg-node-or       { background: rgba(245,158,11,0.12);  border-color: rgba(245,158,11,0.4);  color: #FCD34D; }
.rg-node-not      { background: rgba(239,68,68,0.12);   border-color: rgba(239,68,68,0.4);   color: #FCA5A5; }
.rg-node-stateful { background: rgba(139,92,246,0.12);  border-color: rgba(139,92,246,0.4);  color: #C4B5FD; }
.rg-node-output   { background: rgba(34,211,238,0.12);  border-color: rgba(34,211,238,0.4);  color: #67E8F9; }

/* ─── Status bar ──────────────────────────────────────────────────────────── */
.rg-statusbar {
  height: 32px;
  min-height: 32px;
  display: flex;
  align-items: center;
  padding: 0 16px;
  background: #0F172A;
  border-top: 1px solid #1E293B;
  z-index: 10;
  flex-shrink: 0;
  overflow: hidden;
}

.rg-legend {
  display: flex;
  align-items: center;
  gap: 18px;
  overflow: hidden;
}

.rg-legend-item {
  display: flex;
  align-items: center;
  gap: 6px;
  flex-shrink: 0;
}

.rg-legend-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  flex-shrink: 0;
}

.rg-legend-label {
  font-size: 10px;
  color: #334155;
  white-space: nowrap;
}

/* ─── Panel slide animations ─────────────────────────────────────────────── */
.panel-left-enter-active,
.panel-left-leave-active,
.panel-right-enter-active,
.panel-right-leave-active {
  transition: opacity 0.18s ease, transform 0.18s ease;
}

.panel-left-enter-from,
.panel-left-leave-to {
  opacity: 0;
  transform: translateX(-16px);
}

.panel-right-enter-from,
.panel-right-leave-to {
  opacity: 0;
  transform: translateX(16px);
}
</style>
