<script setup lang="ts">
import { computed } from 'vue';

const props = defineProps<{
  data: {
    label: string;
    operator: 'AND' | 'OR' | 'NOT';
    id: string;
    inputs: Record<string, { id: string; label?: string; socket: { name: string } }>;
    outputs: Record<string, { id: string; label?: string; socket: { name: string } }>;
  };
  emit: (event: any) => void;
}>();

const colors: Record<string, string> = {
  AND: '#10B981',
  OR: '#F59E0B',
  NOT: '#EF4444',
};

const borderColor = computed(() => {
  if (!props.data?.operator) return '#6B7280';
  return colors[props.data.operator] ?? '#6B7280';
});

const textColor = computed(() => {
  if (!props.data?.operator) return '#6B7280';
  return colors[props.data.operator] ?? '#6B7280';
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
  <div class="rn-op" :style="{ borderColor: borderColor }">
    <!-- Input sockets on the left -->
    <div class="rn-inputs">
      <div v-for="input in inputsList" :key="input.key" class="rn-input">
        <div
          class="rn-socket input-socket"
          :data-input-key="input.key"
          :style="{ background: borderColor }"
        />
        <span class="rn-input-label">{{ input.label || input.key }}</span>
      </div>
    </div>

    <span class="rn-op-label" :style="{ color: textColor }">{{ data?.operator || 'OP' }}</span>

    <!-- Output socket on the right -->
    <div class="rn-outputs">
      <div v-for="output in outputsList" :key="output.key" class="rn-output">
        <span class="rn-output-label">{{ output.label || 'Result' }}</span>
        <div
          class="rn-socket output-socket"
          :data-output-key="output.key"
          :style="{ background: borderColor }"
        />
      </div>
    </div>
  </div>
</template>

<style scoped>
.rn-op {
  background: #1e293b;
  border: 2px solid;
  border-radius: 8px;
  padding: 12px 16px;
  min-width: 120px;
  display: flex;
  align-items: center;
  gap: 12px;
  position: relative;
}

.rn-op-label {
  font-size: 14px;
  font-weight: 700;
  letter-spacing: 0.04em;
  flex: 1;
  text-align: center;
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
