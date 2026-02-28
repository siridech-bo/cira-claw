<script setup lang="ts">
import { computed } from 'vue';
import { SOCKET_TYPE_COLORS } from '@gateway/socket-registry';

const props = defineProps<{
  data: {
    label: string;
    ruleId: string;
    ruleName: string;
    socketType: string;
    reads: string[];
  };
}>();

const badgeColor = computed(() =>
  (SOCKET_TYPE_COLORS as Record<string, string>)[props.data.socketType] ?? '#6B7280'
);
</script>

<template>
  <div class="rn-atomic">
    <div class="rn-header">
      <span class="rn-label">ATOMIC RULE</span>
      <span class="rn-badge" :style="{ background: badgeColor }">{{ data.socketType }}</span>
    </div>
    <div class="rn-name">{{ data.label || data.ruleName || data.ruleId }}</div>
    <div class="rn-reads" v-if="data.reads && data.reads.length">
      <span v-for="f in data.reads.slice(0, 2)" :key="f" class="rn-field">{{ f }}</span>
      <span v-if="data.reads.length > 2" class="rn-more">+{{ data.reads.length - 2 }}</span>
    </div>
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
</style>
