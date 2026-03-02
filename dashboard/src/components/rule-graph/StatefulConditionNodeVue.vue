<script setup lang="ts">
import { computed } from 'vue';
import { Ref as ReteRef } from 'rete-vue-plugin';
import { SOCKET_TYPE_COLORS } from '@gateway/socket-registry';

const props = defineProps<{
  data: {
    id: string;
    label: string;
    config: {
      condition: 'count_window' | 'consecutive' | 'rate' | 'sustained' | 'cooldown';
      accepts_socket_type: string;
      count: number;
      window_minutes: number;
    };
    inputs: Record<string, { id: string; label?: string; socket: { name: string } }>;
    outputs: Record<string, { id: string; label?: string; socket: { name: string } }>;
  };
  emit: (data: any) => void;
}>();

const CONDITION_LABELS: Record<string, string> = {
  count_window: 'Count in Window',
  consecutive: 'Consecutive',
  rate: 'Rate per Minute',
  sustained: 'Sustained',
  cooldown: 'Cooldown',
};

const badgeColor = computed(() =>
  (SOCKET_TYPE_COLORS as Record<string, string>)[props.data.config.accepts_socket_type] ?? '#6B7280'
);

const conditionLabel = computed(() =>
  CONDITION_LABELS[props.data.config.condition] ?? props.data.config.condition
);

const params = computed(() => {
  const { count, window_minutes } = props.data.config;
  return `${count}x in ${window_minutes}min`;
});

// Get inputs/outputs as arrays for rendering
const inputsList = computed(() => {
  if (!props.data?.inputs) return [];
  return Object.entries(props.data.inputs).map(([key, input]) => ({
    key,
    ...input,
  }));
});

const outputsList = computed(() => {
  if (!props.data?.outputs) return [];
  return Object.entries(props.data.outputs).map(([key, output]) => ({
    key,
    ...output,
  }));
});
</script>

<template>
  <div class="rn-stateful">
    <!-- Input sockets on the left -->
    <div class="rn-inputs">
      <div v-for="input in inputsList" :key="input.key" class="rn-input">
        <ReteRef
          :data="{ type: 'socket', side: 'input', key: input.key, nodeId: data.id, payload: input.socket }"
          :emit="emit"
        >
          <div
            class="rn-socket input-socket"
            :data-input-key="input.key"
            :style="{ background: badgeColor }"
          />
        </ReteRef>
        <span class="rn-input-label">{{ input.label || input.key }}</span>
      </div>
    </div>

    <div class="rn-content">
      <div class="rn-header">
        <span class="rn-label">STATEFUL</span>
        <span class="rn-badge" :style="{ background: badgeColor }">
          {{ data.config.accepts_socket_type }}
        </span>
      </div>
      <div class="rn-condition">{{ conditionLabel }}</div>
      <div class="rn-params">{{ params }}</div>
    </div>

    <!-- Output sockets on the right -->
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
            :style="{ background: '#f59e0b' }"
          />
        </ReteRef>
      </div>
    </div>
  </div>
</template>

<style scoped>
.rn-stateful {
  background: #1e293b;
  border: 1px solid #f59e0b;
  border-radius: 8px;
  padding: 10px 14px;
  min-width: 170px;
  display: flex;
  align-items: center;
  gap: 12px;
}

.rn-content {
  flex: 1;
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

.rn-condition {
  font-size: 13px;
  font-weight: 600;
  color: #f1f5f9;
  margin-bottom: 4px;
}

.rn-params {
  font-size: 11px;
  color: #94a3b8;
}

.rn-inputs {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.rn-input {
  display: flex;
  align-items: center;
  gap: 6px;
}

.rn-input-label {
  font-size: 10px;
  color: #94a3b8;
}

.rn-outputs {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.rn-output {
  display: flex;
  align-items: center;
  gap: 6px;
}

.rn-output-label {
  font-size: 10px;
  color: #94a3b8;
}

.rn-socket {
  width: 12px;
  height: 12px;
  border-radius: 50%;
  cursor: crosshair;
  border: 2px solid #0f172a;
  transition: transform 0.15s ease;
  flex-shrink: 0;
}

.rn-socket:hover {
  transform: scale(1.3);
}

.input-socket {
  margin-left: -24px;
}

.output-socket {
  margin-right: -24px;
}
</style>
