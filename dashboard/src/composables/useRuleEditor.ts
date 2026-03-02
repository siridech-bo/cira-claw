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
    // Skip sync when suppressed (e.g., during node removal)
    if (_suppressConnectionSync) return context;

    if (context.type === 'connectioncreated') {
      const conn = context.data;
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
          }
        }
      }
    }

    if (context.type === 'connectionremoved') {
      const conn = context.data;
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

// Guard to suppress connection sync during node removal (prevents double-sync race)
let _suppressConnectionSync = false;

// Flag to track if data has been loaded (prevents reloading on navigation)
let _dataLoaded = false;

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

async function loadAll(forceReload = false): Promise<void> {
  // Skip loading if data already loaded (prevents losing local state on navigation)
  if (_dataLoaded && !forceReload) {
    return;
  }

  loading.value = true;
  error.value = null;

  try {
    await Promise.all([loadAtomicRules(), loadCompositeRules()]);
    _dataLoaded = true;
  } finally {
    loading.value = false;
  }
}

function isDataLoaded(): boolean {
  return _dataLoaded;
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
  console.log('[addNode] Called with node:', node.type, node.id);

  if (!currentRuleId.value) {
    console.warn('[addNode] No currentRuleId');
    return;
  }

  const rule = compositeRules.value.find(r => r.id === currentRuleId.value);
  if (rule) {
    rule.nodes.push(node);
    dirty.value = true;
    console.log('[addNode] Added to Vue state. Total nodes:', rule.nodes.length);

    // Also add to Rete.js editor if available
    if (reteEditor.value && reteArea.value) {
      console.log('[addNode] Rete editor available, adding to canvas');
      const editor = reteEditor.value;
      const area = reteArea.value;

      try {
        const reteNode = createReteNode(node);
        if (reteNode) {
          // Add node to editor
          await editor.addNode(reteNode);
          console.log('[addNode] Added to Rete editor. Total nodes in editor:', editor.getNodes().length);

          // Verify node exists in editor
          const nodeInEditor = editor.getNode(reteNode.id);
          console.log('[addNode] Node retrieved from editor:', !!nodeInEditor, nodeInEditor?.id);

          // Position the node
          await area.translate(reteNode.id, { x: node.position.x, y: node.position.y });
          console.log('[addNode] Positioned at:', node.position.x, node.position.y);

          // Force area to update the node view
          try {
            await area.update('node', reteNode.id);
            console.log('[addNode] Area updated for node');
          } catch (updateErr) {
            console.warn('[addNode] Area update warning:', updateErr);
          }

          // Zoom to show all nodes after a short delay
          setTimeout(async () => {
            if (reteArea.value && reteEditor.value) {
              const nodes = reteEditor.value.getNodes();
              console.log('[addNode] Zooming to show', nodes.length, 'nodes');
              if (nodes.length > 0) {
                await AreaExtensions.zoomAt(reteArea.value, nodes);
              }
            }
          }, 150);
        } else {
          console.warn('[addNode] createReteNode returned null for type:', node.type);
        }
      } catch (err) {
        console.error('[addNode] Error adding to Rete:', err);
      }
    } else {
      console.warn('[addNode] Rete editor NOT available. reteEditor:', !!reteEditor.value, 'reteArea:', !!reteArea.value);
    }
  } else {
    console.warn('[addNode] Rule not found for id:', currentRuleId.value);
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

    // Remove from Rete editor (with sync suppression to prevent double-sync race)
    if (reteEditor.value) {
      _suppressConnectionSync = true;
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
      } finally {
        _suppressConnectionSync = false;
      }
    }
  }
}

