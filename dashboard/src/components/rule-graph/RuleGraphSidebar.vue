<script setup lang="ts">
import { ref, computed, watch } from 'vue';
import type {
  CompositeNode,
  CompositeRule,
  AtomicRule,
  AtomicNodeData,
  ThresholdNodeData,
  OutputNodeData,
} from '../../composables/types';
import {
  NODE_TYPE_LABELS,
  NODE_TYPE_COLORS,
  SOCKET_TYPE_COLORS,
  ACTION_TYPE_LABELS,
  ACTION_TYPE_COLORS,
  THRESHOLD_OPERATORS,
  THRESHOLD_OPERATOR_LABELS,
  PAYLOAD_FIELDS,
  type NodeType,
  type ActionType,
  type ThresholdOperator,
} from '../../composables/sockets';

interface Props {
  rule: CompositeRule | null;
  selectedNode: CompositeNode | null;
  atomicRules: AtomicRule[];
  dirty: boolean;
}

const props = defineProps<Props>();

const emit = defineEmits<{
  (e: 'addNode', type: NodeType, data?: unknown): void;
  (e: 'updateNode', nodeId: string, updates: Partial<CompositeNode>): void;
  (e: 'updateRule', updates: Partial<CompositeRule>): void;
}>();

// Add node panel
const showAddPanel = ref(false);
const selectedAtomicRule = ref<string>('');
const thresholdField = ref(PAYLOAD_FIELDS[0].path);
const thresholdOperator = ref<ThresholdOperator>('>');
const thresholdValue = ref(0);
const constantValue = ref(true);

// Output action config
const outputAction = ref<ActionType>('alert');
const outputSeverity = ref<'info' | 'warning' | 'critical'>('warning');
const outputMessage = ref('');
const modbusRegister = ref(0);
const modbusValue = ref(0);

// Node types available to add
const nodeTypes: NodeType[] = ['atomic', 'and', 'or', 'not', 'constant', 'threshold', 'output'];

// Enabled atomic rules for selection
const enabledAtomicRules = computed(() => props.atomicRules.filter(r => r.enabled));

function addNode(type: NodeType) {
  let data: unknown;

  switch (type) {
    case 'atomic':
      if (!selectedAtomicRule.value) return;
      const rule = props.atomicRules.find(r => r.id === selectedAtomicRule.value);
      data = {
        rule_id: selectedAtomicRule.value,
        socket_type: rule?.socket_type || 'any.boolean',
      } as AtomicNodeData;
      break;

    case 'threshold':
      data = {
        field: thresholdField.value,
        operator: thresholdOperator.value,
        threshold: thresholdValue.value,
      } as ThresholdNodeData;
      break;

    case 'constant':
      data = { value: constantValue.value };
      break;

    case 'output':
      data = {
        action: outputAction.value,
        severity: outputAction.value === 'alert' ? outputSeverity.value : undefined,
        message: outputMessage.value || undefined,
        register: outputAction.value === 'modbus_write' ? modbusRegister.value : undefined,
        value: outputAction.value === 'modbus_write' ? modbusValue.value : undefined,
      } as OutputNodeData;
      break;

    default:
      data = { gate_type: type };
  }

  emit('addNode', type, data);
  showAddPanel.value = false;
}

// Update selected node properties
function updateSelectedNode(updates: Partial<CompositeNode>) {
  if (props.selectedNode) {
    emit('updateNode', props.selectedNode.id, updates);
  }
}

// Update rule metadata
function updateRuleName(name: string) {
  emit('updateRule', { name });
}

function updateRuleDescription(description: string) {
  emit('updateRule', { description });
}

function toggleRuleEnabled() {
  if (props.rule) {
    emit('updateRule', { enabled: !props.rule.enabled });
  }
}

