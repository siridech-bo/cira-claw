<script setup lang="ts">
import { computed } from 'vue';
import { Ref as ReteRef } from 'rete-vue-plugin';

const props = defineProps<{
  data: {
    label: string;
    action: string;
    config: Record<string, unknown>;
    id: string;
    inputs: Record<string, { id: string; label?: string; socket: { name: string } }>;
  };
  emit: (data: any) => void;
}>();

const icons: Record<string, string> = {
  reject: '🚫',
  alert: '🔔',
  pass: '✅',
  log: '📋',
  modbus_write: '⚡',
};

// Get inputs as array for rendering
const inputsList = computed(() => {
  if (!props.data?.inputs) return [];
  return Object.entries(props.data.inputs).map(([key, input]) => ({
    key,
    ...input,
  }));
});
</script>

<template>
  <div class="rn-action">
    <!-- Input socket on the left with Rete Ref for position tracking -->
    <div class="rn-inputs">
      <div v-for="input in inputsList" :key="input.key" class="rn-input">
        <ReteRef
          :data="{ type: 'socket', side: 'input', key: input.key, nodeId: data.id, payload: input.socket }"
          :emit="emit"
        >
          <div
            class="rn-socket input-socket"
            :data-input-key="input.key"
          />
        </ReteRef>
      </div>
    </div>
    <span class="rn-action-icon">{{ icons[data?.action] ?? '⚙️' }}</span>
    <span class="rn-action-name">{{ data?.action?.toUpperCase() || 'ACTION' }}</span>
  </div>
</template>

<style scoped>
.rn-action {
  background: #1e293b;
  border: 1px solid #8b5cf6;
  border-radius: 8px;
  padding: 10px 16px;
  display: flex;
  gap: 8px;
  align-items: center;
  min-width: 120px;
}

.rn-inputs {
  display: flex;
  flex-direction: column;
  gap: 4px;
}

.rn-input {
  display: flex;
  align-items: center;
}

.rn-action-icon {
  font-size: 18px;
}

.rn-action-name {
  font-size: 13px;
  font-weight: 700;
  color: #f1f5f9;
}

.rn-socket {
  width: 12px;
  height: 12px;
  border-radius: 50%;
  background: #8b5cf6;
  cursor: crosshair;
  border: 2px solid #0f172a;
  transition: transform 0.15s ease;
}

.rn-socket:hover {
  transform: scale(1.3);
}

.input-socket {
  margin-left: -24px;
}
</style>