async function undoDeleteNode(): Promise<CompositeNode | null> {
  if (undoStack.value.length === 0) {
    return null;
  }

  const entry = undoStack.value.pop()!;

  // Check if we're still on the same rule
  if (currentRuleId.value !== entry.ruleId) {
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
      }
    } catch {
      // Error restoring node to Rete editor
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
 * Verify and repair Rete connections to match Vue state.
 * Call this after operations that might desync Rete from Vue.
 */
async function repairReteConnections(): Promise<void> {
  if (!currentRule.value || !reteEditor.value) {
    return;
  }

  const rule = currentRule.value;
  const reteConnections = reteEditor.value.getConnections();

  // Build a set of existing Rete connections for fast lookup
  const reteConnSet = new Set(
    reteConnections.map(c => `${c.source}:${c.sourceOutput}->${c.target}:${c.targetInput}`)
  );

  // Suppress sync during repair
  _suppressConnectionSync = true;

  try {
    // Add any missing connections
    for (const conn of rule.connections) {
      const connKey = `${conn.source_node}:${conn.source_socket}->${conn.target_node}:${conn.target_socket}`;
      if (!reteConnSet.has(connKey)) {
        // This connection is in Vue but not in Rete - add it
        const sourceNode = reteEditor.value.getNode(conn.source_node);
        const targetNode = reteEditor.value.getNode(conn.target_node);

        if (sourceNode && targetNode) {
          const sourceOutput = sourceNode.outputs[conn.source_socket];
          const targetInput = targetNode.inputs[conn.target_socket];

          if (sourceOutput && targetInput) {
            const reteConnection = new ClassicPreset.Connection(
              sourceNode,
              conn.source_socket,
              targetNode,
              conn.target_socket
            );
            await reteEditor.value.addConnection(reteConnection);
          }
        }
      }
    }
  } finally {
    _suppressConnectionSync = false;
  }
}

/**
 * Sync all nodes from the current rule to the Rete.js editor.
 * Call this after initializing the editor when a rule is selected.
 */
async function syncRuleToEditor(): Promise<void> {
  if (!currentRule.value || !reteEditor.value || !reteArea.value) {
    console.log('[syncRuleToEditor] Missing requirements:', {
      hasRule: !!currentRule.value,
      hasEditor: !!reteEditor.value,
      hasArea: !!reteArea.value,
    });
    return;
  }

  const rule = currentRule.value;
  const editor = reteEditor.value;
  const area = reteArea.value;

  console.log('[syncRuleToEditor] Syncing rule:', rule.id, 'with', rule.nodes.length, 'nodes and', rule.connections.length, 'connections');

  // Suppress connection sync during initial load to prevent duplicates
  _suppressConnectionSync = true;

  try {
    // Add all nodes to the editor
    for (const node of rule.nodes) {
      const reteNode = createReteNode(node);
      if (reteNode) {
        await editor.addNode(reteNode);
        await area.translate(reteNode.id, { x: node.position.x, y: node.position.y });
        console.log('[syncRuleToEditor] Added node:', reteNode.id, 'at', node.position.x, node.position.y);
      }
    }

    // Add all connections to the editor
    for (const conn of rule.connections) {
      try {
        const sourceNode = editor.getNode(conn.source_node);
        const targetNode = editor.getNode(conn.target_node);

        if (sourceNode && targetNode) {
          const sourceOutput = sourceNode.outputs[conn.source_socket];
          const targetInput = targetNode.inputs[conn.target_socket];

          if (sourceOutput && targetInput) {
            const reteConnection = new ClassicPreset.Connection(sourceNode, conn.source_socket, targetNode, conn.target_socket);
            await editor.addConnection(reteConnection);
            console.log('[syncRuleToEditor] Added connection:', conn.source_node, '->', conn.target_node);
          }
        }
      } catch {
        // Skip invalid connection
      }
    }

    // Zoom to fit all nodes after syncing - delay to ensure Vue has rendered the nodes
    const allNodes = editor.getNodes();
    console.log('[syncRuleToEditor] Total nodes in editor after sync:', allNodes.length);
    if (allNodes.length > 0) {
      // Wait for Vue to render the node components before zooming
      // Use multiple delays to ensure DOM is fully ready
      await new Promise<void>(resolve => requestAnimationFrame(() => resolve()));
      await new Promise<void>(resolve => setTimeout(resolve, 200));

      // Now zoom to fit all nodes
      try {
        await AreaExtensions.zoomAt(area, allNodes);
        console.log('[syncRuleToEditor] First zoom attempt completed');

        // Second zoom attempt after another delay for reliability
        await new Promise<void>(resolve => setTimeout(resolve, 100));
        await AreaExtensions.zoomAt(area, allNodes);
        console.log('[syncRuleToEditor] Zoomed to fit', allNodes.length, 'nodes');
      } catch (zoomErr) {
        console.warn('[syncRuleToEditor] Zoom error:', zoomErr);
      }
    }
  } finally {
    _suppressConnectionSync = false;
  }
}

/**
 * Zoom to fit all nodes in the viewport.
 * Sets CSS transform directly for reliable centering.
 */
async function zoomToFit(): Promise<void> {
  if (!reteEditor.value || !reteArea.value) {
    return;
  }

  const editor = reteEditor.value;
  const area = reteArea.value;
  const nodes = editor.getNodes();

  if (nodes.length === 0) {
    return;
  }

  try {
    await new Promise<void>(resolve => setTimeout(resolve, 100));

    // Calculate bounding box
    let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;

    for (const node of nodes) {
      const view = area.nodeViews.get(node.id);
      if (!view) continue;

      const el = view.element as HTMLElement | undefined;
      const width = el?.offsetWidth || 180;
      const height = el?.offsetHeight || 100;

      minX = Math.min(minX, view.position.x);
      minY = Math.min(minY, view.position.y);
      maxX = Math.max(maxX, view.position.x + width);
      maxY = Math.max(maxY, view.position.y + height);
    }

    if (!isFinite(minX)) return;

    const containerW = area.container.clientWidth;
    const containerH = area.container.clientHeight;
    const padding = 60;
    const contentW = maxX - minX + padding * 2;
    const contentH = maxY - minY + padding * 2;

    const k = Math.min(containerW / contentW, containerH / contentH, 1.0);
    const contentCenterX = (minX + maxX) / 2;
    const contentCenterY = (minY + maxY) / 2;
    const x = (containerW / 2) - (contentCenterX * k);
    const y = (containerH / 2) - (contentCenterY * k);

    // Find the transform element (Rete's inner container)
    const transformEl = area.container.firstElementChild as HTMLElement;
    if (transformEl) {
      // Apply CSS transform directly
      transformEl.style.transform = `translate(${x}px, ${y}px) scale(${k})`;
      transformEl.style.transformOrigin = '0 0';

      // Update Rete's internal transform state to match
      const transform = area.area.transform;
      Object.assign(transform, { k, x, y });
    }
  } catch (err) {
    console.error('[zoomToFit] Error:', err);
  }
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
    zoomToFit,

    // Utility
    generateNodeId,
    generateConnectionId,
    getAtomicRule,
    syncRuleToEditor,
    repairReteConnections,
    isDataLoaded,
  };
}