// Sync selected node data to form
watch(() => props.selectedNode, (node) => {
  if (!node) return;

  if (node.type === 'output') {
    const data = node.data as OutputNodeData;
    outputAction.value = data.action;
    outputSeverity.value = data.severity || 'warning';
    outputMessage.value = data.message || '';
    modbusRegister.value = data.register || 0;
    modbusValue.value = data.value || 0;
  } else if (node.type === 'threshold') {
    const data = node.data as ThresholdNodeData;
    thresholdField.value = data.field;
    thresholdOperator.value = data.operator;
    thresholdValue.value = data.threshold;
  } else if (node.type === 'constant') {
    const data = node.data as { value: boolean };
    constantValue.value = data.value;
  } else if (node.type === 'atomic') {
    const data = node.data as AtomicNodeData;
    selectedAtomicRule.value = data.rule_id;
  }
}, { immediate: true });

function applyNodeChanges() {
  if (!props.selectedNode) return;

  let data: unknown;

  switch (props.selectedNode.type) {
    case 'output':
      data = {
        action: outputAction.value,
        severity: outputAction.value === 'alert' ? outputSeverity.value : undefined,
        message: outputMessage.value || undefined,
        register: outputAction.value === 'modbus_write' ? modbusRegister.value : undefined,
        value: outputAction.value === 'modbus_write' ? modbusValue.value : undefined,
      };
      break;

    case 'threshold':
      data = {
        field: thresholdField.value,
        operator: thresholdOperator.value,
        threshold: thresholdValue.value,
      };
      break;

    case 'constant':
      data = { value: constantValue.value };
      break;

    default:
      return;
  }

  updateSelectedNode({ data: data as CompositeNode['data'] });
}
</script>

