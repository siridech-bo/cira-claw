/**
 * useRuleEditor - Composable for managing composite rules with Rete.js editor
 *
 * Handles:
 * - Loading composite rules from API
 * - Saving composite rules to API
 * - Managing Rete.js editor state
 * - Node and connection operations
 */

import { ref, computed, readonly, shallowRef, type Ref } from 'vue';
import { NodeEditor, ClassicPreset } from 'rete';
import { AreaPlugin, AreaExtensions } from 'rete-area-plugin';
import { VuePlugin, Presets as VuePresets } from 'rete-vue-plugin';
import { ConnectionPlugin, Presets as ConnectionPresets } from 'rete-connection-plugin';

import {
  RETE_SOCKETS,
  BOOLEAN_ANY_SOCKET,
  TIME_WINDOW_SOCKET,
  CONTEXT_SOCKET,
  isCompatible,
  getSocketByName,
} from './socketSetup';
import type { SocketType } from '@gateway/socket-registry';
import type {
  CompositeRule,
  CompositeNode,
  CompositeConnection,
  AtomicRule,
  OutputAction,
  AtomicNodeData,
  StatefulConditionNodeData,
  OutputNodeData,
  ConstantNodeData,
  ThresholdNodeData,
} from './types';

// Import Vue node components
import AtomicRuleNodeVue from '../components/rule-graph/AtomicRuleNodeVue.vue';
import OperatorNodeVue from '../components/rule-graph/OperatorNodeVue.vue';
import ActionNodeVue from '../components/rule-graph/ActionNodeVue.vue';
import StatefulConditionNodeVue from '../components/rule-graph/StatefulConditionNodeVue.vue';
import SocketVue from '../components/rule-graph/SocketVue.vue';

// ─── Rete Node Data Classes ────────────────────────────────────────────────────
// These are DATA classes — not Vue components.
// They define the Rete graph structure and port types.
// Vue components are assigned separately via customize.

type Schemes = ClassicPreset.LabeledSchemes;

export class AtomicRuleNode extends ClassicPreset.Node {
  constructor(
    public ruleId: string,
    public ruleName: string,
    public socketType: SocketType,
    public reads: string[]
  ) {
    super(ruleName);
    const socket = RETE_SOCKETS[socketType] ?? BOOLEAN_ANY_SOCKET;
    this.addOutput('out', new ClassicPreset.Output(socket, 'Result'));
  }
}

export class OperatorNode extends ClassicPreset.Node {
  constructor(public operator: 'AND' | 'OR' | 'NOT') {
    super(operator);
    this.addInput('in1', new ClassicPreset.Input(BOOLEAN_ANY_SOCKET, 'A'));
    if (operator !== 'NOT') {
      this.addInput('in2', new ClassicPreset.Input(BOOLEAN_ANY_SOCKET, 'B'));
    }
    this.addOutput('out', new ClassicPreset.Output(BOOLEAN_ANY_SOCKET, 'Result'));
  }
}

export class ActionNode extends ClassicPreset.Node {
  constructor(public action: string, public config: Record<string, unknown> = {}) {
    super(action.toUpperCase());
    this.addInput('in', new ClassicPreset.Input(BOOLEAN_ANY_SOCKET, 'Execute'));
  }
}

export class StatefulConditionNode extends ClassicPreset.Node {
  constructor(
    public config: {
      condition: 'count_window' | 'consecutive' | 'rate' | 'sustained' | 'cooldown';
      accepts_socket_type: SocketType;
      count: number;
      window_minutes: number;
    }
  ) {
    super('Stateful Condition');
    const inputSocket = RETE_SOCKETS[config.accepts_socket_type] ?? BOOLEAN_ANY_SOCKET;
    this.addInput('in', new ClassicPreset.Input(inputSocket, config.accepts_socket_type));
    this.addOutput('out', new ClassicPreset.Output(TIME_WINDOW_SOCKET, 'time.window'));
  }
}

