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
  addConnection,
  removeConnection,
  generateNodeId,
  generateConnectionId,
  setReteEditor,
  syncRuleToEditor,
} = useRuleEditor();

const { startPolling, stopPolling } = useRuleEvaluation();

const selectedNodeId = ref<string | null>(null);
const panelOpen = ref(false);

const selectedNode = computed(() => {
  if (!currentRule.value || !selectedNodeId.value) return null;
  return currentRule.value.nodes.find(n => n.id === selectedNodeId.value) || null;
});

// Rete.js canvas ref
const canvasRef = ref<HTMLElement | null>(null);
let reteDestroy: (() => void) | null = null;

// Initialize Rete editor when canvas becomes available
async function initReteEditor() {
  console.log('initReteEditor: starting...');

  // Destroy previous editor if exists
  if (reteDestroy) {
    console.log('initReteEditor: destroying previous editor');
    reteDestroy();
    reteDestroy = null;
    setReteEditor(null, null);
  }

  // Wait for DOM to update multiple ticks to ensure layout is complete
  await nextTick();
  await nextTick();

  // Also wait for next animation frame to ensure rendering
  await new Promise<void>(resolve => requestAnimationFrame(() => resolve()));

  if (!canvasRef.value) {
    console.error('initReteEditor: canvasRef is null after nextTick');
    return;
  }

  const container = canvasRef.value;
  console.log('initReteEditor: canvas container:', {
    clientWidth: container.clientWidth,
    clientHeight: container.clientHeight,
    offsetWidth: container.offsetWidth,
    offsetHeight: container.offsetHeight,
  });

  // Ensure container has dimensions
  if (container.clientWidth === 0 || container.clientHeight === 0) {
    console.warn('initReteEditor: canvas has 0 dimensions, waiting...');
    await new Promise<void>(resolve => setTimeout(resolve, 100));
  }

  try {
    const { editor, area, destroy } = await createReteEditor(container);
    setReteEditor(editor, area);
    reteDestroy = destroy;
    console.log('initReteEditor: Rete.js editor initialized successfully');

    // Sync existing nodes from the rule to the editor
    await syncRuleToEditor();
  } catch (err) {
    console.error('initReteEditor: failed to initialize Rete.js editor:', err);
  }
}

// Watch for rule selection to initialize editor
watch(currentRule, async (newRule) => {
  if (newRule) {
    await initReteEditor();
  }
});

// Lifecycle
onMounted(async () => {
  await loadAll();
  startPolling(2000);
});

onUnmounted(() => {
  stopPolling();
  // MANDATORY — leaks memory if skipped
  reteDestroy?.();
  setReteEditor(null, null);
});

// Rule list operations
function onSelectRule(id: string) {
  selectRule(id);
  selectedNodeId.value = null;
}

function onCreateRule() {
  createNewRule();
  selectedNodeId.value = null;
}

async function onSaveRule() {
  if (currentRule.value) {
    await saveCompositeRule(currentRule.value);
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

function onDeleteNode(nodeId: string) {
  removeNode(nodeId);
  if (selectedNodeId.value === nodeId) {
    selectedNodeId.value = null;
  }
}

function onMoveNode(nodeId: string, x: number, y: number) {
  updateNode(nodeId, { position: { x, y } });
}

async function onAddNode(type: NodeType, data?: unknown) {
  if (!currentRule.value) return;

  const node: CompositeNode = {
    id: generateNodeId(),
    type: type as CompositeNode['type'],
    position: { x: 100 + Math.random() * 200, y: 100 + Math.random() * 200 },
    data: (data || { gate_type: type }) as CompositeNode['data'],
  };

  await addNode(node);
}

function onUpdateNode(nodeId: string, updates: Partial<CompositeNode>) {
  updateNode(nodeId, updates);
}

function onUpdateRule(updates: { name?: string; description?: string; enabled?: boolean }) {
  updateCurrentRule(updates);
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
        <span v-if="currentRule" class="rg-rule-name">
          {{ currentRule.name }}<span v-if="dirty" class="rg-dirty"> *</span>
        </span>
        <span v-else class="rg-no-rule">No rule selected</span>
        <span v-if="currentRule" class="rg-meta">
          {{ currentRule.nodes.length }} nodes · {{ currentRule.connections.length }} connections
        </span>
      </div>
      <div class="rg-toolbar-right">
        <button class="rg-btn rg-btn-secondary" @click="onCreateRule">+ New</button>
        <button class="rg-btn rg-btn-primary" :disabled="!dirty" @click="onSaveRule">Save</button>
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
            @click="onSelectRule(rule.id); panelOpen = false"
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
          <span class="rg-panel-title">ADD NODE</span>
        </div>
        <div v-if="currentRule" class="rg-node-buttons">
          <button class="rg-node-btn rg-node-atomic"   @click="onAddNode('atomic')">Atomic Rule</button>
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
          @save="onSaveRule"
          @delete="onDeleteRule"
        />
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

.rg-btn-danger {
  background: transparent;
  color: #EF4444;
  border: 1px solid #EF444440;
}

.rg-btn-danger:hover:not(:disabled) {
  background: #EF444418;
  opacity: 1;
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