<template>
  <div class="rule-graph-sidebar">
    <!-- Rule Info -->
    <section v-if="rule" class="sidebar-section">
      <h3>Rule Settings</h3>

      <div class="form-group">
        <label>Name</label>
        <input
          type="text"
          :value="rule.name"
          @input="updateRuleName(($event.target as HTMLInputElement).value)"
          placeholder="Rule name"
        />
      </div>

      <div class="form-group">
        <label>Description</label>
        <textarea
          :value="rule.description"
          @input="updateRuleDescription(($event.target as HTMLTextAreaElement).value)"
          placeholder="What does this rule do?"
          rows="2"
        ></textarea>
      </div>

      <div class="form-group checkbox">
        <label>
          <input
            type="checkbox"
            :checked="rule.enabled"
            @change="toggleRuleEnabled"
          />
          Enabled
        </label>
      </div>

    </section>

    <!-- Add Node -->
    <section class="sidebar-section">
      <h3>Add Node</h3>

      <div class="node-buttons">
        <button
          v-for="type in nodeTypes"
          :key="type"
          class="node-btn"
          :style="{ borderColor: NODE_TYPE_COLORS[type] }"
          @click="type === 'atomic' || type === 'threshold' || type === 'constant' || type === 'output' ? showAddPanel = true : addNode(type)"
          :data-type="type"
        >
          {{ NODE_TYPE_LABELS[type] }}
        </button>
      </div>

      <!-- Add Node Panel -->
      <div v-if="showAddPanel" class="add-panel">
        <div class="panel-header">
          <span>Configure Node</span>
          <button class="close-btn" @click="showAddPanel = false">×</button>
        </div>

        <!-- Atomic Rule Selection -->
        <div class="form-group">
          <label>Atomic Rule</label>
          <select v-model="selectedAtomicRule">
            <option value="">Select a rule...</option>
            <option v-for="ar in enabledAtomicRules" :key="ar.id" :value="ar.id">
              {{ ar.name }}
            </option>
          </select>
          <button
            class="btn-small"
            :disabled="!selectedAtomicRule"
            @click="addNode('atomic')"
          >
            Add Atomic
          </button>
        </div>

        <!-- Threshold Config -->
        <div class="form-group">
          <label>Threshold</label>
          <select v-model="thresholdField">
            <option v-for="field in PAYLOAD_FIELDS" :key="field.path" :value="field.path">
              {{ field.label }}
            </option>
          </select>
          <div class="inline-group">
            <select v-model="thresholdOperator">
              <option v-for="op in THRESHOLD_OPERATORS" :key="op" :value="op">
                {{ THRESHOLD_OPERATOR_LABELS[op] }}
              </option>
            </select>
            <input type="number" v-model.number="thresholdValue" />
          </div>
          <button class="btn-small" @click="addNode('threshold')">Add Threshold</button>
        </div>

        <!-- Constant Config -->
        <div class="form-group">
          <label>Constant</label>
          <div class="inline-group">
            <label class="radio">
              <input type="radio" v-model="constantValue" :value="true" /> TRUE
            </label>
            <label class="radio">
              <input type="radio" v-model="constantValue" :value="false" /> FALSE
            </label>
          </div>
          <button class="btn-small" @click="addNode('constant')">Add Constant</button>
        </div>

        <!-- Output Config -->
        <div class="form-group">
          <label>Output Action</label>
          <select v-model="outputAction">
            <option v-for="(label, action) in ACTION_TYPE_LABELS" :key="action" :value="action">
              {{ label }}
            </option>
          </select>

          <div v-if="outputAction === 'alert'" class="sub-form">
            <select v-model="outputSeverity">
              <option value="info">Info</option>
              <option value="warning">Warning</option>
              <option value="critical">Critical</option>
            </select>
            <input type="text" v-model="outputMessage" placeholder="Alert message" />
          </div>

          <div v-if="outputAction === 'modbus_write'" class="sub-form">
            <input type="number" v-model.number="modbusRegister" placeholder="Register" />
            <input type="number" v-model.number="modbusValue" placeholder="Value" />
          </div>

          <button class="btn-small" @click="addNode('output')">Add Output</button>
        </div>
      </div>
    </section>

    <!-- Node Properties -->
    <section v-if="selectedNode" class="sidebar-section">
      <h3>Node Properties</h3>

      <div class="property-row">
        <span class="property-label">Type:</span>
        <span class="property-value">{{ NODE_TYPE_LABELS[selectedNode.type] }}</span>
      </div>

      <div class="property-row">
        <span class="property-label">ID:</span>
        <span class="property-value mono">{{ selectedNode.id }}</span>
      </div>

      <!-- Editable properties based on node type -->
      <template v-if="selectedNode.type === 'output'">
        <div class="form-group">
          <label>Action</label>
          <select v-model="outputAction" @change="applyNodeChanges">
            <option v-for="(label, action) in ACTION_TYPE_LABELS" :key="action" :value="action">
              {{ label }}
            </option>
          </select>
        </div>

        <div v-if="outputAction === 'alert'" class="form-group">
          <label>Severity</label>
          <select v-model="outputSeverity" @change="applyNodeChanges">
            <option value="info">Info</option>
            <option value="warning">Warning</option>
            <option value="critical">Critical</option>
          </select>

          <label>Message</label>
          <input type="text" v-model="outputMessage" @blur="applyNodeChanges" />
        </div>

        <div v-if="outputAction === 'modbus_write'" class="form-group">
          <label>Register</label>
          <input type="number" v-model.number="modbusRegister" @blur="applyNodeChanges" />

          <label>Value</label>
          <input type="number" v-model.number="modbusValue" @blur="applyNodeChanges" />
        </div>
      </template>

      <template v-else-if="selectedNode.type === 'threshold'">
        <div class="form-group">
          <label>Field</label>
          <select v-model="thresholdField" @change="applyNodeChanges">
            <option v-for="field in PAYLOAD_FIELDS" :key="field.path" :value="field.path">
              {{ field.label }}
            </option>
          </select>

          <label>Condition</label>
          <div class="inline-group">
            <select v-model="thresholdOperator" @change="applyNodeChanges">
              <option v-for="op in THRESHOLD_OPERATORS" :key="op" :value="op">
                {{ op }}
              </option>
            </select>
            <input type="number" v-model.number="thresholdValue" @blur="applyNodeChanges" />
          </div>
        </div>
      </template>

      <template v-else-if="selectedNode.type === 'constant'">
        <div class="form-group">
          <label>Value</label>
          <div class="inline-group">
            <label class="radio">
              <input type="radio" v-model="constantValue" :value="true" @change="applyNodeChanges" /> TRUE
            </label>
            <label class="radio">
              <input type="radio" v-model="constantValue" :value="false" @change="applyNodeChanges" /> FALSE
            </label>
          </div>
        </div>
      </template>
    </section>
  </div>