export class ConstantNode extends ClassicPreset.Node {
  constructor(public value: boolean) {
    super(value ? 'TRUE' : 'FALSE');
    this.addOutput('out', new ClassicPreset.Output(BOOLEAN_ANY_SOCKET, 'Value'));
  }
}

export class ThresholdNode extends ClassicPreset.Node {
  constructor(
    public field: string,
    public operator: string,
    public threshold: number
  ) {
    super(`${field} ${operator} ${threshold}`);
    this.addOutput('out', new ClassicPreset.Output(BOOLEAN_ANY_SOCKET, 'Result'));
  }
}

// ─── Rete Types ──────────────────────────────────────────────────────────────

export type ReteArea = AreaPlugin<Schemes, any>;

// ─── Editor factory ─────────────────────────────────────────────────────────────

export async function createReteEditor(container: HTMLElement): Promise<{
  editor: NodeEditor<Schemes>;
  area: ReteArea;
  destroy: () => void;
}> {
  console.log('createReteEditor: initializing with container:', {
    tagName: container.tagName,
    clientWidth: container.clientWidth,
    clientHeight: container.clientHeight,
    offsetWidth: container.offsetWidth,
    offsetHeight: container.offsetHeight,
  });

  const editor = new NodeEditor<Schemes>();
  const area = new AreaPlugin<Schemes, any>(container);
  const render = new VuePlugin<Schemes, any>();
  const connection = new ConnectionPlugin<Schemes, any>();

  // Socket compatibility enforcement
  // This is the core of the spec — incompatible sockets snap back.
  connection.addPreset(ConnectionPresets.classic.setup());

  // Custom Vue renderers for nodes and sockets
  render.addPreset(
    VuePresets.classic.setup({
      customize: {
        node(context) {
          if (context.payload instanceof AtomicRuleNode)        return AtomicRuleNodeVue;
          if (context.payload instanceof OperatorNode)          return OperatorNodeVue;
          if (context.payload instanceof ActionNode)            return ActionNodeVue;
          if (context.payload instanceof StatefulConditionNode) return StatefulConditionNodeVue;
          return VuePresets.classic.Node;
        },
        socket() {
          return SocketVue;
        },
      },
    })
  );

  editor.use(area);
  area.use(render);
  area.use(connection);

  // ── Socket compatibility enforcement via pipe ──────────────────────────
  // Intercepts every connectioncreate event before it is committed.
  // Returning undefined cancels the connection — edge snaps back.
  // Returning context allows it through.
  editor.addPipe((context) => {
    if (context.type !== 'connectioncreate') return context;

    const { data } = context;
    const sourceNode = editor.getNode(data.source);
    const targetNode = editor.getNode(data.target);

    if (sourceNode && targetNode) {
      const output = sourceNode.outputs[data.sourceOutput];
      const input  = targetNode.inputs[data.targetInput];

      if (output?.socket && input?.socket) {
        const allowed = isCompatible(
          output.socket as ClassicPreset.Socket,
          input.socket  as ClassicPreset.Socket
        );
        if (!allowed) return undefined; // reject — snap back
      }
    }

    return context; // allow
  });

  // Set up zoom and node ordering
  AreaExtensions.zoomAt(area, editor.getNodes());
  AreaExtensions.simpleNodesOrder(area);

  // ── Sync connection events to Vue state ──────────────────────────────
  // Listen for connection create/remove events and update the rule's connections array
  editor.addPipe((context) => {
    if (context.type === 'connectioncreated') {
      const conn = context.data;
      console.log('connectioncreated:', conn);
      // Add to Vue state
      if (currentRuleId.value) {
        const rule = compositeRules.value.find(r => r.id === currentRuleId.value);
        if (rule) {
          // Check if connection already exists (avoid duplicates)
          const exists = rule.connections.some(
            c =>
              c.source_node === conn.source &&
              c.source_socket === conn.sourceOutput &&
              c.target_node === conn.target &&
              c.target_socket === conn.targetInput
          );
          if (!exists) {
            rule.connections.push({
              id: `conn-${Date.now()}-${Math.random().toString(36).substr(2, 9)}`,
              source_node: conn.source,
              source_socket: conn.sourceOutput,
              target_node: conn.target,
              target_socket: conn.targetInput,
            });
            dirty.value = true;
            console.log('Connection added to Vue state, total:', rule.connections.length);
          }
        }
      }
    }

    if (context.type === 'connectionremoved') {
      const conn = context.data;
      console.log('connectionremoved:', conn);
      // Remove from Vue state
      if (currentRuleId.value) {
        const rule = compositeRules.value.find(r => r.id === currentRuleId.value);
        if (rule) {
          const idx = rule.connections.findIndex(
            c =>
              c.source_node === conn.source &&
              c.source_socket === conn.sourceOutput &&
              c.target_node === conn.target &&
              c.target_socket === conn.targetInput
          );
          if (idx >= 0) {
            rule.connections.splice(idx, 1);
            dirty.value = true;
            console.log('Connection removed from Vue state, total:', rule.connections.length);
          }
        }
      }
    }

    return context;
  });

  return {
    editor,
    area,
    destroy: () => area.destroy(), // MUST be called in onUnmounted
  };
}

