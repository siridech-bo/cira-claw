<script setup lang="ts">
import { ref, computed, watch, onMounted, onUnmounted } from 'vue';
import RuleGraphNode from './RuleGraphNode.vue';
import type { CompositeNode, CompositeConnection, AtomicRule } from '../../composables/types';
import { useRuleEvaluation } from '../../composables/useRuleEvaluation';

interface Props {
  nodes: CompositeNode[];
  connections: CompositeConnection[];
  atomicRules: AtomicRule[];
  selectedNodeId?: string | null;
}

const props = withDefaults(defineProps<Props>(), {
  selectedNodeId: null,
});

const emit = defineEmits<{
  (e: 'selectNode', nodeId: string | null): void;
  (e: 'deleteNode', nodeId: string): void;
  (e: 'moveNode', nodeId: string, x: number, y: number): void;
  (e: 'addConnection', source: string, sourceSocket: string, target: string, targetSocket: string): void;
  (e: 'deleteConnection', connectionId: string): void;
}>();

const { isAtomicRuleActive } = useRuleEvaluation();

const canvasRef = ref<HTMLElement | null>(null);
const svgRef = ref<SVGSVGElement | null>(null);

// Drag state
const draggingNode = ref<string | null>(null);
const dragOffset = ref({ x: 0, y: 0 });

// Connection drawing state
const drawingConnection = ref(false);
const connectionStart = ref<{ nodeId: string; socket: string; x: number; y: number } | null>(null);
const connectionEnd = ref<{ x: number; y: number }>({ x: 0, y: 0 });

// Get atomic rule name for display
function getAtomicRuleName(ruleId: string): string {
  const rule = props.atomicRules.find(r => r.id === ruleId);
  return rule?.name || ruleId;
}

// Check if a node is currently active
function isNodeActive(node: CompositeNode): boolean {
  if (node.type === 'atomic') {
    const data = node.data as { rule_id: string };
    return isAtomicRuleActive(data.rule_id);
  }
  return false;
}

// Calculate SVG path for connection
function getConnectionPath(conn: CompositeConnection): string {
  const sourceNode = props.nodes.find(n => n.id === conn.source_node);
  const targetNode = props.nodes.find(n => n.id === conn.target_node);

  if (!sourceNode || !targetNode) return '';

  // Calculate socket positions (approximate)
  const sourceX = sourceNode.position.x + 140; // Right side
  const sourceY = sourceNode.position.y + 50;  // Middle
  const targetX = targetNode.position.x;        // Left side
  const targetY = targetNode.position.y + 50;   // Middle

  // Bezier curve
  const midX = (sourceX + targetX) / 2;
  return `M ${sourceX} ${sourceY} C ${midX} ${sourceY}, ${midX} ${targetY}, ${targetX} ${targetY}`;
}

// Handle node selection
function onSelectNode(nodeId: string) {
  emit('selectNode', nodeId);
}

// Handle canvas click (deselect)
function onCanvasClick() {
  emit('selectNode', null);
}

// Handle node deletion
function onDeleteNode(nodeId: string) {
  emit('deleteNode', nodeId);
}

// Handle node drag
function onDragStart(e: DragEvent, nodeId: string) {
  if (!e.dataTransfer) return;

  draggingNode.value = nodeId;
  const node = props.nodes.find(n => n.id === nodeId);
  if (node && canvasRef.value) {
    const rect = canvasRef.value.getBoundingClientRect();
    dragOffset.value = {
      x: e.clientX - rect.left - node.position.x,
      y: e.clientY - rect.top - node.position.y,
    };
  }

  e.dataTransfer.effectAllowed = 'move';
  e.dataTransfer.setData('text/plain', nodeId);

  // Create transparent drag image
  const img = new Image();
  img.src = 'data:image/gif;base64,R0lGODlhAQABAIAAAAAAAP///yH5BAEAAAAALAAAAAABAAEAAAIBRAA7';
  e.dataTransfer.setDragImage(img, 0, 0);
}

function onDragOver(e: DragEvent) {
  e.preventDefault();
  if (!draggingNode.value || !canvasRef.value) return;

  const rect = canvasRef.value.getBoundingClientRect();
  const x = Math.max(0, e.clientX - rect.left - dragOffset.value.x);
  const y = Math.max(0, e.clientY - rect.top - dragOffset.value.y);

  emit('moveNode', draggingNode.value, x, y);
}

function onDragEnd() {
  draggingNode.value = null;
}

// Handle connection drawing
function onSocketMouseDown(e: MouseEvent) {
  const target = e.target as HTMLElement;
  const socket = target.closest('.socket');
  if (!socket) return;

  const nodeId = socket.getAttribute('data-node');
  const socketName = socket.getAttribute('data-socket');

  if (!nodeId || !socketName) return;

  // Only start connection from output sockets
  if (!socket.classList.contains('output')) return;

  e.preventDefault();
  e.stopPropagation();

  const rect = canvasRef.value?.getBoundingClientRect();
  if (!rect) return;

  const node = props.nodes.find(n => n.id === nodeId);
  if (!node) return;

  drawingConnection.value = true;
  connectionStart.value = {
    nodeId,
    socket: socketName,
    x: node.position.x + 140,
    y: node.position.y + 50,
  };
  connectionEnd.value = {
    x: e.clientX - rect.left,
    y: e.clientY - rect.top,
  };
}