</template>

<style scoped>
.rule-graph-sidebar {
  width: 100%;
  background: transparent;
  padding: 8px 0;
  display: flex;
  flex-direction: column;
  overflow-y: auto;
  flex: 1;
  min-height: 0;
}

.sidebar-section {
  border-bottom: 1px solid #334155;
  padding-bottom: 16px;
}

.sidebar-section:last-child {
  border-bottom: none;
}

.sidebar-section h3 {
  font-size: 0.875rem;
  font-weight: 600;
  color: #94A3B8;
  margin-bottom: 12px;
  text-transform: uppercase;
}

.form-group {
  margin-bottom: 12px;
}

.form-group label {
  display: block;
  font-size: 0.75rem;
  color: #94A3B8;
  margin-bottom: 4px;
}

.form-group.checkbox label {
  display: flex;
  align-items: center;
  gap: 8px;
  cursor: pointer;
}

.form-group input[type="text"],
.form-group input[type="number"],
.form-group select,
.form-group textarea {
  width: 100%;
  padding: 8px;
  background: #0F172A;
  border: 1px solid #334155;
  border-radius: 4px;
  color: #E2E8F0;
  font-size: 0.875rem;
}

.form-group textarea {
  resize: vertical;
}

.form-group input:focus,
.form-group select:focus,
.form-group textarea:focus {
  outline: none;
  border-color: #6366F1;
}

.inline-group {
  display: flex;
  gap: 8px;
}

.inline-group select,
.inline-group input {
  flex: 1;
}

.radio {
  display: flex;
  align-items: center;
  gap: 4px;
  cursor: pointer;
  font-size: 0.875rem;
  color: #E2E8F0;
}

.btn-small {
  padding: 4px 12px;
  background: #334155;
  color: #E2E8F0;
  margin-top: 8px;
}

.btn-small:hover {
  background: #475569;
}

.btn-small:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.node-buttons {
  display: grid;
  grid-template-columns: repeat(2, 1fr);
  gap: 8px;
}

.node-btn {
  padding: 8px;
  background: #0F172A;
  border: 2px solid #334155;
  border-radius: 4px;
  color: #E2E8F0;
  font-size: 0.75rem;
  cursor: pointer;
  text-align: center;
}

.node-btn:hover {
  background: #1E293B;
}

.add-panel {
  margin-top: 12px;
  padding: 12px;
  background: #0F172A;
  border-radius: 4px;
}

.panel-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 12px;
  color: #E2E8F0;
  font-weight: 500;
}

.close-btn {
  background: none;
  border: none;
  color: #94A3B8;
  font-size: 1.25rem;
  cursor: pointer;
}

.close-btn:hover {
  color: #E2E8F0;
}

.sub-form {
  margin-top: 8px;
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.property-row {
  display: flex;
  justify-content: space-between;
  padding: 4px 0;
  font-size: 0.875rem;
}

.property-label {
  color: #94A3B8;
}

.property-value {
  color: #E2E8F0;
}

.property-value.mono {
  font-family: 'JetBrains Mono', monospace;
  font-size: 0.75rem;
}
</style>