// ─── State ───────────────────────────────────────────────────────────────────

const compositeRules = ref<CompositeRule[]>([]);
const atomicRules = ref<AtomicRule[]>([]);
const currentRuleId = ref<string | null>(null);
const loading = ref(false);
const error = ref<string | null>(null);
const dirty = ref(false);

// Rete editor refs
const reteEditor = shallowRef<NodeEditor<Schemes> | null>(null);
const reteArea = shallowRef<ReteArea | null>(null);

// Undo stack for deleted nodes (stores node + its connections)
interface DeletedNodeEntry {
  ruleId: string;
  node: CompositeNode;
  connections: CompositeConnection[];
}
const undoStack = ref<DeletedNodeEntry[]>([]);
const MAX_UNDO_STACK = 20;

// ─── Computed ────────────────────────────────────────────────────────────────

const currentRule = computed(() => {
  if (!currentRuleId.value) return null;
  return compositeRules.value.find(r => r.id === currentRuleId.value) || null;
});

const enabledAtomicRules = computed(() => {
  return atomicRules.value.filter(r => r.enabled);
});

// ─── API Operations ──────────────────────────────────────────────────────────

async function loadAtomicRules(): Promise<void> {
  try {
    const response = await fetch('/api/rules');
    if (!response.ok) {
      throw new Error(`Failed to load atomic rules: ${response.statusText}`);
    }
    const data = await response.json();
    atomicRules.value = data.rules || [];
  } catch (err) {
    error.value = err instanceof Error ? err.message : 'Failed to load atomic rules';
    throw err;
  }
}

async function loadCompositeRules(): Promise<void> {
  try {
    loading.value = true;
    error.value = null;

    const response = await fetch('/api/composite-rules');
    if (!response.ok) {
      throw new Error(`Failed to load composite rules: ${response.statusText}`);
    }
    const data = await response.json();
    compositeRules.value = data.rules || [];
  } catch (err) {
    error.value = err instanceof Error ? err.message : 'Failed to load composite rules';
    throw err;
  } finally {
    loading.value = false;
  }
}

async function loadAll(): Promise<void> {
  loading.value = true;
  error.value = null;

  try {
    await Promise.all([loadAtomicRules(), loadCompositeRules()]);
  } finally {
    loading.value = false;
  }
}

