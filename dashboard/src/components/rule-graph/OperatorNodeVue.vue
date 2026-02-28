<script setup lang="ts">
import { computed } from 'vue';

const props = defineProps<{
  data: {
    label: string;
    operator: 'AND' | 'OR' | 'NOT';
  };
}>();

const colors: Record<string, string> = {
  AND: '#10B981',
  OR: '#F59E0B',
  NOT: '#EF4444',
};

const borderColor = computed(() => colors[props.data.operator] ?? '#6B7280');
const textColor = computed(() => colors[props.data.operator] ?? '#6B7280');
</script>

<template>
  <div class="rn-op" :style="{ borderColor: borderColor }">
    <span class="rn-op-label" :style="{ color: textColor }">{{ data.operator }} GATE</span>
  </div>
</template>

<style scoped>
.rn-op {
  background: #1e293b;
  border: 2px solid;
  border-radius: 8px;
  padding: 12px 20px;
  min-width: 100px;
  text-align: center;
}

.rn-op-label {
  font-size: 16px;
  font-weight: 700;
  letter-spacing: 0.04em;
}
</style>
