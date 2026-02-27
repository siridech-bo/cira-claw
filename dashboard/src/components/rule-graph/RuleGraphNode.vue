<script setup lang="ts">
import { computed } from 'vue';
import type { CompositeNode, AtomicNodeData, ThresholdNodeData, OutputNodeData } from '../../composables/types';
import { NODE_TYPE_COLORS, NODE_TYPE_LABELS, SOCKET_TYPE_COLORS, ACTION_TYPE_COLORS } from '../../composables/sockets';

interface Props {
  node: CompositeNode;
  selected?: boolean;
  active?: boolean;
  atomicRuleName?: string;
}

const props = withDefaults(defineProps<Props>(), {
  selected: false,
  active: false,
  atomicRuleName: '',
});

const emit = defineEmits<{
  (e: 'select', nodeId: string): void;
  (e: 'delete', nodeId: string): void;
  (e: 'dragstart', event: DragEvent, nodeId: string): void;
}>();

const nodeColor = computed(() => {
  if (props.node.type === 'atomic') {
    const data = props.node.data as AtomicNodeData;
    return SOCKET_TYPE_COLORS[data.socket_type] || NODE_TYPE_COLORS.atomic;
  }
  if (props.node.type === 'output') {
    const data = props.node.data as OutputNodeData;
    return ACTION_TYPE_COLORS[data.action] || NODE_TYPE_COLORS.output;
  }
  return NODE_TYPE_COLORS[props.node.type] || '#6B7280';
});

const nodeLabel = computed(() => {
  switch (props.node.type) {
    case 'atomic': {
      const data = props.node.data as AtomicNodeData;
      return data.label || props.atomicRuleName || data.rule_id;
    }
    case 'threshold': {
      const data = props.node.data as ThresholdNodeData;
      return `${data.field} ${data.operator} ${data.threshold}`;
    }
    case 'output': {
      const data = props.node.data as OutputNodeData;
      return data.action.toUpperCase();
    }
    case 'constant': {
      const data = props.node.data as { value: boolean };
      return data.value ? 'TRUE' : 'FALSE';
    }
    default:
      return NODE_TYPE_LABELS[props.node.type] || props.node.type;
  }
});

const nodeTypeLabel = computed(() => NODE_TYPE_LABELS[props.node.type] || props.node.type);

const hasInput = computed(() => {
  return ['and', 'or', 'not', 'output'].includes(props.node.type);
});

const hasOutput = computed(() => {
  return ['atomic', 'and', 'or', 'not', 'constant', 'threshold'].includes(props.node.type);
});

function onSelect() {
  emit('select', props.node.id);
}

function onDelete(e: Event) {
  e.stopPropagation();
  emit('delete', props.node.id);
}

function onDragStart(e: DragEvent) {
  emit('dragstart', e, props.node.id);
}
</script>

<template>
  <div
    class="rule-graph-node"
    :class="{ selected, active }"
    :style="{
      left: `${node.position.x}px`,
      top: `${node.position.y}px`,
      borderColor: nodeColor,
    }"
    @click="onSelect"
    draggable="true"
    @dragstart="onDragStart"
  >
    <div class="node-header" :style="{ background: nodeColor }">
      <span class="node-type">{{ nodeTypeLabel }}</span>
      <button class="delete-btn" @click="onDelete" title="Delete node">×</button>
    </div>

    <div class="node-body">
      <div class="node-label">{{ nodeLabel }}</div>

      <div class="node-sockets">
        <div v-if="hasInput" class="socket input" :data-node="node.id" data-socket="input">
          <span class="socket-label">IN</span>
        </div>
        <div v-if="hasOutput" class="socket output" :data-node="node.id" data-socket="output">
          <span class="socket-label">OUT</span>
        </div>
      </div>
    </div>

    <div v-if="active" class="active-indicator" title="Currently triggered"></div>
  </div>
</template>

<style scoped>
.rule-graph-node {
  position: absolute;
  min-width: 140px;
  background: #1E293B;
  border: 2px solid #6366F1;
  border-radius: 8px;
  cursor: move;
  user-select: none;
  box-shadow: 0 2px 8px rgba(0, 0, 0, 0.3);
  transition: box-shadow 0.2s;
}

.rule-graph-node:hover {
  box-shadow: 0 4px 16px rgba(0, 0, 0, 0.4);
}

.rule-graph-node.selected {
  box-shadow: 0 0 0 3px rgba(99, 102, 241, 0.4);
}

.rule-graph-node.active {
  animation: pulse 1s ease-in-out infinite;
}

@keyframes pulse {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.8; }
}

.node-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 4px 8px;
  border-radius: 6px 6px 0 0;
  font-size: 0.7rem;
  font-weight: 600;
  text-transform: uppercase;
  color: white;
}

.delete-btn {
  background: transparent;
  border: none;
  color: white;
  font-size: 1rem;
  cursor: pointer;
  opacity: 0.6;
  line-height: 1;
  padding: 0 2px;
}

.delete-btn:hover {
  opacity: 1;
}

.node-body {
  padding: 8px;
}

.node-label {
  font-size: 0.85rem;
  color: #E2E8F0;
  margin-bottom: 8px;
  word-break: break-word;
}

.node-sockets {
  display: flex;
  justify-content: space-between;
}

.socket {
  display: flex;
  align-items: center;
  gap: 4px;
  cursor: crosshair;
}

.socket.input::before,
.socket.output::after {
  content: '';
  width: 10px;
  height: 10px;
  border-radius: 50%;
  background: #6B7280;
  border: 2px solid #374151;
}

.socket.input::before {
  margin-left: -16px;
}

.socket.output::after {
  margin-right: -16px;
}

.socket:hover::before,
.socket:hover::after {
  background: #22D3EE;
  border-color: #06B6D4;
}

.socket-label {
  font-size: 0.65rem;
  color: #94A3B8;
  text-transform: uppercase;
}

.active-indicator {
  position: absolute;
  top: -4px;
  right: -4px;
  width: 12px;
  height: 12px;
  background: #22C55E;
  border-radius: 50%;
  border: 2px solid #1E293B;
}
</style>