async function saveCompositeRule(rule: CompositeRule): Promise<void> {
  try {
    loading.value = true;
    error.value = null;

    // Check if this is a new rule or update
    const existingRule = compositeRules.value.find(r => r.id === rule.id);
    const method = existingRule ? 'PUT' : 'POST';
    const url = existingRule ? `/api/composite-rules/${rule.id}` : '/api/composite-rules';

    const response = await fetch(url, {
      method,
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(rule),
    });

    if (!response.ok) {
      const data = await response.json();
      throw new Error(data.message || data.error || `Failed to save rule: ${response.statusText}`);
    }

    // Update local state
    const index = compositeRules.value.findIndex(r => r.id === rule.id);
    if (index >= 0) {
      compositeRules.value[index] = rule;
    } else {
      compositeRules.value.push(rule);
    }

    dirty.value = false;
  } catch (err) {
    error.value = err instanceof Error ? err.message : 'Failed to save rule';
    throw err;
  } finally {
    loading.value = false;
  }
}

async function deleteCompositeRule(id: string): Promise<void> {
  try {
    loading.value = true;
    error.value = null;

    const response = await fetch(`/api/composite-rules/${id}`, {
      method: 'DELETE',
    });

    if (!response.ok) {
      throw new Error(`Failed to delete rule: ${response.statusText}`);
    }

    // Update local state
    compositeRules.value = compositeRules.value.filter(r => r.id !== id);

    if (currentRuleId.value === id) {
      currentRuleId.value = null;
    }
  } catch (err) {
    error.value = err instanceof Error ? err.message : 'Failed to delete rule';
    throw err;
  } finally {
    loading.value = false;
  }
}

// ─── Editor Operations ───────────────────────────────────────────────────────

function setReteEditor(editor: NodeEditor<Schemes> | null, area: ReteArea | null): void {
  reteEditor.value = editor;
  reteArea.value = area;
}

function selectRule(id: string | null): void {
  currentRuleId.value = id;
  dirty.value = false;
}

function createNewRule(): CompositeRule {
  const id = `composite-${Date.now()}`;
  const rule: CompositeRule = {
    id,
    name: 'New Composite Rule',
    description: '',
    enabled: false, // Start disabled until configured
    created_at: new Date().toISOString(),
    created_by: 'dashboard',
    nodes: [],
    connections: [],
    output_action: { action: 'pass' },
  };

  compositeRules.value.push(rule);
  currentRuleId.value = id;
  dirty.value = true;

  return rule;
}

function updateCurrentRule(updates: Partial<CompositeRule>): void {
  if (!currentRuleId.value) return;

  const rule = compositeRules.value.find(r => r.id === currentRuleId.value);
  if (rule) {
    Object.assign(rule, updates);
    dirty.value = true;
  }
}

async function addNode(node: CompositeNode): Promise<void> {
  if (!currentRuleId.value) {
    console.warn('addNode: no current rule selected');
    return;
  }

  const rule = compositeRules.value.find(r => r.id === currentRuleId.value);
  if (rule) {
    rule.nodes.push(node);
    dirty.value = true;
    console.log(`addNode: added node ${node.id} (type: ${node.type}) to rule state`);

    // Also add to Rete.js editor if available
    if (reteEditor.value && reteArea.value) {
      try {
        const reteNode = createReteNode(node);
        if (reteNode) {
          console.log(`addNode: created Rete node:`, {
            id: reteNode.id,
            label: reteNode.label,
            inputs: Object.keys(reteNode.inputs),
            outputs: Object.keys(reteNode.outputs),
          });
          await reteEditor.value.addNode(reteNode);
          console.log(`addNode: node added to editor, total nodes: ${reteEditor.value.getNodes().length}`);
          // Position the node
          await reteArea.value.translate(reteNode.id, { x: node.position.x, y: node.position.y });
          console.log(`addNode: positioned node at (${node.position.x}, ${node.position.y})`);

          // DEBUG: Inspect DOM structure after adding node
          setTimeout(async () => {
            const areaContainer = (reteArea.value as any)?.container;
            if (areaContainer) {
              console.log('addNode: DOM inspection:', {
                containerTagName: areaContainer.tagName,
                childCount: areaContainer.children.length,
                childTags: Array.from(areaContainer.children).map((c: any) => c.tagName),
                firstChildInnerHTML: areaContainer.children[0]?.innerHTML?.substring(0, 200) || 'none',
              });
            }
            // Try to zoom to show all nodes
            if (reteArea.value && reteEditor.value) {
              const nodes = reteEditor.value.getNodes();
              if (nodes.length > 0) {
                await AreaExtensions.zoomAt(reteArea.value, nodes);
                console.log('addNode: zoomed to show nodes');
              }
            }
          }, 100);
        } else {
          console.warn(`addNode: createReteNode returned null for node type ${node.type}`);
        }
      } catch (err) {
        console.error('addNode: error adding node to Rete editor:', err);
      }
    } else {
      console.warn('addNode: Rete editor not available', {
        hasEditor: !!reteEditor.value,
        hasArea: !!reteArea.value,
      });
    }
  }
}