function onCanvasMouseMove(e: MouseEvent) {
  if (!drawingConnection.value || !canvasRef.value) return;

  const rect = canvasRef.value.getBoundingClientRect();
  connectionEnd.value = {
    x: e.clientX - rect.left,
    y: e.clientY - rect.top,
  };
}

function onCanvasMouseUp(e: MouseEvent) {
  if (!drawingConnection.value || !connectionStart.value) {
    drawingConnection.value = false;
    connectionStart.value = null;
    return;
  }

  // Check if we're over an input socket
  const target = e.target as HTMLElement;
  const socket = target.closest('.socket.input');

  if (socket) {
    const targetNodeId = socket.getAttribute('data-node');
    const targetSocketName = socket.getAttribute('data-socket');

    if (targetNodeId && targetSocketName && targetNodeId !== connectionStart.value.nodeId) {
      emit(
        'addConnection',
        connectionStart.value.nodeId,
        connectionStart.value.socket,
        targetNodeId,
        targetSocketName
      );
    }
  }

  drawingConnection.value = false;
  connectionStart.value = null;
}

// Connection path for drawing
const drawingPath = computed(() => {
  if (!drawingConnection.value || !connectionStart.value) return '';

  const startX = connectionStart.value.x;
  const startY = connectionStart.value.y;
  const endX = connectionEnd.value.x;
  const endY = connectionEnd.value.y;

  const midX = (startX + endX) / 2;
  return `M ${startX} ${startY} C ${midX} ${startY}, ${midX} ${endY}, ${endX} ${endY}`;
});

// Handle connection click (delete)
function onConnectionClick(connectionId: string) {
  emit('deleteConnection', connectionId);
}

onMounted(() => {
  document.addEventListener('mouseup', onCanvasMouseUp);
});

onUnmounted(() => {
  document.removeEventListener('mouseup', onCanvasMouseUp);
});
</script>

<template>
  <div
    ref="canvasRef"
    class="rule-graph-canvas"
    @click.self="onCanvasClick"
    @dragover="onDragOver"
    @drop="onDragEnd"
    @mousedown="onSocketMouseDown"
    @mousemove="onCanvasMouseMove"
  >
    <!-- SVG layer for connections -->
    <svg ref="svgRef" class="connections-layer">
      <!-- Existing connections -->
      <g v-for="conn in connections" :key="conn.id">
        <path
          :d="getConnectionPath(conn)"
          class="connection"
          @click="onConnectionClick(conn.id)"
        />
      </g>

      <!-- Drawing connection -->
      <path
        v-if="drawingConnection"
        :d="drawingPath"
        class="connection drawing"
      />
    </svg>

    <!-- Nodes -->
    <RuleGraphNode
      v-for="node in nodes"
      :key="node.id"
      :node="node"
      :selected="node.id === selectedNodeId"
      :active="isNodeActive(node)"
      :atomic-rule-name="node.type === 'atomic' ? getAtomicRuleName((node.data as any).rule_id) : ''"
      @select="onSelectNode"
      @delete="onDeleteNode"
      @dragstart="onDragStart"
    />

    <!-- Empty state -->
    <div v-if="nodes.length === 0" class="empty-state">
      <p>No nodes yet</p>
      <p class="hint">Add nodes from the sidebar to build your rule</p>
    </div>
  </div>
</template>

<style scoped>
.rule-graph-canvas {
  position: relative;
  flex: 1;
  min-height: 500px;
  background: #0F172A;
  background-image:
    linear-gradient(rgba(99, 102, 241, 0.05) 1px, transparent 1px),
    linear-gradient(90deg, rgba(99, 102, 241, 0.05) 1px, transparent 1px);
  background-size: 20px 20px;
  border-radius: 8px;
  overflow: hidden;
}

.connections-layer {
  position: absolute;
  top: 0;
  left: 0;
  width: 100%;
  height: 100%;
  pointer-events: none;
}

.connection {
  fill: none;
  stroke: #6366F1;
  stroke-width: 2;
  pointer-events: stroke;
  cursor: pointer;
}

.connection:hover {
  stroke: #EF4444;
  stroke-width: 3;
}

.connection.drawing {
  stroke: #22D3EE;
  stroke-dasharray: 5;
}

.empty-state {
  position: absolute;
  top: 50%;
  left: 50%;
  transform: translate(-50%, -50%);
  text-align: center;
  color: #64748B;
}

.empty-state p {
  margin: 0;
}

.empty-state .hint {
  font-size: 0.875rem;
  margin-top: 8px;
}
</style>
