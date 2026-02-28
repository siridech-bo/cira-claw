<script setup lang="ts">
import { ref, computed, onMounted, onUnmounted } from 'vue';
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
} = useRuleEditor();

const { startPolling, stopPolling } = useRuleEvaluation();

const selectedNodeId = ref<string | null>(null);

const selectedNode = computed(() => {
  if (!currentRule.value || !selectedNodeId.value) return null;
  return currentRule.value.nodes.find(n => n.id === selectedNodeId.value) || null;
});

// Rete.js canvas ref
const canvasRef = ref<HTMLElement | null>(null);
let reteDestroy: (() => void) | null = null;

// Lifecycle
onMounted(async () => {
  await loadAll();
  startPolling(2000);

  // Initialize Rete.js editor
  if (canvasRef.value) {
    try {
      const { editor, area, destroy } = await createReteEditor(canvasRef.value);
      setReteEditor(editor, area);
      reteDestroy = destroy;
    } catch (err) {
      console.error('Failed to initialize Rete.js editor:', err);
    }
  }
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

function onAddNode(type: NodeType, data?: unknown) {
  if (!currentRule.value) return;

  const node: CompositeNode = {
    id: generateNodeId(),
    type: type as CompositeNode['type'],
    position: { x: 100 + Math.random() * 200, y: 100 + Math.random() * 200 },
    data: (data || { gate_type: type }) as CompositeNode['data'],
  };

  addNode(node);
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
  <div class="rule-graph-page">
    <header class="page-header">
      <div class="header-left">
        <h2>Rule Graph</h2>
        <span v-if="currentRule" class="current-rule-name">
          {{ currentRule.name }}
          <span v-if="dirty" class="dirty-indicator">*</span>
        </span>
      </div>
      <div class="header-actions">
        <button class="btn-secondary" @click="onCreateRule">
          + New Rule
        </button>
      </div>
    </header>

    <div v-if="error" class="error-banner">
      {{ error }}
    </div>

    <div v-if="loading" class="loading-overlay">
      Loading...
    </div>

    <div class="rule-graph-layout">
      <!-- Rule List Panel -->
      <div class="rule-list-panel">
        <h3>Composite Rules</h3>
        <div class="rule-list">
          <div
            v-for="rule in compositeRules"
            :key="rule.id"
            class="rule-item"
            :class="{ selected: rule.id === currentRuleId, disabled: !rule.enabled }"
            @click="onSelectRule(rule.id)"
          >
            <div class="rule-item-header">
              <span class="rule-name">{{ rule.name }}</span>
              <span class="rule-status" :class="{ enabled: rule.enabled }">
                {{ rule.enabled ? 'ON' : 'OFF' }}
              </span>
            </div>
            <div class="rule-item-meta">
              {{ rule.nodes.length }} nodes · {{ rule.connections.length }} connections
            </div>
          </div>

          <div v-if="compositeRules.length === 0" class="empty-list">
            No composite rules yet.
            <br />
            Click "New Rule" to create one.
          </div>
        </div>
      </div>

      <!-- Rete.js Canvas -->
      <div class="canvas-container">
        <div ref="canvasRef" class="rete-canvas" v-if="currentRule"></div>
        <div v-else class="no-rule-selected">
          <p>Select a composite rule from the list</p>
          <p>or create a new one</p>
        </div>
      </div>

      <!-- Sidebar -->
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
  </div>
</template>

<style scoped>
.rule-graph-page {
  display: flex;
  flex-direction: column;
  height: calc(100vh - 48px);
}

.page-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 16px;
}

.header-left {
  display: flex;
  align-items: center;
  gap: 16px;
}

.page-header h2 {
  font-size: 1.5rem;
  font-weight: 600;
}

.current-rule-name {
  color: #94A3B8;
  font-size: 0.875rem;
}

.dirty-indicator {
  color: #F59E0B;
  font-weight: bold;
}

.btn-secondary {
  padding: 8px 16px;
  background: #334155;
  color: #E2E8F0;
  border: none;
  border-radius: 6px;
  cursor: pointer;
  font-weight: 500;
}

.btn-secondary:hover {
  background: #475569;
}

.error-banner {
  background: #7F1D1D;
  color: #FCA5A5;
  padding: 12px 16px;
  border-radius: 6px;
  margin-bottom: 16px;
}

.loading-overlay {
  position: fixed;
  top: 0;
  left: 0;
  right: 0;
  bottom: 0;
  background: rgba(15, 23, 42, 0.8);
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 100;
  color: #E2E8F0;
  font-size: 1.25rem;
}

.rule-graph-layout {
  flex: 1;
  display: flex;
  gap: 16px;
  min-height: 0;
}

.rule-list-panel {
  width: 240px;
  background: #1E293B;
  border-radius: 8px;
  padding: 16px;
  display: flex;
  flex-direction: column;
}

.rule-list-panel h3 {
  font-size: 0.875rem;
  font-weight: 600;
  color: #94A3B8;
  margin-bottom: 12px;
  text-transform: uppercase;
}

.rule-list {
  flex: 1;
  overflow-y: auto;
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.rule-item {
  padding: 12px;
  background: #0F172A;
  border: 2px solid transparent;
  border-radius: 6px;
  cursor: pointer;
}

.rule-item:hover {
  background: #1E293B;
}

.rule-item.selected {
  border-color: #6366F1;
}

.rule-item.disabled {
  opacity: 0.6;
}

.rule-item-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 4px;
}

.rule-name {
  font-weight: 500;
  color: #E2E8F0;
}

.rule-status {
  font-size: 0.65rem;
  padding: 2px 6px;
  border-radius: 4px;
  background: #475569;
  color: #94A3B8;
}

.rule-status.enabled {
  background: #166534;
  color: #86EFAC;
}

.rule-item-meta {
  font-size: 0.75rem;
  color: #64748B;
}

.empty-list {
  text-align: center;
  color: #64748B;
  padding: 24px;
  font-size: 0.875rem;
}

.canvas-container {
  flex: 1;
  display: flex;
  min-width: 0;
  position: relative;
}

.rete-canvas {
  width: 100%;
  height: 100%;
  background: #0F172A;
  background-image:
    linear-gradient(rgba(99, 102, 241, 0.05) 1px, transparent 1px),
    linear-gradient(90deg, rgba(99, 102, 241, 0.05) 1px, transparent 1px);
  background-size: 20px 20px;
  border-radius: 8px;
}

.no-rule-selected {
  flex: 1;
  display: flex;
  flex-direction: column;
  align-items: center;
  justify-content: center;
  background: #0F172A;
  border-radius: 8px;
  color: #64748B;
}

.no-rule-selected p {
  margin: 4px 0;
}
</style>