/**
 * Create a Rete.js node from a CompositeNode definition.
 * IMPORTANT: Sets the Rete node ID to match the CompositeNode ID for synchronization.
 */
function createReteNode(node: CompositeNode): AtomicRuleNode | OperatorNode | ActionNode | StatefulConditionNode | ConstantNode | ThresholdNode | null {
  let reteNode: AtomicRuleNode | OperatorNode | ActionNode | StatefulConditionNode | ConstantNode | ThresholdNode | null = null;

  switch (node.type) {
    case 'atomic': {
      const data = node.data as AtomicNodeData;
      reteNode = new AtomicRuleNode(
        data.rule_id,
        data.label || data.rule_id,
        data.socket_type,
        []
      );
      break;
    }
    case 'and':
      reteNode = new OperatorNode('AND');
      break;
    case 'or':
      reteNode = new OperatorNode('OR');
      break;
    case 'not':
      reteNode = new OperatorNode('NOT');
      break;
    case 'output': {
      const data = node.data as OutputNodeData;
      reteNode = new ActionNode(data.action, data);
      break;
    }
    case 'stateful_condition': {
      const data = node.data as StatefulConditionNodeData;
      reteNode = new StatefulConditionNode(data);
      break;
    }
    case 'constant': {
      const data = node.data as ConstantNodeData;
      reteNode = new ConstantNode(data.value);
      break;
    }
    case 'threshold': {
      const data = node.data as ThresholdNodeData;
      reteNode = new ThresholdNode(data.field, data.operator, data.threshold);
      break;
    }
    default:
      console.warn(`Unknown node type: ${node.type}`);
      return null;
  }

  // CRITICAL: Set the Rete node ID to match the CompositeNode ID
  // This ensures synchronization between Vue state and Rete graph
  if (reteNode) {
    reteNode.id = node.id;
  }

  return reteNode;
}

function updateNode(nodeId: string, updates: Partial<CompositeNode>): void {
  if (!currentRuleId.value) return;

  const rule = compositeRules.value.find(r => r.id === currentRuleId.value);
  if (rule) {
    const node = rule.nodes.find(n => n.id === nodeId);
    if (node) {
      Object.assign(node, updates);
      dirty.value = true;
    }
  }
}

async function removeNode(nodeId: string): Promise<void> {
  if (!currentRuleId.value) return;

  const rule = compositeRules.value.find(r => r.id === currentRuleId.value);
  if (rule) {
    // Find the node to delete
    const nodeToDelete = rule.nodes.find(n => n.id === nodeId);
    if (!nodeToDelete) return;

    // Find connections involving this node (for undo)
    const connectionsToDelete = rule.connections.filter(
      c => c.source_node === nodeId || c.target_node === nodeId
    );

    // Save to undo stack
    undoStack.value.push({
      ruleId: currentRuleId.value,
      node: JSON.parse(JSON.stringify(nodeToDelete)), // Deep clone
      connections: JSON.parse(JSON.stringify(connectionsToDelete)),
    });

    // Trim undo stack if too large
    if (undoStack.value.length > MAX_UNDO_STACK) {
      undoStack.value.shift();
    }

    // Remove from Vue state
    rule.nodes = rule.nodes.filter(n => n.id !== nodeId);
    rule.connections = rule.connections.filter(
      c => c.source_node !== nodeId && c.target_node !== nodeId
    );
    dirty.value = true;

    // Remove from Rete editor
    if (reteEditor.value) {
      try {
        // First remove all connections involving this node
        const reteConnections = reteEditor.value.getConnections();
        for (const conn of reteConnections) {
          if (conn.source === nodeId || conn.target === nodeId) {
            await reteEditor.value.removeConnection(conn.id);
          }
        }
        // Then remove the node
        await reteEditor.value.removeNode(nodeId);
        console.log(`removeNode: removed node ${nodeId} from Rete editor`);
      } catch (err) {
        console.error('removeNode: error removing from Rete editor:', err);
      }
    }
  }
}

