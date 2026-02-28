<script setup lang="ts">
const props = defineProps<{
  data: {
    label: string;
    action: string;
    config: Record<string, unknown>;
  };
}>();

const icons: Record<string, string> = {
  reject: '🚫',
  alert: '🔔',
  pass: '✅',
  log: '📋',
  modbus_write: '⚡',
};
</script>

<template>
  <div class="rn-action">
    <span class="rn-action-icon">{{ icons[data.action] ?? '⚙️' }}</span>
    <span class="rn-action-name">{{ data.action.toUpperCase() }}</span>
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

.rn-action-icon {
  font-size: 18px;
}

.rn-action-name {
  font-size: 13px;
  font-weight: 700;
  color: #f1f5f9;
}
</style>
