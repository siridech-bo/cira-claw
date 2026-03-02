<script setup lang="ts">
import { computed } from 'vue';
import { Ref as ReteRef } from 'rete-vue-plugin';
import { SOCKET_TYPE_COLORS } from '@gateway/socket-registry';

const props = defineProps<{
  data: {
    id: string;
    label: string;
    ruleId: string;
    ruleName: string;
    socketType: string;
    reads: string[];
    outputs: Record<string, { id: string; label?: string; socket: { name: string } }>;
  };
  emit: (data: any) => void;
}>();

const badgeColor = computed(() => {
  if (!props.data?.socketType) return '#6B7280';
  return (SOCKET_TYPE_COLORS as Record<string, string>)[props.data.socketType] ?? '#6B7280';
});

const socketColor = computed(() => {
  if (!props.data?.socketType) return '#6B7280';
  return (SOCKET_TYPE_COLORS as Record<string, string>)[props.data.socketType] ?? '#6B7280';
});

// Get outputs as array for rendering
const outputsList = computed(() => {
  if (!props.data?.outputs) return [];
  return Object.entries(props.data.outputs).map(([key, output]) => ({
    key,
    ...output,
  }));
});
</script>

<template>
  <div class="rn-atomic">
    <template v-if="data">
      <div class="rn-header">
        <span class="rn-label">ATOMIC RULE</span>
        <span class="rn-badge" :style="{ background: badgeColor }">{{ data.socketType || 'unknown' }}</span>
      </div>
      <div class="rn-name">{{ data.label || data.ruleName || data.ruleId || 'Unnamed' }}</div>
      <div class="rn-reads" v-if="data.reads && data.reads.length">
        <span v-for="f in data.reads.slice(0, 2)" :key="f" class="rn-field">{{ f }}</span>
        <span v-if="data.reads.length > 2" class="rn-more">+{{ data.reads.length - 2 }}</span>
      </div>
      <!-- Output sockets with Rete Ref for position tracking -->
      <div class="rn-outputs">
        <div v-for="output in outputsList" :key="output.key" class="rn-output">
          <span class="rn-output-label">{{ output.label || 'Result' }}</span>
          <ReteRef
            :data="{ type: 'socket', side: 'output', key: output.key, nodeId: data.id, payload: output.socket }"
            :emit="emit"
          >
            <div
              class="rn-socket output-socket"
              :data-output-key="output.key"
              :style="{ background: socketColor }"
            />
          </ReteRef>
        </div>
      </div>
    </template>
    <template v-else>
      <div class="rn-error">No data</div>
    </template>
  </div>
</template>

<style scoped>
.rn-atomic {
  background: #1e293b;
  border: 1px solid #6366f1;
  border-radius: 8px;
  padding: 10px 14px;
  min-width: 160px;
}

.rn-header {
  display: flex;
  gap: 8px;
  align-items: center;
  margin-bottom: 6px;
}

.rn-label {
  font-size: 9px;
  font-weight: 700;
  letter-spacing: 0.08em;
  color: #94a3b8;
  text-transform: uppercase;
}

.rn-badge {
  font-size: 10px;
  color: #fff;
  padding: 1px 7px;
  border-radius: 8px;
  font-weight: 600;
}

.rn-name {
  font-size: 13px;
  font-weight: 600;
  color: #f1f5f9;
  margin-bottom: 4px;
}

.rn-reads {
  display: flex;
  flex-wrap: wrap;
  gap: 3px;
}

.rn-field {
  font-size: 10px;
  font-family: monospace;
  background: #0f172a;
  color: #94a3b8;
  padding: 1px 5px;
  border-radius: 3px;
  border: 1px solid #334155;
}

.rn-more {
  font-size: 10px;
  color: #64748b;
  padding: 1px 4px;
}

.rn-error {
  color: #ef4444;
  font-size: 12px;
}

.rn-outputs {
  display: flex;
  flex-direction: column;
  align-items: flex-end;
  margin-top: 8px;
  gap: 4px;
}

.rn-output {
  display: flex;
  align-items: center;
  gap: 6px;
}

.rn-output-label {
  font-size: 10px;
  color: #64748b;
}

.rn-socket {
  width: 14px;
  height: 14px;
  border-radius: 50%;
  cursor: crosshair;
  border: 2px solid #1e293b;
  transition: transform 0.15s ease;
}

.rn-socket:hover {
  transform: scale(1.3);
}

.output-socket {
  margin-right: -20px;
}
</style>