async function undoDeleteNode(): Promise<CompositeNode | null> {
  if (undoStack.value.length === 0) {
    console.log('undoDeleteNode: nothing to undo');
    return null;
  }

  const entry = undoStack.value.pop()!;

  // Check if we're still on the same rule
  if (currentRuleId.value !== entry.ruleId) {
    console.warn('undoDeleteNode: rule context changed, cannot undo');
    return null;
  }

  const rule = compositeRules.value.find(r => r.id === currentRuleId.value);
  if (!rule) return null;

  // Restore the node
  rule.nodes.push(entry.node);

  // Restore connections (only those whose other endpoint still exists)
  for (const conn of entry.connections) {
    const otherNodeId = conn.source_node === entry.node.id ? conn.target_node : conn.source_node;
    const otherNodeExists = rule.nodes.some(n => n.id === otherNodeId);
    if (otherNodeExists) {
      rule.connections.push(conn);
    }
  }

  dirty.value = true;
  console.log(`undoDeleteNode: restored node ${entry.node.id}`);

  // Add back to Rete editor
  if (reteEditor.value && reteArea.value) {
    try {
      const reteNode = createReteNode(entry.node);
      if (reteNode) {
        await reteEditor.value.addNode(reteNode);
        await reteArea.value.translate(reteNode.id, {
          x: entry.node.position.x,
          y: entry.node.position.y,
        });
        console.log(`undoDeleteNode: re-added node to Rete editor at (${entry.node.position.x}, ${entry.node.position.y})`);
      }
    } catch (err) {
      console.error('undoDeleteNode: error adding node to Rete editor:', err);
    }
  }

  return entry.node;
}

function canUndo(): boolean {
  return undoStack.value.length > 0 && undoStack.value[undoStack.value.length - 1]?.ruleId === currentRuleId.value;
}

function clearUndoStack(): void {
  undoStack.value = [];
}

function addConnection(connection: CompositeConnection): void {
  if (!currentRuleId.value) return;

  const rule = compositeRules.value.find(r => r.id === currentRuleId.value);
  if (rule) {
    // Check for duplicate
    const exists = rule.connections.some(
      c =>
        c.source_node === connection.source_node &&
        c.source_socket === connection.source_socket &&
        c.target_node === connection.target_node &&
        c.target_socket === connection.target_socket
    );

    if (!exists) {
      rule.connections.push(connection);
      dirty.value = true;
    }
  }
}

function removeConnection(connectionId: string): void {
  if (!currentRuleId.value) return;

  const rule = compositeRules.value.find(r => r.id === currentRuleId.value);
  if (rule) {
    rule.connections = rule.connections.filter(c => c.id !== connectionId);
    dirty.value = true;
  }
}

function updateOutputAction(action: OutputAction): void {
  if (!currentRuleId.value) return;

  const rule = compositeRules.value.find(r => r.id === currentRuleId.value);
  if (rule) {
    rule.output_action = action;
    dirty.value = true;
  }
}

// ─── Utility ─────────────────────────────────────────────────────────────────

function generateNodeId(): string {
  return `node-${Date.now()}-${Math.random().toString(36).substr(2, 9)}`;
}

function generateConnectionId(): string {
  return `conn-${Date.now()}-${Math.random().toString(36).substr(2, 9)}`;
}

function getAtomicRule(id: string): AtomicRule | undefined {
  return atomicRules.value.find(r => r.id === id);
}

/**
 * Sync all nodes from the current rule to the Rete.js editor.
 * Call this after initializing the editor when a rule is selected.
 */
async function syncRuleToEditor(): Promise<void> {
  if (!currentRule.value) {
    console.log('syncRuleToEditor: no current rule');
    return;
  }
  if (!reteEditor.value) {
    console.log('syncRuleToEditor: no Rete editor');
    return;
  }
  if (!reteArea.value) {
    console.log('syncRuleToEditor: no Rete area');
    return;
  }

  const rule = currentRule.value;
  console.log(`syncRuleToEditor: syncing ${rule.nodes.length} nodes and ${rule.connections.length} connections to Rete editor`);

  // Add all nodes to the editor
  for (const node of rule.nodes) {
    const reteNode = createReteNode(node);
    if (reteNode) {
      console.log(`syncRuleToEditor: adding node ${reteNode.id} (type: ${node.type})`);
      await reteEditor.value.addNode(reteNode);
      await reteArea.value.translate(reteNode.id, { x: node.position.x, y: node.position.y });
    } else {
      console.warn(`syncRuleToEditor: createReteNode returned null for node type ${node.type}`);
    }
  }

  // Add all connections to the editor
  for (const conn of rule.connections) {
    try {
      const sourceNode = reteEditor.value.getNode(conn.source_node);
      const targetNode = reteEditor.value.getNode(conn.target_node);

      if (sourceNode && targetNode) {
        const sourceOutput = sourceNode.outputs[conn.source_socket];
        const targetInput = targetNode.inputs[conn.target_socket];

        if (sourceOutput && targetInput) {
          const reteConnection = new ClassicPreset.Connection(sourceNode, conn.source_socket, targetNode, conn.target_socket);
          await reteEditor.value.addConnection(reteConnection);
          console.log(`syncRuleToEditor: added connection ${conn.source_node}:${conn.source_socket} -> ${conn.target_node}:${conn.target_socket}`);
        } else {
          console.warn(`syncRuleToEditor: missing socket for connection`, {
            sourceSocket: conn.source_socket,
            targetSocket: conn.target_socket,
            availableOutputs: Object.keys(sourceNode.outputs),
            availableInputs: Object.keys(targetNode.inputs),
          });
        }
      } else {
        console.warn(`syncRuleToEditor: missing node for connection`, {
          sourceNodeId: conn.source_node,
          targetNodeId: conn.target_node,
          sourceExists: !!sourceNode,
          targetExists: !!targetNode,
        });
      }
    } catch (err) {
      console.error('syncRuleToEditor: error adding connection:', err, conn);
    }
  }

  // Zoom to fit all nodes after syncing
  const allNodes = reteEditor.value.getNodes();
  if (allNodes.length > 0) {
    await AreaExtensions.zoomAt(reteArea.value, allNodes);
    console.log(`syncRuleToEditor: zoomed to fit ${allNodes.length} nodes`);
  }

  console.log('syncRuleToEditor: done');
}

// ─── Export ──────────────────────────────────────────────────────────────────

export function useRuleEditor() {
  return {
    // State (readonly)
    compositeRules: readonly(compositeRules),
    atomicRules: readonly(atomicRules),
    currentRuleId: readonly(currentRuleId),
    currentRule,
    enabledAtomicRules,
    loading: readonly(loading),
    error: readonly(error),
    dirty: readonly(dirty),

    // Rete editor refs
    reteEditor: readonly(reteEditor),
    reteArea: readonly(reteArea),
    setReteEditor,

    // API Operations
    loadAll,
    loadAtomicRules,
    loadCompositeRules,
    saveCompositeRule,
    deleteCompositeRule,

    // Editor Operations
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
    updateOutputAction,

    // Utility
    generateNodeId,
    generateConnectionId,
    getAtomicRule,
    syncRuleToEditor,
  };
}
